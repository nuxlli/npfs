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

static Npuser *np_unix_uname2user(Npuserpool *, char *uname);
static Npuser *np_unix_uid2user(Npuserpool *, u32 uid);
static Npgroup *np_unix_gname2group(Npuserpool *, char *gname);
static Npgroup *np_unix_gid2group(Npuserpool *, u32 gid);
static int np_unix_ismember(Npuserpool *, Npuser *u, Npgroup *g);
static void np_unix_udestroy(Npuserpool *, Npuser *);
static void np_unix_gdestroy(Npuserpool *, Npgroup *);
static int np_init_user_groups(Npuser *u);

static Npuserpool upool = {
	.uname2user = np_unix_uname2user,
	.uid2user = np_unix_uid2user,
	.gname2group = np_unix_gname2group,
	.gid2group = np_unix_gid2group,
	.ismember = np_unix_ismember,
	.udestroy = np_unix_udestroy,
	.gdestroy = np_unix_gdestroy,
};

Npuserpool *np_unix_users = &upool;

static struct Npusercache {
	int		init;
	int		hsize;
	Npuser**	htable;
	pthread_mutex_t	lock;
} usercache = { 0 };

static struct Npgroupcache {
	int		init;
	int		hsize;
	Npgroup**	htable;
	pthread_mutex_t	lock;
} groupcache = { 0 };

static void
initusercache(void)
{
	if (!usercache.init) {
		usercache.hsize = 64;
		usercache.htable = calloc(usercache.hsize, sizeof(Npuser *));
		usercache.init = 1;
		
	}
}

static void
initgroupcache(void)
{
	if (!groupcache.init) {
		groupcache.hsize = 64;
		groupcache.htable = calloc(groupcache.hsize, sizeof(Npgroup *));
		if (!groupcache.htable) {
			np_werror(Ennomem, ENOMEM);
			return;
		}
		groupcache.init = 1;
	}
}

static Npuser *
np_unix_uname2user(Npuserpool *up, char *uname)
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
			if (strcmp(uname, u->uname) == 0)
				break;
	pthread_mutex_unlock(&usercache.lock);

	if (u)
		goto done;

	bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
	if (bufsize < 256)
		bufsize = 256;

	buf = np_malloc(bufsize);
	if (!buf)
		return NULL;

	i = getpwnam_r(uname, &pw, buf, bufsize, &pwp);
	if (i) {
		np_uerror(i);
		free(buf);
		return NULL;
	}

	u = np_malloc(sizeof(*u) + strlen(pw.pw_name) + 1);
	if (!u) {
		free(buf);
		return NULL;
	}

	pthread_mutex_init(&u->lock, NULL);
	u->refcount = 1;
	u->upool = up;
	u->uid = pw.pw_uid;
	u->uname = (char *)u + sizeof(*u);
	strcpy(u->uname, pw.pw_name);
	u->dfltgroup = (*up->gid2group)(up, pw.pw_gid);

	u->ngroups = 0;
	u->groups = NULL;
	np_init_user_groups(u);
	free(buf);

	pthread_mutex_lock(&usercache.lock);
	n = u->uid % usercache.hsize;
	u->next = usercache.htable[n];
	usercache.htable[n] = u;
	pthread_mutex_unlock(&usercache.lock);

done:
	np_user_incref(u);
	return u;
}

static Npuser *
np_unix_uid2user(Npuserpool *up, u32 uid)
{
	int n, i;
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
		goto done;

	bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
	if (bufsize < 256)
		bufsize = 256;

	buf = np_malloc(bufsize);
	if (!buf)
		return NULL;

	i = getpwuid_r(uid, &pw, buf, bufsize, &pwp);
	if (i) {
		np_uerror(i);
		free(buf);
		return NULL;
	}

	u = np_malloc(sizeof(*u) + strlen(pw.pw_name) + 1);
	if (!u) {
		free(buf);
		return NULL;
	}

	pthread_mutex_init(&u->lock, NULL);
	u->refcount = 1;
	u->upool = up;
	u->uid = uid;
	u->uname = (char *)u + sizeof(*u);
	strcpy(u->uname, pw.pw_name);
	u->dfltgroup = up->gid2group(up, pw.pw_gid);

	u->ngroups = 0;
	u->groups = NULL;
	np_init_user_groups(u);
	free(buf);

	pthread_mutex_lock(&usercache.lock);
	u->next = usercache.htable[n];
	usercache.htable[n] = u;
	pthread_mutex_unlock(&usercache.lock);

done:
	np_user_incref(u);
	return u;
}

static Npgroup *
np_unix_gname2group(Npuserpool *up, char *gname)
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
			if (strcmp(g->gname, gname) == 0)
				break;
	pthread_mutex_unlock(&groupcache.lock);

	if (g)
		goto done;

	bufsize = sysconf(_SC_GETGR_R_SIZE_MAX);
	if (bufsize < 256)
		bufsize = 256;

	buf = np_malloc(bufsize);
	if (!buf)
		return NULL;

	i = getgrnam_r(gname, &grp, buf, bufsize, &pgrp);
	if (i) {
		np_uerror(i);
		free(buf);
		return NULL;
	}

	g = np_malloc(sizeof(*g) + strlen(grp.gr_name) + 1);
	if (!g) {
		free(buf);
		return NULL;
	}

	g->refcount = 1;
	g->upool = up;
	g->gid = grp.gr_gid;
	g->gname = (char *)g + sizeof(*g);
	strcpy(g->gname, grp.gr_name);

	pthread_mutex_lock(&groupcache.lock);
	n = g->gid % groupcache.hsize;
	g->next = groupcache.htable[n];
	groupcache.htable[n] = g;
	pthread_mutex_unlock(&groupcache.lock);

	free(buf);

done:
	np_group_incref(g);
	return g;
}

static Npgroup *
np_unix_gid2group(Npuserpool *up, u32 gid)
{
	int n, err;
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
		goto done;

	bufsize = sysconf(_SC_GETGR_R_SIZE_MAX);
	if (bufsize < 256)
		bufsize = 256;

	buf = np_malloc(bufsize);
	if (!buf)
		return NULL;

	err = getgrgid_r(gid, &grp, buf, bufsize, &pgrp);
	if (err) {
		np_uerror(err);
		free(buf);
		return NULL;
	}

	g = np_malloc(sizeof(*g) + strlen(grp.gr_name) + 1);
	if (!g) {
		free(buf);
		return NULL;
	}

	pthread_mutex_init(&g->lock, NULL);
	g->refcount = 1;
	g->upool = up;
	g->gid = grp.gr_gid;
	g->gname = (char *)g + sizeof(*g);
	strcpy(g->gname, grp.gr_name);

	pthread_mutex_lock(&groupcache.lock);
	g->next = groupcache.htable[n];
	groupcache.htable[n] = g;
	pthread_mutex_unlock(&groupcache.lock);

	free(buf);

done:
	np_group_incref(g);
	return g;
}

static int
np_unix_ismember(Npuserpool *up, Npuser *u, Npgroup *g)
{
	int i;

	if (!u->groups && np_init_user_groups(u)<0)
		return -1;

	for(i = 0; i < u->ngroups; i++) {
		if (g == u->groups[i])
			return 1;
	}

	return 0;
}

static void
np_unix_udestroy(Npuserpool *up, Npuser *u)
{
}

static void
np_unix_gdestroy(Npuserpool *up, Npgroup *g)
{
}

static int
np_init_user_groups(Npuser *u)
{
	int i, n=0;
	int maxgroups = 256; /* warning: configurable in kernel */
	Npgroup **grps;
	struct group *g;
	gid_t gids[maxgroups];

	free(u->groups);
	u->ngroups = 0;

	setgrent(); 
	
	if(u->dfltgroup) {
		gids[0] = u->dfltgroup->gid;
		n++;
	}
	
	while ((g = getgrent()) != NULL) { 
		for (i = 0; g->gr_mem[i]; i++) { 
			if (strcmp(u->uname, g->gr_mem[0]) == 0) { 
				n++; 
				if(n < maxgroups) 
					gids[n] = g->gr_gid; 
			} 
		}
	}
	
	endgrent(); 

	grps = np_malloc(sizeof(*grps) * n);
	if (!grps) {
		free(gids);
		return -1;
	}
	
	for(i = 0; i < n; i++) {
		grps[i] = u->upool->gid2group(u->upool, gids[i]);
		if (grps[i]) {
			free(grps);
			return -1;
		}
	}

	u->groups = grps;
	u->ngroups = n;
	return 0;
}
