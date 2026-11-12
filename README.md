<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com> -->
# titan2-gnss: GNSS of the Titan 2 on mainline Linux

GNSS (GPS, GLONASS, …) of the **Unihertz Titan 2** (MediaTek MT6878) on mainline Linux with
an Alpine/musl user space, delivered to applications through GeoClue. The closed parts come
from **your own phone**: the MediaTek navigation daemon `mnld` runs natively in a bionic
chroot of the vendor root, and the legacy GPS HAL module `gps.default.so` is loaded
in-process by `t2-gnssd` through
[bionic_translation](https://gitlab.com/android_translation_layer/bionic_translation).
Assistance data (EPO predicted ephemeris) is downloaded by an open client, `t2-epo-fetch`.

> **Status: work in progress, for developers.** Partially tested on the phone; expect rough
> edges (see "Not done" below).

> **Status:** satellite fix and position through GeoClue work on our phone, with EPO A-GPS.
> The setup below has not yet been run end to end on a freshly installed phone; please report
> what differs.

## What is not here

No vendor binaries, libraries, configuration, calibration or EPO files: nothing of the factory
firmware is in this repository or in the packages. `files/gnss-files.txt` only **names** the
files; `t2-vendor-setup` (titan2-vendor-runtime) copies them from your own phone's partitions
when you run it on your device. The EPO file is downloaded at run time on the phone, from the
MediaTek EPO server, with a generic User-Agent.

## Requirements

- **titan2-vendor-runtime** from [titan2-tools](https://github.com/Serlk/titan2-tools)
  (branch `main`, `tools/titan2-vendor-runtime`, Alpine package from
  `tools/aports/titan2-vendor-runtime`) and **bionic_translation** with the patches in
  `tools/titan2-vendor-runtime/patches/bionic_translation` of that repository.
- Kernel: [titan2-linux](https://github.com/Serlk/titan2-linux) branch `titan2` with the
  patches in `kernel/patches/` (`git am`; see [kernel/README.md](kernel/README.md)); config fragment
  `kernel/titan2-gnss.config`.
- GeoClue (Alpine `geoclue`), `curl` for the EPO download.

```
 t2-gnssd (musl) ──bionic_translation──▶ gps.default.so (bionic, hardware/gps.h)
      │  callbacks: NMEA, fixes, SV, status,        │ abstract datagram sockets
      │  wakelock, time/XTRA requests               │ mtk_hal2mnl / mtk_mnl2hal
      │                                     mnld (OpenRC t2-mnld: chroot /var/lib/titan2-vendor,
      │                                           t2-propd properties, t2-logd, /dev/socket/mnld)
      │                                             │ /dev/gpsdl0 (mtk-gps-dl, GPL) → GNSS DSP
      ▼
 /run/t2-gnss/nmea.sock ──▶ GeoClue ([network-nmea]) ──▶ apps / xdg-desktop-portal Location
```

Why in-process and not the AIDL service over binder: `gps.default.so` needs only
liblog/libcutils/libhardware/libc++ and talks to mnld over Unix datagram sockets, so no
servicemanager, SELinux policy, binderfs or AIDL service is involved.

## Pieces

| file | role |
|---|---|
| `src/t2-gnssd.c`, `openrc/t2-gnss` | the bridge: GPS HAL in-process → NMEA socket for GeoClue; time, last-fix and EPO handling |
| `openrc/t2-mnld`, `src/t2-initsock.c` | mnld (and optionally `mtk_agpsd`) native in the vendor chroot: binds, properties, logd, init-style control sockets |
| `files/gnss-files.txt`, `files/gnss.props` | drop-ins for t2-vendor-setup: the file names (complete DT_NEEDED closure, self-sufficient) and the chip properties |
| `geoclue/90-titan2-gnss.conf` | GeoClue drop-in: `[network-nmea]` on the t2-gnssd socket |
| `epo/` | `t2-epo-fetch` and its OpenRC/cron/NetworkManager hooks (package titan2-epo, [README](epo/README.md)) |
| `include/hardware/` | AOSP GPS HAL headers (Apache-2.0) |
| `aports/titan2-gnss`, `aports/titan2-epo` | Alpine packages |
| `netroot/` | package list and service setup for an image built by titan2-tools' `scripts/netroot-serve.sh` |

## How it runs

- `rc-service t2-mnld start`: loads `mtk_gps_dl`, waits for `/dev/gpsdl0`, `t2-vendor-env up`
  (binds, t2-propd, t2-logd), sets the properties mnld reads (`persist.vendor.connsys.chipid`,
  `vendor.connsys.gps.adie.chipid`, `vendor.connsys.driver.ready`, `vendor.service.nvram_init`,
  `hwservicemanager.disabled` — there is no HIDL sensor service here), mounts the phone's
  `nvdata`/`nvcfg` partitions read-only under the chroot (GNSS calibration, almanac), then runs
  `t2-initsock mnld -- chroot … /vendor/bin/mnld` under supervise-daemon. Logs:
  `/var/log/t2-vendor/mnld.log` (stderr) and `logcat.log` (tag `MNLD`). `T2_MNLD_AGPS=1` also
  starts `mtk_agpsd` (SUPL; experimental).
- `rc-service t2-gnss start`: `t2-gnssd -S` under supervise-daemon. It re-execs itself with
  `BIONIC_VENDOR_ROOT`, loads the module (`HMI`, `hw_module_t` → `gps_device_t` →
  `GpsInterface`), `init()`s it with our callbacks (retrying while mnld starts), opens the
  `gps-xtra`, `agps` and `agps_ril` extensions, and serves `/run/t2-gnss/nmea.sock` (0666).
  The Titan 2 module checks the size of the callback table exactly ("Bad callback, size: 96,
  expected: 120"): t2-gnssd hands it a 120-byte `GpsCallbacks` (12 AOSP slots + 3 vendor
  slots filled with logging stubs; `-C BYTES` overrides); the extension tables are padded the
  same way. The receiver is started on the first socket client and stopped 10 s after the last
  one left (`-a` always on, `-g SEC` grace). Before every `start()`: `inject_time()` (system
  clock, uncertainty 1 s when NTP-synced, 10 s otherwise) and `inject_location()` (the last fix
  from `/var/lib/t2-gnss/last-fix`, accuracy inflated 30 m/s with age, dropped past 300 km).
- EPO: the Titan 2 module has no `gps-xtra` extension (its PSDS entry is an unpublished
  vendor extension, tried only with `-X`), so the file fetched by `t2-epo-fetch` is installed as
  `<root>/data/vendor/gps/EPO.DAT`, the file mnld reads itself (t2-mnld copies it before mnld
  starts; a stale file spawns `t2-epo-fetch -q --install`).
- The first two location callbacks are dumped raw (`-R`: all) because the module fills a
  longer, vendor-specific `GpsLocation`. `-c` is a cold start (delete all aiding data, no
  injection) for TTFF comparisons; the first fix is logged with the TTFF.
- NMEA: what mnld sends through `nmea_cb` (GGA/RMC/GSA/GSV…); if fixes come without sentences,
  GGA/RMC (+GSV with `-S`) are synthesized. The daemon takes `/sys/power/wake_lock t2-gnss` on
  `acquire_wakelock_cb`.
- GeoClue: `/etc/geoclue/conf.d/90-titan2-gnss.conf` enables `[network-nmea]` on the socket;
  GeoClue connects only while an application holds a client. Applications need a GeoClue agent
  on the session bus (for example `/usr/libexec/geoclue-2.0/demos/agent`); the Location portal
  of xdg-desktop-portal is a system client of GeoClue and needs none.

## Build and install

```
git clone https://github.com/Serlk/titan2-tools ../titan2-tools   # VENDOR_RUNTIME default
make                      # needs bionic_translation-dev
make dist                 # tarballs for aports/titan2-gnss and aports/titan2-epo, then abuild checksum
```

On the phone: kernel from titan2-linux branch `titan2` with `kernel/patches/` and
`kernel/titan2-gnss.config` (kernel/README.md); `apk add titan2-gnss titan2-epo`; the
vendor root from your own partitions, e.g. mounted read-only under `/mnt/factory`:
`t2-vendor-setup /mnt/factory /var/lib/titan2-vendor /usr/share/titan2-vendor-runtime/files.d/gnss.txt`
(the GNSS list alone is enough); then `rc-service t2-mnld start; rc-service t2-gnss start`.

Test by hand: `rc-service t2-mnld start; t2-gnssd -o -a -S -v -T 120` prints the callbacks and
NMEA for two minutes; `nc -U /run/t2-gnss/nmea.sock` reads the socket;
`/usr/libexec/geoclue-2.0/demos/where-am-i` checks the GeoClue path. EPO path:
`t2-epo-fetch -f -v`, then `t2-gnssd -a -o -v -T 90`.

## Not done

A client for the vendor extensions (measurements, geofence); SUPL (`mtk_agpsd` needs a DNS and
data path the chroot does not provide); a reference location from the modem's cell.

## How this project is made / affiliation

- This project is developed with the help of AI coding assistants (Anthropic Claude). Commits
  carry a `Co-Authored-By: Claude` trailer; every change is reviewed and tested on the device
  by the human maintainer (serlk), who is responsible for it.
- This project is not affiliated with Unihertz or MediaTek.

## License

Copyright (c) 2026 serlk. GPL-2.0-or-later, see [LICENSE](LICENSE). Every file names its
license in an `SPDX-License-Identifier` line (or in `REUSE.toml`); the AOSP headers in
`include/hardware/` are Apache-2.0 ([LICENSES/](LICENSES/)).

## Related repositories

- [titan2-linux](https://github.com/Serlk/titan2-linux): the kernel, branch `titan2` (the mainline port). The optional camera, fingerprint and GNSS kernel parts are not in it: each lives as a patch series in `kernel/patches/` of its own repository below, applied with `git am` on `titan2`.
- [titan2-boot](https://github.com/Serlk/titan2-boot): the boot menu in `boot_a` (Android and Linux side by side).
- [titan2-tools](https://github.com/Serlk/titan2-tools): image builders, initramfs, network and disk root, modem, power and sensor services, and the shared `titan2-vendor-runtime`.
- [titan2-docs](https://github.com/Serlk/titan2-docs): documentation and hardware notes.
- [titan2-camera](https://github.com/Serlk/titan2-camera): cameras (libcamera patches, configuration, tests).
- [titan2-fingerprint](https://github.com/Serlk/titan2-fingerprint): the fingerprint reader (FocalTech sensor through the TEE).
- [titan2-gnss](https://github.com/Serlk/titan2-gnss): GNSS (navigation daemon, GPS HAL bridge, EPO assistance data).
- [titan2-android](https://github.com/Serlk/titan2-android): Android builds for the Titan 2.
