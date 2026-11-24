/*
 * l2client.c - asynchronous L2 command queue on top of l2net.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Intrinsic.h>

#include "l2client.h"
#include "l2net.h"
#include "l2proto.h"
#include "demo.h"
#include "util.h"

typedef struct Job {
    char       *cmd;
    L2Done      cb;
    void       *ud;
    int         retried;
    struct Job *next;
} Job;

static XtAppContext     app;
static const AppConfig *cfg;
static int              demo;
static L2StatusFn       status_fn;

static Job         *head, *tail;
static int          running;        /* head job has been sent */
static XtInputId    input_id;
static int          input_active;
static XtIntervalId timer_id;
static int          timer_active;
static int          dispatch_scheduled;

static void dispatch(void);

static void status(const char *msg, int busy)
{
    if (status_fn) status_fn(msg, busy);
}

static void remove_timer(void)
{
    if (timer_active) XtRemoveTimeOut(timer_id);
    timer_active = 0;
}

static void remove_input(void)
{
    if (input_active) XtRemoveInput(input_id);
    input_active = 0;
}

static Job *pop(void)
{
    Job *j = head;
    if (j) {
        head = j->next;
        if (!head) tail = NULL;
    }
    running = 0;
    return j;
}

static void finish(int ok, const char *resp)
{
    Job *j = pop();
    remove_timer();
    /* status first, so the callback can replace the message */
    if (!head) status(ok ? "Ready" : resp, 0);
    if (j) {
        if (j->cb) j->cb(ok, resp, j->ud);
        free(j->cmd);
        free(j);
    }
    dispatch();
}

static void fail_all(const char *err)
{
    Job *j;
    remove_timer();
    while ((j = pop()) != NULL) {
        if (j->cb) j->cb(0, err, j->ud);
        free(j->cmd);
        free(j);
    }
    status(err, 0);
}

static void dead_connection(void)
{
    remove_input();
    l2n_close();
}

static void on_input(XtPointer cd, int *fd, XtInputId *id)
{
    int rc, irerr;
    char *resp, msg[200];
    (void)cd;
    (void)fd;
    (void)id;

    rc = l2n_pump();
    if (!running) {
        /* idle: the L2 closed the session */
        if (rc == L2N_EOF || !l2n_connected()) {
            dead_connection();
            status("L2 closed the idle connection (reconnects on demand)", 0);
        }
        return;
    }
    if (rc == L2N_AGAIN) return;
    if (rc == L2N_DONE) {
        resp = l2n_take_response(&irerr);
        if (!l2n_connected()) dead_connection();
        if (irerr) {
            sprintf(msg, "L2 error %d: %s", irerr, l2p_err_text(irerr));
            finish(0, msg);
        } else {
            finish(1, resp);
        }
        free(resp);
        return;
    }
    /* EOF while running: retry once on a fresh connection */
    dead_connection();
    remove_timer();
    if (head && !head->retried && !l2n_rx_since_begin()) {
        head->retried = 1;
        running = 0;
        dispatch();
        return;
    }
    finish(0, "Connection to L2 lost");
}

static void on_timeout(XtPointer cd, XtIntervalId *id)
{
    (void)cd;
    (void)id;
    timer_active = 0;
    if (!running) return;
    /* the session is out of sync now - start fresh next time */
    dead_connection();
    finish(0, "Timeout waiting for the L2");
}

static int ensure_connected(void)
{
    char err[256], msg[400];
    if (l2n_connected()) return 1;

    sprintf(msg, "Connecting to %.200s ...", cfg->host);
    status(msg, 1);
    if (l2n_connect(cfg->host, cfg->port, 5000, err, sizeof(err)) < 0) {
        sprintf(msg, "Connect to %.100s failed: %.250s", cfg->host, err);
        fail_all(msg);
        return 0;
    }
    input_id = XtAppAddInput(app, l2n_fd(), (XtPointer)XtInputReadMask, on_input, NULL);
    input_active = 1;
    sprintf(msg, "Connected to %.200s", cfg->host);
    status(msg, 1);
    return 1;
}

/* demo mode: answer from the simulator after a short delay */
static void demo_reply(XtPointer cd, XtIntervalId *id)
{
    char *resp;
    (void)cd;
    (void)id;
    timer_active = 0;
    if (!head) return;
    resp = demo_exec(head->cmd);
    finish(1, resp);
    free(resp);
}

static void do_dispatch(XtPointer cd, XtIntervalId *id)
{
    char msg[300], *resp;
    int rc, irerr;
    (void)cd;
    (void)id;
    dispatch_scheduled = 0;
    if (running || !head) return;

    if (demo) {
        running = 1;
        sprintf(msg, "L2 (demo): %.250s", head->cmd);
        status(msg, 1);
        timer_id = XtAppAddTimeOut(app, 120, demo_reply, NULL);
        timer_active = 1;
        return;
    }
    if (!ensure_connected()) return;
    sprintf(msg, "L2: %.250s", head->cmd);
    status(msg, 1);
    running = 1;
    rc = l2n_begin(head->cmd);
    if (rc == L2N_EOF) {
        dead_connection();
        running = 0;
        if (!head->retried) {
            head->retried = 1;
            dispatch();
        } else {
            finish(0, "Could not send command to L2");
        }
        return;
    }
    if (rc == L2N_DONE) {           /* no response will come (reboot) */
        resp = l2n_take_response(&irerr);
        finish(1, resp);
        free(resp);
        return;
    }
    timer_id = XtAppAddTimeOut(app, (unsigned long)cfg->timeout * 1000UL, on_timeout, NULL);
    timer_active = 1;
}

/* run from the event loop so callbacks never nest */
static void dispatch(void)
{
    if (dispatch_scheduled || running || !head) return;
    dispatch_scheduled = 1;
    XtAppAddTimeOut(app, 0, do_dispatch, NULL);
}

void l2c_init(XtAppContext a, const AppConfig *c, int d, L2StatusFn fn)
{
    app = a;
    cfg = c;
    demo = d;
    status_fn = fn;
    if (demo) demo_init();
}

void l2c_exec(const char *cmd, L2Done cb, void *ud)
{
    Job *j = (Job *)xmalloc(sizeof(Job));
    j->cmd = s_dup(cmd);
    j->cb = cb;
    j->ud = ud;
    j->retried = 0;
    j->next = NULL;
    if (tail) tail->next = j;
    else head = j;
    tail = j;
    dispatch();
}

int l2c_pending(void)
{
    return head != NULL;
}

int l2c_connected(void)
{
    return demo || l2n_connected();
}

void l2c_disconnect(void)
{
    if (demo) return;
    if (running) {
        dead_connection();
        finish(0, "Disconnected");
    }
    dead_connection();
    status("Disconnected", 0);
}

const char *l2c_prompt(void)
{
    return demo ? "L2-demo>" : "L2>";
}
