/*
 * @f ccn-iribu-prefix.c
 * @b CCN lite, core CCNx protocol logic
 *
 * Copyright (C) 2011-18 University of Basel
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
#include "ccn-iribu-prefix.h"
#include "ccn-iribu-pkt-ndntlv.h"
#include "ccn-iribu-pkt-ccntlv.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#if !defined(CCN_IRIBU_RIOT) && !defined(CCN_IRIBU_ANDROID)
#include <openssl/sha.h>
#endif // !defined(CCN_IRIBU_RIOT) && !defined(CCN_IRIBU_ANDROID)
#else //CCN_IRIBU_LINUXKERNEL
#include "../include/ccn-iribu-prefix.h"
#include "../../ccn-iribu-pkt/include/ccn-iribu-pkt-ndntlv.h"
#include "../../ccn-iribu-pkt/include/ccn-iribu-pkt-ccntlv.h"

#endif //CCN_IRIBU_LINUXKERNEL


struct ccn_iribu_prefix_s*
ccn_iribu_prefix_new(char suite, uint32_t cnt)
{
    struct ccn_iribu_prefix_s *p;

    p = (struct ccn_iribu_prefix_s *) ccn_iribu_calloc(1, sizeof(struct ccn_iribu_prefix_s));
    if (!p){
        return NULL;
    }
    p->comp = (uint8_t **) ccn_iribu_malloc(cnt * sizeof(uint8_t*));
    p->complen = (size_t *) ccn_iribu_malloc(cnt * sizeof(size_t));
    if (!p->comp || !p->complen) {
        ccn_iribu_prefix_free(p);
        return NULL;
    }
    p->compcnt = cnt;
    p->suite = suite;
    p->chunknum = NULL;

    return p;
}

void
ccn_iribu_prefix_free(struct ccn_iribu_prefix_s *p)
{
    ccn_iribu_free(p->bytes);
    ccn_iribu_free(p->comp);
    ccn_iribu_free(p->complen);
    ccn_iribu_free(p->chunknum);
    ccn_iribu_free(p);
}

struct ccn_iribu_prefix_s*
ccn_iribu_prefix_dup(struct ccn_iribu_prefix_s *prefix)
{
    uint32_t i = 0;
    size_t len;
    struct ccn_iribu_prefix_s *p;

    p = ccn_iribu_prefix_new(prefix->suite, prefix->compcnt);
    if (!p){
        return NULL;
    }

    p->compcnt = prefix->compcnt;
    p->chunknum = prefix->chunknum;

    for (i = 0, len = 0; i < prefix->compcnt; i++) {
        len += prefix->complen[i];
    }
    p->bytes = (unsigned char*) ccn_iribu_malloc(len);
    if (!p->bytes) {
        ccn_iribu_prefix_free(p);
        return NULL;
    }

    for (i = 0, len = 0; i < prefix->compcnt; i++) {
        p->complen[i] = prefix->complen[i];
        p->comp[i] = p->bytes + len;
        memcpy(p->bytes + len, prefix->comp[i], p->complen[i]);
        len += p->complen[i];
    }

    if (prefix->chunknum) {
        p->chunknum = (uint32_t *) ccn_iribu_malloc(sizeof(uint32_t));
        *p->chunknum = *prefix->chunknum;
    }

    return p;
}

int8_t
ccn_iribu_prefix_appendCmp(struct ccn_iribu_prefix_s *prefix, uint8_t *cmp,
                      size_t cmplen)
{
    uint32_t lastcmp = prefix->compcnt, i;
    size_t *oldcomplen = prefix->complen;
    uint8_t **oldcomp = prefix->comp;
    uint8_t *oldbytes = prefix->bytes;

    size_t prefixlen = 0;

    if (prefix->compcnt >= CCN_IRIBU_MAX_NAME_COMP) {
        return -1;
    }
    for (i = 0; i < lastcmp; i++) {
        prefixlen += prefix->complen[i];
    }

    prefix->compcnt++;
    prefix->comp = (uint8_t **) ccn_iribu_malloc(prefix->compcnt * sizeof(unsigned char*));
    if (!prefix->comp) {
        prefix->comp = oldcomp;
        return -1;
    }
    prefix->complen = (size_t*) ccn_iribu_malloc(prefix->compcnt * sizeof(size_t));
    if (!prefix->complen) {
        ccn_iribu_free(prefix->comp);
        prefix->comp = oldcomp;
        prefix->complen = oldcomplen;
        return -1;
    }
    prefix->bytes = (uint8_t *) ccn_iribu_malloc(prefixlen + cmplen);
    if (!prefix->bytes) {
        ccn_iribu_free(prefix->comp);
        ccn_iribu_free(prefix->complen);
        prefix->comp = oldcomp;
        prefix->complen = oldcomplen;
        prefix->bytes = oldbytes;
        return -1;
    }

    memcpy(prefix->bytes, oldbytes, prefixlen);
    memcpy(prefix->bytes + prefixlen, cmp, cmplen);

    prefixlen = 0;
    for (i = 0; i < lastcmp; i++) {
        prefix->comp[i] = &prefix->bytes[prefixlen];
        prefix->complen[i] = oldcomplen[i];
        prefixlen += oldcomplen[i];
    }
    prefix->comp[lastcmp] = &prefix->bytes[prefixlen];
    prefix->complen[lastcmp] = cmplen;

    ccn_iribu_free(oldcomp);
    ccn_iribu_free(oldcomplen);
    ccn_iribu_free(oldbytes);

    return 0;
}

// TODO: This function should probably be moved to another file to indicate that it should only be used by application level programs
// and not in the ccnl core. Chunknumbers for NDNTLV are only a convention and there no specification on the packet encoding level.
int
ccn_iribu_prefix_addChunkNum(struct ccn_iribu_prefix_s *prefix, uint32_t chunknum)
{
    if (chunknum >= 0xff) {
      DEBUGMSG_CUTL(WARNING, "addChunkNum is only implemented for "
               "chunknum smaller than 0xff (%lu)\n", (long unsigned) chunknum);
        return -1;
    }

    switch(prefix->suite) {
#ifdef USE_SUITE_NDNTLV
        case CCN_IRIBU_SUITE_NDNTLV: {
            uint8_t cmp[2];
            uint32_t *oldchunknum = prefix->chunknum;
            cmp[0] = NDN_Marker_SegmentNumber;
            // TODO: this only works for chunknums smaller than 255
            cmp[1] = (uint8_t) chunknum;
            if (ccn_iribu_prefix_appendCmp(prefix, cmp, 2) < 0) {
                return -1;
            }
            prefix->chunknum = (uint32_t *) ccn_iribu_malloc(sizeof(uint32_t));
            if (!prefix->chunknum) {
                prefix->chunknum = oldchunknum;
                return -1;
            }
            *prefix->chunknum = chunknum;
            if (oldchunknum) {
                ccn_iribu_free(oldchunknum);
            }
        }
        break;
#endif

#ifdef USE_SUITE_CCNTLV
        case CCN_IRIBU_SUITE_CCNTLV: {
            uint8_t cmp[5];
            uint32_t *oldchunknum = prefix->chunknum;
            cmp[0] = 0;
            // TODO: this only works for chunknums smaller than 255
            cmp[1] = CCNX_TLV_N_Chunk;
            cmp[2] = 0;
            cmp[3] = 1;
            cmp[4] = (uint8_t) chunknum;
            if(ccn_iribu_prefix_appendCmp(prefix, cmp, 5) < 0) {
                return -1;
            }
            prefix->chunknum = (uint32_t *) ccn_iribu_malloc(sizeof(uint32_t));
            if (!prefix->chunknum) {
                prefix->chunknum = oldchunknum;
                return -1;
            }
            *prefix->chunknum = chunknum;
            if (oldchunknum) {
                ccn_iribu_free(oldchunknum);
            }
        }
        break;
#endif

        default:
            DEBUGMSG_CUTL(WARNING,
                     "add chunk number not implemented for suite %d\n",
                     prefix->suite);
            return -1;
    }

    return 0;
}

// TODO: move to a util file?
uint8_t
hex2int(char c)
{
    if (c >= '0' && c <= '9') {
        return (uint8_t) (c - '0');
    }
    c = (char) tolower(c);
    if (c >= 'a' && c <= 'f') {
        return (uint8_t) (c - 'a' + 0x0a);
    }
    return 0;
}

size_t
unescape_component(char *comp)
{
    char *in = comp, *out = comp;
    size_t len;

    for (len = 0; *in; len++) {
        if (in[0] != '%' || !in[1] || !in[2]) {
            *out++ = *in++;
            continue;
        }
        *out++ = (char) (hex2int(in[1]) * 16 + hex2int(in[2]));
        in += 3;
    }
    return len;
}

uint32_t
ccn_iribu_URItoComponents(char **compVector, size_t *compLens, char *uri)
{
    uint32_t i;
    size_t len;

    if (*uri == '/') {
        uri++;
    }

    for (i = 0; *uri && i < (CCN_IRIBU_MAX_NAME_COMP - 1); i++) {
        compVector[i] = uri;
        while (*uri && *uri != '/') {
            uri++;
        }
        if (*uri) {
            *uri = '\0';
            uri++;
        }
        len = unescape_component(compVector[i]);

        if (compLens) {
            compLens[i] = len;
        }

        compVector[i][len] = '\0';
    }
    compVector[i] = NULL;

    return i;
}

struct ccn_iribu_prefix_s *
ccn_iribu_URItoPrefix(char* uri, int suite, uint32_t *chunknum)
{
    struct ccn_iribu_prefix_s *p;
    char *compvect[CCN_IRIBU_MAX_NAME_COMP];
    size_t complens[CCN_IRIBU_MAX_NAME_COMP], len, tlen;
    uint32_t cnt, i;

    DEBUGMSG_CUTL(TRACE, "ccn_iribu_URItoPrefix(suite=%s, uri=%s)\n",
             ccn_iribu_suite2str(suite), uri);

    if (strlen(uri)) {
        cnt = ccn_iribu_URItoComponents(compvect, complens, uri);
    } else {
        cnt = 0U;
    }

    p = ccn_iribu_prefix_new(suite, cnt);
    if (!p) {
        return NULL;
    }

    for (i = 0, len = 0; i < cnt; i++) {
        len += complens[i];
    }
#ifdef USE_SUITE_CCNTLV
    if (suite == CCN_IRIBU_SUITE_CCNTLV) {
        len += cnt * 4; // add TL size
    }
#endif

    p->bytes = (unsigned char*) ccn_iribu_malloc(len);
    if (!p->bytes) {
        ccn_iribu_prefix_free(p);
        return NULL;
    }

    for (i = 0, len = 0; i < cnt; i++) {
        char *cp = compvect[i];
        tlen = complens[i];

        p->comp[i] = p->bytes + len;
        tlen = ccn_iribu_pkt_mkComponent(suite, p->comp[i], cp, tlen);
        p->complen[i] = tlen;
        len += tlen;
    }

    p->compcnt = cnt;

    if (chunknum) {
        p->chunknum = (uint32_t*) ccn_iribu_malloc(sizeof(uint32_t));
        if (!p->chunknum) {
            ccn_iribu_prefix_free(p);
            return NULL;
        }
        *p->chunknum = *chunknum;
    }

    return p;
}

#ifdef NEEDS_PREFIX_MATCHING

const char*
ccn_iribu_matchMode2str(int mode)
{
    switch (mode) {
    case CMP_EXACT:
        return CONSTSTR("CMP_EXACT");
    case CMP_MATCH:
        return CONSTSTR("CMP_MATCH");
    case CMP_LONGEST:
        return CONSTSTR("CMP_LONGEST");
    }

    return CONSTSTR("?");
}

int32_t
ccn_iribu_prefix_cmp(struct ccn_iribu_prefix_s *pfx, unsigned char *md,
                struct ccn_iribu_prefix_s *nam, int mode)
/* returns -1 if no match at all (all modes) or exact match failed
   returns  0 if full match (mode = CMP_EXACT) or no components match (mode = CMP_MATCH)
   returns n>0 for matched components (mode = CMP_MATCH, CMP_LONGEST) */
{
    int32_t rc = -1;
    size_t clen;
    uint32_t plen = pfx->compcnt + (md ? 1 : 0), i;
    unsigned char *comp;
    char s[CCN_IRIBU_MAX_PREFIX_SIZE];

    DEBUGMSG(VERBOSE, "prefix_cmp(mode=%s) ", ccn_iribu_matchMode2str(mode));
    DEBUGMSG(VERBOSE, "prefix=<%s>(%p) of? ",
             ccn_iribu_prefix_to_str(pfx, s, CCN_IRIBU_MAX_PREFIX_SIZE), (void *) pfx);
    DEBUGMSG(VERBOSE, "name=<%s>(%p) digest=%p\n",
             ccn_iribu_prefix_to_str(nam, s, CCN_IRIBU_MAX_PREFIX_SIZE), (void *) nam, (void *) md);

    if (mode == CMP_EXACT) {
        if (plen != nam->compcnt) {
            DEBUGMSG(VERBOSE, "comp count mismatch\n");
            goto done;
        }
        if (pfx->chunknum || nam->chunknum) {
            if (!pfx->chunknum || !nam->chunknum) {
                DEBUGMSG(VERBOSE, "chunk mismatch\n");
                goto done;
            }
            if (*pfx->chunknum != *nam->chunknum) {
                DEBUGMSG(VERBOSE, "chunk number mismatch\n");
                goto done;
            }
        }
    }

    for (i = 0; i < plen && i < nam->compcnt; ++i) {
        comp = i < pfx->compcnt ? pfx->comp[i] : md;
        clen = i < pfx->compcnt ? pfx->complen[i] : 32; // SHA256_DIGEST_LEN
        if (clen != nam->complen[i] || memcmp(comp, nam->comp[i], nam->complen[i])) {
            rc = mode == CMP_EXACT ? -1 : (int32_t) i;
            DEBUGMSG(VERBOSE, "component mismatch: %lu\n", (long unsigned) i);
            goto done;
        }
    }
    // FIXME: we must also inspect chunknum here!
    rc = (mode == CMP_EXACT) ? 0 : (int32_t) i;
done:
    DEBUGMSG(TRACE, "  cmp result: pfxlen=%lu cmplen=%lu namlen=%lu matchlen=%ld\n",
             (long unsigned) pfx->compcnt, (long unsigned) plen, (long unsigned) nam->compcnt, (long) rc);
    return rc;
}

int8_t
ccn_iribu_i_prefixof_c(struct ccn_iribu_prefix_s *prefix,
                  uint64_t minsuffix, uint64_t maxsuffix, struct ccn_iribu_content_s *c)
{
    struct ccn_iribu_prefix_s *p = c->pkt->pfx;

    char s[CCN_IRIBU_MAX_PREFIX_SIZE];

    DEBUGMSG(VERBOSE, "ccn_iribu_i_prefixof_c prefix=<%s> ",
             ccn_iribu_prefix_to_str(prefix, s, CCN_IRIBU_MAX_PREFIX_SIZE));
    DEBUGMSG(VERBOSE, "content=<%s> min=%llu max=%llu\n",
             ccn_iribu_prefix_to_str(p, s, CCN_IRIBU_MAX_PREFIX_SIZE), (unsigned long long) minsuffix, (unsigned long long)maxsuffix);
    //
    // CONFORM: we do prefix match, honour min. and maxsuffix,

    // NON-CONFORM: "Note that to match a ContentObject must satisfy
    // all of the specifications given in the Interest Message."
    // >> CCNL does not honour the exclusion filtering

    if ( (prefix->compcnt + minsuffix) > (p->compcnt + 1) ||
         (prefix->compcnt + maxsuffix) < (p->compcnt + 1)) {
        DEBUGMSG(TRACE, "  mismatch in # of components\n");
        return -2;
    }

    unsigned char *md = NULL;

    if ((prefix->compcnt - p->compcnt) == 1) {
        md = compute_ccnx_digest(c->pkt->buf);

        /* computing the ccnx digest failed */
        if (!md) {
            DEBUGMSG(TRACE, "computing the digest failed\n");
            return -3;
        }
    }

    int32_t cmp = ccn_iribu_prefix_cmp(p, md, prefix, CMP_EXACT);
    return cmp;

}

#endif // NEEDS_PREFIX_MATCHING


#ifndef CCN_IRIBU_LINUXKERNEL

char*
ccn_iribu_prefix_to_path_detailed(struct ccn_iribu_prefix_s *pr, int ccntlv_skip,
                             int escape_components, int call_slash)
{
    (void) ccntlv_skip;
    (void) call_slash;
    /*static char *prefix_buf1;
    static char *prefix_buf2;
    static char *buf;*/

    if (!pr) {
        return NULL;
    }

    /*if (!buf) {
        struct ccn_iribu_buf_s *b;
        b = ccn_iribu_buf_new(NULL, PREFIX_BUFSIZE);
        //ccn_iribu_core_addToCleanup(b);
        prefix_buf1 = (char*) b->data;
        b = ccn_iribu_buf_new(NULL, PREFIX_BUFSIZE);
        //ccn_iribu_core_addToCleanup(b);
        prefix_buf2 = (char*) b->data;
        buf = prefix_buf1;
    } else if (buf == prefix_buf2)
        buf = prefix_buf1;
    else
        buf = prefix_buf2;
    */
    char *buf = (char*) ccn_iribu_malloc(CCN_IRIBU_MAX_PREFIX_SIZE+1);
    if (!buf) {
        DEBUGMSG_CUTL(ERROR, "ccn_iribu_prefix_to_path_detailed: malloc failed, exiting\n");
        return NULL;
    }
    memset(buf, 0, CCN_IRIBU_MAX_PREFIX_SIZE+1);
    return ccn_iribu_prefix_to_str_detailed(pr, ccntlv_skip, escape_components, call_slash, buf, CCN_IRIBU_MAX_PREFIX_SIZE);
}

char*
ccn_iribu_prefix_to_str_detailed(struct ccn_iribu_prefix_s *pr, int ccntlv_skip, int escape_components, int call_slash,
                            char *buf, size_t buflen) {
    size_t len = 0, i, j;
    int result;
    (void)i;
    (void)j;
    (void)len;
    (void) call_slash;
    (void) ccntlv_skip;

    uint8_t skip = 0;

#if defined(USE_SUITE_CCNTLV) 
    // In the future it is possibly helpful to see the type information
    // in the logging output. However, this does not work with NFN because
    // it uses this function to create the names in NFN expressions
    // resulting in CCNTLV type information names within expressions.
    if (ccntlv_skip && (0
#ifdef USE_SUITE_CCNTLV
       || pr->suite == CCN_IRIBU_SUITE_CCNTLV
#endif
                         )) {
        skip = 4;
    }
#endif

    for (i = 0; i < (size_t) pr->compcnt; i++) {
        result = snprintf(buf + len, buflen - len, "/");
        DEBUGMSG(TRACE, "result: %d, buf: %s, avail size: %zd, buflen: %zd\n", result, buf+len, buflen - len, buflen);
        if (!(result > -1 && (size_t)result < (buflen - len))) {
            DEBUGMSG(ERROR, "Could not print prefix, since out of allocated memory");
            return NULL;
        }
        len += result;

        for (j = skip; j < pr->complen[i]; j++) {
            char c = pr->comp[i][j];
            char *fmt;
            fmt = (c < 0x20 || c == 0x7f
                            || (escape_components && c == '/' )) ?
#ifdef CCN_IRIBU_ARDUINO
                  (char*)PSTR("%%%02x") : (char*)PSTR("%c");
            result = snprintf_P(buf + len, buflen - len, fmt, c);
            if (!(result > -1 && (size_t)result < (buflen - len))) {
                DEBUGMSG(ERROR, "Could not print prefix, since out of allocated memory");
                return NULL;
            }
            len += result;
#else
                  (char *) "%%%02x" : (char *) "%c";
            result = snprintf(buf + len, buflen - len, fmt, c);
            DEBUGMSG(TRACE, "result: %d, buf: %s, avail size: %zd, buflen: %zd\n", result, buf+len, buflen - len, buflen);
            if (!(result > -1 && (size_t)result < (buflen - len))) {
                DEBUGMSG(ERROR, "Could not print prefix, since out of allocated memory");
                return NULL;
            }
            len += result;
#endif
            if(len > CCN_IRIBU_MAX_PREFIX_SIZE) {
                DEBUGMSG(ERROR, "BUFSIZE SMALLER THAN OUTPUT LEN");
                break;
            }
        }
    }

    return buf;
}

#else // CCN_IRIBU_LINUXKERNEL

/**
* Transforms a prefix into a string, since returning static buffer cannot be called twice into the same statement
* @param[in] pr the prefix to be transformed
* @return a static buffer containing the prefix transformed into a string.
*/
char*
ccn_iribu_prefix_to_path(struct ccn_iribu_prefix_s *pr)
{
    static char prefix_buf[4096];
    int len= 0, i;
    int result;

    if (!pr)
        return NULL;
    return ccn_iribu_prefix_to_str(pr, prefix_buf,4096);
    /*
    for (i = 0; i < pr->compcnt; i++) {
        if(!strncmp("call", (char*)pr->comp[i], 4) && strncmp((char*)pr->comp[pr->compcnt-1], "NFN", 3)){
            result = snprintf(prefix_buf + len, CCN_IRIBU_MAX_PREFIX_SIZE - len, "%.*s", pr->complen[i], pr->comp[i]);
        }
        else{
            result = snprintf(prefix_buf + len, CCN_IRIBU_MAX_PREFIX_SIZE - len, "/%.*s", pr->complen[i], pr->comp[i]);
        }
        if (!(result > -1 && result < (CCN_IRIBU_MAX_PREFIX_SIZE - len))) {
            DEBUGMSG(ERROR, "Could not print prefix, since out of allocated memory");
            return NULL;
        }
        len += result;
    }
    prefix_buf[len] = '\0';
    return prefix_buf;*/
}


char*
ccn_iribu_prefix_to_str(struct ccn_iribu_prefix_s *pr, char *buf, size_t buflen) {
    size_t len = 0, i, j;
    int result;
    int skip = 0;
    (void)i;
    (void)j;
    (void)len;

#if defined(USE_SUITE_CCNTLV)
    // In the future it is possibly helpful to see the type information
    // in the logging output. However, this does not work with NFN because
    // it uses this function to create the names in NFN expressions
    // resulting in CCNTLV type information names within expressions.
    if (1 && (0
#ifdef USE_SUITE_CCNTLV
       || pr->suite == CCN_IRIBU_SUITE_CCNTLV
#endif
                         ))
        skip = 4;
#endif

    for (i = 0; i < (size_t)pr->compcnt; i++) {

            result = snprintf(buf + len, buflen - len, "/");
            if (!(result > -1 && (size_t)result < (buflen - len))) {
                DEBUGMSG(ERROR, "Could not print prefix, since out of allocated memory");
                return NULL;
            }
            len += result;

        for (j = skip; j < (size_t)pr->complen[i]; j++) {
            char c = pr->comp[i][j];
            char *fmt;
            fmt = (c < 0x20 || c == 0x7f
                            || (0 && c == '/' )) ?
#ifdef CCN_IRIBU_ARDUINO
                  (char*)PSTR("%%%02x") : (char*)PSTR("%c");
            result = snprintf_P(buf + len, buflen - len, fmt, c);
            if (!(result > -1 && (size_t)result < (buflen - len))) {
                DEBUGMSG(ERROR, "Could not print prefix, since out of allocated memory");
                return NULL;
            }
            len += result;
#else
                  (char *) "%%%02x" : (char *) "%c";
            result = snprintf(buf + len, buflen - len, fmt, c);
            if (!(result > -1 && (size_t)result < (buflen - len))) {
                DEBUGMSG(ERROR, "Could not print prefix, since out of allocated memory");
                return NULL;
            }
            len += result;
#endif
            if(len > CCN_IRIBU_MAX_PREFIX_SIZE) {
                DEBUGMSG(ERROR, "BUFSIZE SMALLER THAN OUTPUT LEN");
                break;
            }
        }
    }


    return buf;
}

#endif // CCN_IRIBU_LINUXKERNEL

char*
ccn_iribu_prefix_debug_info(struct ccn_iribu_prefix_s *p) {
    size_t len = 0;
    uint32_t i = 0;
    int result;
    char *buf = (char*) ccn_iribu_malloc(CCN_IRIBU_MAX_PACKET_SIZE);
    if (buf == NULL) {
        DEBUGMSG_CUTL(ERROR, "ccn_iribu_prefix_debug_info: malloc failed, exiting\n");
        return NULL;
    }

    result = snprintf(buf + len, CCN_IRIBU_MAX_PACKET_SIZE - len, "<");
    if (!(result > -1 && (unsigned) result < (CCN_IRIBU_MAX_PACKET_SIZE - len))) {
        DEBUGMSG(ERROR, "Could not print prefix, since out of allocated memory");
        ccn_iribu_free(buf);
        return NULL;
    }
    len += result;

    result = snprintf(buf + len, CCN_IRIBU_MAX_PACKET_SIZE - len, "suite:%i, ", p->suite);
    if (!(result > -1 && (unsigned) result < (CCN_IRIBU_MAX_PACKET_SIZE - len))) {
        DEBUGMSG(ERROR, "Could not print prefix, since out of allocated memory");
        ccn_iribu_free(buf);
        return NULL;
    }
    len += result;

    result = snprintf(buf + len, CCN_IRIBU_MAX_PACKET_SIZE - len, "compcnt:%lu ", (long unsigned) p->compcnt);
    if (!(result > -1 && (unsigned) result < (CCN_IRIBU_MAX_PACKET_SIZE - len))) {
        DEBUGMSG(ERROR, "Could not print prefix, since out of allocated memory");
        ccn_iribu_free(buf);
        return NULL;
    }
    len += result;

    result = snprintf(buf + len, CCN_IRIBU_MAX_PACKET_SIZE - len, "complen:(");
    if (!(result > -1 && (unsigned) result < (CCN_IRIBU_MAX_PACKET_SIZE - len))) {
        DEBUGMSG(ERROR, "Could not print prefix, since out of allocated memory");
        ccn_iribu_free(buf);
        return NULL;
    }
    len += result;
    for (i = 0; i < p->compcnt; i++) {
        result = snprintf(buf + len, CCN_IRIBU_MAX_PACKET_SIZE - len, "%zd", p->complen[i]);
        if (!(result > -1 && (unsigned) result < (CCN_IRIBU_MAX_PACKET_SIZE - len))) {
            DEBUGMSG(ERROR, "Could not print prefix, since out of allocated memory");
            ccn_iribu_free(buf);
            return NULL;
        }
        len += result;
        if (i < p->compcnt - 1) {
            result = snprintf(buf + len, CCN_IRIBU_MAX_PACKET_SIZE - len, ",");
            if (!(result > -1 && (unsigned) result < (CCN_IRIBU_MAX_PACKET_SIZE - len))) {
                DEBUGMSG(ERROR, "Could not print prefix, since out of allocated memory");
                ccn_iribu_free(buf);
                return NULL;
            }
            len += result;
        }
    }
    result = snprintf(buf + len, CCN_IRIBU_MAX_PACKET_SIZE - len, "), ");
    if (!(result > -1 && (unsigned) result < (CCN_IRIBU_MAX_PACKET_SIZE - len))) {
        DEBUGMSG(ERROR, "Could not print prefix, since out of allocated memory");
        ccn_iribu_free(buf);
        return NULL;
    }
    len += result;

    result = snprintf(buf + len, CCN_IRIBU_MAX_PACKET_SIZE - len, "comp:(");
    if (!(result > -1 && (unsigned) result < (CCN_IRIBU_MAX_PACKET_SIZE - len))) {
        DEBUGMSG(ERROR, "Could not print prefix, since out of allocated memory");
        ccn_iribu_free(buf);
        return NULL;
    }
    len += result;
    for (i = 0; i < p->compcnt; i++) {
        result = snprintf(buf + len, CCN_IRIBU_MAX_PACKET_SIZE - len, "%.*s", (int) p->complen[i], p->comp[i]);
        if (!(result > -1 && (unsigned) result < (CCN_IRIBU_MAX_PACKET_SIZE - len))) {
            DEBUGMSG(ERROR, "Could not print prefix, since out of allocated memory");
            ccn_iribu_free(buf);
            return NULL;
        }
        len += result;
        if (i < p->compcnt - 1) {
            result = snprintf(buf + len, CCN_IRIBU_MAX_PACKET_SIZE - len, ",");
            if (!(result > -1 && (unsigned) result < (CCN_IRIBU_MAX_PACKET_SIZE - len))) {
                DEBUGMSG(ERROR, "Could not print prefix, since out of allocated memory");
                ccn_iribu_free(buf);
                return NULL;
            }
            len += result;
        }
    }
    result = snprintf(buf + len, CCN_IRIBU_MAX_PACKET_SIZE - len, ")");
    if (!(result > -1 && (unsigned) result < (CCN_IRIBU_MAX_PACKET_SIZE - len))) {
        DEBUGMSG(ERROR, "Could not print prefix, since out of allocated memory");
        ccn_iribu_free(buf);
        return NULL;
    }
    len += result;

    result = snprintf(buf + len, CCN_IRIBU_MAX_PACKET_SIZE - len, ">%c", '\0');
    if (!(result > -1 && (unsigned) result < (CCN_IRIBU_MAX_PACKET_SIZE - len))) {
        DEBUGMSG(ERROR, "Could not print prefix, since out of allocated memory");
        ccn_iribu_free(buf);
        return NULL;
    }

    return buf;
}

