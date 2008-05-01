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
#include <assert.h>
#include "npfs.h"
#include "npfsimpl.h"

struct Reqpool {
	pthread_mutex_t	lock;
	int		reqnum;
	Npreq*		reqlist;
} reqpool = { PTHREAD_MUTEX_INITIALIZER, 0, NULL };

char *Eunknownfid = "unknown fid";
char *Enomem = "no memory";
char *Enoauth = "no authentication required";
char *Enotimpl = "not implemented";
char *Einuse = "fid already exists";
char *Ebadusefid = "bad use of fid";
char *Enotdir = "not a directory";
char *Etoomanywnames = "too many wnames";
char *Eperm = "permission denied";
char *Etoolarge = "i/o count too large";
char *Ebadoffset = "bad offset in directory read";
char *Edirchange = "cannot convert between files and directories";
char *Enotfound = "file not found";
char *Eopen = "file alread exclusively opened";
char *Eexist = "file or directory already exists";
char *Enotempty = "directory not empty";
char *Eunknownuser = "unknown user";

static void np_wthread_create(Npsrv *srv);
static void np_srv_destroy(Npsrv *srv);
static void np_wthread_create(Npsrv *srv);
static void *np_wthread_proc(void *a);

static Npfcall* np_default_version(Npconn *, u32, Npstr *);
static Npfcall* np_default_attach(Npfid *, Npfid *, Npstr *, Npstr *);
static void np_default_flush(Npreq *);
static int np_default_clone(Npfid *, Npfid *);
static int np_default_walk(Npfid *, Npstr*, Npqid *);
static Npfcall* np_default_open(Npfid *, u8);
static Npfcall* np_default_create(Npfid *, Npstr*, u32, u8, Npstr*);
static Npfcall* np_default_read(Npfid *, u64, u32, Npreq *);
static Npfcall* np_default_write(Npfid *, u64, u32, u8*, Npreq *);
static Npfcall* np_default_clunk(Npfid *);
static Npfcall* np_default_remove(Npfid *);
static Npfcall* np_default_stat(Npfid *);
static Npfcall* np_default_wstat(Npfid *, Npstat *);

Npsrv*
np_srv_create(int nwthread)
{
	int i;
	Npsrv *srv;

	srv = malloc(sizeof(*srv));
	pthread_mutex_init(&srv->lock, NULL);
	pthread_cond_init(&srv->reqcond, NULL);
	srv->msize = 8216;
	srv->dotu = 1;
	srv->srvaux = NULL;
	srv->treeaux = NULL;
	srv->shuttingdown = 0;
	srv->auth = NULL;

	srv->start = NULL;
	srv->shutdown = NULL;
	srv->destroy = NULL;
	srv->connopen = NULL;
	srv->connclose = NULL;
	srv->fiddestroy = NULL;

	srv->version = np_default_version;
	srv->attach = np_default_attach;
	srv->flush = np_default_flush;
	srv->clone = np_default_clone;
	srv->walk = np_default_walk;
	srv->open = np_default_open;
	srv->create = np_default_create;
	srv->read = np_default_read;
	srv->write = np_default_write;
	srv->clunk = np_default_clunk;
	srv->remove = np_default_remove;
	srv->stat = np_default_stat;
	srv->wstat = np_default_wstat;

	srv->conns = NULL;
	srv->reqs_first = NULL;
	srv->reqs_last = NULL;
	srv->workreqs = NULL;
	srv->wthreads = NULL;
	srv->debuglevel = 0;

	for(i = 0; i < nwthread; i++)
		np_wthread_create(srv);

	return srv;
}

void
np_srv_start(Npsrv *srv)
{
	if (srv->start)
		(*srv->start)(srv);
}

void
np_srv_shutdown(Npsrv *srv, int shutconns)
{
	Npconn *conn, *conn1;

	pthread_mutex_lock(&srv->lock);
	srv->shuttingdown = 1;
	(*srv->shutdown)(srv);
	if (shutconns) {
		conn = srv->conns;
		srv->conns = NULL;
	}
	pthread_mutex_unlock(&srv->lock);

	while (conn != NULL) {
		conn1 = conn->next;
		np_conn_shutdown(conn);
		conn = conn1;
	}
}

int
np_srv_add_conn(Npsrv *srv, Npconn *conn)
{
	int ret;

	ret = 0;
	pthread_mutex_lock(&srv->lock);
	np_conn_incref(conn);
	if (!srv->shuttingdown) {
		conn->srv = srv;
		conn->next = srv->conns;
		srv->conns = conn;
		ret = 1;
	}
	pthread_mutex_unlock(&srv->lock);

	if (srv->connopen)
		(*srv->connopen)(conn);

	return ret;
}

void
np_srv_remove_conn(Npsrv *srv, Npconn *conn)
{
	Npconn *c, **pc;

	pthread_mutex_lock(&srv->lock);
	pc = &srv->conns;
	c = *pc;
	while (c != NULL) {
		if (c == conn) {
			*pc = c->next;
			c->next = NULL;
			break;
		}

		pc = &c->next;
		c = *pc;
	}

	if (srv->connclose)
		(*srv->connclose)(conn);

	np_conn_decref(conn);
	if (srv->shuttingdown && !srv->conns)
		np_srv_destroy(srv);

	pthread_mutex_unlock(&srv->lock);
}

static void
np_srv_destroy(Npsrv *srv)
{
	Npwthread *wt;

	for(wt = srv->wthreads; wt != NULL; wt = wt->next) {
		wt->shutdown = 1;
	}
	pthread_cond_broadcast(&srv->reqcond);
	(*srv->destroy)(srv);
}

void
np_srv_add_req(Npsrv *srv, Npreq *req)
{
	req->prev = srv->reqs_last;
	if (srv->reqs_last)
		srv->reqs_last->next = req;
	srv->reqs_last = req;
	if (!srv->reqs_first)
		srv->reqs_first = req;
	pthread_cond_signal(&srv->reqcond);
}

void
np_srv_remove_req(Npsrv *srv, Npreq *req)
{
	if (req->prev)
		req->prev->next = req->next;

	if (req->next)
		req->next->prev = req->prev;

	if (req == srv->reqs_first)
		srv->reqs_first = req->next;

	if (req == srv->reqs_last)
		srv->reqs_last = req->prev;
}

void
np_srv_add_workreq(Npsrv *srv, Npreq *req)
{
	if (srv->workreqs)
		srv->workreqs->prev = req;

	req->next = srv->workreqs;
	srv->workreqs = req;
	req->prev = NULL;
}

void
np_srv_remove_workreq(Npsrv *srv, Npreq *req)
{
	if (req->prev)
		req->prev->next = req->next;
	else
		srv->workreqs = req->next;

	if (req->next)
		req->next->prev = req->prev;
}

static void
np_wthread_create(Npsrv *srv)
{
	int err;
	Npwthread *wt;

	wt = malloc(sizeof(*wt));
	wt->srv = srv;
	wt->shutdown = 0;
	err = pthread_create(&wt->thread, NULL, np_wthread_proc, wt);
	if (err) {
		fprintf(stderr, "can't create thread: %d\n", err);
		return;
	}

	pthread_mutex_lock(&srv->lock);
	wt->next = srv->wthreads;
	srv->wthreads = wt;
	pthread_mutex_unlock(&srv->lock);
}

static Npfcall *
np_version(Npreq *req, Npfcall *tc)
{
	if (tc->msize < IOHDRSZ + 1) {
		np_werror("msize too small", EIO);
		return NULL;
	}
	return (*req->conn->srv->version)(req->conn, tc->msize, &tc->version);
}

static Npfcall *
np_auth(Npreq *req, Npfcall *tc)
{
	Npconn *conn;
	Npfid *afid;
	Npfcall *rc;

	rc = NULL;
	conn = req->conn;
	afid = np_fid_find(conn, tc->afid);
	if (afid) {
		np_werror(Einuse, EIO);
		goto done;
	}

	afid = np_fid_create(conn, tc->afid, NULL);
	if (!afid) {
		np_werror(Enomem, ENOMEM);
		goto done;
	} else
		np_fid_incref(afid);

	if (conn->srv->auth)
		rc = (*conn->srv->auth->auth)(afid, &tc->uname, &tc->aname);
	else
		np_werror(Enoauth, EIO);

	afid->type = Qtauth;
done:
	np_fid_decref(afid);
	return rc;
}

static Npfcall *
np_attach(Npreq *req, Npfcall *tc)
{
	Npconn *conn;
	Npfid *fid, *afid;
	Npfcall *rc;

	rc = NULL;
	conn = req->conn;
	afid = NULL;
	fid = np_fid_find(conn, tc->fid);
	if (fid) {
		np_werror(Einuse, EIO);
		goto done;
	}

	fid = np_fid_create(conn, tc->fid, NULL);
	if (!fid) {
		np_werror(Enomem, ENOMEM);
		goto done;
	} else 
		np_fid_incref(fid);

	req->fid = fid;
	afid = np_fid_find(conn, tc->afid);
	if (!afid) {
		if (tc->afid!=NOFID) {
			np_werror(Eunknownfid, EIO);
			goto done;
		}

		if (!afid->type&Qtauth) {
			np_werror(Ebadusefid, EIO);
			goto done;
		}
	} else 
		np_fid_incref(afid);

	if (conn->srv->auth) {
		rc = (*conn->srv->auth->attach)(afid, &tc->uname, &tc->aname);
		if (rc)
			goto done;
	}

	rc = (*conn->srv->attach)(fid, afid, &tc->uname, &tc->aname);

done:
//	np_fid_decref(fid);
	np_fid_decref(afid);
	return rc;
}

static Npfcall*
np_flush(Npreq *req, Npfcall *tc)
{
	u16 oldtag;
	Npreq *creq;
	Npconn *conn;
	Npfcall *ret;

	ret = NULL;
	conn = req->conn;
	oldtag = tc->oldtag;
	pthread_mutex_lock(&conn->srv->lock);
	// check pending requests
	for(creq = conn->srv->reqs_first; creq != NULL; creq = creq->next) {
		if (creq->conn==conn && creq->tag==oldtag) {
			np_srv_remove_req(conn->srv, creq);
			pthread_mutex_lock(&creq->lock);
			np_conn_respond(creq); /* doesn't send anything */
			pthread_mutex_unlock(&creq->lock);
			np_req_unref(creq);
			ret = np_create_rflush();
			creq = NULL;
			goto done;
		}
	}

	// check working requests
	creq = conn->srv->workreqs;
	while (creq != NULL) {
		if (creq->conn==conn && creq->tag==oldtag) {
			np_req_ref(creq);
			pthread_mutex_lock(&creq->lock);
			req->flushreq = creq->flushreq;
			creq->flushreq = req;
			pthread_mutex_unlock(&creq->lock);
			goto done;
		}
		creq = creq->next;
	}

	// if not found, return Rflush
	if (!creq)
		ret = np_create_rflush();

done:
	pthread_mutex_unlock(&conn->srv->lock);

	// if working request found, try to flush it
	if (creq && req->conn->srv->flush) {
		(*req->conn->srv->flush)(creq);
		np_req_unref(creq);
	}

	return ret;
}

static Npfcall *
np_walk(Npreq *req, Npfcall *tc)
{
	int i;
	Npconn *conn;
	Npfid *fid, *newfid;
	Npfcall *rc;
	Npqid wqids[MAXWELEM];

	rc = NULL;
	conn = req->conn;
	newfid = NULL;
	fid = np_fid_find(conn, tc->fid);
	if (!fid) {
		np_werror(Eunknownfid, EIO);
		goto done;
	} else 
		np_fid_incref(fid);

	req->fid = fid;
	if (!fid->type&Qtdir) {
		np_werror(Enotdir, ENOTDIR);
		goto done;
	}

	if (fid->omode != (u16) ~0) {
		np_werror(Ebadusefid, EIO);
		goto done;
	}

	if (tc->nwname > MAXWELEM) {
		np_werror(Etoomanywnames, EIO);
		goto done;
	}

	if (tc->fid != tc->newfid) {
		newfid = np_fid_find(conn, tc->newfid);
		if (newfid) {
			np_werror(Einuse, EIO);
			goto done;
		}
		newfid = np_fid_create(conn, tc->newfid, NULL);
		if (!newfid) {
			np_werror(Enomem, ENOMEM);
			goto done;
		}

		if (!(*conn->srv->clone)(fid, newfid))
			goto done;

		newfid->user = fid->user;
		newfid->type = fid->type;
	} else
		newfid = fid;

	np_fid_incref(newfid);
	for(i = 0; i < tc->nwname;) {
		if (!(*conn->srv->walk)(newfid, &tc->wnames[i], &wqids[i]))
			break;

		newfid->type = wqids[i].type;
		i++;

		if (i<(tc->nwname) && !newfid->type&Qtdir)
			break;
	}

	if (i==0 && tc->nwname!=0)
		goto done;

	np_werror(NULL, 0);
	if (tc->fid != tc->newfid)
		np_fid_incref(newfid);
	rc = np_create_rwalk(i, wqids);

done:
	np_fid_decref(newfid);
	return rc;
}

static Npfcall *
np_open(Npreq *req, Npfcall *tc)
{
	Npconn *conn;
	Npfid *fid;
	Npfcall *rc;

	rc = NULL;
	conn = req->conn;
	fid = np_fid_find(conn, tc->fid);
	if (!fid) {
		np_werror(Eunknownfid, EIO);
		goto done;
	} else 
		np_fid_incref(fid);

	req->fid = fid;
	if (fid->omode != (u16)~0) {
		np_werror(Ebadusefid, EIO);
		goto done;
	}

	if (fid->type&Qtdir && tc->mode != Oread) {
		np_werror(Eperm, EPERM);
		goto done;
	}

	rc = (*conn->srv->open)(fid, tc->mode);
	fid->omode = tc->mode;
done:
//	np_fid_decref(fid);
	return rc;
}

static Npfcall *
np_create(Npreq *req, Npfcall *tc)
{
	Npconn *conn;
	Npfid *fid;
	Npfcall *rc;

	rc = NULL;
	conn = req->conn;
	fid = np_fid_find(conn, tc->fid);
	if (!fid) {
		np_werror(Eunknownfid, EIO);
		goto done;
	} else 
		np_fid_incref(fid);

	req->fid = fid;
	if (fid->omode != (u16)~0) {
		np_werror(Ebadusefid, EIO);
		goto done;
	}

	if (!fid->type&Qtdir) {
		np_werror(Enotdir, ENOTDIR);
		goto done;
	}

	if (tc->perm&Dmdir && tc->mode!=Oread) {
		np_werror(Eperm, EPERM);
		goto done;
	}

	if (tc->perm&(Dmnamedpipe|Dmsymlink|Dmlink|Dmdevice|Dmsocket)
	&& !fid->conn->dotu) {
		np_werror(Eperm, EPERM);
		goto done;
	}

	rc = (*conn->srv->create)(fid, &tc->name, tc->perm, tc->mode, 
		&tc->extension);
	if (rc && rc->type == Rcreate) {
		fid->omode = tc->mode;
		fid->type = rc->qid.type;
	}

done:
//	np_fid_decref(fid);
	return rc;
}

static Npfcall *
np_read(Npreq *req, Npfcall *tc)
{
	Npconn *conn;
	Npfid *fid;
	Npfcall *rc;

	rc = NULL;
	conn = req->conn;
	fid = np_fid_find(conn, tc->fid);
	if (!fid) {
		np_werror(Eunknownfid, EIO);
		goto error;
	} else 
		np_fid_incref(fid);

	req->fid = fid;
	if (tc->count+IOHDRSZ > conn->msize) {
		np_werror(Etoolarge, EIO);
		goto error;
	}

	if (fid->type&Qtauth) {
		if (conn->srv->auth)
			rc = conn->srv->auth->read(fid, tc->offset, tc->count);
		else
			np_werror(Ebadusefid, EIO);

		goto error;
	}

	if (fid->omode==(u16)~0 || (fid->omode&3)==Owrite) {
		np_werror(Ebadusefid, EIO);
		goto error;
	}

	if (fid->type&Qtdir && tc->offset != fid->diroffset) {
		np_werror(Ebadoffset, EIO);
		goto error;
	}
		
	rc = (*conn->srv->read)(fid, tc->offset, tc->count, req);

/*
	if (rc && rc->id==Rread && fid->type&Qtdir) {
		fid->diroffset = tc->offset + rc->count;
	}
*/
//	if (rc || np_haserror())
//		np_fid_decref(fid);

	return rc;

error:
//	np_fid_decref(fid);
	return NULL;
}

static Npfcall *
np_write(Npreq *req, Npfcall *tc)
{
	Npconn *conn;
	Npfid *fid;
	Npfcall *rc;

	rc = NULL;
	conn = req->conn;
	fid = np_fid_find(conn, tc->fid);
	if (!fid) {
		np_werror(Eunknownfid, EIO);
		goto error;
	} else 
		np_fid_incref(fid);

	req->fid = fid;
	if (fid->type&Qtauth) {
		if (conn->srv->auth) {
			rc = conn->srv->auth->write(fid, tc->offset, 
				tc->count, tc->data);
//			np_fid_decref(fid);
			return rc;
		} else {
			np_werror(Ebadusefid, EIO);
			goto error;
		}
	}

	if (fid->omode==(u16)~0 || fid->type&Qtdir || (fid->omode&3)==Oread) {
		np_werror(Ebadusefid, EIO);
		goto error;
	}

	if (tc->count+IOHDRSZ > conn->msize) {
		np_werror(Etoolarge, EIO);
		goto error;
	}

	rc = (*conn->srv->write)(fid, tc->offset, tc->count, tc->data, req);
//	if (rc || np_haserror())
//		np_fid_decref(fid);

	return rc;

error:
//	np_fid_decref(fid);
	return NULL;
}

static Npfcall *
np_clunk(Npreq *req, Npfcall *tc)
{
	Npconn *conn;
	Npfid *fid;
	Npfcall *rc;

	rc = NULL;
	conn = req->conn;
	fid = np_fid_find(conn, tc->fid);
	if (!fid) {
		np_werror(Eunknownfid, EIO);
		goto done;
	} else 
		np_fid_incref(fid);

	req->fid = fid;
	if (fid->type&Qtauth) {
		if (conn->srv->auth)
			rc = conn->srv->auth->clunk(fid);
		else
			np_werror(Ebadusefid, EIO);

		goto done;
	}

	if (fid->omode!=(u16)~0 && fid->omode==Orclose) {
		rc = (*conn->srv->remove)(fid);
		if (rc->type == Rerror)
			goto done;
		free(rc);
		rc = np_create_rclunk();
	} else
		rc = (*conn->srv->clunk)(fid);

	if (rc && rc->type == Rclunk)
		np_fid_decref(fid);

done:
	return rc;
}

static Npfcall *
np_remove(Npreq *req, Npfcall *tc)
{
	Npconn *conn;
	Npfid *fid;
	Npfcall *rc;

	rc = NULL;
	conn = req->conn;
	fid = np_fid_find(conn, tc->fid);
	if (!fid) {
		np_werror(Eunknownfid, EIO);
		goto done;
	} else 
		np_fid_incref(fid);

	req->fid = fid;
	rc = (*conn->srv->remove)(fid);
	if (rc && rc->type == Rremove)
		np_fid_decref(fid);

done:
	return rc;
}

static Npfcall *
np_stat(Npreq *req, Npfcall *tc)
{
	Npconn *conn;
	Npfid *fid;
	Npfcall *rc;

	rc = NULL;
	conn = req->conn;
	fid = np_fid_find(conn, tc->fid);
	if (!fid) {
		np_werror(Eunknownfid, EIO);
		goto done;
	} else 
		np_fid_incref(fid);

	req->fid = fid;
	rc = (*conn->srv->stat)(fid);

done:
//	np_fid_decref(fid);
	return rc;
}

static Npfcall *
np_wstat(Npreq *req, Npfcall *tc)
{
	Npconn *conn;
	Npfid *fid;
	Npfcall *rc;
	Npstat *stat;

	rc = NULL;
	conn = req->conn;
	stat = &tc->stat;
	fid = np_fid_find(conn, tc->fid);
	if (!fid) {
		np_werror(Eunknownfid, EIO);
		goto done;
	} else 
		np_fid_incref(fid);

	req->fid = fid;
	if (stat->type != (u16)~0 || stat->dev != (u32)~0
	|| stat->qid.version != (u32)~0
	|| stat->qid.path != (u64)~0 ) {
                np_werror(Eperm, EPERM);
                goto done;
        }

	if ((fid->type&Qtdir && !stat->mode&Dmdir)
	|| (!fid->type&Qtdir&&stat->mode&Dmdir)) {
		np_werror(Edirchange, EPERM);
		goto done;
	}

	rc = (*conn->srv->wstat)(fid, &tc->stat);
done:
//	np_fid_decref(fid);
	return rc;
}

typedef Npfcall* (*np_fcall)(Npreq *, Npfcall *);
static np_fcall np_fcalls[] = {
	np_version,
	np_auth,
	np_attach,
	NULL,
	np_flush,
	np_walk,
	np_open,
	np_create,
	np_read,
	np_write,
	np_clunk,
	np_remove,
	np_stat,
	np_wstat,
};

static Npfcall*
np_process_request(Npreq *req)
{
	Npconn *conn;
	Npfcall *tc, *rc;
	np_fcall f;
	char *ename;
	int ecode;

	conn = req->conn;
	rc = NULL;
	tc = req->tcall;

	f = NULL;
	if (tc->type<Tfirst && tc->type>Rlast)
		np_werror("unknown message type", ENOSYS);
	else
		f = np_fcalls[(tc->type-Tfirst)/2];

	np_werror(NULL, 0);
	if (f)
		rc = (*f)(req, tc);
	else
		np_werror("unsupported message", ENOSYS);

	np_rerror(&ename, &ecode);
	if (ename != NULL) {
		if (rc)
			free(rc);
		rc = np_create_rerror(ename, ecode, conn->dotu);
	}

	return rc;
}

static void *
np_wthread_proc(void *a)
{
	Npwthread *wt;
	Npsrv *srv;
	Npreq *req;
	Npfcall *rc;

	wt = a;
	srv = wt->srv;
	req = NULL;

	pthread_mutex_lock(&srv->lock);
	while (!wt->shutdown) {
		req = srv->reqs_first;
		if (!req) {
			pthread_cond_wait(&srv->reqcond, &srv->lock);
			continue;
		}

		np_srv_remove_req(srv, req);
		np_srv_add_workreq(srv, req);
		pthread_mutex_unlock(&srv->lock);

		req->wthread = wt;
		rc = np_process_request(req);
		if (rc)
			np_respond(req, rc);

		pthread_mutex_lock(&srv->lock);
	}

	return NULL;
}

void
np_respond(Npreq *req, Npfcall *rc)
{
	Npsrv *srv;
	Npreq *freq;

	srv = req->conn->srv;
	pthread_mutex_lock(&req->lock);
	if (req->responded) {
		free(rc);
		pthread_mutex_unlock(&req->lock);
		np_req_unref(req);
		return;
	}
	req->responded = 1;
	pthread_mutex_unlock(&req->lock);

	pthread_mutex_lock(&srv->lock);
	np_srv_remove_workreq(srv, req);
	for(freq = req->flushreq; freq != NULL; freq = freq->flushreq)
		np_srv_remove_workreq(srv, freq);
	pthread_mutex_unlock(&srv->lock);

	pthread_mutex_lock(&req->lock);
	req->rcall = rc;
	if (req->rcall) {
		if (req->rcall->type==Rread && req->fid->type&Qtdir)
			req->fid->diroffset = req->tcall->offset + req->rcall->count;

		np_set_tag(req->rcall, req->tag);
		if (req->fid != NULL) {
			np_fid_decref(req->fid);
			req->fid = NULL;
		}
		np_conn_respond(req);		
	}

	for(freq = req->flushreq; freq != NULL; freq = freq->flushreq) {
		pthread_mutex_lock(&freq->lock);
		freq->rcall = np_create_rflush();
		np_set_tag(freq->rcall, freq->tag);
		np_conn_respond(freq);
		pthread_mutex_unlock(&freq->lock);
		np_req_unref(freq);
	}
	pthread_mutex_unlock(&req->lock);
	np_req_unref(req);
}

void
np_respond_error(Npreq *req, char *ename, int ecode)
{
	Npfcall *rc;

	rc = np_create_rerror(ename, ecode, req->conn->dotu);
	np_respond(req, rc);
}

static Npfcall*
np_default_version(Npconn *conn, u32 msize, Npstr *version) 
{
	int dotu;
	char *ver;
	Npfcall *rc;

	if (msize > conn->srv->msize)
		msize = conn->srv->msize;

	dotu = 0;
	if (np_strcmp(version, "9P2000.u")==0 && conn->srv->dotu) {
		ver = "9P2000.u";
		dotu = 1;
	} else if (np_strncmp(version, "9P2000", 6) == 0)
		ver = "9P2000";
	else
		ver = NULL;

	if (msize < IOHDRSZ)
		np_werror("msize too small", EIO);
	else if (ver) {
		np_conn_reset(conn, msize, dotu);
		rc = np_create_rversion(msize, ver);
	} else
		np_werror("unsupported 9P version", EIO);

	return rc;
}

static Npfcall*
np_default_attach(Npfid *fid, Npfid *afid, Npstr *uname, Npstr *aname)
{
	np_werror(Enotimpl, EIO);
	return NULL;
}

static void
np_default_flush(Npreq *req)
{
}

static int
np_default_clone(Npfid *fid, Npfid *newfid)
{
	return 0;
}

static int
np_default_walk(Npfid *fid, Npstr* wname, Npqid *wqid)
{
	np_werror(Enotimpl, ENOSYS);
	return 0;
}

static Npfcall*
np_default_open(Npfid *fid, u8 perm)
{
	np_werror(Enotimpl, ENOSYS);
	return NULL;
}

static Npfcall*
np_default_create(Npfid *fid, Npstr *name, u32 mode, u8 perm, Npstr *extension)
{
	np_werror(Enotimpl, ENOSYS);
	return NULL;
}

static Npfcall*
np_default_read(Npfid *fid, u64 offset, u32 count, Npreq *req)
{
	np_werror(Enotimpl, ENOSYS);
	return NULL;
}

static Npfcall*
np_default_write(Npfid *fid, u64 offset, u32 count, u8 *data, Npreq *req)
{
	np_werror(Enotimpl, ENOSYS);
	return NULL;
}

static Npfcall*
np_default_clunk(Npfid *fid)
{
	np_werror(Enotimpl, ENOSYS);
	return NULL;
}

static Npfcall*
np_default_remove(Npfid *fid)
{
	np_werror(Enotimpl, ENOSYS);
	return NULL;
}

static Npfcall*
np_default_stat(Npfid *fid)
{
	np_werror(Enotimpl, ENOSYS);
	return NULL;
}

static Npfcall*
np_default_wstat(Npfid *fid, Npstat *stat)
{
	np_werror(Enotimpl, ENOSYS);
	return NULL;
}

Npreq *np_req_alloc(Npconn *conn, Npfcall *tc) {
	Npreq *req;

	req = NULL;
	pthread_mutex_lock(&reqpool.lock);
	if (reqpool.reqlist) {
		req = reqpool.reqlist;
		reqpool.reqlist = req->next;
		reqpool.reqnum--;
	}
	pthread_mutex_unlock(&reqpool.lock);
	
	if (!req)
		req = malloc(sizeof(*req));

	np_conn_incref(conn);
	pthread_mutex_init(&req->lock, NULL);
	req->refcount = 1;
	req->conn = conn;
	req->tag = tc->tag;
	req->tcall = tc;
	req->rcall = NULL;
	req->responded = 0;
	req->flushreq = NULL;
	req->next = NULL;
	req->prev = NULL;
	req->wthread = NULL;
	req->fid = NULL;

	return req;
}

Npreq *
np_req_ref(Npreq *req)
{
	pthread_mutex_lock(&req->lock);
	req->refcount++;
	pthread_mutex_unlock(&req->lock);
	return req;
}

void
np_req_unref(Npreq *req)
{
	pthread_mutex_lock(&req->lock);
	assert(req->refcount > 0);
	req->refcount--;
	if (req->refcount) {
		pthread_mutex_unlock(&req->lock);
		return;
	}
	pthread_mutex_unlock(&req->lock);

	if (req->conn)
		np_conn_decref(req->conn);

	pthread_mutex_lock(&reqpool.lock);
	if (reqpool.reqnum < 64) {
		req->next = reqpool.reqlist;
		reqpool.reqlist = req;
		reqpool.reqnum++;
		req = NULL;
	}

	pthread_mutex_unlock(&reqpool.lock);
	if (req)
		free(req);
}

