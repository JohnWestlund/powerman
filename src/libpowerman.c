/*****************************************************************************\
 *  $Id:$
 *****************************************************************************
 *  Copyright (C) 2008 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Jim Garlick <garlick@llnl.gov>
 *  UCRL-CODE-2002-008.
 *  
 *  This file is part of PowerMan, a remote power management program.
 *  For details, see <http://www.llnl.gov/linux/powerman/>.
 *  
 *  PowerMan is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *  
 *  PowerMan is distributed in the hope that it will be useful, but WITHOUT 
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or 
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License 
 *  for more details.
 *  
 *  You should have received a copy of the GNU General Public License along
 *  with PowerMan; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
\*****************************************************************************/

/*
 * libpowerman.c - simple powerman client library
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include "powerman.h"
#include "client_proto.h"
#include "libpowerman.h"

#define PMH_MAGIC 0x44445555
struct pm_handle_struct {
    int         pmh_magic;
    int         pmh_fd;
};


typedef void (*list_free_t) (void *x);
struct list_struct {
    char *              data;
    struct list_struct *next;
    list_free_t         freefun;
};

#define PMI_MAGIC 0x41a452b5
struct pm_node_iterator_struct {
    int                 pmi_magic;
    struct list_struct *pmi_nodes;
    struct list_struct *pmi_pos;
};

static pm_err_t _list_add(struct list_struct **head, char *s, 
                                list_free_t freefun);
static void     _list_free(struct list_struct **head);
static int      _list_search(struct list_struct *head, char *s);

static int      _strncmpend(char *s1, char *s2, int len);
static char *   _strndup(char *s, int len);
static pm_err_t _connect_to_server_tcp(pm_handle_t pmh, char *host, char *port);
static pm_err_t _parse_response(char *buf, int len, 
                                struct list_struct **respp);
static pm_err_t _server_recv_response(pm_handle_t pmh, 
                                struct list_struct **respp);
static pm_err_t _server_send_command(pm_handle_t pmh, char *cmd, char *arg);
static pm_err_t _server_command(pm_handle_t pmh, char *cmd, char *arg, 
                                struct list_struct **respp);


/* Add [s] to the list referenced by [head], registering [freefun] to 
 * be called on [s] when the list is freed.
 */
static pm_err_t
_list_add(struct list_struct **head, char *s, list_free_t freefun)
{
    struct list_struct *new;

    if (!(new = malloc(sizeof(struct list_struct))))
        return PM_ENOMEM;
    new->data = s;
    new->next = *head;
    new->freefun = freefun;
    *head = new;
    return PM_ESUCCESS;
}

/* Free the list referred to by [head].
 */
static void
_list_free(struct list_struct **head)
{
    struct list_struct *lp, *tmp;

    for (lp = *head; lp != NULL; lp = tmp->next) {
        tmp = lp;
        if (lp->freefun)
            lp->freefun(lp->data); 
        free(lp);
    }
    *head = NULL;
}

/* Scan the list referenced by [head] looking for an element containing
 * exactly the string [s], returning true if found.
 */
static int
_list_search(struct list_struct *head, char *s)
{
    struct list_struct *lp;

    for (lp = head; lp != NULL; lp = lp->next)
        if (strcmp(lp->data, s) == 0)
            return 1;
    return 0;
}

/* Test if [s2] is a terminating substring of [s1].
 */
static int
_strncmpend(char *s1, char *s2, int len)
{
    return strncmp(s1 + len - strlen(s2), s2, strlen(s2));
}

/* Duplicate string [s] of length [len].
 * Result will be NULL terminated.  [s] doesn't have to be.
 */
static char *
_strndup(char *s, int len)
{
    char *c;

    if ((c = (char *)malloc(len + 1))) {
        memcpy(c, s, len);
        c[len] = '\0';
    }
    return c;     
}

/* Establish connection to powerman server on [host] and [port].
 * Connection state is returned in the handle.
 */
static pm_err_t 
_connect_to_server_tcp(pm_handle_t pmh, char *host, char *port)
{
    struct addrinfo hints, *res, *r;
    pm_err_t err = PM_ECONNECT;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &res) != 0 || res == NULL)
        return PM_ENOADDR;
    for (r = res; r != NULL; r = r->ai_next) {
        if ((pmh->pmh_fd = socket(r->ai_family, r->ai_socktype, 0)) < 0)
            continue;
        if (connect(pmh->pmh_fd, r->ai_addr, r->ai_addrlen) < 0) {
            close(pmh->pmh_fd);
            continue;
        }
        err = PM_ESUCCESS;
        break;
    }
    freeaddrinfo(res);
    return err;
}

/* Parse a server response stored in [buf] of length [len] into
 * an array of lines stored in [respp] which caller must free.
 */
static pm_err_t
_parse_response(char *buf, int len, struct list_struct **respp)
{
    int i, l = strlen(CP_EOL);
    char *p, *cpy;
    struct list_struct *resp = NULL;
    pm_err_t err = PM_ESUCCESS;

    p = buf;
    for (i = 0; i < len - l; i++) {
        if (strncmp(&buf[i], CP_EOL, l) != 0)
            continue;
        if ((cpy = _strndup(p, &buf[i] - p + l)) == NULL) {
            err = PM_ENOMEM;
            break;
        }
        if ((err = _list_add(&resp, cpy, (list_free_t)free)) != PM_ESUCCESS)
            break;
        p = &buf[i + l];
    }
    if (err == PM_ESUCCESS && respp != NULL)
        *respp = resp;
    else
        _list_free(&resp);
    return err;
}

/* Scan response from server for return code.
 */
static pm_err_t
_server_retcode(struct list_struct *resp)
{
    int code;
    pm_err_t err = PM_EPARSE;
   
    /* N.B. there will only be one 1xx or 2xx code in a response */ 
    while (resp != NULL) {
        if (sscanf(resp->data, "%d ", &code) == 1) {
            switch (code) {
                case 1:     /* hello */
                case 101:   /* goodbye */
                case 102:   /* command completed successfully */
                case 103:   /* query complete */
                case 104:   /* telemetry on|off */
                case 105:   /* hostrange expansion on|off */
                    err = PM_ESUCCESS;
                    break; 
                case 201:   /* unknown command */
                case 202:   /* parse error */
                case 203:   /* command too long */
                case 204:   /* internal powermand error... */
                case 205:   /* hostlist error */
                case 208:   /* command in progress */
                    err = PM_ESERVER;
                    break;
                case 209:   /* no such nodes */
                    err = PM_EBADNODE;
                    break;
                case 210:   /* command completed with errors */
                case 211:   /* query completed with errors */
                    err = PM_EPARTIAL;
                    break;
                case 213:   /* command cannot be handled by PDU */
                    err = PM_EUNIMPL;
                    break;
                default:    /* 3xx (ignore) */
                    break;
            }
        }
        resp = resp->next;
    }
    return err;
}

/* Read response from server handle [pmh] and store it in
 * [resp], an array of lines.  Caller must free [resp].
 */
static pm_err_t
_server_recv_response(pm_handle_t pmh, struct list_struct **respp)
{
    int buflen = 0, count = 0, n;
    char *buf = NULL; 
    pm_err_t err = PM_ESUCCESS;
    struct list_struct *resp;

    do {
        if (buflen - count == 0) {
            buflen += CP_LINEMAX;
            buf = buf ? realloc(buf, buflen) : malloc(buflen);
            if (buf == NULL) {
                err = PM_ENOMEM;
                break;
            }
        } 
        n = read(pmh->pmh_fd, buf + count, buflen - count);
        if (n == 0) {
            err = PM_ESERVEREOF;
            break;
        }
        if (n < 0) {
            err = PM_ERRNOVALID;
            break;
        }
        count += n;
    } while (_strncmpend(buf, CP_PROMPT, count) != 0);

    if (err == PM_ESUCCESS) {
        err = _parse_response(buf, count, &resp);
        if (err == PM_ESUCCESS) {
            err = _server_retcode(resp);
            if (err == PM_ESUCCESS && respp != NULL)
                *respp = resp;
            else
                _list_free(&resp);
        }
    }
    if (buf != NULL)
        free(buf);
    return err;
}

/* Send command [cmd] with argument [arg] to server handle [pmh].
 * [cmd] is treated as a printf format string with [arg] as the
 * first printf argument (can be NULL).
 */
static pm_err_t
_server_send_command(pm_handle_t pmh, char *cmd, char *arg)
{
    char buf[CP_LINEMAX];    
    int count, len, n;
    pm_err_t err = PM_ESUCCESS;

    snprintf(buf, sizeof(buf), cmd, arg);
    snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), CP_EOL);
    count = 0;
    len = strlen(buf);
    while (count < len) {
        n = write(pmh->pmh_fd, buf + count, len - count);
        if (n < 0) {
            err = PM_ERRNOVALID;
            break;
        }
        count += n;
    }
    return err;
}

/* Send command [cmd] with argument [arg] to server handle [pmh].
 * If [respp] is non-NULL, return list of response lines which
 * the caller must free.
 */
static pm_err_t
_server_command(pm_handle_t pmh, char *cmd, char *arg, struct list_struct **respp)
{
    pm_err_t err;

    if ((err = _server_send_command(pmh, cmd, arg)) != PM_ESUCCESS)
        return err;
    if ((err = _server_recv_response(pmh, respp)) != PM_ESUCCESS)
        return err;
    return PM_ESUCCESS;
}

static pm_err_t
_parse_hostport(char *str, char **hp, char **pp)
{
    char *h = NULL, *p = NULL;

    if (str) {
        if (!(h = strdup(str)))
            goto nomem;
        if ((p = strchr(h, ':'))) {
            *p = '\0';
            if (!(p = strdup(p + 1))) {
                goto nomem;
            }
        }
    }
    if (h == NULL && !(h = strdup(DFLT_HOSTNAME)))
        goto nomem;
    if (p == NULL && !(p = strdup(DFLT_PORT)))
        goto nomem;
    *hp = h;
    *pp = p;
    return PM_ESUCCESS;
nomem:
    if (h)
        free(h);
    if (p)
        free(p);
    return PM_ENOMEM;
}

pm_err_t
pm_connect(char *server, void *arg, pm_handle_t *pmhp)
{
    pm_handle_t pmh = NULL;
    char *host = NULL, *port = NULL;
    pm_err_t err;

    if (pmhp == NULL) {
        err = PM_EBADARG;
        goto error;
    }
    if ((err = _parse_hostport(server, &host, &port)) != PM_ESUCCESS)
        goto error;
    if ((pmh = (pm_handle_t)malloc(sizeof(struct pm_handle_struct))) == NULL) {
        err = PM_ENOMEM;
        goto error;
    }
    pmh->pmh_magic = PMH_MAGIC;

    if ((err = _connect_to_server_tcp(pmh, host, port)) != PM_ESUCCESS)
        goto error;

    /* eat version + prompt */
    if ((err = _server_recv_response(pmh, NULL)) != PM_ESUCCESS) {
        (void)close(pmh->pmh_fd);
        goto error;
    }
    /* tell server not to compress output */
    if ((err = _server_command(pmh, CP_EXPRANGE, NULL, NULL)) != PM_ESUCCESS) {
        (void)close(pmh->pmh_fd);
        goto error;
    }
    if (err == PM_ESUCCESS) {
        *pmhp = pmh;
        free(host);
        free(port);
        return PM_ESUCCESS;
    }

error:
    free(host);
    free(port);
    if (pmh)
        free(pmh);
    return err;
}

static pm_err_t
_node_iterator_create(pm_node_iterator_t *pmip)
{
    pm_node_iterator_t pmi; 

    if (!(pmi = malloc(sizeof(struct pm_node_iterator_struct))))
        return PM_ENOMEM;
    pmi->pmi_magic = PMI_MAGIC;
    pmi->pmi_pos = pmi->pmi_nodes = NULL;
    *pmip = pmi;
    return PM_ESUCCESS;
}

void
pm_node_iterator_destroy(pm_node_iterator_t pmi)
{
    _list_free(&pmi->pmi_nodes);
    pmi->pmi_pos = NULL;
    pmi->pmi_magic = 0;
    free(pmi);
}

pm_err_t
pm_node_iterator_create(pm_handle_t pmh, pm_node_iterator_t *pmip)
{
    pm_node_iterator_t pmi;
    struct list_struct *lp, *resp;
    char *cpy, *p, node[CP_LINEMAX];
    pm_err_t err;
    int code;

    if (pmh == NULL || pmh->pmh_magic != PMH_MAGIC)
        return PM_EBADHAND;
    if ((err = _node_iterator_create(&pmi)) != PM_ESUCCESS)
        return err;
    if ((err = _server_command(pmh, CP_NODES, NULL, &resp)) != PM_ESUCCESS) {
        pm_node_iterator_destroy(pmi);
        return err;
    }
    for (lp = resp; lp != NULL; lp = lp->next) {
        if (sscanf(lp->data, CP_INFO_XNODES, node) == 1) {
            if (!(cpy = strdup(node))) {
                err = PM_ENOMEM;
                break;
            }
            err = _list_add(&pmi->pmi_nodes, cpy, (list_free_t)free);
            if (err != PM_ESUCCESS)
                break;
        }
    }

    if (err == PM_ESUCCESS && pmip != NULL) {
        pm_node_iterator_reset(pmi);
        *pmip = pmi;
    } else
        pm_node_iterator_destroy(pmi);
    _list_free(&resp);
    return err;
}

char *
pm_node_next(pm_node_iterator_t pmi)
{
    struct list_struct *cur = pmi->pmi_pos;

    if (!cur)
        return NULL;
    pmi->pmi_pos = pmi->pmi_pos->next;

    return cur->data;
}

void
pm_node_iterator_reset(pm_node_iterator_t pmi)
{
    pmi->pmi_pos = pmi->pmi_nodes;
}


/* Disconnect from server handle [pmh] and free the handle.
 */
void 
pm_disconnect(pm_handle_t pmh)
{
    if (pmh != NULL && pmh->pmh_magic == PMH_MAGIC) {
        (void)_server_command(pmh, CP_QUIT, NULL, NULL); /* PM_ESERVEREOF */
        (void)close(pmh->pmh_fd);
        free(pmh);
    }
}

/* Query server [pmh] for the power status of [node], and store it
 * in [statep].
 */
pm_err_t
pm_node_status(pm_handle_t pmh, char *node, pm_node_state_t *statep)
{
    char offstr[CP_LINEMAX], onstr[CP_LINEMAX];
    pm_err_t err;
    struct list_struct *resp;
    pm_node_state_t state;

    if (pmh == NULL || pmh->pmh_magic != PMH_MAGIC)
        return PM_EBADHAND;
    if ((err = _server_command(pmh, CP_STATUS, node, &resp)) != PM_ESUCCESS)
        return err;

    snprintf(offstr, sizeof(offstr), CP_INFO_XSTATUS, node, "off");
    snprintf(onstr,  sizeof(onstr),  CP_INFO_XSTATUS, node, "on");
    if (_list_search(resp, offstr))
        state = PM_OFF;
    else if (_list_search(resp, onstr))
        state = PM_ON;
    else
        state = PM_UNKNOWN;
    _list_free(&resp);

    if (statep)
        *statep = state;
    return PM_ESUCCESS;
}

/* Tell server [pmh] to turn [node] on.
 */
pm_err_t
pm_node_on(pm_handle_t pmh, char *node)
{
    if (pmh == NULL || pmh->pmh_magic != PMH_MAGIC)
        return PM_EBADHAND;
    return _server_command(pmh, CP_ON, node, NULL);
}

/* Tell server [pmh] to turn [node] off.
 */
pm_err_t
pm_node_off(pm_handle_t pmh, char *node)
{
    if (pmh == NULL || pmh->pmh_magic != PMH_MAGIC)
        return PM_EBADHAND;
    return _server_command(pmh, CP_OFF, node, NULL);
}

/* Tell server [pmh] to cycle [node].
 */
pm_err_t
pm_node_cycle(pm_handle_t pmh, char *node)
{
    if (pmh == NULL || pmh->pmh_magic != PMH_MAGIC)
        return PM_EBADHAND;
    return _server_command(pmh, CP_CYCLE, node, NULL);
}

/* Convert error code to human readable string.
 */
char *
pm_strerror(pm_err_t err, char *str, int len)
{
    switch (err) {
        case PM_ESUCCESS:
            strncpy(str, "success", len);
            break;
        case PM_ERRNOVALID:
            strncpy(str, strerror(errno), len);
            break;
        case PM_ENOADDR:
            strncpy(str, "failed to get address info for host:port", len);
            break;
        case PM_ECONNECT:
            strncpy(str, "connect failed", len);
            break;
        case PM_ENOMEM:
            strncpy(str, "out of memory", len);
            break;
        case PM_EBADHAND:
            strncpy(str, "bad server handle", len);
            break;
        case PM_EBADNODE:
            strncpy(str, "unknown node name", len);
            break;
        case PM_EBADARG:
            strncpy(str, "bad argument", len);
            break;
        case PM_ETIMEOUT:
            strncpy(str, "client timed out", len);
            break;
        case PM_ESERVEREOF:
            strncpy(str, "received unexpected EOF from server", len);
            break;
        case PM_ESERVER:
            strncpy(str, "server error", len);
            break;
        case PM_EPARSE:
            strncpy(str, "received unexpected response from server", len);
            break;
        case PM_EPARTIAL:
            strncpy(str, "command completed with errors", len);
            break;
        case PM_EUNIMPL:
            strncpy(str, "command is not implemented by device", len);
            break;
    }
    return str;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
