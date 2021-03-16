/*
 * @f ccn-iribu-ext-http.c
 * @b CCN lite extension: web server to display the relay's status
 *
 * Copyright (C) 2013, Christian Tschudin, University of Basel
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * File history:
 * 2013-04-11 created
 */

void null_func(void);

#ifdef USE_HTTP_STATUS


#include "ccn-iribu-http-status.h"
#include "ccn-iribu-os-time.h"

// ----------------------------------------------------------------------

struct ccn_iribu_http_s*
ccn_iribu_http_new(struct ccn_iribu_relay_s *ccnl, int serverport)
{
    int s, i = 1;
    struct sockaddr_in me;
    struct ccn_iribu_http_s *http;
    (void) ccnl;

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!s) {
        DEBUGMSG(INFO, "could not create socket for http server\n");
        return NULL;
    }
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &i, sizeof(i));

    me.sin_family = AF_INET;
    me.sin_port = htons(serverport);
    me.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr*) &me, sizeof(me)) < 0) {
        close(s);
        DEBUGMSG(INFO, "could not bind socket for http server\n");
        return NULL;
    }
    listen(s, 2);

    http = (struct ccn_iribu_http_s*) ccn_iribu_calloc(1, sizeof(*http));
    if (!http) {
        close(s);
        return NULL;
    }
    http->server = s;

    DEBUGMSG(INFO, "HTTP status server listening at TCP port %d\n", serverport);

    return http;
}


struct ccn_iribu_http_s*
ccn_iribu_http_cleanup(struct ccn_iribu_http_s *http)
{
    if (!http)
        return NULL;
    if (http->server)
        close(http->server);
    if (http->client)
        close(http->client);
    ccn_iribu_free(http);
    return NULL;
}


int
ccn_iribu_http_anteselect(struct ccn_iribu_relay_s *ccnl, struct ccn_iribu_http_s *http,
                     fd_set *readfs, fd_set *writefs, int *maxfd)
{
    (void) ccnl;
    if (!http)
        return -1;
    if (!http->client) {
        FD_SET(http->server, readfs);
        if (*maxfd <= http->server)
            *maxfd = http->server + 1;
    } else {
        if ((unsigned long)http->inlen < sizeof(http->in))
            FD_SET(http->client, readfs);
        if (http->outlen > 0)
            FD_SET(http->client, writefs);
        if (*maxfd <= http->client)
            *maxfd = http->client + 1;
    }
    return 0;
}


int
ccn_iribu_http_postselect(struct ccn_iribu_relay_s *ccnl, struct ccn_iribu_http_s *http,
                     fd_set *readfs, fd_set *writefs)
{
    if (!http)
        return -1;
    // accept only one client at the time:
    if (!http->client && FD_ISSET(http->server, readfs)) {
        struct sockaddr_in peer;
        socklen_t len = sizeof(peer);
        http->client = accept(http->server, (struct sockaddr*) &peer, &len);
        if (http->client < 0)
            http->client = 0;
        else {
            DEBUGMSG(INFO, "accepted web server client %s\n",
                     ccn_iribu_addr2ascii((sockunion*)&peer));
            http->inlen = http->outlen = http->inoffs = http->outoffs = 0;
        }
    }
    if (http->client && FD_ISSET(http->client, readfs)) {
        int len = sizeof(http->in) - http->inlen - 1;
        len = recv(http->client, http->in + http->inlen, len, 0);
        if (len == 0) {
            DEBUGMSG(INFO, "web client went away\n");
            close(http->client);
            http->client = 0;
        } else if (len > 0) {
            http->in[len] = 0;
            ccn_iribu_http_status(ccnl, http);
        }
    }
    if (http->client && FD_ISSET(http->client, writefs) && http->out) {
        int len = send(http->client, http->out + http->outoffs,
                       http->outlen, 0);
        if (len > 0) {
            http->outlen -= len;
            http->outoffs += len;
            if (http->outlen == 0) {
                close(http->client);
                http->client = 0;
            }
        }
    }
    return 0;
}

int
ccn_iribu_cmpfaceid(const void *a, const void *b)
{
    int aa = (*(struct ccn_iribu_face_s**)a)->faceid;
    int bb = (*(struct ccn_iribu_face_s**)b)->faceid;
    return  aa - bb;
}

int
ccn_iribu_cmpfib(const void *a, const void *b)
{
    struct ccn_iribu_prefix_s *p1 = (*(struct ccn_iribu_forward_s**)a)->prefix;
    struct ccn_iribu_prefix_s *p2 = (*(struct ccn_iribu_forward_s**)b)->prefix;
    int r;
    size_t len;
    uint32_t i; //TODO: Is uint32_t correct here?
    for (i = 0; ; i++) {
        if (p1->compcnt <= i) {
            return p2->compcnt <= i ? 0 : -1;
        }
        if (p2->compcnt <= i) {
            return 1;
        }
        len = p1->complen[i];
        if (len > p2->complen[i]) {
            len = p2->complen[i];
        }
        r = memcmp(p1->comp[i], p2->comp[i], len);
        if (r) {
            return r;
        }
        if (p1->complen[i] > len) {
            return 1;
        }
        if (p2->complen[i] > len) {
            return -1;
        }
    }
    return 0;
}

int
ccn_iribu_http_status(struct ccn_iribu_relay_s *ccnl, struct ccn_iribu_http_s *http)
{
    static char txt[64000];
    char *hdr =
        "HTTP/1.1 200 OK\n\r"
        "Content-Type: text/html; charset=utf-8\n\r"
        "Connection: close\n\r\n\r", *cp;
    size_t len = strlen(hdr);
    int i, j, cnt;
    time_t t;
    //struct utsname uts;
    struct ccn_iribu_face_s *f;
    struct ccn_iribu_forward_s *fwd;
    struct ccn_iribu_interest_s *ipt;
    struct ccn_iribu_buf_s *bpt;
    char s[CCN_IRIBU_MAX_PREFIX_SIZE];

    strcpy(txt, hdr);
    len += snprintf(txt+len, sizeof(txt) - len,
                   "<html><head><title>ccn-lite-relay status</title>\n"
                   "<style type=\"text/css\">\n"
                   "body {font-family: sans-serif;}\n"
                   "</style>\n"
                   "</head><body>\n");
    len += snprintf(txt+len, sizeof(txt) - len, "\n<table borders=0>\n<tr><td>"
                   "<a href=\"\">[refresh]</a>&nbsp;&nbsp;<td>"
                   "ccn-lite-relay Status Page &nbsp;&nbsp;");
    //uname(&uts);
    //len += sprintf(txt+len, "node <strong>%s (%d)</strong>\n",
    //               uts.nodename, getpid());
    t = time(NULL);
    cp = ctime(&t);
    cp[strlen(cp)-1] = 0;
    len += snprintf(txt+len, sizeof(txt) - len, "<tr><td><td><font size=-1>%s &nbsp;&nbsp;", cp);
    cp = ctime(&ccn-iribu->startup_time);
    cp[strlen(cp)-1] = 0;
    len += snprintf(txt+len, sizeof(txt) - len, " (started %s)</font>\n</table>\n", cp);

    len += snprintf(txt+len, sizeof(txt) - len, "\n<p><table borders=0 width=100%% bgcolor=#e0e0ff>"
                   "<tr><td><em>Forwarding table</em></table><ul>\n");
    for (fwd = ccn-iribu->fib, cnt = 0; fwd; fwd = fwd->next, cnt++);
    if (cnt > 0) {
        struct ccn_iribu_forward_s **fwda;
        fwda = (struct ccn_iribu_forward_s**) ccn_iribu_malloc(cnt * sizeof(fwd));
        for (fwd = ccn-iribu->fib, i = 0; fwd; fwd = fwd->next, i++)
            fwda[i] = fwd;
        qsort(fwda, cnt, sizeof(fwd), ccn_iribu_cmpfib);
        for (i = 0; i < cnt; i++) {
            char fname[16];
#ifdef USE_ECHO
            if (fwda[i]->tap)
                strcpy(fname, "'echoserver'");
            else
#endif
            if(fwda[i]->face)
                snprintf(fname, sizeof(fname), "f%d", fwda[i]->face->faceid);
            else
                sprintf(fname, "?");
            len += snprintf(txt+len, sizeof(txt) - len,
                           "<li>via %4s: <font face=courier>%s</font>\n",
                           fname, ccn_iribu_prefix_to_str(fwda[i]->prefix,s,CCN_IRIBU_MAX_PREFIX_SIZE));
        }
        ccn_iribu_free(fwda);
    }
    len += snprintf(txt+len, sizeof(txt) - len, "</ul>\n");

    len += snprintf(txt+len, sizeof(txt) - len, "\n<p><table borders=0 width=100%% bgcolor=#e0e0ff>"
                   "<tr><td><em>Faces</em></table><ul>\n");
    for (f = ccn-iribu->faces, cnt = 0; f; f = f->next, cnt++);
    if (cnt > 0) {
        struct ccn_iribu_face_s **fa;
        fa = (struct ccn_iribu_face_s**) ccn_iribu_malloc(cnt * sizeof(f));
        for (f = ccn-iribu->faces, i = 0; f; f = f->next, i++)
            fa[i] = f;
        qsort(fa, cnt, sizeof(f), ccn_iribu_cmpfaceid);
        for (i = 0; i < cnt; i++) {
            len += snprintf(txt+len, sizeof(txt) - len,
                           "<li><strong>f%d</strong> (via i%d) &nbsp;"
                           "peer=<font face=courier>%s</font> &nbsp;ttl=",
                           fa[i]->faceid, fa[i]->ifndx,
                           ccn_iribu_addr2ascii(&(fa[i]->peer)));
            if (fa[i]->flags & CCN_IRIBU_FACE_FLAGS_STATIC)
                len += snprintf(txt+len, sizeof(txt) - len, "static");
            else
                len += snprintf(txt+len, sizeof(txt) - len, "%.1fsec",
                        fa[i]->last_used + CCN_IRIBU_FACE_TIMEOUT - CCN_IRIBU_NOW());
            for (j = 0, bpt = fa[i]->outq; bpt; bpt = bpt->next, j++);
            len += snprintf(txt+len, sizeof(txt) - len, " &nbsp;qlen=%d\n", j);
        }
        ccn_iribu_free(fa);
    }
    len += snprintf(txt+len, sizeof(txt) - len, "</ul>\n");

    len += snprintf(txt+len, sizeof(txt) - len, "\n<p><table borders=0 width=100%% bgcolor=#e0e0ff>"
                   "<tr><td><em>Interfaces</em></table><ul>\n");
    for (i = 0; i < ccn-iribu->ifcount; i++) {
#ifdef USE_STATS
        len += snprintf(txt+len, sizeof(txt) - len, "<li><strong>i%d</strong>&nbsp;&nbsp;"
                       "addr=<font face=courier>%s</font>&nbsp;&nbsp;"
                       "qlen=%zu/%d"
                       "&nbsp;&nbsp;rx=%u&nbsp;&nbsp;tx=%u"
                       "\n",
                       i, ccn_iribu_addr2ascii(&ccn-iribu->ifs[i].addr),
                       ccn-iribu->ifs[i].qlen, CCN_IRIBU_MAX_IF_QLEN,
                       ccn-iribu->ifs[i].rx_cnt, ccn-iribu->ifs[i].tx_cnt);
#else
        len += snprintf(txt+len, sizeof(txt) - len, "<li><strong>i%d</strong>&nbsp;&nbsp;"
                       "addr=<font face=courier>%s</font>&nbsp;&nbsp;"
                       "qlen=%d/%d"
                       "\n",
                       i, ccn_iribu_addr2ascii(&ccn-iribu->ifs[i].addr),
                       ccn-iribu->ifs[i].qlen, CCN_IRIBU_MAX_IF_QLEN);
#endif
    }
    len += snprintf(txt+len, sizeof(txt) - len, "</ul>\n");

    len += snprintf(txt+len, sizeof(txt) - len, "\n<p><table borders=0 width=100%% bgcolor=#e0e0ff>"
                   "<tr><td><em>Misc stats</em></table><ul>\n");
    for (cnt = 0, bpt = ccn-iribu->nonces; bpt; bpt = bpt->next, cnt++);
    len += snprintf(txt+len, sizeof(txt) - len, "<li>Nonces: %d\n", cnt);
    for (cnt = 0, ipt = ccn-iribu->pit; ipt; ipt = ipt->next, cnt++);
    len += snprintf(txt+len, sizeof(txt) - len, "<li>Pending interests: %d\n", cnt);
    len += snprintf(txt+len, sizeof(txt) - len, "<li>Content chunks: %d (max=%d)\n",
                   ccn-iribu->contentcnt, ccn-iribu->max_cache_entries);
    len += snprintf(txt+len, sizeof(txt) - len, "</ul>\n");

    len += snprintf(txt+len, sizeof(txt) - len, "\n<p><table borders=0 width=100%% bgcolor=#e0e0ff>"
                   "<tr><td><em>Config</em></table><table borders=0>\n");
    len += snprintf(txt+len, sizeof(txt) - len, "<tr><td>content.timeout:"
                   "<td align=right> %d<td>\n", CCN_IRIBU_CONTENT_TIMEOUT);
    len += snprintf(txt+len, sizeof(txt) - len, "<tr><td>face.timeout:"
                   "<td align=right> %d<td>\n", CCN_IRIBU_FACE_TIMEOUT);
    len += snprintf(txt+len, sizeof(txt) - len, "<tr><td>interest.maxretransmit:"
                   "<td align=right> %d<td>\n", CCN_IRIBU_MAX_INTEREST_RETRANSMIT);
    len += snprintf(txt+len, sizeof(txt) - len, "<tr><td>interest.timeout:"
                   "<td align=right> %d<td>\n", CCN_IRIBU_INTEREST_TIMEOUT);
    len += snprintf(txt+len, sizeof(txt) - len, "<tr><td>nonces.max:"
                   "<td align=right> %d<td>\n", CCN_IRIBU_MAX_NONCES);

    //len += sprintf(txt+len, "<tr><td>compile.featureset:<td><td> %s\n",
    //               compile_string);
    len += snprintf(txt+len, sizeof(txt) - len, "<tr><td>compile.time:"
                   "<td><td>%s %s\n", __DATE__, __TIME__);
    len += snprintf(txt+len, sizeof(txt) - len, "<tr><td>compile.ccn_iribu_core_version:"
                   "<td><td>%s\n", CCN_IRIBU_VERSION);
    len += snprintf(txt+len, sizeof(txt) - len, "</table>\n");

    len += snprintf(txt+len, sizeof(txt) - len, "\n<p><hr></body></html>\n");

    http->out = (unsigned char*) txt;
    http->outoffs = 0;
    http->outlen = len;

    return 0;
}

#endif // USE_HTTP_STATUS

// eof
