/*
 * Copyright (C) 2006 by Latchesar Ionkov <lucho@ionkov.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include "npfs.h"
#include "npclient.h"
#include "npcimpl.h"

Npcfsys *
npc_netmount(char *address, char *uname, int dfltport)
{
	int fd, port;
	char *addr, *name, *p, *s;
	struct sockaddr_in saddr;
	struct hostent *hostinfo;

	addr = strdup(address);
	if (strncmp(addr, "tcp!", 4) == 0)
		name = addr + 4;
	else
		name = addr;

	port = dfltport;
	p = strrchr(name, '!');
	if (p) {
		*p = '\0';
		p++;
		port = strtol(p, &s, 10);
		if (*s != '\0') {
			np_werror("invalid port format", EIO);
			goto error;
		}
	}

	fd = socket(PF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		np_uerror(errno);
		goto error;
	}

	hostinfo = gethostbyname(name);
	if (!hostinfo) {
		np_werror("cannot resolve name", EIO);
		goto error;
	}

	saddr.sin_family = AF_INET;
	saddr.sin_port = htons(port);
	saddr.sin_addr = *(struct in_addr *) hostinfo->h_addr;

	if (connect(fd, (struct sockaddr *) &saddr, sizeof(saddr)) < 0) {
		np_uerror(errno);
		goto error;
	}

	free(addr);
	return npc_mount(fd, NULL, uname);

error:
	free(addr);
	return NULL;
}

