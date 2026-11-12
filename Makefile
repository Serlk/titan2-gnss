# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# titan2-gnss user space: t2-gnssd (the vendor GPS HAL in-process -> mnld -> NMEA socket
# for GeoClue), t2-initsock (init-style control sockets for mnld), the OpenRC services
# t2-mnld and t2-gnss, the GeoClue drop-in and the vendor-root drop-ins for
# t2-vendor-setup. The EPO downloader is in epo/ (its own Makefile, package titan2-epo).
#
#   make                 needs bionic_translation-dev
#   make dist            titan2-gnss.tar.gz and titan2-epo.tar.gz for aports/
#
# VENDOR_RUNTIME: the t2-hal / bionic_exe sources of titan2-vendor-runtime. Default: a
# checkout of titan2-tools next to this one:
#   git clone https://github.com/Serlk/titan2-tools ../titan2-tools
# The Alpine package uses the installed copy (titan2-vendor-runtime-dev):
#   make VENDOR_RUNTIME=/usr/share/titan2-vendor-runtime/src
VENDOR_RUNTIME ?= ../titan2-tools/tools/titan2-vendor-runtime
CC ?= gcc
CFLAGS ?= -O2 -g -Wall
CPPFLAGS += -Iinclude -I$(VENDOR_RUNTIME) -I$(VENDOR_RUNTIME)/include
PREFIX ?= /usr
DESTDIR ?=
SHARE = $(DESTDIR)$(PREFIX)/share/titan2-vendor-runtime

all: t2-gnssd t2-initsock

t2-gnssd.o: src/t2-gnssd.c $(VENDOR_RUNTIME)/t2-hal.h include/hardware/gps.h include/hardware/gnss-base.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

t2-hal.o: $(VENDOR_RUNTIME)/t2-hal.c $(VENDOR_RUNTIME)/t2-hal.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

bionic_exe.o: $(VENDOR_RUNTIME)/bionic_exe.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

t2-gnssd: t2-gnssd.o t2-hal.o bionic_exe.o
	$(CC) $(LDFLAGS) -o $@ $^ -ldl_bio -lpthread -lm

t2-initsock: src/t2-initsock.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

install: all
	install -Dm755 t2-gnssd $(DESTDIR)$(PREFIX)/bin/t2-gnssd
	install -Dm755 t2-initsock $(DESTDIR)$(PREFIX)/bin/t2-initsock
	install -Dm755 openrc/t2-mnld $(DESTDIR)/etc/init.d/t2-mnld
	install -Dm644 openrc/t2-mnld.confd $(DESTDIR)/etc/conf.d/t2-mnld
	install -Dm755 openrc/t2-gnss $(DESTDIR)/etc/init.d/t2-gnss
	install -Dm644 openrc/t2-gnss.confd $(DESTDIR)/etc/conf.d/t2-gnss
	install -Dm644 geoclue/90-titan2-gnss.conf $(DESTDIR)/etc/geoclue/conf.d/90-titan2-gnss.conf
	install -Dm644 files/gnss-files.txt $(SHARE)/files.d/gnss.txt
	install -Dm644 files/gnss.props $(SHARE)/props.d/gnss.props

dist:
	tar -C .. --exclude=.git --exclude='*.o' --exclude='$(notdir $(CURDIR))/t2-gnssd' \
		--exclude='$(notdir $(CURDIR))/t2-initsock' \
		--exclude='*.tar.gz' --exclude=__pycache__ --owner=0 --group=0 --mtime=@0 --sort=name \
		--transform 's,^$(notdir $(CURDIR)),titan2-gnss,' \
		-czf aports/titan2-gnss/titan2-gnss.tar.gz $(notdir $(CURDIR))
	tar -C . --owner=0 --group=0 --mtime=@0 --sort=name \
		-czf aports/titan2-epo/titan2-epo.tar.gz epo

clean:
	rm -f t2-gnssd t2-initsock *.o

.PHONY: all install dist clean
