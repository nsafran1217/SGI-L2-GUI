/*
 * l2proto.c - the L2's client protocol (see l2proto.h)
 *
 * The format is described in SGI-L2's PROTOCOL.md; contrib/l2client.py
 * there is the same client in Python.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "l2proto.h"
#include "util.h"

unsigned long l2p_get32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

unsigned int l2p_get16(const unsigned char *p)
{
    return ((unsigned int)p[0] << 8) | (unsigned int)p[1];
}

void l2p_put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)((v >> 24) & 0xff);
    p[1] = (unsigned char)((v >> 16) & 0xff);
    p[2] = (unsigned char)((v >> 8) & 0xff);
    p[3] = (unsigned char)(v & 0xff);
}

void l2p_put16(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)((v >> 8) & 0xff);
    p[1] = (unsigned char)(v & 0xff);
}

/* ------------------------------------------------------------------ */
/* Router packets                                                      */
/* ------------------------------------------------------------------ */

void l2p_router_pack(DBuf *out, unsigned int opcode, int clas,
                     const L2PArg *args, int nargs)
{
    unsigned char hdr[L2P_HDR], w[4];
    static const char zero[4] = { 0, 0, 0, 0 };
    unsigned long len = 0;
    int i;

    for (i = 0; i < nargs; i++)
        len += 4 + ((args[i].len + 3) & ~3UL);
    l2p_put16(hdr, opcode);
    hdr[2] = (unsigned char)clas;
    hdr[3] = 0;                         /* id: only for REQUEST */
    hdr[4] = 0;                         /* config: not compressed */
    hdr[5] = (unsigned char)((len >> 16) & 0xff);
    hdr[6] = (unsigned char)((len >> 8) & 0xff);
    hdr[7] = (unsigned char)(len & 0xff);
    dbuf_append(out, (const char *)hdr, sizeof(hdr));
    for (i = 0; i < nargs; i++) {
        /* class 1 (data) << 24 | size, then the data padded to 4 bytes */
        l2p_put32(w, (1UL << 24) | (args[i].len & 0xffffffUL));
        dbuf_append(out, (const char *)w, 4);
        if (args[i].len > 0)
            dbuf_append(out, (const char *)args[i].data, (size_t)args[i].len);
        dbuf_append(out, zero, (size_t)((4 - (args[i].len & 3)) & 3));
    }
}

int l2p_router_unpack(const unsigned char *body, unsigned long len,
                      L2PArg *args, int max)
{
    unsigned long p = 0, w, size;
    int n = 0;

    /* (like SGI's RouterUnpack, never read an argument in the last 4 bytes) */
    while (p + 4 < len && n < max) {
        w = l2p_get32(body + p);
        p += 4;
        if ((w >> 24) == 0) {
            /* a value, in the sender's host order: i386, little-endian */
            if (p + 4 > len) break;
            args[n].isval = 1;
            args[n].val = (unsigned long)body[p] | ((unsigned long)body[p + 1] << 8) |
                          ((unsigned long)body[p + 2] << 16) |
                          ((unsigned long)body[p + 3] << 24);
            args[n].data = NULL;
            args[n].len = 0;
            p += 4;
        } else {
            size = w & 0xffffffUL;
            if (size > len - p) break;
            args[n].isval = 0;
            args[n].val = 0;
            args[n].data = body + p;
            args[n].len = size;
            p += (size + 3) & ~3UL;
        }
        n++;
    }
    return n;
}

int l2p_uncompress(const unsigned char *body, unsigned long len, DBuf *out)
{
    unsigned long size, i = 4, n;
    unsigned char c;

    dbuf_clear(out);
    if (len < 4) return -1;
    size = l2p_get32(body);
    if (size > L2P_MAX_PACKET) return -1;
    while (i < len && out->len < size) {
        c = body[i++];
        n = 1;
        if (c == 0xfe) {
            if (i + 2 > len) return -1;
            n = body[i];
            c = body[i + 1];
            i += 2;
        } else if (c == 0xfd) {
            if (i + 3 > len) return -1;
            n = l2p_get16(body + i);
            c = body[i + 2];
            i += 3;
        }
        while (n-- > 0 && out->len < size) dbuf_appendc(out, (char)c);
    }
    return out->len == size ? 0 : -1;
}

void l2p_router_id(DBuf *out, const char *procname, const char *user)
{
    char name[17], usr[9];
    static const char zero[5] = { 0, 0, 0, 0, 0 };

    s_copy(name, procname ? procname : "", sizeof(name));
    s_copy(usr, user ? user : "", sizeof(usr));
    dbuf_append(out, name, strlen(name) + 1);
    dbuf_append(out, usr, strlen(usr) + 1);
    /* empty full name, then IPv4 address 0 */
    dbuf_append(out, zero, 5);
}

/* ------------------------------------------------------------------ */
/* IRouter packets                                                     */
/* ------------------------------------------------------------------ */

static void put_irarg(unsigned char *p, int clas, int frame, unsigned int size,
                      unsigned long val)
{
    p[0] = (unsigned char)clas;
    p[1] = (unsigned char)frame;
    l2p_put16(p + 2, size);
    l2p_put32(p + 4, val);
}

int l2p_ir_request(DBuf *out, unsigned int seq, unsigned long dest,
                   unsigned long src, const char *text)
{
    /* the 16 byte prefix + frame; byte 6 of the prefix: big-endian frame */
    unsigned char buf[L2P_FRAME_PREFIX + L2P_MAX_REQ_FRAME];
    unsigned char *f = buf + L2P_FRAME_PREFIX;
    unsigned long tlen = text ? strlen(text) + 1 : 0;
    int argc = text ? 2 : 1;
    unsigned long size = L2P_FRAME_HDR + 8UL * (unsigned long)argc + tlen;
    L2PArg ra[2];

    if (size > L2P_MAX_REQ_FRAME) return -1;
    memset(buf, 0, sizeof(buf));
    buf[6] = 1;

    l2p_put16(f, (unsigned int)size);
    f[2] = 2;                           /* revision */
    f[3] = 0;                           /* error */
    f[4] = (unsigned char)(seq & 0xff);
    f[5] = 0x81;                        /* first of one frame */
    f[6] = L2P_IR_REQ;
    f[7] = 0;                           /* pdata / authority */
    l2p_put32(f + 8, dest);
    l2p_put32(f + 12, src);
    f[16] = (unsigned char)argc;
    f[17] = (unsigned char)((seq >> 8) & 0xff);
    l2p_put16(f + 18, 0);               /* opcode */
    put_irarg(f + L2P_FRAME_HDR, L2P_ARG_VALUE, 0, 0, 0);
    if (text) {
        /* a data argument's value is its offset in the frame + 16 */
        put_irarg(f + L2P_FRAME_HDR + 8, L2P_ARG_STRING, 1, (unsigned int)tlen,
                  L2P_FRAME_HDR + 16UL + L2P_FRAME_PREFIX);
        memcpy(f + L2P_FRAME_HDR + 16, text, (size_t)tlen);
    }

    ra[0].isval = 0;
    ra[0].data = (const unsigned char *)"IR";
    ra[0].len = 3;
    ra[1].isval = 0;
    ra[1].data = buf;
    ra[1].len = L2P_FRAME_PREFIX + size;
    l2p_router_pack(out, 0, L2P_EVENT, ra, 2);
    return 0;
}

void l2p_packet_free(L2PPacket *p)
{
    int i;
    for (i = 0; i < p->argc; i++) {
        free(p->args[i].data);
        p->args[i].data = NULL;
    }
    p->argc = 0;
}

int l2p_ir_decode(const L2PArg *args, int nargs, L2PPacket *p)
{
    const unsigned char *f;
    unsigned long flen, size, start, got, take;
    int i, k, nframes, clas, fno;

    memset(p, 0, sizeof(*p));
    if (nargs < 2 || args[0].isval || args[0].len < 3 ||
        memcmp(args[0].data, "IR", 3) != 0)
        return 1;
    nframes = nargs - 1;
    for (k = 1; k < nargs; k++)
        if (args[k].isval || args[k].len < L2P_FRAME_PREFIX + L2P_FRAME_HDR) return -1;

    /* the first frame, after its 16 byte prefix */
    f = args[1].data + L2P_FRAME_PREFIX;
    flen = args[1].len - L2P_FRAME_PREFIX;
    p->rev = f[2];
    p->err = f[3];
    p->seq = ((unsigned int)f[17] << 8) | f[4];
    p->clas = f[6];
    p->pdata = f[7] >> 3;
    p->dest = l2p_get32(f + 8);
    p->src = l2p_get32(f + 12);
    p->opcode = l2p_get16(f + 18);
    if ((unsigned long)L2P_FRAME_HDR + 8UL * f[16] > flen) return -1;

    for (i = 0; i < f[16]; i++) {
        const unsigned char *a = f + L2P_FRAME_HDR + 8 * i;
        L2PIrArg *ia = &p->args[i];
        clas = a[0] & 7;
        fno = a[1];
        size = l2p_get16(a + 2);
        ia->val = l2p_get32(a + 4);
        p->argc = i + 1;
        if (clas == L2P_ARG_VALUE) {
            ia->type = L2P_ARG_VALUE;
            continue;
        }
        ia->type = clas == L2P_ARG_STRING ? L2P_ARG_STRING : L2P_ARG_BYTES;
        ia->size = size;
        ia->data = (char *)xmalloc((size_t)size + 1);
        /*
         * size bytes from frame fno at offset val - 16, running on into
         * the following frames after their 20 byte header
         */
        if (fno < 1 || fno > nframes || ia->val < L2P_FRAME_PREFIX) goto bad;
        k = fno;
        start = ia->val - L2P_FRAME_PREFIX;
        got = 0;
        while (got < size) {
            if (k > nframes) goto bad;
            f = args[k].data + L2P_FRAME_PREFIX;
            flen = args[k].len - L2P_FRAME_PREFIX;
            if (start > flen) goto bad;
            take = flen - start;
            if (take > size - got) take = size - got;
            memcpy(ia->data + got, f + start, (size_t)take);
            got += take;
            k++;
            start = L2P_FRAME_HDR;
        }
        ia->data[size] = '\0';
    }
    return 0;

bad:
    l2p_packet_free(p);
    return -1;
}

char *l2p_cmd_output(const L2PPacket *p, long *status)
{
    DBuf out;
    char *res;
    size_t n;
    unsigned long v;
    int i;

    if (status) {
        *status = 0;
        if (p->argc > 0 && p->args[0].type == L2P_ARG_VALUE) {
            v = p->args[0].val & 0xffffffffUL;
            *status = (v & 0x80000000UL) ? -(long)((~v & 0xffffffffUL) + 1) : (long)v;
        }
    }
    dbuf_init(&out);
    for (i = 0; i < p->argc; i++) {
        if (p->args[i].type != L2P_ARG_STRING) continue;
        dbuf_appends(&out, p->args[i].data);
        n = strlen(p->args[i].data);
        if (n == 0 || p->args[i].data[n - 1] != '\n') dbuf_appendc(&out, '\n');
    }
    res = s_dup(out.data);
    dbuf_free(&out);
    return res;
}

static const char *ir_errors[] = {
    "no error",
    "bad packet type",
    "bad task ID",
    "duplicate target ID",
    "malloc failed",
    "duplicate task ID",
    "bad event entry",
    "bad argument list argument count",
    "no receiver for packet",
    "no device to read/write",
    "attempt to send to full queue",
    "queue is full",
    "read failed - read error",
    "read failed - not first frame",
    "read failed - unexpected frame",
    "read failed - select() error",
    "read timeout on multiframe packet",
    "write failed",
    "sequence ID mismatch",
    "wait further for response",
    "timeout waiting for packet",
    "read failed - end of file",
    "write failed - end of file",
    "framing error, frame incomplete",
    "framing error, frame overflow",
    "framing error, frame underflow",
    "framing error, frame control",
    "response packet is too large"
};

const char *l2p_err_text(int err)
{
    if (err < 0 || err >= (int)(sizeof(ir_errors) / sizeof(ir_errors[0])))
        return "unknown error";
    return ir_errors[err];
}
