/*
 * l2net.c - connection to the SGI L2 controller's client port (see l2net.h)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#ifdef __sgi
#include <bstring.h>    /* FD_ZERO uses bzero on IRIX */
#endif

#include "l2net.h"
#include "l2proto.h"
#include "util.h"

#ifndef INADDR_NONE
#define INADDR_NONE 0xffffffffU
#endif

#ifdef __sgi
typedef int l2n_socklen_t;
#else
typedef socklen_t l2n_socklen_t;
#endif

/* CLIENT_ID process ID on port 8000: the events we want (none, as l2cmd) */
#define EVENT_MASK  0
#define MAX_ARGS    (L2P_MAX_ARGS + 2)

static int           l2_sock = -1;
static DBuf          rbuf;              /* received, not yet processed */
static size_t        rpos;
static DBuf          body;              /* the current packet's body */
static unsigned int  next_seq;
static int           inflight;          /* waiting for the response to seq */
static unsigned int  inflight_seq;
static int           resp_ready;
static int           rx_since;
static L2PPacket     rx_pkt, resp_pkt;
static L2PArg        rargs[MAX_ARGS];

static long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long)tv.tv_sec * 1000L + (long)(tv.tv_usec / 1000);
}

static int write_all(const char *data, size_t len)
{
    fd_set wfds;
    struct timeval tv;
    ssize_t n;

    if (l2_sock < 0) return -1;
    while (len > 0) {
        n = write(l2_sock, data, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                FD_ZERO(&wfds);
                FD_SET(l2_sock, &wfds);
                tv.tv_sec = 5;
                tv.tv_usec = 0;
                if (select(l2_sock + 1, NULL, &wfds, NULL, &tv) <= 0) return -1;
                continue;
            }
            return -1;
        }
        data += n;
        len -= (size_t)n;
    }
    return 0;
}

static int send_packet(unsigned int opcode, int clas, const L2PArg *args, int nargs)
{
    DBuf out;
    int rc;
    dbuf_init(&out);
    l2p_router_pack(&out, opcode, clas, args, nargs);
    rc = write_all(out.data, out.len);
    dbuf_free(&out);
    return rc;
}

static void data_arg(L2PArg *a, const void *data, unsigned long len)
{
    a->isval = 0;
    a->val = 0;
    a->data = (const unsigned char *)data;
    a->len = len;
}

/* read everything currently available; returns bytes read or -1 on EOF */
static int read_available(void)
{
    char buf[4096];
    ssize_t n;
    int total = 0;

    if (l2_sock < 0) return -1;
    for (;;) {
        n = read(l2_sock, buf, sizeof(buf));
        if (n > 0) {
            dbuf_append(&rbuf, buf, (size_t)n);
            total += (int)n;
            rx_since = 1;
            continue;
        }
        if (n == 0) return -1;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return total;
        return -1;
    }
}

/* the header of the next packet, if 8 bytes are there */
static int peek_header(unsigned int *opcode, int *clas)
{
    const unsigned char *h = (const unsigned char *)rbuf.data + rpos;
    if (rbuf.len - rpos < L2P_HDR) return 0;
    *opcode = l2p_get16(h);
    *clas = h[2];
    return 1;
}

/*
 * Take the next complete packet out of rbuf into body (uncompressed).
 * returns 1, 0 if incomplete, -1 if it can't be decoded.
 */
static int next_packet(unsigned int *opcode, int *clas)
{
    const unsigned char *h;
    unsigned long len;
    int config, rc = 1;

    if (!peek_header(opcode, clas)) return 0;
    h = (const unsigned char *)rbuf.data + rpos;
    config = h[4];
    len = ((unsigned long)h[5] << 16) | ((unsigned long)h[6] << 8) | h[7];
    if (rbuf.len - rpos < L2P_HDR + len) return 0;
    if (config & L2P_COMPRESSED) {
        if (l2p_uncompress(h + L2P_HDR, len, &body) < 0) rc = -1;
    } else {
        dbuf_clear(&body);
        dbuf_append(&body, (const char *)h + L2P_HDR, (size_t)len);
    }
    rpos += L2P_HDR + len;
    if (rpos == rbuf.len) {
        dbuf_clear(&rbuf);
        rpos = 0;
    } else if (rpos > 65536) {
        memmove(rbuf.data, rbuf.data + rpos, rbuf.len - rpos);
        rbuf.len -= rpos;
        rbuf.data[rbuf.len] = '\0';
        rpos = 0;
    }
    return rc;
}

static int unpack_body(void)
{
    return l2p_router_unpack((const unsigned char *)body.data, (unsigned long)body.len,
                             rargs, MAX_ARGS);
}

/* an EVENT from the L2: an IRouter response */
static void handle_event(void)
{
    int n = unpack_body();
    unsigned int seq;

    if (l2p_ir_decode(rargs, n, &rx_pkt) != 0) return;
    if (rx_pkt.clas == L2P_IR_RSP && inflight) {
        /* (revision 0 packets carry only 8 bits of sequence number) */
        seq = rx_pkt.rev == 0 ? (inflight_seq & 0xff) : inflight_seq;
        if (rx_pkt.seq == seq) {
            l2p_packet_free(&resp_pkt);
            memcpy(&resp_pkt, &rx_pkt, sizeof(resp_pkt));
            rx_pkt.argc = 0;            /* the arguments moved to resp_pkt */
            inflight = 0;
            resp_ready = 1;
            return;
        }
    }
    l2p_packet_free(&rx_pkt);
}

/* process every complete packet; returns -1 if the L2 closes the session */
static int process_input(void)
{
    unsigned int opcode;
    int clas, rc;

    while ((rc = next_packet(&opcode, &clas)) != 0) {
        if (rc < 0) continue;           /* undecodable compressed body */
        if (clas == L2P_EVENT) {
            handle_event();
        } else if (clas == L2P_DISCONNECT_REQ) {
            send_packet(0, L2P_DISCONNECT_RSP, NULL, 0);
            return -1;
        } else if (clas == L2P_DISCONNECT_RSP || clas == L2P_DENIED) {
            return -1;
        }
        /* PING, REQUEST, RESPONSE, ...: not used between an L2 and a client */
    }
    return 0;
}

static int wait_readable(long deadline)
{
    fd_set rfds;
    struct timeval tv;
    long left;
    int rc;

    for (;;) {
        left = deadline - now_ms();
        if (left <= 0) return 0;
        FD_ZERO(&rfds);
        FD_SET(l2_sock, &rfds);
        tv.tv_sec = left / 1000;
        tv.tv_usec = (left % 1000) * 1000;
        rc = select(l2_sock + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0 && errno == EINTR) continue;
        return rc;
    }
}

static int tcp_connect(const char *host, int port, int timeout_ms, char *err, size_t errlen)
{
    struct sockaddr_in sa;
    struct hostent *he;
    fd_set wfds;
    struct timeval tv;
    int flags, rc, soerr;
    l2n_socklen_t slen;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = inet_addr(host);
    if (sa.sin_addr.s_addr == (unsigned)INADDR_NONE) {
        he = gethostbyname(host);
        if (!he || he->h_addrtype != AF_INET) {
            s_copy(err, "Unknown host: ", errlen);
            s_cat(err, host, errlen);
            return -1;
        }
        memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof(sa.sin_addr));
    }

    l2_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (l2_sock < 0) {
        s_copy(err, strerror(errno), errlen);
        return -1;
    }
    flags = fcntl(l2_sock, F_GETFL, 0);
    fcntl(l2_sock, F_SETFL, flags | O_NONBLOCK);

    rc = connect(l2_sock, (struct sockaddr *)&sa, sizeof(sa));
    if (rc < 0 && errno != EINPROGRESS) {
        s_copy(err, strerror(errno), errlen);
        return -1;
    }
    if (rc < 0) {
        FD_ZERO(&wfds);
        FD_SET(l2_sock, &wfds);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        rc = select(l2_sock + 1, NULL, &wfds, NULL, &tv);
        if (rc <= 0) {
            s_copy(err, rc == 0 ? "Connection timed out" : strerror(errno), errlen);
            return -1;
        }
        soerr = 0;
        slen = (l2n_socklen_t)sizeof(soerr);
        getsockopt(l2_sock, SOL_SOCKET, SO_ERROR, (char *)&soerr, &slen);
        if (soerr != 0) {
            s_copy(err, strerror(soerr), errlen);
            return -1;
        }
    }
    return 0;
}

static void not_an_l2(int port, char *err, size_t errlen)
{
    char msg[160];
    sprintf(msg, "No L2 client protocol on port %d (the L2's is port 8000)", port);
    s_copy(err, msg, errlen);
}

/*
 * The Router handshake (PROTOCOL.md, "Connecting"): ACCEPT (or
 * PASSWORD_REQ / DENIED) from the L2, our CLIENT_ID, then CONFIG_INIT,
 * echoed as CONFIG_REQ, and CONFIG_RSP.
 */
static int handshake(int port, long deadline, char *err, size_t errlen)
{
    static const unsigned char client_type[8] = { 0, 0, 0x40, 0, 0, 0, 0, 0 };
    unsigned int opcode;
    int clas, rc, accepted = 0, n;
    DBuf id;
    L2PArg a[2];
    const char *user;

    for (;;) {
        /* check the first header early: a telnet or console port isn't an L2 */
        if (!accepted && peek_header(&opcode, &clas) &&
            (opcode != L2P_SERVER_PID ||
             (clas != L2P_ACCEPT && clas != L2P_PASSWORD_REQ && clas != L2P_DENIED))) {
            not_an_l2(port, err, errlen);
            return -1;
        }
        rc = next_packet(&opcode, &clas);
        if (rc < 0) {
            not_an_l2(port, err, errlen);
            return -1;
        }
        if (rc == 0) {
            rc = wait_readable(deadline);
            if (rc <= 0) {
                s_copy(err, rc == 0 ? "Timeout in the L2 handshake" : strerror(errno),
                       errlen);
                return -1;
            }
            if (read_available() < 0) {
                s_copy(err, "Connection closed by L2", errlen);
                return -1;
            }
            continue;
        }
        if (clas == L2P_DENIED || (!accepted && clas == L2P_PASSWORD_REQ)) {
            s_copy(err, "Connection refused by L2", errlen);
            return -1;
        }
        if (clas == L2P_DISCONNECT_REQ) {
            send_packet(0, L2P_DISCONNECT_RSP, NULL, 0);
            s_copy(err, "Connection closed by L2", errlen);
            return -1;
        }
        if (!accepted) {
            /* ACCEPT: who we are */
            accepted = 1;
            user = getenv("USER");
            if (!user || !*user) user = getenv("LOGNAME");
            dbuf_init(&id);
            l2p_router_id(&id, "sgil2gui", user ? user : "sgil2gui");
            data_arg(&a[0], client_type, sizeof(client_type));
            data_arg(&a[1], id.data, (unsigned long)id.len);
            rc = send_packet(EVENT_MASK, L2P_CLIENT_ID, a, 2);
            dbuf_free(&id);
            if (rc < 0) break;
            continue;
        }
        if (clas == L2P_CONFIG_INIT) {
            n = unpack_body();
            if (n < 1 || rargs[0].isval) {
                s_copy(err, "Bad configuration packet from L2", errlen);
                return -1;
            }
            if (send_packet(0, L2P_CONFIG_REQ, rargs, 1) < 0) break;
        } else if (clas == L2P_CONFIG_RSP) {
            return 0;
        }
    }
    s_copy(err, "Connection closed by L2", errlen);
    return -1;
}

int l2n_connect(const char *host, int port, int timeout_ms, char *err, size_t errlen)
{
    static int sigpipe_ignored = 0;
    long deadline = now_ms() + timeout_ms;

    l2n_close();
    if (!sigpipe_ignored) {
        /* a write to a dropped connection must not kill the GUI */
        signal(SIGPIPE, SIG_IGN);
        sigpipe_ignored = 1;
    }
    if (!host || !*host) {
        s_copy(err, "No L2 host configured", errlen);
        return -1;
    }
    if (!rbuf.data) dbuf_init(&rbuf);
    if (!body.data) dbuf_init(&body);
    dbuf_clear(&rbuf);
    rpos = 0;
    inflight = 0;
    resp_ready = 0;
    next_seq = 0;

    if (tcp_connect(host, port, timeout_ms, err, errlen) < 0 ||
        handshake(port, deadline, err, errlen) < 0) {
        l2n_close();
        return -1;
    }
    return 0;
}

void l2n_close(void)
{
    if (l2_sock >= 0) close(l2_sock);
    l2_sock = -1;
    inflight = 0;
}

int l2n_connected(void)
{
    return l2_sock >= 0;
}

int l2n_fd(void)
{
    return l2_sock;
}

int l2n_begin(const char *cmd)
{
    DBuf out;
    int rc;

    if (l2_sock < 0) return L2N_EOF;
    /* take in what came meanwhile (events, a dropped connection) */
    if (read_available() < 0 || process_input() < 0) {
        l2n_close();
        return L2N_EOF;
    }
    resp_ready = 0;
    rx_since = 0;
    inflight_seq = next_seq;
    next_seq = (next_seq + 1) & 0xffff;

    dbuf_init(&out);
    rc = l2p_ir_request(&out, inflight_seq, L2P_ADDR_L2_CMD, L2P_ADDR_L3, cmd);
    if (rc == 0) rc = write_all(out.data, out.len);
    dbuf_free(&out);
    if (rc < 0) {
        l2n_close();
        return L2N_EOF;
    }
    if (strstr(cmd, "reboot")) {
        /* the L2 never answers a reboot: an empty success */
        l2p_packet_free(&resp_pkt);
        memset(&resp_pkt, 0, sizeof(resp_pkt));
        resp_ready = 1;
        return L2N_DONE;
    }
    inflight = 1;
    return L2N_AGAIN;
}

int l2n_pump(void)
{
    int eof;

    if (l2_sock < 0) return resp_ready ? L2N_DONE : L2N_EOF;
    eof = read_available() < 0;
    if (process_input() < 0) eof = 1;
    if (eof) l2n_close();
    if (resp_ready) return L2N_DONE;
    return eof ? L2N_EOF : L2N_AGAIN;
}

int l2n_rx_since_begin(void)
{
    return rx_since;
}

char *l2n_take_response(int *irerr)
{
    char *text;
    long status;

    if (irerr) *irerr = 0;
    if (!resp_ready) return s_dup("");
    resp_ready = 0;
    if (resp_pkt.err) {
        if (irerr) *irerr = resp_pkt.err;
        l2p_packet_free(&resp_pkt);
        return s_dup("");
    }
    text = l2p_cmd_output(&resp_pkt, &status);
    l2p_packet_free(&resp_pkt);
    /* some L2s (NL4R) send stray control bytes */
    s_strip_nonprint(text);
    return text;
}

int l2n_wait(int timeout_ms)
{
    long deadline = now_ms() + timeout_ms;
    int rc;

    for (;;) {
        rc = l2n_pump();
        if (rc != L2N_AGAIN) return rc;
        rc = wait_readable(deadline);
        if (rc == 0) return L2N_TIMEOUT;
        if (rc < 0) return L2N_EOF;
    }
}

int l2n_exec(const char *cmd, int timeout_ms, char **out, char *err, size_t errlen)
{
    int rc, irerr;
    char *text;
    char msg[160];

    *out = NULL;
    rc = l2n_begin(cmd);
    if (rc == L2N_AGAIN) rc = l2n_wait(timeout_ms);
    if (rc == L2N_TIMEOUT) {
        l2n_close();
        s_copy(err, "Timeout waiting for the L2", errlen);
        return -1;
    }
    if (rc != L2N_DONE) {
        s_copy(err, "Connection to L2 lost", errlen);
        return -1;
    }
    text = l2n_take_response(&irerr);
    if (irerr) {
        free(text);
        sprintf(msg, "L2 error %d: %s", irerr, l2p_err_text(irerr));
        s_copy(err, msg, errlen);
        return -1;
    }
    *out = text;
    return 0;
}
