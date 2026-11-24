/*
 * l2proto.h - the L2's client protocol: Router packets carrying IRouter
 *             packets (encode / decode only, no sockets)
 *
 * This is how SGI's l2cmd talks to an L2 on TCP port 8000. A connection
 * carries Router packets: an 8 byte header (opcode, class, id, config,
 * 24 bit length) and a list of arguments. After a short handshake both
 * sides send only Router EVENTs, each carrying one IRouter packet: the
 * argument "IR\0", then one argument per IRouter frame, each a 16 byte
 * prefix followed by the big-endian frame.
 *
 * A text command is an IRouter request to the L2's command task
 * (0x1ffff003) with two arguments, the value 0 and the command line. The
 * response carries a status value and the output as strings.
 *
 * Everything is packed and unpacked a byte at a time, so the code does not
 * depend on the host's byte order (IRIX is big-endian, x86 little-endian).
 */
#ifndef SGIL2_L2PROTO_H
#define SGIL2_L2PROTO_H

#include "util.h"

/* Router packet classes */
#define L2P_PASSWORD_REQ    0x01
#define L2P_ACCEPT          0x04
#define L2P_CLIENT_ID       0x08
#define L2P_REQUEST         0x10
#define L2P_DISCONNECT_REQ  0x11
#define L2P_DISCONNECT_RSP  0x12
#define L2P_CONFIG_INIT     0x14
#define L2P_CONFIG_REQ      0x18
#define L2P_CONFIG_RSP      0x21
#define L2P_DENIED          0x22
#define L2P_PING            0x24
#define L2P_RESPONSE        0x40
#define L2P_EVENT           0x80

#define L2P_SERVER_PID      2       /* an L2's Router process ID */
#define L2P_COMPRESSED      0x80    /* header config bit */
#define L2P_HDR             8       /* Router header size */
#define L2P_MAX_PACKET      0xffffffUL

/* IRouter */
#define L2P_IR_REQ          0
#define L2P_IR_RSP          1
#define L2P_IR_EVT          2
#define L2P_ARG_VALUE       0
#define L2P_ARG_BYTES       1
#define L2P_ARG_STRING      2
#define L2P_FRAME_HDR       20
#define L2P_FRAME_PREFIX    16
#define L2P_MAX_REQ_FRAME   1024
#define L2P_MAX_ARGS        255

#define L2P_ADDR_L2_CMD     0x1ffff003UL    /* the local L2's command task */
#define L2P_ADDR_L3         0x2ffff000UL    /* source address of L3 clients */

/* a Router argument: data (pointer into the packet) or a value */
typedef struct {
    int                  isval;
    unsigned long        val;
    const unsigned char *data;
    unsigned long        len;
} L2PArg;

/* an IRouter argument. data is malloc'd and always NUL terminated */
typedef struct {
    int            type;            /* L2P_ARG_* */
    unsigned long  val;
    char          *data;
    unsigned long  size;
} L2PIrArg;

typedef struct {
    int            rev;
    int            err;             /* IRouter error code, 0 = none */
    unsigned int   seq;
    int            clas;            /* L2P_IR_* */
    int            pdata;
    unsigned long  dest;
    unsigned long  src;
    unsigned int   opcode;
    int            argc;
    L2PIrArg       args[L2P_MAX_ARGS];
} L2PPacket;

/* big-endian helpers */
unsigned long l2p_get32(const unsigned char *p);
unsigned int  l2p_get16(const unsigned char *p);
void          l2p_put32(unsigned char *p, unsigned long v);
void          l2p_put16(unsigned char *p, unsigned int v);

/* append a Router packet whose arguments are all data */
void l2p_router_pack(DBuf *out, unsigned int opcode, int clas,
                     const L2PArg *args, int nargs);
/* split a Router body into arguments (pointing into body); returns count */
int  l2p_router_unpack(const unsigned char *body, unsigned long len,
                       L2PArg *args, int max);
/* undo the Router run-length compression; returns 0 or -1 if malformed */
int  l2p_uncompress(const unsigned char *body, unsigned long len, DBuf *out);

/* our RouterID: process name, user name, full name, IPv4 address */
void l2p_router_id(DBuf *out, const char *procname, const char *user);

/*
 * Append the Router EVENT carrying a one-frame IRouter request: the value
 * 0, then text as a string (text NULL: the value 0 alone, as a
 * configuration query). Returns 0, or -1 if the frame would be too big.
 */
int  l2p_ir_request(DBuf *out, unsigned int seq, unsigned long dest,
                    unsigned long src, const char *text);

/*
 * Decode the IRouter packet in the arguments of a Router EVENT.
 * Returns 0, 1 if the event is not an IRouter packet, -1 if malformed.
 * Free the result with l2p_packet_free.
 */
int  l2p_ir_decode(const L2PArg *args, int nargs, L2PPacket *p);
void l2p_packet_free(L2PPacket *p);

/*
 * A command response's output as l2cmd prints it (malloc'd): the string
 * arguments, each ended with a newline. *status: the first value
 * argument as a signed number (0 = success).
 */
char *l2p_cmd_output(const L2PPacket *p, long *status);

/* the IRouter error message for err */
const char *l2p_err_text(int err);

#endif
