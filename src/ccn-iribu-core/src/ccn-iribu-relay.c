/*
 * @f ccn-iribu-relay.c
 * @b CCN lite (CCNL), core source file (internal data structures)
 *
 * Copyright (C) 2011-17, University of Basel
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
 * 2017-06-16 created
 */

#ifndef CCN_IRIBU_LINUXKERNEL
#include "ccn-iribu-core.h"
#include <stdio.h>
#include <inttypes.h>
#include <assert.h>
#else //CCN_IRIBU_LINUXKERNEL
#include "../include/ccn-iribu-core.h"
#endif //CCN_IRIBU_LINUXKERNEL

#ifdef CCN_IRIBU_RIOT
#include "ccn-iribu-riot.h"
#endif



/**
 * caching strategy removal function
 */
static ccn_iribu_cache_strategy_func _cs_remove_func = NULL;

/**
 * caching strategy decision function
 */
static ccn_iribu_cache_strategy_func _cs_decision_func = NULL;

struct ccn_iribu_face_s*
ccn_iribu_get_face_or_create(struct ccn_iribu_relay_s *ccn_iribu, int ifndx,
                        struct sockaddr *sa, size_t addrlen)
// sa==NULL means: local(=in memory) client, search for existing ifndx being -1
// sa!=NULL && ifndx==-1: search suitable interface for given sa_family
// sa!=NULL && ifndx!=-1: use this (incoming) interface for outgoing
{
    static int seqno;
    int i;
    struct ccn_iribu_face_s *f;

    DEBUGMSG_CORE(TRACE, "ccn_iribu_get_face_or_create src=%s\n",
             ccn_iribu_addr2ascii((sockunion*)sa));

    for (f = ccn_iribu->faces; f; f = f->next) {
        if (!sa) {
            if (f->ifndx == -1)
                return f;
            continue;
        }
        if (ifndx != -1 && (f->ifndx == ifndx) &&
            !ccn_iribu_addr_cmp(&f->peer, (sockunion*)sa)) {
            f->last_used = CCN_IRIBU_NOW();
#ifdef CCN_IRIBU_RIOT
            ccn_iribu_evtimer_reset_face_timeout(f);
#endif
            return f;
        }
    }

    if (sa && ifndx == -1) {
        for (i = 0; i < ccn_iribu->ifcount; i++) {
            if (sa->sa_family != ccn_iribu->ifs[i].addr.sa.sa_family) {
                continue;
            }
            ifndx = i;
            break;
        }
        if (ifndx == -1) {  // no suitable interface found
            return NULL;
        }
    }
    DEBUGMSG_CORE(VERBOSE, "  found suitable interface %d for %s\n", ifndx,
                ccn_iribu_addr2ascii((sockunion*)sa));

    f = (struct ccn_iribu_face_s *) ccn_iribu_calloc(1, sizeof(struct ccn_iribu_face_s));
    if (!f) {
        DEBUGMSG_CORE(VERBOSE, "  no memory for face\n");
        return NULL;
    }
    f->faceid = ++seqno;
    f->ifndx = ifndx;

    if (ifndx >= 0) {
        if (ccn_iribu->defaultFaceScheduler) {
            f->sched = ccn_iribu->defaultFaceScheduler(ccn_iribu,
                                                  (void (*)(void *, void *)) ccn_iribu_face_CTS);
        }
        if (ccn_iribu->ifs[ifndx].reflect) {
            f->flags |= CCN_IRIBU_FACE_FLAGS_REFLECT;
        }
        if (ccn_iribu->ifs[ifndx].fwdalli) {
            f->flags |= CCN_IRIBU_FACE_FLAGS_FWDALLI;
        }
    }

    if (sa) {
        memcpy(&f->peer, sa, addrlen);
    } else {  // local client
        f->ifndx = -1;
    }
    f->last_used = CCN_IRIBU_NOW();
    DBL_LINKED_LIST_ADD(ccn_iribu->faces, f);

    TRACEOUT();

#ifdef CCN_IRIBU_RIOT
    ccn_iribu_evtimer_reset_face_timeout(f);
#endif

    return f;
}

struct ccn_iribu_face_s*
ccn_iribu_face_remove(struct ccn_iribu_relay_s *ccn_iribu, struct ccn_iribu_face_s *f)
{
    struct ccn_iribu_face_s *f2;
    struct ccn_iribu_interest_s *pit;
    struct ccn_iribu_forward_s **ppfwd;

    DEBUGMSG_CORE(DEBUG, "face_remove relay=%p face=%p\n",
             (void*)ccn_iribu, (void*)f);

    ccn_iribu_sched_destroy(f->sched);
#ifdef USE_FRAG
    ccn_iribu_frag_destroy(f->frag);
#endif
    DEBUGMSG_CORE(TRACE, "face_remove: cleaning PIT\n");
    for (pit = ccn_iribu->pit; pit; ) {
        struct ccn_iribu_pendint_s **ppend, *pend;
        if (pit->from == f) {
            pit->from = NULL;
        }
        for (ppend = &pit->pending; *ppend;) {
            if ((*ppend)->face == f) {
                pend = *ppend;
                *ppend = pend->next;
                ccn_iribu_free(pend);
            } else {
                ppend = &(*ppend)->next;
            }
        }
        if (pit->pending) {
            pit = pit->next;
        } else {
            DEBUGMSG_CORE(TRACE, "before interest_remove 0x%p\n",
                          (void*)pit);
            pit = ccn_iribu_interest_remove(ccn_iribu, pit);
        }
    }
    DEBUGMSG_CORE(TRACE, "face_remove: cleaning fwd table\n");
    for (ppfwd = &ccn_iribu->fib; *ppfwd;) {
        if ((*ppfwd)->face == f) {
            struct ccn_iribu_forward_s *pfwd = *ppfwd;
            ccn_iribu_prefix_free(pfwd->prefix);
            *ppfwd = pfwd->next;
            ccn_iribu_free(pfwd);
        } else {
            ppfwd = &(*ppfwd)->next;
        }
    }
    DEBUGMSG_CORE(TRACE, "face_remove: cleaning pkt queue\n");
    while (f->outq) {
        struct ccn_iribu_buf_s *tmp = f->outq->next;
        ccn_iribu_free(f->outq);
        f->outq = tmp;
    }
    DEBUGMSG_CORE(TRACE, "face_remove: unlinking1 %p %p\n",
             (void*)f->next, (void*)f->prev);
    f2 = f->next;
    DEBUGMSG_CORE(TRACE, "face_remove: unlinking2\n");
    DBL_LINKED_LIST_REMOVE(ccn_iribu->faces, f);
    DEBUGMSG_CORE(TRACE, "face_remove: unlinking3\n");
    ccn_iribu_free(f);

    TRACEOUT();
    return f2;
}

void
ccn_iribu_interface_enqueue(void (tx_done)(void*, int, int), struct ccn_iribu_face_s *f,
                       struct ccn_iribu_relay_s *ccn_iribu, struct ccn_iribu_if_s *ifc,
                       struct ccn_iribu_buf_s *buf, sockunion *dest)
{
    if (ifc) {
        struct ccn_iribu_txrequest_s *r;
        
        if (buf) { 
            DEBUGMSG_CORE(TRACE, "enqueue interface=%p buf=%p len=%zu (qlen=%zu)\n",
                  (void*)ifc, (void*)buf,
                  buf ? buf->datalen : 0, ifc ? ifc->qlen : 0);
        }

        if (ifc->qlen >= CCN_IRIBU_MAX_IF_QLEN) {
            if (buf) {
                DEBUGMSG_CORE(WARNING, "  DROPPING buf=%p\n", (void*)buf); 
                ccn_iribu_free(buf); 
                return;
            }
        }
        
        r = ifc->queue + ((ifc->qfront + ifc->qlen) % CCN_IRIBU_MAX_IF_QLEN); 
        r->buf = buf;
        memcpy(&r->dst, dest, sizeof(sockunion));
        r->txdone = tx_done;
        r->txdone_face = f;
        ifc->qlen++;

#ifdef USE_SCHEDULER
        ccn_iribu_sched_RTS(ifc->sched, 1, buf->datalen, ccn_iribu, ifc);
#else 
        ccn_iribu_interface_CTS(ccn_iribu, ifc);
#endif
    }
}

struct ccn_iribu_buf_s*
ccn_iribu_face_dequeue(struct ccn_iribu_relay_s *ccn_iribu, struct ccn_iribu_face_s *f)
{
    struct ccn_iribu_buf_s *pkt;
    DEBUGMSG_CORE(TRACE, "dequeue face=%p (id=%d.%d)\n",
             (void *) f, ccn_iribu->id, f->faceid);

    if (!f->outq) {
        return NULL;
    }
    pkt = f->outq;
    f->outq = pkt->next;
    if (!pkt->next) {
        f->outqend = NULL;
    }
    pkt->next = NULL;
    return pkt;
}

void
ccn_iribu_face_CTS_done(void *ptr, int cnt, int len)
{
    DEBUGMSG_CORE(TRACE, "CTS_done face=%p cnt=%d len=%d\n", ptr, cnt, len);

#ifdef USE_SCHEDULER
    struct ccn_iribu_face_s *f = (struct ccn_iribu_face_s*) ptr;
    ccn_iribu_sched_CTS_done(f->sched, cnt, len);
#endif
}

void
ccn_iribu_face_CTS(struct ccn_iribu_relay_s *ccn_iribu, struct ccn_iribu_face_s *f)
{
    struct ccn_iribu_buf_s *buf;
    DEBUGMSG_CORE(TRACE, "CTS face=%p sched=%p\n", (void*)f, (void*)f->sched);

    if (!f->frag || f->frag->protocol == CCN_IRIBU_FRAG_NONE) {
        buf = ccn_iribu_face_dequeue(ccn_iribu, f);
        if (buf) {
            ccn_iribu_interface_enqueue(ccn_iribu_face_CTS_done, f,
                                   ccn_iribu, ccn_iribu->ifs + f->ifndx, buf, &f->peer);
        }
    }
#ifdef USE_FRAG
    else {
        sockunion dst;
        int ifndx = f->ifndx;
        buf = ccn_iribu_frag_getnext(f->frag, &ifndx, &dst);
        if (!buf) {
            buf = ccn_iribu_face_dequeue(ccn_iribu, f);
            ccn_iribu_frag_reset(f->frag, buf, f->ifndx, &f->peer);
            buf = ccn_iribu_frag_getnext(f->frag, &ifndx, &dst);
        }
        if (buf) {
            ccn_iribu_interface_enqueue(ccn_iribu_face_CTS_done, f,
                                   ccn_iribu, ccn_iribu->ifs + ifndx, buf, &dst);
#ifndef USE_SCHEDULER
            ccn_iribu_face_CTS(ccn_iribu, f); // loop to push more fragments
#endif
        }
    }
#endif
}

int
ccn_iribu_send_pkt(struct ccn_iribu_relay_s *ccn_iribu, struct ccn_iribu_face_s *to,
                struct ccn_iribu_pkt_s *pkt)
{
    return ccn_iribu_face_enqueue(ccn_iribu, to, buf_dup(pkt->buf));
}

int
ccn_iribu_face_enqueue(struct ccn_iribu_relay_s *ccn_iribu, struct ccn_iribu_face_s *to,
                 struct ccn_iribu_buf_s *buf)
{
    struct ccn_iribu_buf_s *msg;
    if (buf == NULL) {
        DEBUGMSG_CORE(ERROR, "enqueue face: buf most not be NULL\n");
        return -1;
    }
    DEBUGMSG_CORE(TRACE, "enqueue face=%p (id=%d.%d) buf=%p len=%zd\n",
             (void*) to, ccn_iribu->id, to->faceid, (void*) buf, buf ? buf->datalen : 0);

    for (msg = to->outq; msg; msg = msg->next) { // already in the queue?
        if (buf_equal(msg, buf)) {
            DEBUGMSG_CORE(VERBOSE, "    not enqueued because already there\n");
            ccn_iribu_free(buf);
            return -1;
        }
    }
    buf->next = NULL;
    if (to->outqend) {
        to->outqend->next = buf;
    } else {
        to->outq = buf;
    }
    to->outqend = buf;
#ifdef USE_SCHEDULER
    if (to->sched) {
#ifdef USE_FRAG
        int len, cnt = ccn_iribu_frag_getfragcount(to->frag, buf->datalen, &len);
#else
        int len = buf->datalen, cnt = 1;
#endif
        ccn_iribu_sched_RTS(to->sched, cnt, len, ccn_iribu, to);
    } else
        ccn_iribu_face_CTS(ccn_iribu, to);
#else
    ccn_iribu_face_CTS(ccn_iribu, to);
#endif

    return 0;
}


struct ccn_iribu_interest_s*
ccn_iribu_interest_remove(struct ccn_iribu_relay_s *ccn_iribu, struct ccn_iribu_interest_s *i)
{
    struct ccn_iribu_interest_s *i2;

/*
    if (!i)
        return NULL;
*/
    DEBUGMSG_CORE(TRACE, "ccn_iribu_interest_remove %p\n", (void *) i);

#ifdef CCN_IRIBU_RIOT
    ccn_iribu_riot_interest_remove((evtimer_t *)(&ccn_iribu_evtimer), i);
#endif

    while (i->pending) {
        struct ccn_iribu_pendint_s *tmp = i->pending->next;          \
        ccn_iribu_free(i->pending);
        i->pending = tmp;
    }
    i2 = i->next;

    ccn_iribu->pitcnt--;

    DBL_LINKED_LIST_REMOVE(ccn_iribu->pit, i);

    if (i->pkt) {
        ccn_iribu_pkt_free(i->pkt);
    }
    if (i) {
        ccn_iribu_free(i);
    }
    return i2;
}

void
ccn_iribu_interest_propagate(struct ccn_iribu_relay_s *ccn_iribu, struct ccn_iribu_interest_s *i)
{
    struct ccn_iribu_forward_s *fwd;
    int rc = 0;
    char s[CCN_IRIBU_MAX_PREFIX_SIZE];
    (void) s;

#if defined(USE_RONR)
    int matching_face = 0;
#endif

    if (!i) {
        return;
    }
    DEBUGMSG_CORE(DEBUG, "ccn_iribu_interest_propagate\n");

    // CONFORM: "A node MUST implement some strategy rule, even if it is only to
    // transmit an Interest Message on all listed dest faces in sequence."
    // CCNL strategy: we forward on all FWD entries with a prefix match

    for (fwd = ccn_iribu->fib; fwd; fwd = fwd->next) {
        if (!fwd->prefix) {
            continue;
        }

        //Only for matching suite
        if (!i->pkt->pfx || fwd->suite != i->pkt->pfx->suite) {
            DEBUGMSG_CORE(VERBOSE, "  not same suite (%d/%d)\n",
                     fwd->suite, i->pkt->pfx ? i->pkt->pfx->suite : -1);
            continue;
        }

        rc = ccn_iribu_prefix_cmp(fwd->prefix, NULL, i->pkt->pfx, CMP_LONGEST);

        DEBUGMSG_CORE(DEBUG, "  ccn_iribu_interest_propagate, rc=%ld/%ld\n",
                 (long) rc, (long) fwd->prefix->compcnt);
        if (rc < (signed) fwd->prefix->compcnt) {
            continue;
        }

        DEBUGMSG_CORE(DEBUG, "  ccn_iribu_interest_propagate, fwd==%p\n", (void*)fwd);
        // suppress forwarding to origin of interest, except wireless
        if (!i->from || fwd->face != i->from ||
                                (i->from->flags & CCN_IRIBU_FACE_FLAGS_REFLECT)) {
            int nonce = 0;
            if (i->pkt != NULL && i->pkt->s.ndntlv.nonce != NULL) {
                if (i->pkt->s.ndntlv.nonce->datalen == 4) {
                    memcpy(&nonce, i->pkt->s.ndntlv.nonce->data, 4);
                }
            }

            DEBUGMSG_CFWD(INFO, "  outgoing interest=<%s> nonce=%i to=%s\n",
                          ccn_iribu_prefix_to_str(i->pkt->pfx,s,CCN_IRIBU_MAX_PREFIX_SIZE), nonce,
                          fwd->face ? ccn_iribu_addr2ascii(&fwd->face->peer)
                                    : "<tap>");

            // DEBUGMSG(DEBUG, "%p %p %p\n", (void*)i, (void*)i->pkt, (void*)i->pkt->buf);
            if (fwd->tap) {
                (fwd->tap)(ccn_iribu, i->from, i->pkt->pfx, i->pkt->buf);
            }
            if (fwd->face) {
                ccn_iribu_send_pkt(ccn_iribu, fwd->face, i->pkt);
            }
#if defined(USE_RONR)
            matching_face = 1;
#endif
        } else {
            DEBUGMSG_CORE(DEBUG, "  no matching fib entry found\n");
        }
    }

#ifdef USE_RONR
    if (!matching_face) {
        ccn_iribu_interest_broadcast(ccn_iribu, i);
    }
#endif

    return;
}

void
ccn_iribu_interest_broadcast(struct ccn_iribu_relay_s *ccn_iribu, struct ccn_iribu_interest_s *interest)
{
    sockunion sun;
    struct ccn_iribu_face_s *fibface = NULL;
    extern int ccn_iribu_suite2defaultPort(int suite);
    unsigned i = 0;
    for (i = 0; i < CCN_IRIBU_MAX_INTERFACES; i++) {
        switch (ccn_iribu->ifs[i].addr.sa.sa_family) {
#ifdef USE_LINKLAYER 
#if !(defined(__FreeBSD__) || defined(__APPLE__))
            case (AF_PACKET): {
                /* initialize address with 0xFF for broadcast */
                uint8_t relay_addr[CCN_IRIBU_MAX_ADDRESS_LEN];
                memset(relay_addr, CCN_IRIBU_BROADCAST_OCTET, CCN_IRIBU_MAX_ADDRESS_LEN);

                sun.sa.sa_family = AF_PACKET;
                memcpy(&(sun.linklayer.sll_addr), relay_addr, CCN_IRIBU_MAX_ADDRESS_LEN);
                sun.linklayer.sll_halen = CCN_IRIBU_MAX_ADDRESS_LEN;
                sun.linklayer.sll_protocol = htons(CCN_IRIBU_ETH_TYPE);

                fibface = ccn_iribu_get_face_or_create(ccn_iribu, i, &sun.sa, sizeof(sun.linklayer));
                break;
                              }
#endif
#endif
#ifdef USE_WPAN
            case (AF_IEEE802154): {
                /* initialize address with 0xFF for broadcast */
                sun.sa.sa_family = AF_IEEE802154;
                sun.wpan.addr.addr_type = IEEE802154_ADDR_SHORT;
                sun.wpan.addr.pan_id = 0xffff;
                sun.wpan.addr.addr.short_addr = 0xffff;

                fibface = ccn_iribu_get_face_or_create(ccn_iribu, i, &sun.sa, sizeof(sun.wpan));
                break;
                              }
#endif
#ifdef USE_IPV4
            case (AF_INET):
                sun.sa.sa_family = AF_INET;
                sun.ip4.sin_addr.s_addr = INADDR_BROADCAST;
                sun.ip4.sin_port = htons(ccn_iribu_suite2defaultPort(interest->pkt->suite));
                fibface = ccn_iribu_get_face_or_create(ccn_iribu, i, &sun.sa, sizeof(sun.ip4));
                break;
#endif
#ifdef USE_IPV6
            case (AF_INET6):
                sun.sa.sa_family = AF_INET6;
                sun.ip6.sin6_addr = in6addr_any;
                sun.ip6.sin6_port = ccn_iribu_suite2defaultPort(interest->pkt->suite);
                fibface = ccn_iribu_get_face_or_create(ccn_iribu, i, &sun.sa, sizeof(sun.ip6));
                break;
#endif
        }
        if (fibface) {
            ccn_iribu_send_pkt(ccn_iribu, fibface, interest->pkt);
            DEBUGMSG_CORE(DEBUG, "  broadcasting interest (%s)\n", ccn_iribu_addr2ascii(&sun));
        }
    }
}

struct ccn_iribu_content_s*
ccn_iribu_content_remove(struct ccn_iribu_relay_s *ccn_iribu, struct ccn_iribu_content_s *c)
{
    struct ccn_iribu_content_s *c2;
    DEBUGMSG_CORE(TRACE, "ccn_iribu_content_remove\n");

    c2 = c->next;
    DBL_LINKED_LIST_REMOVE(ccn_iribu->contents, c);

//    free_content(c);
    if (c->pkt) {
        ccn_iribu_prefix_free(c->pkt->pfx);
        ccn_iribu_free(c->pkt->buf);
        ccn_iribu_free(c->pkt);
    }
    //    ccn_iribu_prefix_free(c->name);
    ccn_iribu_free(c);

    ccn_iribu->contentcnt--;
#ifdef CCN_IRIBU_RIOT
    evtimer_del((evtimer_t *)(&ccn_iribu_evtimer), (evtimer_event_t *)&c->evtmsg_cstimeout);
#endif
    return c2;
}

struct ccn_iribu_content_s*
ccn_iribu_content_add2cache(struct ccn_iribu_relay_s *ccn_iribu, struct ccn_iribu_content_s *c)
{
    struct ccn_iribu_content_s *cit;
    char s[CCN_IRIBU_MAX_PREFIX_SIZE];
    (void) s;

    DEBUGMSG_CORE(DEBUG, "ccn_iribu_content_add2cache (%d/%d) --> %p = %s [%d]\n",
                  ccn_iribu->contentcnt, ccn_iribu->max_cache_entries,
                  (void*)c, ccn_iribu_prefix_to_str(c->pkt->pfx,s,CCN_IRIBU_MAX_PREFIX_SIZE), (c->pkt->pfx->chunknum)? (signed) *(c->pkt->pfx->chunknum) : -1);

    for (cit = ccn_iribu->contents; cit; cit = cit->next) {
        if (ccn_iribu_prefix_cmp(c->pkt->pfx, NULL, cit->pkt->pfx, CMP_EXACT) == 0) {
            DEBUGMSG_CORE(DEBUG, "--- Already in cache ---\n");
            return NULL;
        }
    }

    if (ccn_iribu->max_cache_entries > 0 &&
        ccn_iribu->contentcnt >= ccn_iribu->max_cache_entries && !cache_strategy_remove(ccn_iribu, c)) {
        // remove oldest content
        struct ccn_iribu_content_s *c2, *oldest = NULL;
        uint32_t age = 0;
        for (c2 = ccn_iribu->contents; c2; c2 = c2->next) {
             if (!(c2->flags & CCN_IRIBU_CONTENT_FLAGS_STATIC)) {
                 if ((age == 0) || c2->last_used < age) {
                     age = c2->last_used;
                     oldest = c2;
                 }
             }
         }
         if (oldest) {
             DEBUGMSG_CORE(DEBUG, " remove old entry from cache\n");
             ccn_iribu_content_remove(ccn_iribu, oldest);
         }
    }
    if ((ccn_iribu->max_cache_entries <= 0) ||
         (ccn_iribu->contentcnt <= ccn_iribu->max_cache_entries)) {
            DBL_LINKED_LIST_ADD(ccn_iribu->contents, c);
            ccn_iribu->contentcnt++;
#ifdef CCN_IRIBU_RIOT
            /* set cache timeout timer if content is not static */
            if (!(c->flags & CCN_IRIBU_CONTENT_FLAGS_STATIC)) {
                ccn_iribu_evtimer_set_cs_timeout(c);
            }
#endif
    }

    return c;
}

int
ccn_iribu_content_serve_pending(struct ccn_iribu_relay_s *ccn_iribu, struct ccn_iribu_content_s *c)
{
    struct ccn_iribu_interest_s *i;
    struct ccn_iribu_face_s *f;
    int cnt = 0;
    DEBUGMSG_CORE(TRACE, "ccn_iribu_content_serve_pending\n");
    char s[CCN_IRIBU_MAX_PREFIX_SIZE];

    for (f = ccn_iribu->faces; f; f = f->next){
                f->flags &= ~CCN_IRIBU_FACE_FLAGS_SERVED; // reply on a face only once
    }
    for (i = ccn_iribu->pit; i;) {
        struct ccn_iribu_pendint_s *pi;
        if (!i->pkt->pfx) {
            continue;
        }

        switch (i->pkt->pfx->suite) {
#ifdef USE_SUITE_CCNB
        case CCN_IRIBU_SUITE_CCNB:
            if (ccn_iribu_i_prefixof_c(i->pkt->pfx, i->pkt->s.ccnb.minsuffix,
                       i->pkt->s.ccnb.maxsuffix, c) < 0) {
                // XX must also check i->ppkd
                i = i->next;
                continue;
            }
            break;
#endif
#ifdef USE_SUITE_CCNTLV
        case CCN_IRIBU_SUITE_CCNTLV:
            if (ccn_iribu_prefix_cmp(c->pkt->pfx, NULL, i->pkt->pfx, CMP_EXACT)) {
                // XX must also check keyid
                i = i->next;
                continue;
            }
            break;
#endif
#ifdef USE_SUITE_NDNTLV
        case CCN_IRIBU_SUITE_NDNTLV:
            if (ccn_iribu_i_prefixof_c(i->pkt->pfx, i->pkt->s.ndntlv.minsuffix,
                    i->pkt->s.ndntlv.maxsuffix, c) < 0) {
                // XX must also check i->ppkl,
                i = i->next;
                continue;
            }
            break;
#endif
        default:
            i = i->next;
            continue;
        }

        //Hook for add content to cache by callback:
        if(i && ! i->pending){
            DEBUGMSG_CORE(WARNING, "releasing interest 0x%p OK?\n", (void*)i);
            c->flags |= CCN_IRIBU_CONTENT_FLAGS_STATIC;
            i = ccn_iribu_interest_remove(ccn_iribu, i);

            c->served_cnt++;
            cnt++;
            continue;
            //return 1;

        }

        // CONFORM: "Data MUST only be transmitted in response to
        // an Interest that matches the Data."
        for (pi = i->pending; pi; pi = pi->next) {
            if (pi->face->flags & CCN_IRIBU_FACE_FLAGS_SERVED) {
                continue;
            }
            pi->face->flags |= CCN_IRIBU_FACE_FLAGS_SERVED;
            if (pi->face->ifndx >= 0) {
                int32_t nonce = 0;
                if (i->pkt != NULL && i->pkt->s.ndntlv.nonce != NULL) {
                    if (i->pkt->s.ndntlv.nonce->datalen == 4) {
                        memcpy(&nonce, i->pkt->s.ndntlv.nonce->data, 4);
                    }
                }

#ifndef CCN_IRIBU_LINUXKERNEL
                DEBUGMSG_CFWD(INFO, "  outgoing data=<%s>%s nonce=%"PRIi32" to=%s\n",
                          ccn_iribu_prefix_to_str(i->pkt->pfx,s,CCN_IRIBU_MAX_PREFIX_SIZE),
                          ccn_iribu_suite2str(i->pkt->pfx->suite), nonce,
                          ccn_iribu_addr2ascii(&pi->face->peer));
#else
                DEBUGMSG_CFWD(INFO, "  outgoing data=<%s>%s nonce=%d to=%s\n",
                          ccn_iribu_prefix_to_str(i->pkt->pfx,s,CCN_IRIBU_MAX_PREFIX_SIZE),
                          ccn_iribu_suite2str(i->pkt->pfx->suite), nonce,
                          ccn_iribu_addr2ascii(&pi->face->peer));
#endif
                DEBUGMSG_CORE(VERBOSE, "    Serve to face: %d (pkt=%p)\n",
                         pi->face->faceid, (void*) c->pkt);

                ccn_iribu_send_pkt(ccn_iribu, pi->face, c->pkt);


            } else {// upcall to deliver content to local client
#ifdef CCN_IRIBU_APP_RX
                ccn_iribu_app_RX(ccn_iribu, c);
#endif
            }
            c->served_cnt++;
            cnt++;
        }
        i = ccn_iribu_interest_remove(ccn_iribu, i);
    }

    return cnt;
}

#define DEBUGMSG_AGEING(trace, debug, buf, buf_len)    \
DEBUGMSG_CORE(TRACE, "%s %p\n", (trace), (void*) i);   \
DEBUGMSG_CORE(DEBUG, " %s 0x%p <%s>\n", (debug),       \
    (void*)i,                                          \
    ccn_iribu_prefix_to_str(i->pkt->pfx,buf,buf_len));

void
ccn_iribu_do_ageing(void *ptr, void *dummy)
{

    struct ccn_iribu_relay_s *relay = (struct ccn_iribu_relay_s*) ptr;
    struct ccn_iribu_content_s *c = relay->contents;
    struct ccn_iribu_interest_s *i = relay->pit;
    struct ccn_iribu_face_s *f = relay->faces;
    time_t t = CCN_IRIBU_NOW();
    DEBUGMSG_CORE(VERBOSE, "ageing t=%d\n", (int)t);
    (void) dummy;
    char s[CCN_IRIBU_MAX_PREFIX_SIZE];
    (void) s;

    while (c) {
        if ((c->last_used + CCN_IRIBU_CONTENT_TIMEOUT) <= (uint32_t) t &&
                                !(c->flags & CCN_IRIBU_CONTENT_FLAGS_STATIC)){
            DEBUGMSG_CORE(TRACE, "AGING: CONTENT REMOVE %p\n", (void*) c);
            c = ccn_iribu_content_remove(relay, c);
        }
        else {
#ifdef USE_SUITE_NDNTLV
            if (c->pkt->suite == CCN_IRIBU_SUITE_NDNTLV) {
                // Mark content as stale if its freshness period expired and it is not static
                if ((c->last_used + (c->pkt->s.ndntlv.freshnessperiod / 1000)) <= (uint32_t) t &&
                        !(c->flags & CCN_IRIBU_CONTENT_FLAGS_STATIC)) {
                    c->flags |= CCN_IRIBU_CONTENT_FLAGS_STALE;
                }
            }
#endif
            c = c->next;
        }
    }
    while (i) { // CONFORM: "Entries in the PIT MUST timeout rather
                // than being held indefinitely."
        if ((i->last_used + i->lifetime) <= (uint32_t) t ||
                                i->retries >= CCN_IRIBU_MAX_INTEREST_RETRANSMIT) {
                DEBUGMSG_AGEING("AGING: REMOVE INTEREST", "timeout: remove interest", s, CCN_IRIBU_MAX_PREFIX_SIZE);
                i = ccn_iribu_interest_remove(relay, i);
        } else {
            // CONFORM: "A node MUST retransmit Interest Messages
            // periodically for pending PIT entries."
            DEBUGMSG_CORE(DEBUG, " retransmit %d <%s>\n", i->retries,
                     ccn_iribu_prefix_to_str(i->pkt->pfx,s,CCN_IRIBU_MAX_PREFIX_SIZE));
                DEBUGMSG_CORE(TRACE, "AGING: PROPAGATING INTEREST %p\n", (void*) i);
                ccn_iribu_interest_propagate(relay, i);

            i->retries++;
            i = i->next;
        }
    }
    while (f) {
        if (!(f->flags & CCN_IRIBU_FACE_FLAGS_STATIC) &&
                (f->last_used + CCN_IRIBU_FACE_TIMEOUT) <= (uint32_t) t){
            DEBUGMSG_CORE(TRACE, "AGING: FACE REMOVE %p\n", (void*) f);
            f = ccn_iribu_face_remove(relay, f);
        } else {
            f = f->next;
        }
    }
}

int
ccn_iribu_nonce_find_or_append(struct ccn_iribu_relay_s *ccn_iribu, struct ccn_iribu_buf_s *nonce)
{
    struct ccn_iribu_buf_s *n, *n2 = 0;
    int i;
    DEBUGMSG_CORE(TRACE, "ccn_iribu_nonce_find_or_append\n");

    for (n = ccn_iribu->nonces, i = 0; n; n = n->next, i++) {
        if (buf_equal(n, nonce)) {
            return -1;
        }
        if (n->next) {
            n2 = n;
        }
    }
    n = ccn_iribu_buf_new(nonce->data, nonce->datalen);
    if (n) {
        n->next = ccn_iribu->nonces;
        ccn_iribu->nonces = n;
        if (i >= CCN_IRIBU_MAX_NONCES && n2) {
            ccn_iribu_free(n2->next);
            n2->next = 0;
        }
    }
    return 0;
}

int
ccn_iribu_nonce_isDup(struct ccn_iribu_relay_s *relay, struct ccn_iribu_pkt_s *pkt)
{
    if(CCN_IRIBU_MAX_NONCES < 0){
        struct ccn_iribu_interest_s *i = NULL;
        for (i = relay->pit; i; i = i->next) {
            if(buf_equal(i->pkt->s.ndntlv.nonce, pkt->s.ndntlv.nonce)){
                return 1;
            }
        }
        return 0;
    }
    switch (pkt->suite) {
#ifdef USE_SUITE_CCNB
    case CCN_IRIBU_SUITE_CCNB:
        return pkt->s.ccnb.nonce &&
            ccn_iribu_nonce_find_or_append(relay, pkt->s.ccnb.nonce);
#endif
#ifdef USE_SUITE_NDNTLV
    case CCN_IRIBU_SUITE_NDNTLV:
        return pkt->s.ndntlv.nonce &&
            ccn_iribu_nonce_find_or_append(relay, pkt->s.ndntlv.nonce);
#endif
    default:
        break;
    }
    return 0;
}

#ifdef NEEDS_PREFIX_MATCHING

/* add a new entry to the FIB */
int
ccn_iribu_fib_add_entry(struct ccn_iribu_relay_s *relay, struct ccn_iribu_prefix_s *pfx,
                   struct ccn_iribu_face_s *face)
{
    struct ccn_iribu_forward_s *fwd, **fwd2;
    char s[CCN_IRIBU_MAX_PREFIX_SIZE];
    (void) s;

    DEBUGMSG_CUTL(INFO, "adding FIB for <%s>, suite %s\n",
             ccn_iribu_prefix_to_str(pfx,s,CCN_IRIBU_MAX_PREFIX_SIZE), ccn_iribu_suite2str(pfx->suite));

    for (fwd = relay->fib; fwd; fwd = fwd->next) {
        if (fwd->suite == pfx->suite &&
                        !ccn_iribu_prefix_cmp(fwd->prefix, NULL, pfx, CMP_EXACT)) {
            ccn_iribu_prefix_free(fwd->prefix);
            fwd->prefix = NULL;
            break;
        }
    }
    if (!fwd) {
        fwd = (struct ccn_iribu_forward_s *) ccn_iribu_calloc(1, sizeof(*fwd));
        if (!fwd) {
            return -1;
        }
        fwd2 = &relay->fib;
        while (*fwd2) {
            fwd2 = &((*fwd2)->next);
        }
        *fwd2 = fwd;
        fwd->suite = pfx->suite;
    }
    fwd->prefix = pfx;
    fwd->face = face;
    DEBUGMSG_CUTL(DEBUG, "added FIB via %s\n", ccn_iribu_addr2ascii(&fwd->face->peer));

    return 0;
}

/* remove a new entry to the FIB */
int
ccn_iribu_fib_rem_entry(struct ccn_iribu_relay_s *relay, struct ccn_iribu_prefix_s *pfx,
                   struct ccn_iribu_face_s *face)
{
    struct ccn_iribu_forward_s *fwd;
    int res = -1;
    struct ccn_iribu_forward_s *last = NULL;
    char s[CCN_IRIBU_MAX_PREFIX_SIZE];
    (void) s;

    if (pfx != NULL) {
        char *s = NULL;
        DEBUGMSG_CUTL(INFO, "removing FIB for <%s>, suite %s\n",
                      ccn_iribu_prefix_to_str(pfx,s,CCN_IRIBU_MAX_PREFIX_SIZE), ccn_iribu_suite2str(pfx->suite));
    }

    for (fwd = relay->fib; fwd; last = fwd, fwd = fwd->next) {
        if (((pfx == NULL) || (fwd->suite == pfx->suite)) &&
            ((pfx == NULL) || !ccn_iribu_prefix_cmp(fwd->prefix, NULL, pfx, CMP_EXACT)) &&
            ((face == NULL) || (fwd->face == face))) {
            res = 0;
            if (!last) {
                relay->fib = fwd->next;
            }
            else {
                last->next = fwd->next;
            }
            ccn_iribu_prefix_free(fwd->prefix);
            ccn_iribu_free(fwd);
            break;
        }
    }

    if (fwd) {
        if (fwd->face) {
            DEBUGMSG_CUTL(DEBUG, "added FIB via %s\n", ccn_iribu_addr2ascii(&fwd->face->peer));
        }
    }

    return res;
}
#endif

/* prints the current FIB */
void
ccn_iribu_fib_show(struct ccn_iribu_relay_s *relay)
{
#ifndef CCN_IRIBU_LINUXKERNEL
    char s[CCN_IRIBU_MAX_PREFIX_SIZE];
    struct ccn_iribu_forward_s *fwd;

    printf("%-30s | %-10s | %-9s | Peer\n",
           "Prefix", "Suite",
#ifdef CCN_IRIBU_RIOT
           "Interface"
#else
           "Socket"
#endif
           );
    puts("-------------------------------|------------|-----------|------------------------------------");

    for (fwd = relay->fib; fwd; fwd = fwd->next) {
        printf("%-30s | %-10s |        %02i | %s\n", ccn_iribu_prefix_to_str(fwd->prefix,s,CCN_IRIBU_MAX_PREFIX_SIZE),
                                     ccn_iribu_suite2str(fwd->suite), (int)
                                     /* TODO: show correct interface instead of always 0 */
#ifdef CCN_IRIBU_RIOT
                                     (relay->ifs[0]).if_pid,
#else
                                     (relay->ifs[0]).sock,
#endif
                                     ccn_iribu_addr2ascii(&fwd->face->peer));
    }
#endif
}

void
ccn_iribu_cs_dump(struct ccn_iribu_relay_s *ccn_iribu)
{
#ifndef CCN_IRIBU_LINUXKERNEL
    struct ccn_iribu_content_s *c = ccn_iribu->contents;
    unsigned i = 0;
    char s[CCN_IRIBU_MAX_PREFIX_SIZE];
    (void) s;

    while (c) {
        printf("CS[%u]: %s [%d]: %.*s\n", i++,
               ccn_iribu_prefix_to_str(c->pkt->pfx,s,CCN_IRIBU_MAX_PREFIX_SIZE),
               (c->pkt->pfx->chunknum)? (signed) *(c->pkt->pfx->chunknum) : -1,
               (int) c->pkt->contlen, c->pkt->content);
        c = c->next;
    }
#endif
}

void
ccn_iribu_interface_CTS(void *aux1, void *aux2)
{
    struct ccn_iribu_relay_s *ccn_iribu = (struct ccn_iribu_relay_s *)aux1;
    struct ccn_iribu_if_s *ifc = (struct ccn_iribu_if_s *)aux2;
    struct ccn_iribu_txrequest_s *r, req;

    DEBUGMSG_CORE(TRACE, "interface_CTS interface=%p, qlen=%zu, sched=%p\n",
             (void*)ifc, ifc->qlen, (void*)ifc->sched);

    if (ifc->qlen <= 0) {
        return;
    }

#ifdef USE_STATS
    ifc->tx_cnt++;
#endif

    r = ifc->queue + ifc->qfront;
    memcpy(&req, r, sizeof(req));
    ifc->qfront = (ifc->qfront + 1) % CCN_IRIBU_MAX_IF_QLEN;
    ifc->qlen--;
#ifndef CCN_IRIBU_LINUXKERNEL
    assert(ccn_iribu->ccn_iribu_ll_TX_ptr != 0);
#endif
    ccn_iribu->ccn_iribu_ll_TX_ptr(ccn_iribu, ifc, &req.dst, req.buf);
#ifdef USE_SCHEDULER
    ccn_iribu_sched_CTS_done(ifc->sched, 1, req.buf->datalen);
    if (req.txdone)
        req.txdone(req.txdone_face, 1, req.buf->datalen);
#endif
    ccn_iribu_free(req.buf);
}

int
ccn_iribu_cs_add(struct ccn_iribu_relay_s *ccn_iribu, struct ccn_iribu_content_s *c)
{
    struct ccn_iribu_content_s *content;

    content = ccn_iribu_content_add2cache(ccn_iribu, c);
    if (content) {
        ccn_iribu_content_serve_pending(ccn_iribu, content);
        return 0;
    }

    return -1;
}

int
ccn_iribu_cs_remove(struct ccn_iribu_relay_s *ccn_iribu, char *prefix)
{
    struct ccn_iribu_content_s *c;

    if (!ccn_iribu || !prefix) {
        return -1;
    }

    for (c = ccn_iribu->contents; c; c = c->next) {
        char *spref = ccn_iribu_prefix_to_path(c->pkt->pfx);
        if (!spref) {
            return -2;
        }
        if (memcmp(prefix, spref, strlen(spref)) == 0) {
            ccn_iribu_free(spref);
            ccn_iribu_content_remove(ccn_iribu, c);
            return 0;
        }
        ccn_iribu_free(spref);
    }
    return -3;
}

struct ccn_iribu_content_s *
ccn_iribu_cs_lookup(struct ccn_iribu_relay_s *ccn_iribu, char *prefix)
{
    struct ccn_iribu_content_s *c;

    if (!ccn_iribu || !prefix) {
        return NULL;
    }

    for (c = ccn_iribu->contents; c; c = c->next) {
        char *spref = ccn_iribu_prefix_to_path(c->pkt->pfx);
        if (!spref) {
            return NULL;
        }
        if (memcmp(prefix, spref, strlen(spref)) == 0) {
            ccn_iribu_free(spref);
            return c;
        }
        ccn_iribu_free(spref);
    }
    return NULL;
}

void
ccn_iribu_set_cache_strategy_remove(ccn_iribu_cache_strategy_func func)
{
    _cs_remove_func = func;
}

void
ccn_iribu_set_cache_strategy_cache(ccn_iribu_cache_strategy_func func)
{
    _cs_decision_func = func;
}

int
cache_strategy_remove(struct ccn_iribu_relay_s *relay, struct ccn_iribu_content_s *c)
{
    if (_cs_remove_func) {
        return _cs_remove_func(relay, c);
    }
    return 0;
}

int
cache_strategy_cache(struct ccn_iribu_relay_s *relay, struct ccn_iribu_content_s *c)
{
    if (_cs_decision_func) {
        return _cs_decision_func(relay, c);
    }
    // If no caching decision strategy is defined, cache everything
    return 1;
}
