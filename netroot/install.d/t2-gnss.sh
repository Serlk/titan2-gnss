# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# GNSS in the image; sourced by titan2-tools' scripts/netroot-serve.sh build when this
# repository is enabled ($R = image root, $X = this repository, mounted read-only).
# The packages (netroot/packages) are installed already. The vendor root
# /var/lib/titan2-vendor is built ON THE PHONE by t2-vendor-setup from its own partitions
# (never in the image). Services: t2-mnld (mnld in the chroot), t2-gnss (t2-gnssd -> NMEA
# socket), t2-epo (EPO fetch when the network is up), crond (daily refresh).
for s in t2-mnld t2-gnss t2-epo crond; do
	[ -e $R/etc/init.d/$s ] && mkdir -p $R/etc/runlevels/default && ln -sf /etc/init.d/$s $R/etc/runlevels/default/$s
done
echo "GNSS: t2-mnld, t2-gnss, t2-epo, crond in default (vendor root: t2-vendor-setup on the phone)"
