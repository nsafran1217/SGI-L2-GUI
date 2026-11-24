/*
 * l2client.h - asynchronous L2 command queue on top of l2net.c
 *
 * Commands are queued and executed one after another on the single L2
 * connection. Socket input is handled with XtAppAddInput so the UI
 * never blocks while waiting for the L2's response. The connection is opened
 * on demand and re-opened transparently if the L2 dropped it.
 */
#ifndef SGIL2_L2CLIENT_H
#define SGIL2_L2CLIENT_H

#include <X11/Intrinsic.h>
#include "config.h"

/* ok = 1 and resp = cleaned response text, or ok = 0 and resp = error */
typedef void (*L2Done)(int ok, const char *resp, void *ud);
typedef void (*L2StatusFn)(const char *msg, int busy);

void l2c_init(XtAppContext app, const AppConfig *cfg, int demo, L2StatusFn status);
void l2c_exec(const char *cmd, L2Done cb, void *ud);
int  l2c_pending(void);
int  l2c_connected(void);
void l2c_disconnect(void);
const char *l2c_prompt(void);

#endif
