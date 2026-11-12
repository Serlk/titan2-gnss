// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * t2-initsock: the "socket NAME stream MODE" lines of an Android init .rc,
 * for factory-firmware daemons started natively (chroot). Android init creates
 * /dev/socket/NAME (bound, not listening), passes the descriptor to the service
 * and names it in the environment as ANDROID_SOCKET_NAME=<fd>; libcutils'
 * android_get_control_socket() finds it there. Without it a daemon that was
 * written for init (mnld: "socket mnld stream 660 gps system", mtk_agpsd:
 * agpsd/agpsd2/agpsd3) has no control socket.
 *
 *   t2-initsock [-d DIR] [-m MODE] NAME[:MODE]... -- COMMAND [ARG...]
 *     -d DIR    socket directory (default /dev/socket)
 *     -m MODE   octal permissions (default 0660)
 *   The sockets are SOCK_STREAM, SOCK_CLOEXEC is NOT set (the child inherits
 *   them). Then COMMAND is exec'ed with the environment variables added.
 *
 * Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	const char *dir = "/dev/socket";
	unsigned mode = 0660;
	int i, cmd = -1;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d") && i + 1 < argc)
			dir = argv[++i];
		else if (!strcmp(argv[i], "-m") && i + 1 < argc)
			mode = strtoul(argv[++i], NULL, 8);
		else if (!strcmp(argv[i], "--")) {
			cmd = i + 1;
			break;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "t2-initsock: unknown option %s\n", argv[i]);
			return 2;
		}
	}
	if (cmd < 0 || cmd >= argc) {
		fprintf(stderr, "usage: t2-initsock [-d DIR] [-m MODE] NAME[:MODE]... -- COMMAND [ARG...]\n");
		return 2;
	}
	mkdir(dir, 0755);
	for (i = 1; i < cmd - 1; i++) {
		char name[64], env[96];
		struct sockaddr_un sa;
		unsigned m = mode;
		char *colon;
		int fd;

		if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "-m")) {
			i++;
			continue;
		}
		snprintf(name, sizeof(name), "%s", argv[i]);
		colon = strchr(name, ':');
		if (colon) {
			*colon = 0;
			m = strtoul(colon + 1, NULL, 8);
		}
		memset(&sa, 0, sizeof(sa));
		sa.sun_family = AF_UNIX;
		if (snprintf(sa.sun_path, sizeof(sa.sun_path), "%s/%s", dir, name) >= (int)sizeof(sa.sun_path)) {
			fprintf(stderr, "t2-initsock: path too long for %s\n", name);
			return 1;
		}
		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0) {
			perror("t2-initsock: socket");
			return 1;
		}
		unlink(sa.sun_path);
		if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
			fprintf(stderr, "t2-initsock: bind %s: %s\n", sa.sun_path, strerror(errno));
			return 1;
		}
		chmod(sa.sun_path, m);
		snprintf(env, sizeof(env), "ANDROID_SOCKET_%s", name);
		{
			char val[16];
			snprintf(val, sizeof(val), "%d", fd);
			setenv(env, val, 1);
		}
	}
	execvp(argv[cmd], &argv[cmd]);
	fprintf(stderr, "t2-initsock: exec %s: %s\n", argv[cmd], strerror(errno));
	return 127;
}
