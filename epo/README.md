<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com> -->
# titan2-epo: open EPO (predicted ephemeris) downloader for the Titan 2

MediaTek GNSS chips cut the time to first fix with EPO: a file of predicted orbits for a few
days, which the Android stack of the factory firmware downloads (mnld's own libcurl thread, or the framework's
PSDS downloader) and feeds to the navigation daemon. On our mainline system mnld runs in a
bionic chroot without netd, so it cannot resolve names; this package does the download on the
Linux side and the file is placed where mnld reads it (`/data/vendor/gps/EPO.DAT` of the vendor
chroot) — no closed SUPL client, no network client in the chroot.

Interface: server
`https://uepodownload.mediatek.com/`, files `EPO_GR_3_<N>.DAT` (GPS+GLONASS) and
`EPO_GPS_3_<N>.DAT` (GPS), N = 1..10, each with a `.MD5` (the hex digest alone); stored by mnld
as `/data/vendor/gps/EPO.DAT` (own download, merged) or `EPOHAL.DAT` (injected). Quarter EPO
(`QG_R.DAT` 4032 B, `QGA.DAT` 1656 B, `QBD2.DAT` 2736 B + `.MD5` from
`https://ugepodownload.mediatek.com/`) → `QEPO*.DAT`. Records are 72 bytes (one satellite, one
6-hour set). Checked 2026-11-10 from the PC: only the `_1` files are served — `EPO_GR_3_1.DAT` is
48384 B = 12 sets × 56 SV × 72 B (3 days), `EPO_GPS_3_1.DAT` 27648 B; `_2`..`_10` answer 403.

- `t2-epo-fetch [-f] [-q] [-v] [--install [ROOT] | --install-only [ROOT] | --no-install]`: fetches
  `EPO_GR_3_1.DAT` (+ MD5 check) into `/var/lib/t2-gnss/epo/EPO.DAT` when the stored file is older
  than 6 h (`/etc/conf.d/t2-epo`) and — by default whenever `<root>/data/vendor/gps` exists — copies
  it (if newer) into the vendor root as `/data/vendor/gps/EPO.DAT`, the file mnld reads itself: the
  Titan 2 GPS HAL module has no XTRA injection entry, so the file is the working path.
  `--install-only` copies without downloading (t2-mnld runs it before starting mnld).
- OpenRC `t2-epo` (after `net`, detached), `/etc/periodic/daily/t2-epo` (busybox crond),
  `/etc/NetworkManager/dispatcher.d/90-t2-epo` (every connection up / connectivity FULL).
- t2-gnssd: injects a new file as soon as it appears while the receiver runs; spawns
  `t2-epo-fetch -q` when the file is stale at a session start or when the HAL asks for XTRA data.
