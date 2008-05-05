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
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include "npfs.h"
#include "npfsimpl.h"

Npfid**
np_fidpool_create(void)
{
	Npfid **ret;

	return calloc(FID_HTABLE_SIZE, sizeof(*ret));
}

void
np_fidpool_destroy(Npfid **pool)
{
	int i;
	Npfid *f, *ff;
	Npsrv *srv;

	for(i = 0; i < FID_HTABLE_SIZE; i++) {
		f = pool[i];
		while (f != NULL) {
			ff = f->next;
			srv = f->conn->srv;
			if (f->type&Qtauth && srv->auth->clunk)
				(*srv->auth->clunk)(f);
			else if (!(f->type&Qtauth) && srv->fiddestroy)
				(*srv->fiddestroy)(f);
			free(f);
			f = ff;
		}
	}

	free(pool);
}

Npfid*
np_fid_find(Npconn *conn, u32 fid)
{
	int hash;
	Npfid **htable, *f, **prevp;

	hash = fid % FID_HTABLE_SIZE;
	htable = conn->fidpool;
	pthread_mutex_lock(&conn->lock);
	prevp = &htable[hash];
	f = *prevp;
	while (f != NULL) {
		if (f->fid == fid) {
			*prevp = f->next;
			f->next = htable[hash];
			htable[hash] = f;
			break;
		}

		prevp = &f->next;
		f = *prevp;
	}
	pthread_mutex_unlock(&conn->lock);
	return f;
}

Npfid*
np_fid_create(Npconn *conn, u32 fid, void *aux)
{
	int hash;
	Npfid **htable, *f;

	hash = fid % FID_HTABLE_SIZE;
	htable = conn->fidpool;
	f = np_fid_find(conn, fid);
	if (f)
		return NULL;

	f = malloc(sizeof(*f));
	if (!f)
		return NULL;

	f->fid = fid;
	f->conn = conn;
	f->refcount = 0;
	f->omode = ~0;
	f->type = 0;
	f->diroffset = 0;
	f->user = NULL;
	f->aux = aux;

	pthread_mutex_lock(&conn->lock);
	f->next = htable[hash];
	htable[hash] = f;
	pthread_mutex_unlock(&conn->lock);

	return f;
}

int
np_fid_destroy(Npfid *fid)
{
	int hash;
	Npconn *conn;
	Npsrv *srv;
	Npfid **htable, *f, **prevp;

	conn = fid->conn;
	hash = fid->fid % FID_HTABLE_SIZE;
	htable = conn->fidpool;
	if(!htable)
		return 0;

	pthread_mutex_lock(&conn->lock);
	prevp = &htable[hash];
	f = *prevp;
	while (f != NULL) {
		if (f->fid == fid->fid) {
			*prevp = f->next;
			srv = f->conn->srv;
			if (f->type & Qtauth && srv->auth->clunk)
				(*srv->auth->clunk)(f);
			else if (!(f->type&Qtauth) && srv->fiddestroy)
				(*srv->fiddestroy)(f);

			if (f->user)
				np_user_decref(f->user);

			free(f);
			break;
		}

		prevp = &f->next;
		f = *prevp;
	}
	pthread_mutex_unlock(&conn->lock);
	return f != NULL;
}

void
np_fid_incref(Npfid *fid)
{
	if (!fid)
		return;

	fid->refcount++;
}

void
np_fid_decref(Npfid *fid)
{
	if (!fid)
		return;

	fid->refcount--;

	if (!fid->refcount)
		np_fid_destroy(fid);
}
