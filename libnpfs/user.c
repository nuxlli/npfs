/*
 * Copyright (C) 2005 by Latchesar Ionkov <lucho@ionkov.net>
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
 * LATCHESAR IONKOV AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <pwd.h>
#include <grp.h>
#include <errno.h>
#include <pthread.h>
#include "npfs.h"
#include "npfsimpl.h"

struct Usercache {
	int		init;
	pthread_mutex_t	lock;
	int		hsize;
	Npuser**	htable;
} usercache = { 0, PTHREAD_MUTEX_INITIALIZER };

struct Npgroupcache {
	int		init;
	pthread_mutex_t lock;
	int		hsize;
	Npgroup**	htable;
} groupcache = { 0, PTHREAD_MUTEX_INITIALIZER };

static pthread_key_t currthreaduser;

static void
initusercache(void)
{
	if (!usercache.init) {
		usercache.hsize = 64;
		usercache.htable = calloc(usercache.hsize, sizeof(Npuser *));
		pthread_key_create(&currthreaduser, NULL);
		usercache.init = 1;
	}
}

Npuser*
np_uid2user(int uid)
{
	int n;
	Npuser *u;
	struct passwd pw, *pwp;
	int bufsize;
	char *buf;

	pthread_mutex_lock(&usercache.lock);
	if (!usercache.init)
		initusercache();

	n = uid % usercache.hsize;
	for(u = usercache.htable[n]; u != NULL; u = u->next)
		if (u->uid == uid)
			break;

	pthread_mutex_unlock(&usercache.lock);

	if (u)
		return u;

	bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
	if (bufsize < 256)
		bufsize = 256;

	buf = malloc(bufsize);
	getpwuid_r(uid, &pw, buf, bufsize, &pwp);

	if (!pwp) {
		free(buf);
		return NULL;
	}

	u = malloc(sizeof(*u) + strlen(pw.pw_name) + 1);
	u->uid = uid;
	u->uname = (char *)u + sizeof(*u);
	strcpy(u->uname, pw.pw_name);
	u->dfltgroup = np_gid2group(pw.pw_gid);

	u->ngroups = 0;
	u->groups = NULL;

	pthread_mutex_lock(&usercache.lock);
	u->next = usercache.htable[n];
	usercache.htable[n] = u;
	pthread_mutex_unlock(&usercache.lock);

	free(buf);
	return u;
}

Npuser*
np_uname2user(char *uname)
{
	int i, n;
	struct passwd pw, *pwp;
	int bufsize;
	char *buf;
	Npuser *u;

	pthread_mutex_lock(&usercache.lock);
	if (!usercache.init)
		initusercache();

	for(i = 0; i<usercache.hsize; i++)
		for(u = usercache.htable[i]; u != NULL; u = u->next)
			if (strcmp(uname, u->uname) == 0) {
				pthread_mutex_unlock(&usercache.lock);
				return u;
			}

	pthread_mutex_unlock(&usercache.lock);
	bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
	if (bufsize < 256)
		bufsize = 256;

	buf = malloc(bufsize);
	getpwnam_r(uname, &pw, buf, bufsize, &pwp);
	if (!pwp) {
		free(buf);
		return NULL;
	}

	u = malloc(sizeof(*u) + strlen(pw.pw_name) + 1);
	u->uid = pw.pw_uid;
	u->uname = (char *)u + sizeof(*u);
	strcpy(u->uname, pw.pw_name);
	u->dfltgroup = np_gid2group(pw.pw_gid);

	u->ngroups = 0;
	u->groups = NULL;

//	pthread_mutex_lock(&usercache.lock);
	n = u->uid % usercache.hsize;
	u->next = usercache.htable[n];
	usercache.htable[n] = u;
	pthread_mutex_unlock(&usercache.lock);

	free(buf);
	return u;
}

int
np_usergroups(Npuser *u, gid_t **gids)
{
	int n;
	gid_t *g;
	gid_t *grps;

	if (!u->groups) {
		n = 0;
		getgrouplist(u->uname, u->dfltgroup->gid, NULL, &n);
		grps = malloc(sizeof(*grps) * n);
		getgrouplist(u->uname, u->dfltgroup->gid, g, &n);

		pthread_mutex_lock(&usercache.lock);
		if (u->groups)
			free(u->groups);
		u->groups = grps;
		u->ngroups = n;
		pthread_mutex_unlock(&usercache.lock);
	}

	*gids = u->groups;
	return u->ngroups;
}

static void
initgroupcache(void)
{
	if (!groupcache.init) {
		groupcache.hsize = 64;
		groupcache.htable = calloc(groupcache.hsize, sizeof(Npuser *));
		groupcache.init = 1;
	}
}

Npgroup*
np_gid2group(gid_t gid)
{
	int n;
	Npgroup *g;
	struct group grp, *pgrp;
	int bufsize;
	char *buf;

	pthread_mutex_lock(&groupcache.lock);
	if (!groupcache.init)
		initgroupcache();

	n = gid % groupcache.hsize;
	for(g = groupcache.htable[n]; g != NULL; g = g->next)
		if (g->gid == gid)
			break;

	pthread_mutex_unlock(&groupcache.lock);

	if (g)
		return g;

	bufsize = sysconf(_SC_GETGR_R_SIZE_MAX);
	if (bufsize < 256)
		bufsize = 256;

	buf = malloc(bufsize);
	getgrgid_r(gid, &grp, buf, bufsize, &pgrp);
	if (!pgrp) {
		free(buf);
		return NULL;
	}

	g = malloc(sizeof(*g) + strlen(grp.gr_name) + 1);
	g->gid = grp.gr_gid;
	g->gname = (char *)g + sizeof(*g);
	strcpy(g->gname, grp.gr_name);

	pthread_mutex_lock(&groupcache.lock);
	g->next = groupcache.htable[n];
	groupcache.htable[n] = g;
	pthread_mutex_unlock(&groupcache.lock);

	free(buf);
	return g;
}

Npgroup*
np_gname2group(char *gname)
{
	int i, n, bufsize;
	Npgroup *g;
	struct group grp, *pgrp;
	char *buf;

	pthread_mutex_lock(&groupcache.lock);
	if (!groupcache.init)
		initgroupcache();

	for(i = 0; i < groupcache.hsize; i++) 
		for(g = groupcache.htable[i]; g != NULL; g = g->next)
			if (strcmp(g->gname, gname) == 0) {
				pthread_mutex_unlock(&groupcache.lock);
				return g;
			}

	pthread_mutex_unlock(&groupcache.lock);
	bufsize = sysconf(_SC_GETGR_R_SIZE_MAX);
	if (bufsize < 256)
		bufsize = 256;

	buf = malloc(bufsize);
	getgrnam_r(gname, &grp, buf, bufsize, &pgrp);
	if (!pgrp) {
		free(buf);
		return NULL;
	}

	g = malloc(sizeof(*g) + strlen(grp.gr_name) + 1);
	g->gid = grp.gr_gid;
	g->gname = (char *)g + sizeof(*g);
	strcpy(g->gname, grp.gr_name);

	pthread_mutex_lock(&groupcache.lock);
	n = g->gid % groupcache.hsize;
	g->next = groupcache.htable[n];
	groupcache.htable[n] = g;
	pthread_mutex_unlock(&groupcache.lock);

	free(buf);
	return g;
}

int
np_change_user(Npuser *u)
{
	int n, ret;
	Npuser *cu;
	gid_t *gids;

	cu = pthread_getspecific(currthreaduser);
	if (cu == u)
		return 0;

	if (setreuid(0, 0) < 0) {
		ret = errno;
//		fprintf(stderr, "cannot setuid to root\n");
		return ret;
	}

	if (!u->groups) {
		if (initgroups(u->uname, u->dfltgroup->gid) < 0) {
			ret = errno;
//			fprintf(stderr, "initgroups failed\n");
			return ret;
		}

		gids = malloc(sizeof(gid_t) * 32);
		gids[0] = u->dfltgroup->gid;
		n = getgroups(31, &gids[1]);
		if (n < 0) {
			ret = errno;
//			fprintf(stderr, "cannot get groups\n");
			return ret;
		}
		n++;

		pthread_mutex_lock(&usercache.lock);
		u->groups = gids;
		u->ngroups = n;
		pthread_mutex_unlock(&usercache.lock);
	} else {
		setgroups(u->ngroups, u->groups);
	}

	if (setregid(-1, u->dfltgroup->gid) < 0) {
		ret = errno;
//		fprintf(stderr, "setregid(%d) failed: %d\n", u->dfltgroup->gid, ret);
		return ret;
	}

	if (setreuid(-1, u->uid) < 0) {
		ret = errno;
//		fprintf(stderr, "setreuid failed\n");
		return ret;
	}

	pthread_setspecific(currthreaduser, u);

	return 0;
}
