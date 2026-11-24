/*
 * l2net.h - connection to the SGI L2 controller's client port
 *
 * One connection at a time to TCP port 8000 (8002: not logged by the L2),
 * as SGI's l2cmd makes it. Each command is an IRouter request to the L2's
 * command task and gets exactly its own output and a status back, so there
 * is no shell prompt to wait for (see l2proto.h).
 *
 * The API is split into begin / pump / take so it can be driven either
 * synchronously (l2n_exec, CLI mode) or from an event loop (XtAppAddInput).
 */
#ifndef SGIL2_L2NET_H
#define SGIL2_L2NET_H

#include <stddef.h>

#define L2N_AGAIN    0   /* need more data */
#define L2N_DONE     1   /* response complete */
#define L2N_EOF     -1   /* connection closed / error */
#define L2N_TIMEOUT -2

/*
 * Connect and log in: the Router handshake.
 * Blocks for at most timeout_ms. returns 0 on success.
 */
int         l2n_connect(const char *host, int port, int timeout_ms, char *err, size_t errlen);
void        l2n_close(void);
int         l2n_connected(void);
int         l2n_fd(void);

/*
 * Send one command line (no ';' splitting). Returns L2N_AGAIN when the
 * response is to be waited for, L2N_DONE when none will come (a line
 * containing "reboot": its response is an empty text), L2N_EOF on error.
 */
int         l2n_begin(const char *cmd);
/* non-blocking read of what is available; L2N_AGAIN / L2N_DONE / L2N_EOF */
int         l2n_pump(void);
/* did any data arrive since l2n_begin? */
int         l2n_rx_since_begin(void);
/*
 * malloc'd output of the completed command (the L2's error text if the
 * command failed). *irerr: an IRouter error code; then there is no output.
 */
char       *l2n_take_response(int *irerr);

/* block until L2N_DONE / L2N_EOF / L2N_TIMEOUT */
int         l2n_wait(int timeout_ms);
/*
 * Synchronous: run one command line. *out receives the malloc'd output.
 * returns 0, or -1 with err set.
 */
int         l2n_exec(const char *cmd, int timeout_ms, char **out,
                     char *err, size_t errlen);

#endif
