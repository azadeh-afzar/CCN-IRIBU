/*
 * @f ccnl-ext-mgmt.c
 * @b CCN lite extension, management logic (face mgmt and registration)
 *
 * Copyright (C) 2012-13, Christian Tschudin, University of Basel
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
 * 2012-05-06 created
 */

#ifdef USE_MGMT

#include "ccnx.h"
#include "ccnl-pdu.c"
#include "ccnl.h"

// ----------------------------------------------------------------------

int
ccnl_is_local_addr(sockunion *su)
{
    if (!su)
	return 0;
    if (su->sa.sa_family == AF_UNIX)
	return -1;
    if (su->sa.sa_family == AF_INET)
	return su->ip4.sin_addr.s_addr == htonl(0x7f000001);
    return 0;
}

struct ccnl_prefix_s*
ccnl_prefix_clone(struct ccnl_prefix_s *p)
{
    int i, len;
    struct ccnl_prefix_s *p2;

    p2 = (struct ccnl_prefix_s*) ccnl_calloc(1, sizeof(struct ccnl_prefix_s));
    if (!p2) return NULL;
    for (i = 0, len = 0; i < p->compcnt; len += p->complen[i++]);
    p2->path = (unsigned char*) ccnl_malloc(len);
    p2->comp = (unsigned char**) ccnl_malloc(p->compcnt*sizeof(char *));
    p2->complen = (int*) ccnl_malloc(p->compcnt*sizeof(int));
    if (!p2->comp || !p2->complen || !p2->path) goto Bail;
    p2->compcnt = p->compcnt;
    for (i = 0, len = 0; i < p->compcnt; len += p2->complen[i++]) {
	p2->complen[i] = p->complen[i];
	p2->comp[i] = p2->path + len;
	memcpy(p2->comp[i], p->comp[i], p2->complen[i]);
    }
    return p2;
Bail:
    free_prefix(p2);
    return NULL;
}

// ----------------------------------------------------------------------
// management protocols

#define extractStr(VAR,DTAG) \
    if (typ == CCN_TT_DTAG && num == DTAG) { \
	char *s; unsigned char *valptr; int vallen; \
	if (consume(typ, num, &buf, &buflen, &valptr, &vallen) < 0) goto Bail; \
	s = ccnl_malloc(vallen+1); if (!s) goto Bail; \
	memcpy(s, valptr, vallen); s[vallen] = '\0'; \
	ccnl_free(VAR); \
	VAR = (unsigned char*) s; \
	continue; \
    } do {} while(0)


void
ccnl_mgmt_return_msg(struct ccnl_relay_s *ccnl, struct ccnl_buf_s *orig,
		     struct ccnl_face_s *from, char *msg)
{
    struct ccnl_buf_s *buf;

    // this is a temporary non-solution: a CCN-content reply should
    // be returned instead of a string message

    buf = ccnl_buf_new(msg, strlen(msg));
    ccnl_face_enqueue(ccnl, from, buf);
}


int
ccnl_mgmt_debug(struct ccnl_relay_s *ccnl, struct ccnl_buf_s *orig,
		struct ccnl_prefix_s *prefix, struct ccnl_face_s *from)
{
    unsigned char *buf, *action, *debugaction;
    unsigned char cdump[2000];
    int buflen, num, typ;
    char *cp = "debug cmd failed";
    int rc = -1;

    DEBUGMSG(99, "ccnl_mgmt_debug from=%s\n", ccnl_addr2ascii(&from->peer));
    action = debugaction = NULL;

    buf = prefix->comp[3];
    buflen = prefix->complen[3];
    if (dehead(&buf, &buflen, &num, &typ) < 0) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_CONTENTOBJ) goto Bail;
    if (dehead(&buf, &buflen, &num, &typ) != 0) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_CONTENT) goto Bail;
    if (dehead(&buf, &buflen, &num, &typ) != 0) goto Bail;
    if (typ != CCN_TT_BLOB) goto Bail;
    buflen = num;
    if (dehead(&buf, &buflen, &num, &typ) != 0) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCNL_DTAG_DEBUGREQUEST) goto Bail;

    while (dehead(&buf, &buflen, &num, &typ) == 0) {
	if (num==0 && typ==0)
	    break; // end
	extractStr(action, CCN_DTAG_ACTION);
	extractStr(debugaction, CCNL_DTAG_DEBUGACTION);

	if (consume(typ, num, &buf, &buflen, 0, 0) < 0) goto Bail;
    }

    // should (re)verify that action=="debug"

    if (debugaction) {
	cp = "debug cmd worked";
	DEBUGMSG(99, "  debugaction is %s\n",
	       debugaction);
	if (!strcmp((const char*)debugaction, "dump")){
	    ccnl_dump(0, CCNL_RELAY, ccnl);
            ccnl_dump_str(0, CCNL_RELAY, ccnl, cdump, 0);
        }
	else if (!strcmp((const char*)debugaction, "halt")){
            sprintf(cdump, "%s", "halt");
	    ccnl->halt_flag = 1;
        }
	else if (!strcmp((const char*)debugaction, "dump+halt")) {
	    ccnl_dump(0, CCNL_RELAY, ccnl);
            ccnl_dump_str(0, CCNL_RELAY, ccnl, cdump, 0);
	    ccnl->halt_flag = 1;
	} else
	    cp = "unknown debug action, ignored";
    } else
	cp = "no debug action given, ignored";

    rc = 0;

Bail:
    ccnl_free(action);
    ccnl_free(debugaction);
    
    /*ANSWER*/
    struct ccnl_buf_s *retbuf;
    unsigned char out[2000];
    int len = 0, len2, len3;
    unsigned char contentobj[2000];
    unsigned char stmt[1000];

    len = mkHeader(out, CCN_DTAG_CONTENT, CCN_TT_DTAG);   // interest
    len += mkHeader(out+len, CCN_DTAG_NAME, CCN_TT_DTAG);  // name

    len += mkStrBlob(out+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG, "ccnx");
    len += mkStrBlob(out+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG, "");
    len += mkStrBlob(out+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG, "debug");

    // prepare debug statement
    len3 = mkHeader(stmt, CCNL_DTAG_DEBUGREQUEST, CCN_TT_DTAG);
    len3 += mkStrBlob(stmt+len3, CCN_DTAG_ACTION, CCN_TT_DTAG, "debug");
    len3 += mkStrBlob(stmt+len3, CCNL_DTAG_DEBUGACTION, CCN_TT_DTAG, cp);
    len3 += mkStrBlob(stmt+len3, CCNL_DTAG_DEBUGREPLY, CCN_TT_DTAG, cdump);
    stmt[len3++] = 0; // end-of-debugstmt

    // prepare CONTENTOBJ with CONTENT
    len2 = mkHeader(contentobj, CCN_DTAG_CONTENTOBJ, CCN_TT_DTAG);   // contentobj
    len2 += mkBlob(contentobj+len2, CCN_DTAG_CONTENT, CCN_TT_DTAG,  // content
		   (char*) stmt, len3);
    contentobj[len2++] = 0; // end-of-contentobj

    // add CONTENTOBJ as the final name component
    len += mkBlob(out+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG,  // comp
		  (char*) contentobj, len2);

    out[len++] = 0; // end-of-name
    out[len++] = 0; // end-of-interest
    
    retbuf = ccnl_buf_new((char *)out, len);
    ccnl_face_enqueue(ccnl, from, retbuf);
    
    /*END ANWER*/

    //ccnl_mgmt_return_msg(ccnl, orig, from, cp);
    return rc;
}


int
ccnl_mgmt_newface(struct ccnl_relay_s *ccnl, struct ccnl_buf_s *orig,
		struct ccnl_prefix_s *prefix, struct ccnl_face_s *from)
{
    unsigned char *buf;
    int buflen, num, typ;
    unsigned char *action, *macsrc, *ip4src, *proto, *host, *port,
	*path, *encaps, *flags;
    char *cp = "newface cmd failed";
    int rc = -1;
    struct ccnl_face_s *f;

    DEBUGMSG(99, "ccnl_mgmt_newface from=%p, ifndx=%d\n", from, from->ifndx);
    action = macsrc = ip4src = proto = host = port = NULL;
    path = encaps = flags = NULL;

    buf = prefix->comp[3];
    buflen = prefix->complen[3];
    if (dehead(&buf, &buflen, &num, &typ) < 0) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_CONTENTOBJ) goto Bail;
    if (dehead(&buf, &buflen, &num, &typ) != 0) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_CONTENT) goto Bail;
    if (dehead(&buf, &buflen, &num, &typ) != 0) goto Bail;
    if (typ != CCN_TT_BLOB) goto Bail;
    buflen = num;
    if (dehead(&buf, &buflen, &num, &typ) != 0) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_FACEINSTANCE) goto Bail;

    while (dehead(&buf, &buflen, &num, &typ) == 0) {
	if (num==0 && typ==0)
	    break; // end
	extractStr(action, CCN_DTAG_ACTION);
	extractStr(macsrc, CCNL_DTAG_MACSRC);
	extractStr(ip4src, CCNL_DTAG_IP4SRC);
	extractStr(path, CCNL_DTAG_UNIXSRC);
	extractStr(proto, CCN_DTAG_IPPROTO);
	extractStr(host, CCN_DTAG_HOST);
	extractStr(port, CCN_DTAG_PORT);
//	extractStr(encaps, CCNL_DTAG_ENCAPS);
	extractStr(flags, CCNL_DTAG_FACEFLAGS);

	if (consume(typ, num, &buf, &buflen, 0, 0) < 0) goto Bail;
    }

    // should (re)verify that action=="newface"

    f = NULL;
#ifdef USE_ETHERNET
    if (macsrc && host && port) {
	sockunion su;
	DEBUGMSG(99, "  adding ETH face macsrc=%s, host=%s, ethtype=%s\n",
		 macsrc, host, port);
	memset(&su, 0, sizeof(su));
	su.eth.sll_family = AF_PACKET;
	su.eth.sll_protocol = htons(strtol((const char*)port, NULL, 0));
	if (sscanf((const char*) host, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
		   su.eth.sll_addr,   su.eth.sll_addr+1,
		   su.eth.sll_addr+2, su.eth.sll_addr+3,
		   su.eth.sll_addr+4, su.eth.sll_addr+5) == 6) {
	// if (!strcmp(macsrc, "any")) // honouring macsrc not implemented yet
	    f = ccnl_get_face_or_create(ccnl, -1, &su.sa, sizeof(su.eth));
	}
    } else
#endif
    if (proto && host && port && !strcmp((const char*)proto, "17")) {
	sockunion su;
	DEBUGMSG(99, "  adding IP face ip4src=%s, proto=%s, host=%s, port=%s\n",
		 ip4src, proto, host, port);
	su.sa.sa_family = AF_INET;
	inet_aton((const char*)host, &su.ip4.sin_addr);
	su.ip4.sin_port = htons(strtol((const char*)port, NULL, 0));
	// not implmented yet: honor the requested ip4src parameter
	f = ccnl_get_face_or_create(ccnl, -1, // from->ifndx,
				    &su.sa, sizeof(struct sockaddr_in));
    }
#ifdef USE_UNIXSOCKET
    if (path) {
	sockunion su;
	DEBUGMSG(99, "  adding UNIX face unixsrc=%s\n", path);
	su.sa.sa_family = AF_UNIX;
	strcpy(su.ux.sun_path, (char*) path);
	f = ccnl_get_face_or_create(ccnl, -1, // from->ifndx,
				    &su.sa, sizeof(struct sockaddr_un));
    }
#endif

    if (f) {
	int flagval = flags ?
	    strtol((const char*)flags, NULL, 0) : CCNL_FACE_FLAGS_STATIC;
	//	printf("  flags=%s %d\n", flags, flagval);
	DEBUGMSG(99, "  adding a new face (id=%d) worked!\n", f->faceid);
	f->flags = flagval &
	    (CCNL_FACE_FLAGS_STATIC|CCNL_FACE_FLAGS_REFLECT);

#ifdef USE_ENCAPS
	if (encaps) {
	    int mtu = 1500;
	    if (f->ifndx >= 0 && ccnl->ifs[f->ifndx].mtu > 0)
		mtu = ccnl->ifs[f->ifndx].mtu;
	    f->encaps = ccnl_encaps_new(strtol((const char*)encaps, NULL, 0),
					mtu); 
	}
#endif
	cp = "newface cmd worked";
    } else {
	DEBUGMSG(99, "  newface request for (macsrc=%s ip4src=%s proto=%s host=%s port=%s encaps=%s flags=%s) failed or was ignored\n",
		 macsrc, ip4src, proto, host, port, encaps, flags);
    }
    rc = 0;

Bail:
    ccnl_free(action);

     /*ANSWER*/
    struct ccnl_buf_s *retbuf;
    unsigned char out[2000];
    int len = 0, len2, len3;
    unsigned char contentobj[2000];
    unsigned char faceinst[2000];
    unsigned char faceidstr[100];
    unsigned char retstr[200];

    len = mkHeader(out, CCN_DTAG_CONTENT, CCN_TT_DTAG);   // content
    len += mkHeader(out+len, CCN_DTAG_NAME, CCN_TT_DTAG);  // name

    len += mkStrBlob(out+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG, "ccnx");
    len += mkStrBlob(out+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG, "");
    
    len += mkStrBlob(out+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG, "newface");

    // prepare FACEINSTANCE
    len3 = mkHeader(faceinst, CCN_DTAG_FACEINSTANCE, CCN_TT_DTAG);
    sprintf(retstr,"newface:  %s", cp);
    len3 += mkStrBlob(faceinst+len3, CCN_DTAG_ACTION, CCN_TT_DTAG, retstr);
    if (macsrc)
	len3 += mkStrBlob(faceinst+len3, CCNL_DTAG_MACSRC, CCN_TT_DTAG, macsrc);
    if (ip4src) {
	len3 += mkStrBlob(faceinst+len3, CCNL_DTAG_IP4SRC, CCN_TT_DTAG, ip4src);
        len3 += mkStrBlob(faceinst+len3, CCN_DTAG_IPPROTO, CCN_TT_DTAG, "17");
    }
    if (host)
	len3 += mkStrBlob(faceinst+len3, CCN_DTAG_HOST, CCN_TT_DTAG, host);
    if (port)
	len3 += mkStrBlob(faceinst+len3, CCN_DTAG_PORT, CCN_TT_DTAG, port);
    /*
    if (encaps)
	len3 += mkStrBlob(faceinst+len3, CCNL_DTAG_ENCAPS, CCN_TT_DTAG, encaps);
    */
    if (flags)
	len3 += mkStrBlob(faceinst+len3, CCNL_DTAG_FACEFLAGS, CCN_TT_DTAG, flags);
    if(f->faceid){
        sprintf(faceidstr,"%i",f->faceid);
        len3 += mkStrBlob(faceinst+len3, CCN_DTAG_FACEID, CCN_TT_DTAG, faceidstr);
    }
    
    faceinst[len3++] = 0; // end-of-faceinst

    // prepare CONTENTOBJ with CONTENT
    len2 = mkHeader(contentobj, CCN_DTAG_CONTENTOBJ, CCN_TT_DTAG);   // contentobj
    len2 += mkBlob(contentobj+len2, CCN_DTAG_CONTENT, CCN_TT_DTAG,  // content
		   (char*) faceinst, len3);
    contentobj[len2++] = 0; // end-of-contentobj

    // add CONTENTOBJ as the final name component
    len += mkBlob(out+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG,  // comp
		  (char*) contentobj, len2);

    out[len++] = 0; // end-of-name
    out[len++] = 0; // end-of-interest
    
    retbuf = ccnl_buf_new((char *)out, len);
    ccnl_face_enqueue(ccnl, from, retbuf);
    
    /*END ANWER*/  
            
            
    ccnl_free(action);
    ccnl_free(macsrc);
    ccnl_free(ip4src);
    ccnl_free(proto);
    ccnl_free(host);
    ccnl_free(port);
    ccnl_free(encaps);
    ccnl_free(flags);

    //ccnl_mgmt_return_msg(ccnl, orig, from, cp);
    return rc;
}

int
ccnl_mgmt_setencaps(struct ccnl_relay_s *ccnl, struct ccnl_buf_s *orig,
		struct ccnl_prefix_s *prefix, struct ccnl_face_s *from)
{
    unsigned char *buf;
    int buflen, num, typ;
    unsigned char *action, *faceid, *encaps, *mtu;
    char *cp = "setencaps cmd failed";
    int rc = -1;
    struct ccnl_face_s *f;

    DEBUGMSG(99, "ccnl_mgmt_setencaps from=%p, ifndx=%d\n", from, from->ifndx);
    action = faceid = encaps = mtu = NULL;

    buf = prefix->comp[3];
    buflen = prefix->complen[3];
    if (dehead(&buf, &buflen, &num, &typ) < 0) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_CONTENTOBJ) goto Bail;
    if (dehead(&buf, &buflen, &num, &typ) != 0) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_CONTENT) goto Bail;
    if (dehead(&buf, &buflen, &num, &typ) != 0) goto Bail;
    if (typ != CCN_TT_BLOB) goto Bail;
    buflen = num;
    if (dehead(&buf, &buflen, &num, &typ) != 0) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_FACEINSTANCE) goto Bail;

    while (dehead(&buf, &buflen, &num, &typ) == 0) {
	if (num==0 && typ==0)
	    break; // end
	extractStr(action, CCN_DTAG_ACTION);
	extractStr(faceid, CCN_DTAG_FACEID);
	extractStr(encaps, CCNL_DTAG_ENCAPS);
	extractStr(mtu, CCNL_DTAG_MTU);

	if (consume(typ, num, &buf, &buflen, 0, 0) < 0) goto Bail;
    }

    // should (re)verify that action=="newface"

    if (faceid && encaps && mtu) {
	int e = -1, mtuval, fi = strtol((const char*)faceid, NULL, 0);

	for (f = ccnl->faces; f && f->faceid != fi; f = f->next);
	if (!f) goto Error;
	mtuval = strtol((const char*)mtu, NULL, 0);

#ifdef USE_ENCAPS
	if (f->encaps) {
	    ccnl_encaps_destroy(f->encaps);
	    f->encaps = 0;
	}
	if (!strcmp((const char*)encaps, "none"))
	    e = CCNL_ENCAPS_NONE;
	else if (!strcmp((const char*)encaps, "seqd2012")) {
	    e = CCNL_ENCAPS_SEQUENCED2012;
	} else if (!strcmp((const char*)encaps, "ccnp2013")) {
	    e = CCNL_ENCAPS_CCNPDU2013;
	}
	if (e < 0)
	    goto Error;
	f->encaps = ccnl_encaps_new(e, mtuval);
	cp = "setencaps cmd worked";
#else
	cp = "no encapsulation support" + 0*e; // use e to silence compiler
#endif
    } else {
Error:
	DEBUGMSG(99, "  setencaps request for (faceid=%s encaps=%s mtu=%s) failed or was ignored\n",
		 faceid, encaps, mtu);
    }
    rc = 0;

Bail:
    ccnl_free(action);
        
    struct ccnl_buf_s *retbuf;
    unsigned char out[2000];
    int len = 0, len2, len3;
    unsigned char contentobj[2000];
    unsigned char faceinst[2000];

    len = mkHeader(out, CCN_DTAG_CONTENT, CCN_TT_DTAG);   // interest
    len += mkHeader(out+len, CCN_DTAG_NAME, CCN_TT_DTAG);  // name

    len += mkStrBlob(out+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG, "ccnx");
    len += mkStrBlob(out+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG, "");
    len += mkStrBlob(out+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG, "setencaps");

    // prepare FACEINSTANCE
    len3 = mkHeader(faceinst, CCN_DTAG_FACEINSTANCE, CCN_TT_DTAG);
    len3 += mkStrBlob(faceinst+len3, CCN_DTAG_ACTION, CCN_TT_DTAG, cp);
    len3 += mkStrBlob(faceinst+len3, CCN_DTAG_FACEID, CCN_TT_DTAG, faceid);
    len3 += mkStrBlob(faceinst+len3, CCNL_DTAG_ENCAPS, CCN_TT_DTAG, encaps);
    len3 += mkStrBlob(faceinst+len3, CCNL_DTAG_MTU, CCN_TT_DTAG, mtu);
    faceinst[len3++] = 0; // end-of-faceinst

    // prepare CONTENTOBJ with CONTENT
    len2 = mkHeader(contentobj, CCN_DTAG_CONTENTOBJ, CCN_TT_DTAG);   // contentobj
    len2 += mkBlob(contentobj+len2, CCN_DTAG_CONTENT, CCN_TT_DTAG,  // content
		   (char*) faceinst, len3);
    contentobj[len2++] = 0; // end-of-contentobj

    // add CONTENTOBJ as the final name component
    len += mkBlob(out+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG,  // comp
		  (char*) contentobj, len2);

    out[len++] = 0; // end-of-name
    out[len++] = 0; // end-of-interest

    retbuf = ccnl_buf_new((char *)out, len);
    ccnl_face_enqueue(ccnl, from, retbuf);

    ccnl_free(faceid);
    ccnl_free(encaps);
    ccnl_free(mtu);

    //ccnl_mgmt_return_msg(ccnl, orig, from, cp);
    return rc;
}

int
ccnl_mgmt_destroyface(struct ccnl_relay_s *ccnl, struct ccnl_buf_s *orig,
		      struct ccnl_prefix_s *prefix, struct ccnl_face_s *from)
{
    unsigned char *buf;
    int buflen, num, typ;
    unsigned char *action, *faceid;
    char *cp = "destroyface cmd failed";
    int rc = -1;

    DEBUGMSG(99, "ccnl_mgmt_destroyface\n");
    action = faceid = NULL;

    buf = prefix->comp[3];
    buflen = prefix->complen[3];
    if (dehead(&buf, &buflen, &num, &typ) < 0) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_CONTENTOBJ) goto Bail;
    if (dehead(&buf, &buflen, &num, &typ) != 0) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_CONTENT) goto Bail;
    if (dehead(&buf, &buflen, &num, &typ) != 0) goto Bail;
    if (typ != CCN_TT_BLOB) goto Bail;
    buflen = num;
    if (dehead(&buf, &buflen, &num, &typ) != 0) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_FACEINSTANCE) goto Bail;

    while (dehead(&buf, &buflen, &num, &typ) == 0) {
	if (num==0 && typ==0)
	    break; // end
	extractStr(action, CCN_DTAG_ACTION);
	extractStr(faceid, CCN_DTAG_FACEID);

	if (consume(typ, num, &buf, &buflen, 0, 0) < 0) goto Bail;
    }

    // should (re)verify that action=="destroyface"

    if (faceid) {
	struct ccnl_face_s *f;
	int fi = strtol((const char*)faceid, NULL, 0);
	for (f = ccnl->faces; f && f->faceid != fi; f = f->next);
	if (!f) {
	    DEBUGMSG(99, "  could not find face=%s\n", faceid);
	    goto Bail;
	}
	ccnl_face_remove(ccnl, f);
	DEBUGMSG(99, "  face %s destroyed\n", faceid);
	cp = "facedestroy cmd worked";
    } else {
	DEBUGMSG(99, "  missing faceid\n");
    }
    rc = 0;

Bail:
    ccnl_free(action);
    ccnl_free(faceid);
    
    /*ANSWER*/
    struct ccnl_buf_s *retbuf;
    unsigned char out[2000];
    int len = 0, len2, len3;
    unsigned char contentobj[2000];
    unsigned char faceinst[2000];
//    char num[20];

//    sprintf(num, "%d", faceID);

    len = mkHeader(out, CCN_DTAG_CONTENT, CCN_TT_DTAG);   // interest
    len += mkHeader(out+len, CCN_DTAG_NAME, CCN_TT_DTAG);  // name

    len += mkStrBlob(out+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG, "ccnx");
    len += mkStrBlob(out+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG, "");
    len += mkStrBlob(out+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG, "destroyface");

    // prepare FACEINSTANCE
    len3 = mkHeader(faceinst, CCN_DTAG_FACEINSTANCE, CCN_TT_DTAG);
    len3 += mkStrBlob(faceinst+len3, CCN_DTAG_ACTION, CCN_TT_DTAG, cp);
    len3 += mkStrBlob(faceinst+len3, CCN_DTAG_FACEID, CCN_TT_DTAG, faceid);
    faceinst[len3++] = 0; // end-of-faceinst

    // prepare CONTENTOBJ with CONTENT
    len2 = mkHeader(contentobj, CCN_DTAG_CONTENTOBJ, CCN_TT_DTAG);   // contentobj
    len2 += mkBlob(contentobj+len2, CCN_DTAG_CONTENT, CCN_TT_DTAG,  // content
		   (char*) faceinst, len3);
    contentobj[len2++] = 0; // end-of-contentobj

    // add CONTENTOBJ as the final name component
    len += mkBlob(out+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG,  // comp
		  (char*) contentobj, len2);

    out[len++] = 0; // end-of-name
    out[len++] = 0; // end-o
    
    retbuf = ccnl_buf_new((char *)out, len);
    ccnl_face_enqueue(ccnl, from, retbuf);
    
    /*END ANWER*/  
    
    //ccnl_mgmt_return_msg(ccnl, orig, from, cp);
    return rc;
}

int
ccnl_mgmt_newdev(struct ccnl_relay_s *ccnl, struct ccnl_buf_s *orig,
		 struct ccnl_prefix_s *prefix, struct ccnl_face_s *from)
{
    unsigned char *buf;
    int buflen, num, typ;
    unsigned char *action, *devname, *ip4src, *port, *encaps, *flags;
    char *cp = "newdevice cmd worked";
    int rc = -1;

    DEBUGMSG(99, "ccnl_mgmt_newdev\n");
    action = devname = ip4src = port = encaps = flags = NULL;

    buf = prefix->comp[3];
    buflen = prefix->complen[3];
    if (dehead(&buf, &buflen, &num, &typ) < 0) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_CONTENTOBJ) goto Bail;
    if (dehead(&buf, &buflen, &num, &typ) != 0) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_CONTENT) goto Bail;
    if (dehead(&buf, &buflen, &num, &typ) != 0) goto Bail;
    if (typ != CCN_TT_BLOB) goto Bail;
    buflen = num;
    if (dehead(&buf, &buflen, &num, &typ) != 0) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCNL_DTAG_DEVINSTANCE) goto Bail;

    while (dehead(&buf, &buflen, &num, &typ) == 0) {
	if (num==0 && typ==0)
	    break; // end
	extractStr(action, CCN_DTAG_ACTION);
	extractStr(devname, CCNL_DTAG_DEVNAME);
	extractStr(ip4src, CCNL_DTAG_IP4SRC);
	extractStr(port, CCN_DTAG_PORT);
	extractStr(encaps, CCNL_DTAG_ENCAPS);
	extractStr(flags, CCNL_DTAG_DEVFLAGS);

	if (consume(typ, num, &buf, &buflen, 0, 0) < 0) goto Bail;
    }

    // should (re)verify that action=="newdev"

    if (ccnl->ifcount >= CCNL_MAX_INTERFACES) {
      DEBUGMSG(99, "  too many interfaces, no new interface created\n");
      goto Bail;
    }

#if defined(USE_ETHERNET) && (defined(CCNL_UNIX) || defined(CCNL_LINUXKERNEL))
    if (devname && port) {
	struct ccnl_if_s *i;
	int portnum = port ? strtol((const char*)port, NULL, 0) : CCNL_ETH_TYPE;

	DEBUGMSG(99, "  adding eth device devname=%s, port=%s\n",
		 devname, port);

	// check if it already exists, bail

	// create a new ifs-entry
	i = &ccnl->ifs[ccnl->ifcount];
#ifdef CCNL_LINUXKERNEL
	{
	    struct net_device *nd;
	    int j;
	    nd = ccnl_open_ethdev((char*)devname, &i->addr.eth, portnum);
	    if (!nd) {
		DEBUGMSG(99, "  could not open device %s\n", devname);
		goto Bail;
	    }
	    for (j = 0; j < ccnl->ifcount; j++) {
		if (ccnl->ifs[j].netdev == nd) {
		    dev_put(nd);
		    DEBUGMSG(99, "  device %s already open\n", devname);
		    goto Bail;
		}
	    }
	    i->netdev = nd;
	    i->ccnl_packet.type = htons(portnum);
	    i->ccnl_packet.dev = i->netdev;
	    i->ccnl_packet.func = ccnl_eth_RX;
	    dev_add_pack(&i->ccnl_packet);
	}
#else
	i->sock = ccnl_open_ethdev((char*)devname, &i->addr.eth, portnum);
	if (!i->sock) {
	    DEBUGMSG(99, "  could not open device %s\n", devname);
	    goto Bail;
	}
#endif
//	i->encaps = encaps ? atoi(encaps) : 0;
	i->mtu = 1500;
//	we should analyse and copy flags, here we hardcode some defaults:
	i->reflect = 1;
	i->fwdalli = 1;

	if (ccnl->defaultInterfaceScheduler)
	    i->sched = ccnl->defaultInterfaceScheduler(ccnl, ccnl_interface_CTS);
	ccnl->ifcount++;

	rc = 0;
	goto Bail;
    }
#endif

// #ifdef USE_UDP
    if (ip4src && port) {
	struct ccnl_if_s *i;
	DEBUGMSG(99, "  adding UDP device ip4src=%s, port=%s\n",
		 ip4src, port);

	// check if it already exists, bail

	// create a new ifs-entry
	i = &ccnl->ifs[ccnl->ifcount];
	i->sock = ccnl_open_udpdev(strtol((char*)port, NULL, 0), &i->addr.ip4);
	if (!i->sock) {
	    DEBUGMSG(99, "  could not open UDP device %s/%s\n", ip4src, port);
	    goto Bail;
	}

#ifdef CCNL_LINUXKERNEL
	{
	    int j;
	    for (j = 0; j < ccnl->ifcount; j++) {
		if (!ccnl_addr_cmp(&ccnl->ifs[j].addr, &i->addr)) {
		    sock_release(i->sock);
		    DEBUGMSG(99, "  UDP device %s/%s already open\n",
			     ip4src, port);
		    goto Bail;
		}
	    }
	}

	i->wq = create_workqueue(ccnl_addr2ascii(&i->addr));
	if (!i->wq) {
	    DEBUGMSG(99, "  could not create work queue (UDP device %s/%s)\n", ip4src, port);
	    sock_release(i->sock);
	    goto Bail;
	}
	write_lock_bh(&i->sock->sk->sk_callback_lock);
	i->old_data_ready = i->sock->sk->sk_data_ready;
	i->sock->sk->sk_data_ready = ccnl_udp_data_ready;
//	i->sock->sk->sk_user_data = &theRelay;
	write_unlock_bh(&i->sock->sk->sk_callback_lock);
#endif

//	i->encaps = encaps ? atoi(encaps) : 0;
	i->mtu = CCN_DEFAULT_MTU;
//	we should analyse and copy flags, here we hardcode some defaults:
	i->reflect = 0;
	i->fwdalli = 1;

	if (ccnl->defaultInterfaceScheduler)
	    i->sched = ccnl->defaultInterfaceScheduler(ccnl, ccnl_interface_CTS);
	ccnl->ifcount++;

	cp = "newdevice cmd workd";
	rc = 0;
	goto Bail;
    }

    DEBUGMSG(99, "  newdevice request for (namedev=%s ip4src=%s port=%s encaps=%s) failed or was ignored\n",
	     devname, ip4src, port, encaps);
// #endif // USE_UDP

Bail:
    ccnl_free(action);
    
    struct ccnl_buf_s *retbuf;
    unsigned char out[2000];

    int len = 0, len2, len3;
    unsigned char contentobj[2000];
    unsigned char faceinst[2000];
    
    len = mkHeader(out, CCN_DTAG_CONTENT, CCN_TT_DTAG);   // interest
    len += mkHeader(out+len, CCN_DTAG_NAME, CCN_TT_DTAG);  // name
    
    len += mkStrBlob(out+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG, "ccnx");
    len += mkStrBlob(out+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG, "");
    len += mkStrBlob(out+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG, "newdev");
    
    // prepare DEVINSTANCE
    len3 = mkHeader(faceinst, CCNL_DTAG_DEVINSTANCE, CCN_TT_DTAG);
    len3 += mkStrBlob(faceinst+len3, CCN_DTAG_ACTION, CCN_TT_DTAG, cp);
    if (devname)
    len3 += mkStrBlob(faceinst+len3, CCNL_DTAG_DEVNAME, CCN_TT_DTAG,
                      devname);
    
    if (devname && port) {
        if (port)
            len3 += mkStrBlob(faceinst+len3, CCN_DTAG_PORT, CCN_TT_DTAG, port);
        if (encaps)
            len3 += mkStrBlob(faceinst+len3, CCNL_DTAG_ENCAPS, CCN_TT_DTAG, encaps);
        if (flags)
            len3 += mkStrBlob(faceinst+len3, CCNL_DTAG_DEVFLAGS, CCN_TT_DTAG, flags);
        faceinst[len3++] = 0; // end-of-faceinst 
    }
    else if (ip4src && port) {
        if (ip4src)
            len3 += mkStrBlob(faceinst+len3, CCNL_DTAG_IP4SRC, CCN_TT_DTAG, ip4src);
        if (port)
            len3 += mkStrBlob(faceinst+len3, CCN_DTAG_PORT, CCN_TT_DTAG, port);
        if (encaps)
            len3 += mkStrBlob(faceinst+len3, CCNL_DTAG_ENCAPS, CCN_TT_DTAG, encaps);
        if (flags)
            len3 += mkStrBlob(faceinst+len3, CCNL_DTAG_DEVFLAGS, CCN_TT_DTAG, flags);
        faceinst[len3++] = 0; // end-of-faceinst
    }
    // prepare CONTENTOBJ with CONTENT
    len2 = mkHeader(contentobj, CCN_DTAG_CONTENTOBJ, CCN_TT_DTAG);   // contentobj
    len2 += mkBlob(contentobj+len2, CCN_DTAG_CONTENT, CCN_TT_DTAG,  // content
                   (char*) faceinst, len3);
    contentobj[len2++] = 0; // end-of-contentobj

    // add CONTENTOBJ as the final name component
    len += mkBlob(out+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG,  // comp
                    (char*) contentobj, len2);

    out[len++] = 0; // end-of-name
    out[len++] = 0; // end-of-interest

    retbuf = ccnl_buf_new((char *)out, len);
    ccnl_face_enqueue(ccnl, from, retbuf);

    ccnl_free(devname);
    ccnl_free(port);
    ccnl_free(encaps);

    //ccnl_mgmt_return_msg(ccnl, orig, from, cp);
    return rc;
}


int
ccnl_mgmt_destroydev(struct ccnl_relay_s *ccnl, struct ccnl_buf_s *orig,
		     struct ccnl_prefix_s *prefix, struct ccnl_face_s *from)
{
    char *cp = "destroydevice cmd failed";

    DEBUGMSG(99, "mgmt_destroydev not implemented yet\n");

    ccnl_mgmt_return_msg(ccnl, orig, from, cp);
    return -1;
}


int
ccnl_mgmt_prefixreg(struct ccnl_relay_s *ccnl, struct ccnl_buf_s *orig,
		    struct ccnl_prefix_s *prefix, struct ccnl_face_s *from)
{
    unsigned char *buf;
    int buflen, num, typ;
    struct ccnl_prefix_s *p = NULL;
    unsigned char *action, *faceid;
    char *cp = "prefixreg cmd failed";
    int rc = -1;

    DEBUGMSG(99, "ccnl_mgmt_prefixreg\n");
    action = faceid = NULL;

    buf = prefix->comp[3];
    buflen = prefix->complen[3];
    if (dehead(&buf, &buflen, &num, &typ) < 0) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_CONTENTOBJ) goto Bail;
    if (dehead(&buf, &buflen, &num, &typ) != 0) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_CONTENT) goto Bail;
    if (dehead(&buf, &buflen, &num, &typ) != 0) goto Bail;
    if (typ != CCN_TT_BLOB) goto Bail;
    buflen = num;
    if (dehead(&buf, &buflen, &num, &typ) != 0) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_FWDINGENTRY) goto Bail;

    p = (struct ccnl_prefix_s *) ccnl_calloc(1, sizeof(struct ccnl_prefix_s));
    if (!p) goto Bail;
    p->comp = (unsigned char**) ccnl_malloc(CCNL_MAX_NAME_COMP *
					   sizeof(unsigned char*));
    p->complen = (int*) ccnl_malloc(CCNL_MAX_NAME_COMP * sizeof(int));
    if (!p->comp || !p->complen) goto Bail;

    while (dehead(&buf, &buflen, &num, &typ) == 0) {
	if (num==0 && typ==0)
	    break; // end

	if (typ == CCN_TT_DTAG && num == CCN_DTAG_NAME) {
	    for (;;) {
		if (dehead(&buf, &buflen, &num, &typ) != 0) goto Bail;
		if (num==0 && typ==0)
		    break;
		if (typ == CCN_TT_DTAG && num == CCN_DTAG_COMPONENT &&
		    p->compcnt < CCNL_MAX_NAME_COMP) {
			// if (ccnl_grow_prefix(p)) goto Bail;
		    if (consume(typ, num, &buf, &buflen,
				p->comp + p->compcnt,
				p->complen + p->compcnt) < 0) goto Bail;
		    p->compcnt++;
		} else {
		    if (consume(typ, num, &buf, &buflen, 0, 0) < 0) goto Bail;
		}
	    }
	    continue;
	}

	extractStr(action, CCN_DTAG_ACTION);
	extractStr(faceid, CCN_DTAG_FACEID);

	if (consume(typ, num, &buf, &buflen, 0, 0) < 0) goto Bail;
    }

    // should (re)verify that action=="prefixreg"
    if (faceid && p->compcnt > 0) {
	struct ccnl_face_s *f;
	struct ccnl_forward_s *fwd, **fwd2;
	int fi = strtol((const char*)faceid, NULL, 0);
	DEBUGMSG(99, "mgmt: adding prefix %s to faceid=%s\n",
		 ccnl_prefix_to_path(p), faceid);

	for (f = ccnl->faces; f && f->faceid != fi; f = f->next);
	if (!f) goto Bail;

//	printf("Face %s found\n", faceid);
	fwd = (struct ccnl_forward_s *) ccnl_calloc(1, sizeof(*fwd));
	if (!fwd) goto Bail;
	fwd->prefix = ccnl_prefix_clone(p);
	fwd->face = f;
	fwd2 = &ccnl->fib;
	while (*fwd2)
	    fwd2 = &((*fwd2)->next);
	*fwd2 = fwd;
	cp = "prefixreg cmd worked";
    } else {
	DEBUGMSG(99, "mgmt: ignored prefixreg faceid=%s\n", faceid);
    }

    rc = 0;

Bail:
    ccnl_free(action);

    /*ANSWER*/
    struct ccnl_buf_s *retbuf;
    unsigned char out[2000];
    int len = 0, len2, len3;
    unsigned char contentobj[2000];
    unsigned char fwdentry[2000];
    char *cpath;
   
    len = mkHeader(out, CCN_DTAG_CONTENT, CCN_TT_DTAG);   // interest
    len += mkHeader(out+len, CCN_DTAG_NAME, CCN_TT_DTAG);  // name

    len += mkStrBlob(out+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG, "ccnx");
    len += mkStrBlob(out+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG, "");
    len += mkStrBlob(out+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG, action);

    // prepare FWDENTRY
    len3 = mkHeader(fwdentry, CCN_DTAG_FWDINGENTRY, CCN_TT_DTAG);
    len3 += mkStrBlob(fwdentry+len3, CCN_DTAG_ACTION, CCN_TT_DTAG, cp);
    len3 += mkHeader(fwdentry+len3, CCN_DTAG_NAME, CCN_TT_DTAG); // prefix

    fwdentry[len3++] = 0; // end-of-prefix
    len3 += mkStrBlob(fwdentry+len3, CCN_DTAG_FACEID, CCN_TT_DTAG, faceid);
    fwdentry[len3++] = 0; // end-of-fwdentry

    // prepare CONTENTOBJ with CONTENT
    len2 = mkHeader(contentobj, CCN_DTAG_CONTENTOBJ, CCN_TT_DTAG);   // contentobj
    len2 += mkBlob(contentobj+len2, CCN_DTAG_CONTENT, CCN_TT_DTAG,  // content
		   (char*) fwdentry, len3);
    contentobj[len2++] = 0; // end-of-contentobj

    // add CONTENTOBJ as the final name component
    len += mkBlob(out+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG,  // comp
		  (char*) contentobj, len2);

    out[len++] = 0; // end-of-name
    out[len++] = 0; // end-of-interest
    
    retbuf = ccnl_buf_new((char *)out, len);
    ccnl_face_enqueue(ccnl, from, retbuf);
    
    /*END ANWER*/  


    ccnl_free(faceid);
    free_prefix(p);

    ccnl_mgmt_return_msg(ccnl, orig, from, cp);
    return rc;
}

int
ccnl_mgmt(struct ccnl_relay_s *ccnl, struct ccnl_buf_s *orig,
	  struct ccnl_prefix_s *prefix, struct ccnl_face_s *from)
{
    char cmd[1000];

    if (prefix->complen[2] < sizeof(cmd)) {
	memcpy(cmd, prefix->comp[2], prefix->complen[2]);
	cmd[prefix->complen[2]] = '\0';
    } else
	strcpy(cmd, "cmd-is-too-long-to-display");

    DEBUGMSG(99, "ccnl_mgmt request \"%s\"\n", cmd);

    if (!ccnl_is_local_addr(&from->peer)) {
	DEBUGMSG(99, "  rejecting because src=%s is not a local addr\n",
		 ccnl_addr2ascii(&from->peer));
	ccnl_mgmt_return_msg(ccnl, orig, from,
			     "refused: origin of mgmt cmd is not local");
	return -1;
    }
	
    if (!strcmp(cmd, "newdev"))
	ccnl_mgmt_newdev(ccnl, orig, prefix, from);
    if (!strcmp(cmd, "setencaps"))
	ccnl_mgmt_setencaps(ccnl, orig, prefix, from);
    else if (!strcmp(cmd, "destroydev"))
	ccnl_mgmt_destroydev(ccnl, orig, prefix, from);
    else if (!strcmp(cmd, "newface"))
	ccnl_mgmt_newface(ccnl, orig, prefix, from);
    else if (!strcmp(cmd, "destroyface"))
	ccnl_mgmt_destroyface(ccnl, orig, prefix, from);
    else if (!strcmp(cmd, "prefixreg"))
	ccnl_mgmt_prefixreg(ccnl, orig, prefix, from);
#ifdef USE_DEBUG
    else if (!strcmp(cmd, "debug")) {
      ccnl_mgmt_debug(ccnl, orig, prefix, from);
    }
#endif
    else {
	DEBUGMSG(99, "unknown mgmt command %s\n", cmd);

	ccnl_mgmt_return_msg(ccnl, orig, from, "unknown mgmt command");
	return -1;
    }

    return 0;
}

#endif // USE_MGMT

// eof
