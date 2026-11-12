// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-gnssd: GNSS on the Titan 2 for Linux — the legacy GPS HAL module of the factory firmware
 * (gps.default.so, hardware/gps.h) loaded in-process through bionic_translation,
 * exactly as t2-fp loads the fingerprint HAL. The module is a thin client of the
 * navigation daemon mnld of the factory firmware (abstract datagram sockets mtk_hal2mnl /
 * mtk_mnl2hal; mnld runs natively in the vendor chroot: OpenRC t2-mnld), so no
 * binder, servicemanager, SELinux policy or AIDL service is involved.
 *
 *   gps.default.so ──▶ mnld ──▶ /dev/gpsdl0 (mtk-gps-dl, GPL) ──▶ GNSS DSP
 *        ▲ callbacks (NMEA, fixes, SV status, status, wakelock, time requests)
 *   t2-gnssd ──▶ /run/t2-gnss/nmea.sock ──▶ GeoClue ([network-nmea] nmea-socket)
 *
 * The receiver runs only while somebody is connected to the NMEA socket (GeoClue
 * connects when an application needs a location) or with -a. On every session
 * start the daemon helps the TTFF the way the Android framework does, without
 * any closed SUPL client:
 *   - inject_time(): the system clock (NTP/RTC) with its uncertainty;
 *   - inject_location(): the last fix (cached in /var/lib/t2-gnss/last-fix, its
 *     accuracy inflated with age) as a coarse reference position;
 *   - inject_xtra_data(): the MediaTek EPO predicted-ephemeris file fetched by
 *     the open downloader t2-epo-fetch (/var/lib/t2-gnss/epo/EPO.DAT); the
 *     HAL's XTRA download request also triggers a fetch when the file is stale.
 * NMEA comes from mnld (nmea_cb); if the HAL reports fixes but no sentences,
 * GGA/RMC (and GSV with -S) are synthesized from location_cb/gnss_sv_status_cb.
 *
 * Interface facts only: hardware/gps.h and gnss-base.h are the published AOSP
 * headers (Apache-2.0); nothing of the closed module or of mnld is reproduced.
 *
 * Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timex.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <hardware/gps.h>
#include "t2-hal.h"

#define DEFAULT_LIB	"/vendor/lib64/hw/gps.default.so"
#define DEFAULT_SOCKET	"/run/t2-gnss/nmea.sock"
#define DEFAULT_STATE	"/var/lib/t2-gnss"
#define DEFAULT_EPO	DEFAULT_STATE "/epo/EPO.DAT"
#define FIX_CACHE	"last-fix"
#define WAKE_LOCK_NAME	"t2-gnss"
#define EPO_FETCH	"t2-epo-fetch"
#define MAX_CLIENTS	16
#define NMEA_MAX	256

/*
 * The HAL copies our callback table by pointer and CHECKS ITS SIZE: the Titan 2
 * build of gps.default.so refuses the AOSP table ("gpshal_gpscbs_save: Bad
 * callback, size: 96, expected: 120", phone 2026-11-11) — it wants 15 slots,
 * i.e. 3 MediaTek extension callbacks appended to the 12 of hardware/gps.h.
 * Their signatures are not published, so the extra slots hold harmless stubs
 * (no arguments, return 0: safe to call with any AArch64 argument registers)
 * that log the call; the table lives inside a zero-filled block so an even
 * longer read sees NULL instead of garbage. -C N overrides the size.
 */
#define MTK_GPS_CALLBACKS_SIZE	120
#define AOSP_CB_SLOTS		(sizeof(GpsCallbacks) / sizeof(void *))

union cb_block {
	GpsCallbacks cb;
	void *slot[32];
};

/* extension callback tables have no size field; pad them with stubs too */
union xtra_cb_block { GpsXtraCallbacks cb; void *slot[8]; };
union agps_cb_block { AGpsCallbacks cb; void *slot[8]; };
union ril_cb_block { AGpsRilCallbacks cb; void *slot[8]; };

struct client {
	int fd;
};

static struct {
	/* options */
	const char *root, *lib, *sock_path, *state_dir, *epo_path;
	bool always_on, to_stdout, want_sv, verbose, diag, no_wakelock, no_time;
	bool no_location, no_epo, no_fetch, cold, raw_loc, psds_experiment;
	int raw_dumps;
	int interval_ms, grace_s, run_s, wait_s, mode, time_unc_ms, synth, epo_max_age_s;

	/* HAL */
	const GpsInterface *gi;
	const GpsXtraInterface *xtra;
	const void *psds;	/* MediaTek "aidl_gnss_psds_interface" (layout unpublished; -X) */
	const AGpsInterface *agps;
	const AGpsRilInterface *ril;
	union cb_block cbs;
	union xtra_cb_block xtra_cbs;
	union agps_cb_block agps_cbs;
	union ril_cb_block ril_cbs;
	uint32_t capabilities;
	size_t cb_size;

	/* state (mu) */
	pthread_mutex_t mu;
	struct client clients[MAX_CLIENTS];
	int nclients;
	int listen_fd, wake_fd[2];
	bool running, want_stop, quit, wakelock_held;
	volatile sig_atomic_t ev_time, ev_xtra, ev_agps_open, ev_agps_close;
	int64_t session_started_ms, first_fix_ms, last_hal_nmea_ms, last_cache_ms, stop_at_ms, quit_at_ms;
	int64_t epo_injected_mtime, fetch_started_ms, start_retry_at_ms;
	long nmea_lines, fixes;
	GpsLocation loc;
	bool have_loc;
	GnssSvInfo sv[GNSS_MAX_SVS];
	int nsv;
	bool status_engine_on;
} g = {
	.lib = DEFAULT_LIB, .sock_path = DEFAULT_SOCKET, .state_dir = DEFAULT_STATE, .epo_path = DEFAULT_EPO,
	.interval_ms = 1000, .grace_s = 10, .wait_s = 60, .time_unc_ms = -1, .epo_max_age_s = 6 * 3600,
	.cb_size = MTK_GPS_CALLBACKS_SIZE,
	.mu = PTHREAD_MUTEX_INITIALIZER, .listen_fd = -1, .wake_fd = { -1, -1 },
};

static void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logmsg(const char *fmt, ...)
{
	va_list ap;
	char ts[32];
	time_t t = time(NULL);
	struct tm tm;

	strftime(ts, sizeof(ts), "%H:%M:%S", localtime_r(&t, &tm));
	fprintf(stderr, "%s t2-gnssd: ", ts);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

#define dbg(...) do { if (g.verbose) logmsg(__VA_ARGS__); } while (0)

static int64_t now_ms(clockid_t clk)
{
	struct timespec ts;

	clock_gettime(clk, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* wake the main loop from a HAL thread or a signal handler */
static void wake_main(void)
{
	char c = 0;

	if (g.wake_fd[1] >= 0) {
		ssize_t r = write(g.wake_fd[1], &c, 1);
		(void)r;
	}
}

/* ---------- NMEA output (any thread; mu held by the caller) ---------- */

static void client_close_locked(int idx)
{
	close(g.clients[idx].fd);
	memmove(&g.clients[idx], &g.clients[idx + 1], (g.nclients - idx - 1) * sizeof(g.clients[0]));
	g.nclients--;
	logmsg("NMEA client left (%d left)", g.nclients);
	wake_main();
}

static void nmea_send_locked(const char *line, size_t len)
{
	int i;

	g.nmea_lines++;
	if (g.to_stdout) {
		fwrite(line, 1, len, stdout);
		fflush(stdout);
	}
	for (i = 0; i < g.nclients; ) {
		ssize_t n = send(g.clients[i].fd, line, len, MSG_NOSIGNAL | MSG_DONTWAIT);

		if (n < 0 && errno != EAGAIN && errno != EINTR) {
			dbg("client fd %d: %s, dropped", g.clients[i].fd, strerror(errno));
			client_close_locked(i);
			continue;
		}
		i++;
	}
}

/* body without '$' and '*cs' */
static void nmea_send_sentence_locked(const char *body)
{
	unsigned cs = 0;
	const char *p;
	char line[NMEA_MAX];
	int n;

	for (p = body; *p; p++)
		cs ^= (unsigned char)*p;
	n = snprintf(line, sizeof(line), "$%s*%02X\r\n", body, cs);
	if (n > 0 && n < (int)sizeof(line))
		nmea_send_locked(line, n);
}

static void fmt_latlon(double v, bool is_lat, char *out, size_t n, char *hemi)
{
	double av = fabs(v);
	int deg = (int)av;
	double min = (av - deg) * 60.0;

	snprintf(out, n, is_lat ? "%02d%07.4f" : "%03d%07.4f", deg, min);
	*hemi = is_lat ? (v < 0 ? 'S' : 'N') : (v < 0 ? 'W' : 'E');
}

static const char *talker_of(int constellation)
{
	switch (constellation) {
	case GNSS_CONSTELLATION_GPS: return "GP";
	case GNSS_CONSTELLATION_SBAS: return "GP";
	case GNSS_CONSTELLATION_GLONASS: return "GL";
	case GNSS_CONSTELLATION_QZSS: return "GQ";
	case GNSS_CONSTELLATION_BEIDOU: return "GB";
	case GNSS_CONSTELLATION_GALILEO: return "GA";
	default: return "GN";
	}
}

/* GGA + RMC (what GeoClue parses) from the last fix; GSV from the last SV status with -S */
static void nmea_synthesize_locked(void)
{
	const GpsLocation *l = &g.loc;
	char lat[16], lon[16], body[200], hms[16], dmy[8], alt[24] = "", spd[16] = "", crs[16] = "";
	char ns, ew;
	time_t sec = (time_t)(l->timestamp / 1000);
	struct tm tm;
	int used = 0, i;
	double hdop;

	if (!(l->flags & GPS_LOCATION_HAS_LAT_LONG))
		return;
	gmtime_r(&sec, &tm);
	snprintf(hms, sizeof(hms), "%02d%02d%02d.%02d", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(l->timestamp % 1000) / 10);
	snprintf(dmy, sizeof(dmy), "%02d%02d%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year % 100);
	fmt_latlon(l->latitude, true, lat, sizeof(lat), &ns);
	fmt_latlon(l->longitude, false, lon, sizeof(lon), &ew);
	for (i = 0; i < g.nsv; i++)
		if (g.sv[i].flags & GNSS_SV_FLAGS_USED_IN_FIX)
			used++;
	/* no DOP in GpsLocation: approximate HDOP from the horizontal accuracy (UERE ~5 m) */
	hdop = (l->flags & GPS_LOCATION_HAS_ACCURACY) ? l->accuracy / 5.0 : 1.0;
	if (hdop < 0.5) hdop = 0.5;
	if (hdop > 99.9) hdop = 99.9;
	if (l->flags & GPS_LOCATION_HAS_ALTITUDE)
		snprintf(alt, sizeof(alt), "%.1f", l->altitude);
	if (l->flags & GPS_LOCATION_HAS_SPEED)
		snprintf(spd, sizeof(spd), "%.1f", l->speed * 1.943844);
	if (l->flags & GPS_LOCATION_HAS_BEARING)
		snprintf(crs, sizeof(crs), "%.1f", l->bearing);

	snprintf(body, sizeof(body), "GPGGA,%s,%s,%c,%s,%c,1,%02d,%.1f,%s,M,,M,,", hms, lat, ns, lon, ew, used, hdop, alt);
	nmea_send_sentence_locked(body);
	snprintf(body, sizeof(body), "GPRMC,%s,A,%s,%c,%s,%c,%s,%s,%s,,,A", hms, lat, ns, lon, ew, spd, crs, dmy);
	nmea_send_sentence_locked(body);

	if (g.want_sv && g.nsv > 0) {
		int seen[16], nseen = 0, c;

		for (i = 0; i < g.nsv; i++) {
			bool dup = false;
			for (c = 0; c < nseen; c++)
				if (seen[c] == g.sv[i].constellation)
					dup = true;
			if (!dup && nseen < 16)
				seen[nseen++] = g.sv[i].constellation;
		}
		for (c = 0; c < nseen; c++) {
			int n = 0, total, idx = 0, k;
			for (i = 0; i < g.nsv; i++)
				if (g.sv[i].constellation == seen[c])
					n++;
			total = (n + 3) / 4;
			for (i = 0; i < g.nsv; ) {
				char *w = body;
				int left = sizeof(body);
				int m = snprintf(w, left, "%sGSV,%d,%d,%02d", talker_of(seen[c]), total, ++idx, n);
				w += m; left -= m;
				for (k = 0; k < 4 && i < g.nsv; i++) {
					const GnssSvInfo *s = &g.sv[i];
					if (s->constellation != seen[c])
						continue;
					m = snprintf(w, left, ",%02d,%02d,%03d,%02d", s->svid, (int)s->elevation, (int)s->azimuth, (int)s->c_n0_dbhz);
					w += m; left -= m;
					k++;
				}
				if (k == 0)
					break;
				nmea_send_sentence_locked(body);
				if (idx >= total)
					break;
			}
		}
	}
}

/* ---------- last-fix cache ---------- */

static void fix_cache_path(char *buf, size_t n)
{
	snprintf(buf, n, "%s/%s", g.state_dir, FIX_CACHE);
}

static void fix_cache_write_locked(void)
{
	char path[256], tmp[264];
	FILE *f;

	if (!g.have_loc || !(g.loc.flags & GPS_LOCATION_HAS_LAT_LONG))
		return;
	fix_cache_path(path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "%.6f %.6f %.1f %lld\n", g.loc.latitude, g.loc.longitude,
		(g.loc.flags & GPS_LOCATION_HAS_ACCURACY) ? g.loc.accuracy : 100.0f, (long long)g.loc.timestamp);
	fclose(f);
	rename(tmp, path);
}

/* lat, lon, accuracy (m) of the cached fix, the accuracy inflated by its age; 0 = none/too old */
static int fix_cache_read(double *lat, double *lon, float *acc)
{
	char path[256];
	FILE *f;
	double a;
	long long ts, age_s;

	fix_cache_path(path, sizeof(path));
	f = fopen(path, "r");
	if (!f)
		return 0;
	if (fscanf(f, "%lf %lf %lf %lld", lat, lon, &a, &ts) != 4) {
		fclose(f);
		return 0;
	}
	fclose(f);
	age_s = (now_ms(CLOCK_REALTIME) - ts) / 1000;
	if (age_s < 0)
		age_s = 0;
	/* a phone moves: ~30 m/s worst case for the first hour, then the position is a region hint only */
	a += (double)age_s * 30.0;
	if (a > 300000.0)
		return 0;	/* 300 km: no longer useful as a reference */
	if (a < 50.0)
		a = 50.0;
	*acc = (float)a;
	return 1;
}

/* ---------- assistance: time, reference location, EPO ---------- */

static void inject_time(void)
{
	struct timex tx = { 0 };
	int unc = g.time_unc_ms;
	int64_t utc = now_ms(CLOCK_REALTIME);
	int rc;

	if (g.no_time || !g.gi || !g.gi->inject_time)
		return;
	if (utc < 1735689600000LL) {	/* before 2025: the clock was never set */
		logmsg("inject_time: the system clock is not set, skipping");
		return;
	}
	if (unc < 0) {
		/* STA_UNSYNC: no NTP discipline (RTC only) — still worth injecting, with a wide window */
		unc = (adjtimex(&tx) >= 0 && !(tx.status & STA_UNSYNC)) ? 1000 : 10000;
	}
	rc = g.gi->inject_time(utc, now_ms(CLOCK_BOOTTIME), unc);
	logmsg("inject_time: utc %lld ms, uncertainty %d ms -> %d", (long long)utc, unc, rc);
}

static void inject_location(void)
{
	double lat, lon;
	float acc;
	int rc;

	if (g.no_location || !g.gi || !g.gi->inject_location)
		return;
	if (!fix_cache_read(&lat, &lon, &acc)) {
		dbg("inject_location: no usable cached fix");
		return;
	}
	rc = g.gi->inject_location(lat, lon, acc);
	logmsg("inject_location: %.5f %.5f +-%.0f m (last fix) -> %d", lat, lon, acc, rc);
}

static int64_t file_mtime(const char *path, off_t *size)
{
	struct stat st;

	if (stat(path, &st) < 0)
		return 0;
	if (size)
		*size = st.st_size;
	return (int64_t)st.st_mtime;
}

/* hand the EPO file to mnld through the XTRA interface (what the Android framework's PSDS downloader does) */
static void inject_epo(bool force)
{
	off_t size = 0;
	int64_t mt = file_mtime(g.epo_path, &size);
	char *buf;
	FILE *f;
	int rc;

	if (g.no_epo)
		return;
	if (!g.xtra || !g.xtra->inject_xtra_data) {
		if (!(g.psds && g.psds_experiment))
			return;
	}
	if (!mt) {
		dbg("EPO: no %s yet", g.epo_path);
		return;
	}
	if (!force && mt == g.epo_injected_mtime)
		return;
	if (size <= 0 || size > 4 * 1024 * 1024 || size % 72) {
		logmsg("EPO: %s has an unexpected size %lld (not a multiple of 72 bytes), not injected", g.epo_path, (long long)size);
		return;
	}
	buf = malloc(size);
	f = fopen(g.epo_path, "rb");
	if (!buf || !f || fread(buf, 1, size, f) != (size_t)size) {
		logmsg("EPO: cannot read %s: %s", g.epo_path, strerror(errno));
		free(buf);
		if (f)
			fclose(f);
		return;
	}
	fclose(f);
	if (g.xtra && g.xtra->inject_xtra_data) {
		rc = g.xtra->inject_xtra_data(buf, (int)size);
	} else {
		/*
		 * -X experiment: assume the MediaTek PSDS table mirrors the AIDL IGnssPsds
		 * (size, set_callback, inject_psds_data) with the AIDL argument order
		 * (psdsType, data, length); PsdsType LONG_TERM = 1. The call may be wrong
		 * (unpublished layout) — the log shows what the HAL printed for it.
		 */
		size_t psz = *(const size_t *)g.psds;
		int (*inject)(int, const char *, int) = NULL;
		if (psz >= 3 * sizeof(void *))
			inject = (int (*)(int, const char *, int))((void *const *)g.psds)[2];
		if (!inject) {
			logmsg("EPO: PSDS experiment: no inject entry (struct size %zu)", psz);
			free(buf);
			return;
		}
		logmsg("EPO: PSDS experiment: calling slot 2 as inject_psds_data(LONG_TERM, data, %lld)", (long long)size);
		rc = inject(1, buf, (int)size);
	}
	free(buf);
	g.epo_injected_mtime = mt;
	logmsg("EPO: injected %s (%lld bytes, %lld h old) -> %d", g.epo_path, (long long)size,
	       (long long)(time(NULL) - mt) / 3600, rc);
}

/* run the open downloader (titan2-epo) in the background when the file is stale */
static void epo_fetch_maybe(bool force)
{
	int64_t mt, now = now_ms(CLOCK_MONOTONIC);
	pid_t pid;

	if (g.no_epo || g.no_fetch)
		return;
	mt = file_mtime(g.epo_path, NULL);
	if (!force && mt && time(NULL) - mt < g.epo_max_age_s)
		return;
	if (g.fetch_started_ms && now - g.fetch_started_ms < 10 * 60 * 1000)
		return;	/* one attempt per 10 minutes */
	g.fetch_started_ms = now;
	pid = fork();
	if (pid < 0)
		return;
	if (pid == 0) {
		int fd = open("/dev/null", O_RDWR);

		if (fd >= 0) {
			dup2(fd, 0);
			close(fd);
		}
		/* the downloader prints to stderr: keep our log */
		execlp(EPO_FETCH, EPO_FETCH, "-q", "--install", g.root, (char *)NULL);
		_exit(127);
	}
	logmsg("EPO: %s started (pid %d)", EPO_FETCH, pid);
}

/* ---------- wake lock (CONFIG_PM_WAKELOCKS) ---------- */

static void wakelock(bool acquire)
{
	const char *path = acquire ? "/sys/power/wake_lock" : "/sys/power/wake_unlock";
	int fd;

	if (g.no_wakelock || g.wakelock_held == acquire)
		return;
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		dbg("%s: %s", path, strerror(errno));
		return;
	}
	if (write(fd, WAKE_LOCK_NAME, strlen(WAKE_LOCK_NAME)) > 0)
		g.wakelock_held = acquire;
	close(fd);
	dbg("wake lock %s", acquire ? "held" : "released");
}

/* ---------- HAL callbacks (HAL threads) ---------- */

struct thread_start {
	void (*fn)(void *);
	void *arg;
	char name[16];
};

static void *thread_tramp(void *p)
{
	struct thread_start ts = *(struct thread_start *)p;

	free(p);
	pthread_setname_np(pthread_self(), ts.name);
	ts.fn(ts.arg);
	return NULL;
}

static pthread_t cb_create_thread(const char *name, void (*start)(void *), void *arg)
{
	struct thread_start *ts = calloc(1, sizeof(*ts));
	pthread_t th;
	pthread_attr_t attr;

	if (!ts)
		return 0;
	ts->fn = start;
	ts->arg = arg;
	snprintf(ts->name, sizeof(ts->name), "%s", name ? name : "hal");
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 1024 * 1024);
	if (pthread_create(&th, &attr, thread_tramp, ts)) {
		logmsg("create_thread %s: %s", ts->name, strerror(errno));
		free(ts);
		th = 0;
	}
	pthread_attr_destroy(&attr);
	dbg("HAL thread \"%s\" created", name ? name : "");
	return th;
}

/*
 * The Titan 2 module fills a GpsLocation of its own (longer) layout: the
 * first fixes (and every fix with -R) are dumped raw — the HAL's own size
 * field, hex, and the 8-byte doubles / 4-byte floats at every aligned offset —
 * so the real field order can be read off the log without any vendor header.
 */
static void dump_location_raw(const GpsLocation *l)
{
	const unsigned char *b = (const unsigned char *)l;
	size_t n = l->size, off;
	char line[400];
	int w;

	if (n < sizeof(GpsLocation) || n > 256)
		n = 128;
	logmsg("raw GpsLocation: size field %zu (AOSP %zu), %zu bytes follow", l->size, sizeof(GpsLocation), n);
	for (off = 0; off < n; off += 32) {
		size_t k;
		w = snprintf(line, sizeof(line), "  %3zu:", off);
		for (k = off; k < off + 32 && k < n; k++)
			w += snprintf(line + w, sizeof(line) - w, "%s%02x", (k - off) % 8 ? "" : " ", b[k]);
		logmsg("%s", line);
	}
	w = snprintf(line, sizeof(line), "  doubles:");
	for (off = 8; off + 8 <= n; off += 8) {
		double d;
		memcpy(&d, b + off, 8);
		if (d != 0.0 && fabs(d) > 1e-6 && fabs(d) < 1e13)
			w += snprintf(line + w, sizeof(line) - w, " @%zu=%.6g", off, d);
		if (w > (int)sizeof(line) - 40) { logmsg("%s", line); w = snprintf(line, sizeof(line), "  doubles:"); }
	}
	logmsg("%s", line);
	w = snprintf(line, sizeof(line), "  floats:");
	for (off = 8; off + 4 <= n; off += 4) {
		float f;
		memcpy(&f, b + off, 4);
		if (f != 0.0f && fabsf(f) > 1e-6f && fabsf(f) < 1e7f)
			w += snprintf(line + w, sizeof(line) - w, " @%zu=%.4g", off, f);
		if (w > (int)sizeof(line) - 40) { logmsg("%s", line); w = snprintf(line, sizeof(line), "  floats:"); }
	}
	logmsg("%s", line);
	w = snprintf(line, sizeof(line), "  int64s:");
	for (off = 8; off + 8 <= n; off += 8) {
		int64_t v;
		memcpy(&v, b + off, 8);
		if (v > 1000000000000LL && v < 4000000000000LL)	/* ms since 1970 between 2001 and 2096 */
			w += snprintf(line + w, sizeof(line) - w, " @%zu=%lld(ms-utc?)", off, (long long)v);
		else if (v > 0 && v < 100000000000LL)
			w += snprintf(line + w, sizeof(line) - w, " @%zu=%lld", off, (long long)v);
	}
	logmsg("%s", line);
}

static void cb_location(GpsLocation *l)
{
	int64_t now = now_ms(CLOCK_MONOTONIC);

	if (!l)
		return;
	if (g.raw_loc || g.raw_dumps < 2) {
		g.raw_dumps++;
		dump_location_raw(l);
	}
	pthread_mutex_lock(&g.mu);
	g.loc = *l;
	g.have_loc = true;
	g.fixes++;
	if (!g.first_fix_ms && (l->flags & GPS_LOCATION_HAS_LAT_LONG)) {
		g.first_fix_ms = now;
		logmsg("first fix: %.6f %.6f +-%.0f m, TTFF %.1f s", l->latitude, l->longitude, l->accuracy,
		       (now - g.session_started_ms) / 1000.0);
	}
	dbg("fix: %.6f %.6f alt %.1f acc %.1f m speed %.1f m/s bearing %.0f flags 0x%x ts %lld (AOSP layout; see raw dump)",
	    l->latitude, l->longitude, l->altitude, l->accuracy, l->speed, l->bearing, l->flags, (long long)l->timestamp);
	if (g.synth > 0 || (g.synth == 0 && now - g.last_hal_nmea_ms > 5000))
		nmea_synthesize_locked();
	if (now - g.last_cache_ms > 30000) {
		g.last_cache_ms = now;
		fix_cache_write_locked();
	}
	pthread_mutex_unlock(&g.mu);
}

static void cb_status(GpsStatus *s)
{
	static const char *const names[] = { "NONE", "SESSION_BEGIN", "SESSION_END", "ENGINE_ON", "ENGINE_OFF" };
	int st = s ? (int)s->status : -1;

	logmsg("status %s", st >= 0 && st < 5 ? names[st] : "?");
	if (st == GPS_STATUS_ENGINE_ON)
		g.status_engine_on = true;
	else if (st == GPS_STATUS_ENGINE_OFF)
		g.status_engine_on = false;
}

static void cb_sv_status(GpsSvStatus *s)
{
	dbg("legacy sv status: %d satellites", s ? s->num_svs : -1);
}

static void cb_gnss_sv_status(GnssSvStatus *s)
{
	int i, used = 0;

	if (!s)
		return;
	pthread_mutex_lock(&g.mu);
	g.nsv = s->num_svs < GNSS_MAX_SVS ? s->num_svs : GNSS_MAX_SVS;
	for (i = 0; i < g.nsv; i++) {
		g.sv[i] = s->gnss_sv_list[i];
		if (g.sv[i].flags & GNSS_SV_FLAGS_USED_IN_FIX)
			used++;
	}
	pthread_mutex_unlock(&g.mu);
	dbg("sv status: %d satellites, %d used in fix", g.nsv, used);
}

static void cb_nmea(GpsUtcTime ts, const char *nmea, int length)
{
	char line[NMEA_MAX];
	int n = length;

	(void)ts;
	if (!nmea || n <= 0)
		return;
	while (n && (nmea[n - 1] == '\r' || nmea[n - 1] == '\n' || nmea[n - 1] == 0))
		n--;
	if (!n || n > (int)sizeof(line) - 3)
		return;
	memcpy(line, nmea, n);
	line[n] = '\r'; line[n + 1] = '\n'; line[n + 2] = 0;
	pthread_mutex_lock(&g.mu);
	g.last_hal_nmea_ms = now_ms(CLOCK_MONOTONIC);
	if (g.synth <= 0)
		nmea_send_locked(line, n + 2);
	pthread_mutex_unlock(&g.mu);
}

static void cb_set_capabilities(uint32_t caps)
{
	g.capabilities = caps;
	logmsg("capabilities 0x%x%s%s%s%s%s%s", caps,
	       caps & GPS_CAPABILITY_SCHEDULING ? " scheduling" : "", caps & GPS_CAPABILITY_MSB ? " MSB" : "",
	       caps & GPS_CAPABILITY_MSA ? " MSA" : "", caps & GPS_CAPABILITY_SINGLE_SHOT ? " single-shot" : "",
	       caps & GPS_CAPABILITY_ON_DEMAND_TIME ? " on-demand-time" : "",
	       caps & GPS_CAPABILITY_MEASUREMENTS ? " measurements" : "");
}

static void cb_acquire_wakelock(void) { wakelock(true); }
static void cb_release_wakelock(void) { wakelock(false); }

static void cb_request_utc_time(void)
{
	dbg("the HAL asks for time");
	g.ev_time = 1;
	wake_main();
}

static void cb_set_system_info(const GnssSystemInfo *info)
{
	logmsg("system info: hardware year %u", info ? info->year_of_hw : 0);
}

/* MediaTek extension slots (unknown signatures): log and return 0 */
#define MTK_STUB(n) \
	static long cb_mtk_stub##n(void) \
	{ \
		logmsg("the HAL called MediaTek extension callback slot %d (stub, returns 0)", n); \
		return 0; \
	}
MTK_STUB(12) MTK_STUB(13) MTK_STUB(14) MTK_STUB(15) MTK_STUB(16) MTK_STUB(17) MTK_STUB(18) MTK_STUB(19)
static long cb_ext_stub(void)
{
	logmsg("the HAL called an extension callback beyond the AOSP table (stub, returns 0)");
	return 0;
}

static void cb_xtra_download_request(void)
{
	logmsg("the HAL asks for XTRA/EPO data");
	g.ev_xtra = 1;
	wake_main();
}

static void cb_agps_status(AGpsStatus *s)
{
	if (!s)
		return;
	logmsg("AGPS status: type %d status %d", s->type, s->status);
	if (s->status == GPS_REQUEST_AGPS_DATA_CONN)
		g.ev_agps_open = 1;
	else if (s->status == GPS_RELEASE_AGPS_DATA_CONN)
		g.ev_agps_close = 1;
	wake_main();
}

static void cb_ril_request_setid(uint32_t flags)
{
	dbg("AGPS RIL asks for a set id (flags 0x%x): none available", flags);
}

static void cb_ril_request_refloc(uint32_t flags)
{
	dbg("AGPS RIL asks for a reference location (flags 0x%x): none available", flags);
}

/* ---------- AGPS data connection (only meaningful with mtk_agpsd and MS-based mode) ---------- */

static bool have_default_route(void)
{
	FILE *f = fopen("/proc/net/route", "r");
	char line[256];
	bool found = false;

	if (!f)
		return false;
	while (fgets(line, sizeof(line), f)) {
		char ifname[32];
		unsigned long dest = 1, mask = 1;
		if (sscanf(line, "%31s %lx %*x %*x %*d %*d %*d %lx", ifname, &dest, &mask) == 3 && !dest && !mask) {
			found = true;
			break;
		}
	}
	fclose(f);
	return found;
}

static void agps_data_conn(bool open)
{
	if (!g.agps)
		return;
	if (!open) {
		if (g.agps->data_conn_closed)
			g.agps->data_conn_closed();
		return;
	}
	if (have_default_route()) {
		int rc = -1;
		if (g.agps->data_conn_open_with_apn_ip_type)
			rc = g.agps->data_conn_open_with_apn_ip_type("internet", APN_IP_IPV4V6);
		else if (g.agps->data_conn_open)
			rc = g.agps->data_conn_open("internet");
		logmsg("AGPS data connection: open (default route present) -> %d", rc);
	} else {
		if (g.agps->data_conn_failed)
			g.agps->data_conn_failed();
		logmsg("AGPS data connection: no default route, failed");
	}
}

/* ---------- session ---------- */

static int session_start(void)
{
	int rc;

	if (g.running)
		return 0;
	logmsg("starting the receiver (interval %d ms, mode %s)", g.interval_ms, g.mode ? "MS-based" : "standalone");
	if (g.cold && g.gi->delete_aiding_data) {
		logmsg("cold start: delete_aiding_data(ALL)");
		g.gi->delete_aiding_data(GPS_DELETE_ALL);
	}
	if (g.gi->set_position_mode) {
		rc = g.gi->set_position_mode(g.mode ? GPS_POSITION_MODE_MS_BASED : GPS_POSITION_MODE_STANDALONE,
					     GPS_POSITION_RECURRENCE_PERIODIC, g.interval_ms, 0, 0);
		if (rc)
			logmsg("set_position_mode: %d", rc);
	}
	if (!g.cold) {
		inject_time();
		inject_location();
		inject_epo(false);
	}
	if (g.ril && g.ril->update_network_state)
		g.ril->update_network_state(have_default_route(), 0 /* MOBILE */, 0, NULL);
	rc = g.gi->start();
	if (rc) {
		logmsg("start: %d", rc);
		return rc;
	}
	pthread_mutex_lock(&g.mu);
	g.running = true;
	g.session_started_ms = now_ms(CLOCK_MONOTONIC);
	g.first_fix_ms = 0;
	g.nsv = 0;
	pthread_mutex_unlock(&g.mu);
	epo_fetch_maybe(false);
	return 0;
}

static void session_stop(void)
{
	int rc;

	if (!g.running)
		return;
	rc = g.gi->stop();
	logmsg("stopping the receiver (%ld fixes, %ld NMEA lines) -> %d", g.fixes, g.nmea_lines, rc);
	pthread_mutex_lock(&g.mu);
	g.running = false;
	g.nsv = 0;
	fix_cache_write_locked();
	pthread_mutex_unlock(&g.mu);
}

/* the receiver runs while a client listens (or -a); stop g.grace_s after the last one leaves */
static void policy_update(void)
{
	bool want;

	pthread_mutex_lock(&g.mu);
	want = g.always_on || g.nclients > 0;
	pthread_mutex_unlock(&g.mu);
	if (want) {
		g.stop_at_ms = 0;
		if (!g.running && now_ms(CLOCK_MONOTONIC) >= g.start_retry_at_ms) {
			if (session_start() != 0) {
				g.start_retry_at_ms = now_ms(CLOCK_MONOTONIC) + 5000;
				logmsg("the receiver did not start; retrying in 5 s");
			}
		}
	} else if (g.running && !g.stop_at_ms) {
		dbg("no listeners: stopping in %d s", g.grace_s);
		g.stop_at_ms = now_ms(CLOCK_MONOTONIC) + (int64_t)g.grace_s * 1000;
	}
}

/* ---------- Unix socket server (GeoClue and friends) ---------- */

static int socket_listen(void)
{
	struct sockaddr_un sa;
	char *dir, *slash;

	if (strlen(g.sock_path) >= sizeof(sa.sun_path)) {
		logmsg("socket path too long");
		return -1;
	}
	dir = strdup(g.sock_path);
	slash = strrchr(dir, '/');
	if (slash && slash != dir) {
		*slash = 0;
		t2_mkdirs(dir, 0755);
	}
	free(dir);
	unlink(g.sock_path);
	g.listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (g.listen_fd < 0) {
		logmsg("socket: %s", strerror(errno));
		return -1;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	strcpy(sa.sun_path, g.sock_path);
	if (bind(g.listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(g.listen_fd, 8) < 0) {
		logmsg("bind/listen %s: %s", g.sock_path, strerror(errno));
		return -1;
	}
	chmod(g.sock_path, 0666);	/* geoclue runs as its own user */
	logmsg("NMEA on %s", g.sock_path);
	return 0;
}

static void socket_accept(void)
{
	int fd = accept4(g.listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);

	if (fd < 0)
		return;
	pthread_mutex_lock(&g.mu);
	if (g.nclients >= MAX_CLIENTS) {
		pthread_mutex_unlock(&g.mu);
		close(fd);
		logmsg("too many NMEA clients, one refused");
		return;
	}
	g.clients[g.nclients++].fd = fd;
	logmsg("NMEA client connected (%d now)", g.nclients);
	pthread_mutex_unlock(&g.mu);
}

/* ---------- HAL bring-up ---------- */

static int hal_open(void)
{
	struct hw_device_t *hwdev = NULL;
	struct gps_device_t *dev;
	int64_t deadline = now_ms(CLOCK_MONOTONIC) + (int64_t)g.wait_s * 1000;
	int rc;

	if (!t2_open_hal(g.lib, GPS_HARDWARE_MODULE_ID, &hwdev))
		return -1;
	dev = (struct gps_device_t *)hwdev;
	if (!dev->get_gps_interface) {
		logmsg("the gps device has no get_gps_interface");
		return -1;
	}
	g.gi = dev->get_gps_interface(dev);
	if (!g.gi || !g.gi->init || !g.gi->start || !g.gi->stop) {
		logmsg("no GpsInterface from the module");
		return -1;
	}
	logmsg("GpsInterface size %zu (ours %zu)", g.gi->size, sizeof(GpsInterface));

	memset(&g.cbs, 0, sizeof(g.cbs));
	g.cbs.cb.size = g.cb_size;
	{
		/* slots after the AOSP table: stubs for the MediaTek extension callbacks */
		long (*const stubs[])(void) = { cb_mtk_stub12, cb_mtk_stub13, cb_mtk_stub14, cb_mtk_stub15,
						 cb_mtk_stub16, cb_mtk_stub17, cb_mtk_stub18, cb_mtk_stub19 };
		size_t i;
		for (i = 0; i < sizeof(stubs) / sizeof(stubs[0]); i++)
			g.cbs.slot[AOSP_CB_SLOTS + i] = (void *)stubs[i];
	}
	logmsg("GpsCallbacks size %zu (AOSP %zu, %zu slots + %zu MediaTek stubs)", g.cb_size, sizeof(GpsCallbacks),
	       AOSP_CB_SLOTS, g.cb_size > sizeof(GpsCallbacks) ? (g.cb_size - sizeof(GpsCallbacks)) / sizeof(void *) : 0);
	g.cbs.cb.location_cb = cb_location;
	g.cbs.cb.status_cb = cb_status;
	g.cbs.cb.sv_status_cb = cb_sv_status;
	g.cbs.cb.nmea_cb = cb_nmea;
	g.cbs.cb.set_capabilities_cb = cb_set_capabilities;
	g.cbs.cb.acquire_wakelock_cb = cb_acquire_wakelock;
	g.cbs.cb.release_wakelock_cb = cb_release_wakelock;
	g.cbs.cb.create_thread_cb = cb_create_thread;
	g.cbs.cb.request_utc_time_cb = cb_request_utc_time;
	g.cbs.cb.set_system_info_cb = cb_set_system_info;
	g.cbs.cb.gnss_sv_status_cb = cb_gnss_sv_status;

	/* init() connects the module to mnld: retry while mnld is still starting */
	for (;;) {
		rc = g.gi->init(&g.cbs.cb);
		if (rc == 0)
			break;
		if (now_ms(CLOCK_MONOTONIC) > deadline) {
			logmsg("GpsInterface init: %d after %d s: is mnld running (rc-service t2-mnld status)?", rc, g.wait_s);
			return -1;
		}
		logmsg("GpsInterface init: %d, retrying (mnld not ready?)", rc);
		sleep(2);
	}
	logmsg("GPS HAL initialised");

	if (g.gi->get_extension) {
		g.xtra = g.gi->get_extension(GPS_XTRA_INTERFACE);
		if (g.xtra && g.xtra->init) {
			size_t i;
			for (i = 0; i < 8; i++)
				g.xtra_cbs.slot[i] = (void *)cb_ext_stub;
			g.xtra_cbs.cb.download_request_cb = cb_xtra_download_request;
			g.xtra_cbs.cb.create_thread_cb = cb_create_thread;
			rc = g.xtra->init(&g.xtra_cbs.cb);
			logmsg("XTRA (EPO injection) interface: size %zu, init %d", g.xtra->size, rc);
			if (rc)
				g.xtra = NULL;
		} else {
			g.xtra = NULL;
			/* the Titan 2 module has no "gps-xtra"; its PSDS/EPO injection entry is the MediaTek
			 * extension "aidl_gnss_psds_interface" (entries psds_set_callback, inject_psds_data),
			 * whose C layout is not published — only looked at here, called only with -X */
			g.psds = g.gi->get_extension("aidl_gnss_psds_interface");
			if (g.psds) {
				size_t psz = *(const size_t *)g.psds;
				logmsg("no XTRA interface; MediaTek PSDS extension present: struct size %zu (%zu entries) — "
				       "EPO goes in as a file (t2-epo-fetch --install)%s", psz,
				       psz >= sizeof(size_t) ? (psz - sizeof(size_t)) / sizeof(void *) : 0,
				       g.psds_experiment ? ", -X: injection experiment enabled" : "");
			} else {
				logmsg("no XTRA and no PSDS extension: EPO can only be installed as a file (t2-epo-fetch --install)");
			}
		}
		g.agps = g.gi->get_extension(AGPS_INTERFACE);
		if (g.agps && g.agps->init) {
			size_t i;
			for (i = 0; i < 8; i++)
				g.agps_cbs.slot[i] = (void *)cb_ext_stub;
			g.agps_cbs.cb.status_cb = cb_agps_status;
			g.agps_cbs.cb.create_thread_cb = cb_create_thread;
			g.agps->init(&g.agps_cbs.cb);
			dbg("AGPS interface: size %zu", g.agps->size);
		} else {
			g.agps = NULL;
		}
		g.ril = g.gi->get_extension(AGPS_RIL_INTERFACE);
		if (g.ril && g.ril->init) {
			size_t i;
			for (i = 0; i < 8; i++)
				g.ril_cbs.slot[i] = (void *)cb_ext_stub;
			g.ril_cbs.cb.request_setid = cb_ril_request_setid;
			g.ril_cbs.cb.request_refloc = cb_ril_request_refloc;
			g.ril_cbs.cb.create_thread_cb = cb_create_thread;
			g.ril->init(&g.ril_cbs.cb);
			dbg("AGPS RIL interface: size %zu", g.ril->size);
		} else {
			g.ril = NULL;
		}
	}
	return 0;
}

static void hal_close(void)
{
	if (!g.gi)
		return;
	session_stop();
	wakelock(false);
	if (g.gi->cleanup)
		g.gi->cleanup();
	g.gi = NULL;
}

/* ---------- main loop ---------- */

static void on_signal(int sig)
{
	(void)sig;
	g.quit = true;
	wake_main();
}

static void usage(void)
{
	fputs("t2-gnssd: the vendor GPS HAL in-process (bionic_translation) -> mnld; NMEA on a Unix socket for GeoClue\n"
	      "  -r ROOT   vendor root (default " T2_DEFAULT_ROOT ")\n"
	      "  -l LIB    HAL module (default " DEFAULT_LIB ")\n"
	      "  -s PATH   NMEA Unix socket (default " DEFAULT_SOCKET "; \"\" = none)\n"
	      "  -a        keep the receiver on (default: on while a client is connected)\n"
	      "  -o        also print NMEA to stdout\n"
	      "  -S        synthesize GSV from the SV status (when NMEA is synthesized)\n"
	      "  -i MS     fix interval (default 1000)\n"
	      "  -m MODE   position mode: 0 standalone (default), 1 MS-based (SUPL via mtk_agpsd)\n"
	      "  -g SEC    keep the receiver on this long after the last client (default 10)\n"
	      "  -T SEC    exit after SEC seconds (test runs)\n"
	      "  -t SEC    wait this long for mnld at start (default 60)\n"
	      "  -y MODE   NMEA synthesis from fixes: auto (default, when the HAL sends none), always, never\n"
	      "  -u MS     uncertainty of the injected system time (default: 1000 if NTP-synced, else 10000)\n"
	      "  -N        never inject time;  -L never inject the cached location\n"
	      "  -e FILE   EPO file to inject (default " DEFAULT_EPO ");  -E no EPO at all\n"
	      "  -F        never run " EPO_FETCH " (the file is refreshed by t2-epo alone)\n"
	      "  -A HOURS  EPO age that triggers a fetch (default 6)\n"
	      "  -c        cold start: delete all aiding data before start, no injection (TTFF reference)\n"
	      "  -d DIR    state directory (last fix; default " DEFAULT_STATE ")\n"
	      "  -W        do not take /sys/power/wake_lock on acquire_wakelock_cb\n"
	      "  -C BYTES  GpsCallbacks.size handed to the HAL (default 120 = MediaTek table; AOSP 96)\n"
	      "  -R        dump every location callback raw (default: the first two)\n"
	      "  -X        experiment: inject EPO through the MediaTek PSDS extension (unpublished layout)\n"
	      "  -D        linker diagnostics (missing symbols) instead of failing\n"
	      "  -v        verbose\n", stderr);
}

int main(int argc, char **argv)
{
	struct pollfd pfd[MAX_CLIENTS + 2];
	struct sigaction sa = { .sa_handler = on_signal };
	int opt, i, n, rc = 0;

	g.root = T2_DEFAULT_ROOT;
	while ((opt = getopt(argc, argv, "r:l:s:aoSi:m:g:T:t:y:u:NLe:EFA:cd:C:RXWDvh")) != -1) {
		switch (opt) {
		case 'r': g.root = optarg; break;
		case 'l': g.lib = optarg; break;
		case 's': g.sock_path = optarg; break;
		case 'a': g.always_on = true; break;
		case 'o': g.to_stdout = true; break;
		case 'S': g.want_sv = true; break;
		case 'i': g.interval_ms = atoi(optarg); break;
		case 'm': g.mode = atoi(optarg); break;
		case 'g': g.grace_s = atoi(optarg); break;
		case 'T': g.run_s = atoi(optarg); break;
		case 't': g.wait_s = atoi(optarg); break;
		case 'y': g.synth = !strcmp(optarg, "always") ? 1 : !strcmp(optarg, "never") ? -1 : 0; break;
		case 'u': g.time_unc_ms = atoi(optarg); break;
		case 'N': g.no_time = true; break;
		case 'L': g.no_location = true; break;
		case 'e': g.epo_path = optarg; break;
		case 'E': g.no_epo = true; break;
		case 'F': g.no_fetch = true; break;
		case 'A': g.epo_max_age_s = atoi(optarg) * 3600; break;
		case 'c': g.cold = true; break;
		case 'd': g.state_dir = optarg; break;
		case 'C': g.cb_size = (size_t)atoi(optarg); break;
		case 'R': g.raw_loc = true; break;
		case 'X': g.psds_experiment = true; break;
		case 'W': g.no_wakelock = true; break;
		case 'D': g.diag = true; break;
		case 'v': g.verbose = true; break;
		default: usage(); return opt == 'h' ? 0 : 2;
		}
	}
	if (g.interval_ms < 100)
		g.interval_ms = 100;

	/* libdl_bio reads BIONIC_VENDOR_ROOT in a constructor: set it and start over */
	t2_prog = "t2-gnssd";
	if (t2_vendor_reexec(g.root, g.diag, argv))
		return 1;
	setvbuf(stdout, NULL, _IOLBF, 0);
	t2_mkdirs(g.state_dir, 0755);

	if (pipe2(g.wake_fd, O_CLOEXEC | O_NONBLOCK) < 0)
		return 1;
	signal(SIGCHLD, SIG_IGN);	/* t2-epo-fetch children are not waited for */
	signal(SIGPIPE, SIG_IGN);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	if (hal_open() != 0)
		return 1;
	if (g.sock_path[0] && socket_listen() != 0) {
		hal_close();
		return 1;
	}
	if (g.run_s > 0)
		g.quit_at_ms = now_ms(CLOCK_MONOTONIC) + (int64_t)g.run_s * 1000;
	policy_update();

	while (!g.quit) {
		int64_t now = now_ms(CLOCK_MONOTONIC);
		int timeout = 1000;

		if (g.quit_at_ms && now >= g.quit_at_ms) {
			logmsg("time is up (%d s): %ld fixes, %ld NMEA lines", g.run_s, g.fixes, g.nmea_lines);
			break;
		}
		if (g.stop_at_ms && now >= g.stop_at_ms) {
			g.stop_at_ms = 0;
			pthread_mutex_lock(&g.mu);
			n = g.nclients;
			pthread_mutex_unlock(&g.mu);
			if (!g.always_on && n == 0)
				session_stop();
		}
		if (g.ev_time) {
			g.ev_time = 0;
			inject_time();
		}
		if (g.ev_xtra) {
			g.ev_xtra = 0;
			inject_epo(true);
			epo_fetch_maybe(false);
		}
		if (g.ev_agps_open) {
			g.ev_agps_open = 0;
			agps_data_conn(true);
		}
		if (g.ev_agps_close) {
			g.ev_agps_close = 0;
			agps_data_conn(false);
		}
		/* a fresh EPO file (t2-epo-fetch finished) goes to mnld as soon as it appears */
		if (g.running)
			inject_epo(false);

		pfd[0].fd = g.wake_fd[0]; pfd[0].events = POLLIN;
		pfd[1].fd = g.listen_fd; pfd[1].events = g.listen_fd >= 0 ? POLLIN : 0;
		pthread_mutex_lock(&g.mu);
		n = g.nclients;
		for (i = 0; i < n; i++) {
			pfd[2 + i].fd = g.clients[i].fd;
			pfd[2 + i].events = POLLIN | POLLHUP | POLLERR;
		}
		pthread_mutex_unlock(&g.mu);
		if (poll(pfd, 2 + n, timeout) < 0) {
			if (errno == EINTR)
				continue;
			logmsg("poll: %s", strerror(errno));
			break;
		}
		if (pfd[0].revents & POLLIN) {
			char buf[64];
			while (read(g.wake_fd[0], buf, sizeof(buf)) > 0)
				;
		}
		if (pfd[1].revents & POLLIN)
			socket_accept();
		for (i = 0; i < n; i++) {
			char buf[128];
			ssize_t r;

			if (!pfd[2 + i].revents)
				continue;
			r = recv(pfd[2 + i].fd, buf, sizeof(buf), MSG_DONTWAIT);
			if (r > 0)
				continue;	/* consumers do not talk; ignore input */
			if (r < 0 && (errno == EAGAIN || errno == EINTR) && !(pfd[2 + i].revents & (POLLHUP | POLLERR)))
				continue;
			pthread_mutex_lock(&g.mu);
			{
				int k;
				for (k = 0; k < g.nclients; k++)
					if (g.clients[k].fd == pfd[2 + i].fd) {
						client_close_locked(k);
						break;
					}
			}
			pthread_mutex_unlock(&g.mu);
		}
		policy_update();
	}

	if (g.listen_fd >= 0) {
		close(g.listen_fd);
		unlink(g.sock_path);
	}
	pthread_mutex_lock(&g.mu);
	while (g.nclients)
		client_close_locked(g.nclients - 1);
	pthread_mutex_unlock(&g.mu);
	hal_close();
	return rc;
}
