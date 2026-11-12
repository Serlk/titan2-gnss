# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
# titan2-epo: the open MediaTek EPO (predicted ephemeris) downloader for the Titan 2 GNSS stack
PREFIX ?= /usr
DESTDIR ?=

all:

install:
	install -Dm755 t2-epo-fetch $(DESTDIR)$(PREFIX)/bin/t2-epo-fetch
	install -Dm755 openrc/t2-epo $(DESTDIR)/etc/init.d/t2-epo
	install -Dm644 openrc/t2-epo.confd $(DESTDIR)/etc/conf.d/t2-epo
	install -Dm755 t2-epo.periodic $(DESTDIR)/etc/periodic/daily/t2-epo
	install -Dm755 t2-epo.nm-dispatcher $(DESTDIR)/etc/NetworkManager/dispatcher.d/90-t2-epo
	install -Dm644 README.md $(DESTDIR)$(PREFIX)/share/doc/titan2-epo/README.md

.PHONY: all install
