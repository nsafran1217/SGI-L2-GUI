/*
 * dialogs.c - Motif dialogs: brick, console, settings, text output, about
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <Xm/Xm.h>
#include <Xm/DialogS.h>
#include <Xm/Form.h>
#include <Xm/RowColumn.h>
#include <Xm/Label.h>
#include <Xm/PushB.h>
#include <Xm/ToggleB.h>
#include <Xm/Frame.h>
#include <Xm/Separator.h>
#include <Xm/Text.h>
#include <Xm/TextF.h>
#include <Xm/MessageB.h>

#include "app.h"
#include "l2client.h"
#include "rack.h"
#include "util.h"

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static XmString xms(const char *s)
{
    return XmStringCreateLtoR((char *)s, XmFONTLIST_DEFAULT_TAG);
}

static void set_label(Widget w, const char *s)
{
    XmString x = xms(s);
    XtVaSetValues(w, XmNlabelString, x, NULL);
    XmStringFree(x);
}

static Widget label(Widget parent, const char *name, const char *text)
{
    Widget w;
    XmString x = xms(text);
    w = XtVaCreateManagedWidget(name, xmLabelWidgetClass, parent,
                                XmNlabelString, x,
                                XmNalignment, XmALIGNMENT_BEGINNING,
                                NULL);
    XmStringFree(x);
    return w;
}

static Widget button(Widget parent, const char *name, const char *text,
                     XtCallbackProc cb, XtPointer cd)
{
    Widget w;
    XmString x = xms(text);
    w = XtVaCreateManagedWidget(name, xmPushButtonWidgetClass, parent,
                                XmNlabelString, x, NULL);
    XmStringFree(x);
    if (cb) XtAddCallback(w, XmNactivateCallback, cb, cd);
    return w;
}

static Widget toggle(Widget parent, const char *name, const char *text, int state)
{
    Widget w;
    XmString x = xms(text);
    w = XtVaCreateManagedWidget(name, xmToggleButtonWidgetClass, parent,
                                XmNlabelString, x,
                                XmNset, state ? True : False,
                                NULL);
    XmStringFree(x);
    return w;
}

/* form dialog with a title; destroyOnClose: destroy instead of unmap */
static Widget form_dialog(const char *name, const char *title, int destroyOnClose)
{
    Widget form;
    Arg a[4];
    int n = 0;
    XtSetArg(a[n], XmNautoUnmanage, False); n++;
    XtSetArg(a[n], XmNdialogStyle, XmDIALOG_MODELESS); n++;
    form = XmCreateFormDialog(g_toplevel, (char *)name, a, n);
    XtVaSetValues(XtParent(form),
                  XmNtitle, title,
                  XmNdeleteResponse, destroyOnClose ? XmDESTROY : XmUNMAP,
                  NULL);
    XtVaSetValues(form, XmNmarginWidth, 10, XmNmarginHeight, 10, NULL);
    return form;
}

static void destroy_shell_cb(Widget w, XtPointer cd, XtPointer cbs)
{
    Widget shell = (Widget)cd;
    (void)w;
    (void)cbs;
    XtDestroyWidget(shell);
}

static void unmanage_cb(Widget w, XtPointer cd, XtPointer cbs)
{
    (void)w;
    (void)cbs;
    XtUnmanageChild((Widget)cd);
}

/* horizontal button row attached to the bottom of a form */
static Widget button_row(Widget form)
{
    Widget sep, rc;
    rc = XtVaCreateManagedWidget("buttons", xmRowColumnWidgetClass, form,
                                 XmNorientation, XmHORIZONTAL,
                                 XmNpacking, XmPACK_TIGHT,
                                 XmNspacing, 8,
                                 XmNleftAttachment, XmATTACH_FORM,
                                 XmNrightAttachment, XmATTACH_FORM,
                                 XmNbottomAttachment, XmATTACH_FORM,
                                 NULL);
    sep = XtVaCreateManagedWidget("sep", xmSeparatorWidgetClass, form,
                                  XmNleftAttachment, XmATTACH_FORM,
                                  XmNrightAttachment, XmATTACH_FORM,
                                  XmNbottomAttachment, XmATTACH_WIDGET,
                                  XmNbottomWidget, rc,
                                  XmNbottomOffset, 6,
                                  NULL);
    XtVaSetValues(rc, XmNuserData, (XtPointer)sep, NULL);
    return rc;
}

static Widget row_separator(Widget rc)
{
    XtPointer p = NULL;
    XtVaGetValues(rc, XmNuserData, &p, NULL);
    return (Widget)p;
}

/* ------------------------------------------------------------------ */
/* message boxes                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    ConfirmFn fn;
    char      arg[64];
    int       iarg;
} ConfirmData;

static void confirm_cb(Widget w, XtPointer cd, XtPointer cbs)
{
    ConfirmData *d = (ConfirmData *)cd;
    XmAnyCallbackStruct *c = (XmAnyCallbackStruct *)cbs;
    if (c->reason == XmCR_OK && d->fn) d->fn(d->arg[0] ? d->arg : NULL, d->iarg);
    free(d);
    XtDestroyWidget(XtParent(w));
}

void dlg_confirm(const char *title, const char *msg, ConfirmFn fn, const char *arg, int iarg)
{
    Widget mb;
    XmString m;
    Arg a[4];
    int n = 0;
    ConfirmData *d = (ConfirmData *)xmalloc(sizeof(ConfirmData));

    d->fn = fn;
    s_copy(d->arg, arg ? arg : "", sizeof(d->arg));
    d->iarg = iarg;
    m = xms(msg);
    XtSetArg(a[n], XmNmessageString, m); n++;
    XtSetArg(a[n], XmNdialogStyle, XmDIALOG_FULL_APPLICATION_MODAL); n++;
    mb = XmCreateQuestionDialog(g_toplevel, "confirm", a, n);
    XmStringFree(m);
    XtVaSetValues(XtParent(mb), XmNtitle, title, NULL);
    XtUnmanageChild(XmMessageBoxGetChild(mb, XmDIALOG_HELP_BUTTON));
    XtAddCallback(mb, XmNokCallback, confirm_cb, d);
    XtAddCallback(mb, XmNcancelCallback, confirm_cb, d);
    XtManageChild(mb);
}

static void msgbox_done(Widget w, XtPointer cd, XtPointer cbs)
{
    (void)cd;
    (void)cbs;
    XtDestroyWidget(XtParent(w));
}

void dlg_error(const char *msg)
{
    Widget mb;
    XmString m = xms(msg);
    Arg a[2];
    int n = 0;
    XtSetArg(a[n], XmNmessageString, m); n++;
    mb = XmCreateErrorDialog(g_toplevel, "error", a, n);
    XmStringFree(m);
    XtVaSetValues(XtParent(mb), XmNtitle, APP_NAME " - Error", NULL);
    XtUnmanageChild(XmMessageBoxGetChild(mb, XmDIALOG_HELP_BUTTON));
    XtUnmanageChild(XmMessageBoxGetChild(mb, XmDIALOG_CANCEL_BUTTON));
    XtAddCallback(mb, XmNokCallback, msgbox_done, NULL);
    XtManageChild(mb);
}

void dlg_about(void)
{
    Widget mb;
    XmString m;
    Arg a[2];
    int n = 0;
    m = xms(APP_NAME " " APP_VERSION "\n\n"
            "IRIX / Motif front end for the\n"
            "SGI L2 system controller.\n\n"
            "flexion 2026");
    XtSetArg(a[n], XmNmessageString, m); n++;
    mb = XmCreateInformationDialog(g_toplevel, "about", a, n);
    XmStringFree(m);
    XtVaSetValues(XtParent(mb), XmNtitle, "About " APP_NAME, NULL);
    XtUnmanageChild(XmMessageBoxGetChild(mb, XmDIALOG_HELP_BUTTON));
    XtUnmanageChild(XmMessageBoxGetChild(mb, XmDIALOG_CANCEL_BUTTON));
    XtAddCallback(mb, XmNokCallback, msgbox_done, NULL);
    XtManageChild(mb);
}

/* ------------------------------------------------------------------ */
/* text output dialog                                                  */
/* ------------------------------------------------------------------ */

void dlg_text(const char *title, const char *text)
{
    Widget form, rc, txt, close;
    Arg a[12];
    int n = 0;

    form = form_dialog("textDialog", title, 1);
    rc = button_row(form);
    close = button(rc, "close", "Close", destroy_shell_cb, (XtPointer)XtParent(form));

    XtSetArg(a[n], XmNeditMode, XmMULTI_LINE_EDIT); n++;
    XtSetArg(a[n], XmNeditable, False); n++;
    XtSetArg(a[n], XmNcursorPositionVisible, False); n++;
    XtSetArg(a[n], XmNrows, 20); n++;
    XtSetArg(a[n], XmNcolumns, 80); n++;
    XtSetArg(a[n], XmNwordWrap, False); n++;
    txt = XmCreateScrolledText(form, "outputText", a, n);
    XtVaSetValues(XtParent(txt),
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_WIDGET,
                  XmNbottomWidget, row_separator(rc),
                  XmNbottomOffset, 6,
                  NULL);
    XmTextSetString(txt, (char *)(text && *text ? text : "(no output)"));
    XtManageChild(txt);
    XtVaSetValues(form, XmNdefaultButton, close, NULL);
    XtManageChild(form);
}

/* ------------------------------------------------------------------ */
/* brick dialog                                                        */
/* ------------------------------------------------------------------ */

static Widget  bd_form, bd_title, bd_info, bd_lcd1, bd_lcd2, bd_att, bd_fail;
static Widget  bd_power, bd_refresh, bd_state;
static L2Brick bd_brick;

static void bd_fill(void)
{
    char buf[512];
    const L2Brick *b = &bd_brick;
    const L2Display *d = &b->display;

    sprintf(buf, "%.15s   %.47s", b->id, b->productLabel[0] ? b->productLabel :
            (b->brickType == 'R' ? "router" : ""));
    set_label(bd_title, buf);
    sprintf(buf, "Type:       %.63s\nSerial:     %.23s\nRack/Slot:  %.7s / %.7s\nPartition:  %.15s\nL1 revision: %.15s",
            b->type[0] ? b->type : "-", b->serial[0] ? b->serial : "-",
            b->rack[0] ? b->rack : "-", b->slot[0] ? b->slot : "-",
            b->partition[0] ? b->partition : "-",
            d->revision[0] ? d->revision : "-");
    set_label(bd_info, buf);
    set_label(bd_lcd1, d->line1[0] ? d->line1 : " ");
    set_label(bd_lcd2, d->line2[0] ? d->line2 : " ");
    sprintf(buf, "Power LED: %s", d->pwr ? "ON" : "off");
    set_label(bd_state, buf);
    XmToggleButtonSetState(bd_att, d->service ? True : False, False);
    XmToggleButtonSetState(bd_fail, d->fault ? True : False, False);
    sprintf(buf, "%s %s", d->pwr ? "Power down" : "Power up", b->id);
    set_label(bd_power, buf);
    XtVaSetValues(XtParent(bd_form), XmNtitle, b->id, NULL);
}

static void bd_toggle_cb(Widget w, XtPointer cd, XtPointer cbs)
{
    XmToggleButtonCallbackStruct *t = (XmToggleButtonCallbackStruct *)cbs;
    (void)w;
    app_set_led(bd_brick.id, cd != NULL, t->set ? 1 : 0);
}

static void bd_power_cb(Widget w, XtPointer cd, XtPointer cbs)
{
    (void)w;
    (void)cd;
    (void)cbs;
    app_power_confirm(bd_brick.id, !bd_brick.display.pwr);
}

static void bd_refresh_cb(Widget w, XtPointer cd, XtPointer cbs)
{
    (void)w;
    (void)cd;
    (void)cbs;
    app_refresh_brick(bd_brick.id);
}

static void bd_unmap_cb(Widget w, XtPointer cd, XtPointer cbs)
{
    (void)w;
    (void)cd;
    (void)cbs;
    rack_set_selected(NULL);
}

static void bd_create(void)
{
    Widget rc, col, frame, lcdrc, leds;

    bd_form = form_dialog("brickDialog", "Brick", 0);
    XtAddCallback(bd_form, XmNunmapCallback, bd_unmap_cb, NULL);

    rc = button_row(bd_form);
    bd_refresh = button(rc, "refresh", "Refresh", bd_refresh_cb, NULL);
    button(rc, "close", "Close", unmanage_cb, (XtPointer)bd_form);

    col = XtVaCreateManagedWidget("content", xmRowColumnWidgetClass, bd_form,
                                  XmNorientation, XmVERTICAL,
                                  XmNspacing, 8,
                                  XmNtopAttachment, XmATTACH_FORM,
                                  XmNleftAttachment, XmATTACH_FORM,
                                  XmNrightAttachment, XmATTACH_FORM,
                                  XmNbottomAttachment, XmATTACH_WIDGET,
                                  XmNbottomWidget, row_separator(rc),
                                  XmNbottomOffset, 6,
                                  NULL);
    bd_title = label(col, "brickTitle", " ");
    bd_info = label(col, "brickInfo", " ");

    frame = XtVaCreateManagedWidget("lcdFrame", xmFrameWidgetClass, col,
                                    XmNshadowType, XmSHADOW_IN, NULL);
    lcdrc = XtVaCreateManagedWidget("lcd", xmRowColumnWidgetClass, frame,
                                    XmNorientation, XmVERTICAL,
                                    XmNspacing, 0,
                                    XmNmarginWidth, 10,
                                    XmNmarginHeight, 6,
                                    NULL);
    bd_lcd1 = label(lcdrc, "lcdLine", " ");
    bd_lcd2 = label(lcdrc, "lcdLine", " ");

    bd_state = label(col, "powerState", " ");
    leds = XtVaCreateManagedWidget("leds", xmRowColumnWidgetClass, col,
                                   XmNorientation, XmHORIZONTAL,
                                   XmNspacing, 20,
                                   NULL);
    bd_att = toggle(leds, "attention", "Attention LED", 0);
    bd_fail = toggle(leds, "failure", "Failure LED", 0);
    XtAddCallback(bd_att, XmNvalueChangedCallback, bd_toggle_cb, NULL);
    XtAddCallback(bd_fail, XmNvalueChangedCallback, bd_toggle_cb, (XtPointer)1);

    bd_power = button(col, "powerButton", "Power", bd_power_cb, NULL);
}

void brickdlg_show(const L2Brick *b)
{
    if (!bd_form) bd_create();
    bd_brick = *b;
    bd_fill();
    XtManageChild(bd_form);
    rack_set_selected(b->id);
    /* raise if already open */
    if (XtIsRealized(XtParent(bd_form)))
        XRaiseWindow(XtDisplay(bd_form), XtWindow(XtParent(bd_form)));
}

void brickdlg_update(const L2Brick *b)
{
    if (!bd_form || !XtIsManaged(bd_form)) return;
    if (strcmp(bd_brick.id, b->id) != 0) return;
    bd_brick = *b;
    bd_fill();
}

void brickdlg_busy(const char *id, int busy)
{
    Boolean s = busy ? False : True;
    if (!bd_form || strcmp(bd_brick.id, id) != 0) return;
    XtSetSensitive(bd_power, s);
    XtSetSensitive(bd_refresh, s);
    XtSetSensitive(bd_att, s);
    XtSetSensitive(bd_fail, s);
}

/* ------------------------------------------------------------------ */
/* L2 console                                                          */
/* ------------------------------------------------------------------ */

#define CON_HIST 64

static Widget con_form, con_out, con_in;
static char  *con_hist[CON_HIST];
static int    con_nhist, con_hpos;

static void con_append(const char *s)
{
    XmTextPosition pos = XmTextGetLastPosition(con_out);
    XmTextInsert(con_out, pos, (char *)s);
    pos = XmTextGetLastPosition(con_out);
    XmTextSetInsertionPosition(con_out, pos);
    XmTextShowPosition(con_out, pos);
}

static void con_done(int ok, const char *resp, void *ud)
{
    (void)ud;
    if (!con_form) return;
    if (!ok) con_append("*** ");
    con_append(resp);
    if (resp[0] && resp[strlen(resp) - 1] != '\n') con_append("\n");
}

static void con_activate(Widget w, XtPointer cd, XtPointer cbs)
{
    char *cmd = XmTextFieldGetString(con_in), *t;
    char line[400];
    (void)w;
    (void)cd;
    (void)cbs;
    t = s_trim(cmd);
    if (*t) {
        sprintf(line, "%s %.300s\n", l2c_prompt()[0] ? l2c_prompt() : ">", t);
        con_append(line);
        if (con_nhist == CON_HIST) {
            free(con_hist[0]);
            memmove(con_hist, con_hist + 1, sizeof(char *) * (CON_HIST - 1));
            con_nhist--;
        }
        con_hist[con_nhist++] = s_dup(t);
        con_hpos = con_nhist;
        l2c_exec(t, con_done, NULL);
    }
    XtFree(cmd);
    XmTextFieldSetString(con_in, "");
}

static void con_history(Widget w, XEvent *ev, String *params, Cardinal *np)
{
    (void)ev;
    if (*np < 1 || con_nhist == 0) return;
    if (strcmp(params[0], "prev") == 0) {
        if (con_hpos > 0) con_hpos--;
    } else {
        if (con_hpos < con_nhist) con_hpos++;
    }
    XmTextFieldSetString(w, con_hpos < con_nhist ? con_hist[con_hpos] : "");
    XmTextFieldSetInsertionPosition(w, XmTextFieldGetLastPosition(w));
}

static void con_clear(Widget w, XtPointer cd, XtPointer cbs)
{
    (void)w;
    (void)cd;
    (void)cbs;
    XmTextSetString(con_out, "");
}

void dlg_console(void)
{
    static XtActionsRec actions[] = { { "ConsoleHistory", con_history } };
    Widget rc, lbl;
    Arg a[12];
    int n = 0;

    if (con_form) {
        XtManageChild(con_form);
        XRaiseWindow(XtDisplay(con_form), XtWindow(XtParent(con_form)));
        return;
    }
    XtAppAddActions(g_app, actions, 1);
    con_form = form_dialog("consoleDialog", "L2 Console", 0);

    rc = button_row(con_form);
    button(rc, "clear", "Clear", con_clear, NULL);
    button(rc, "close", "Close", unmanage_cb, (XtPointer)con_form);

    lbl = label(con_form, "consoleLabel", "Command:");
    XtVaSetValues(lbl,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_WIDGET,
                  XmNbottomWidget, row_separator(rc),
                  XmNbottomOffset, 12,
                  NULL);
    con_in = XtVaCreateManagedWidget("consoleIn", xmTextFieldWidgetClass, con_form,
                                     XmNleftAttachment, XmATTACH_WIDGET,
                                     XmNleftWidget, lbl,
                                     XmNrightAttachment, XmATTACH_FORM,
                                     XmNbottomAttachment, XmATTACH_WIDGET,
                                     XmNbottomWidget, row_separator(rc),
                                     XmNbottomOffset, 6,
                                     NULL);
    XtAddCallback(con_in, XmNactivateCallback, con_activate, NULL);
    XtOverrideTranslations(con_in, XtParseTranslationTable(
        "<Key>osfUp: ConsoleHistory(prev)\n"
        "<Key>osfDown: ConsoleHistory(next)"));

    XtSetArg(a[n], XmNeditMode, XmMULTI_LINE_EDIT); n++;
    XtSetArg(a[n], XmNeditable, False); n++;
    XtSetArg(a[n], XmNcursorPositionVisible, False); n++;
    XtSetArg(a[n], XmNrows, 24); n++;
    XtSetArg(a[n], XmNcolumns, 80); n++;
    con_out = XmCreateScrolledText(con_form, "consoleOut", a, n);
    XtVaSetValues(XtParent(con_out),
                  XmNtopAttachment, XmATTACH_FORM,
                  XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM,
                  XmNbottomAttachment, XmATTACH_WIDGET,
                  XmNbottomWidget, con_in,
                  XmNbottomOffset, 6,
                  NULL);
    XtManageChild(con_out);
    XtVaSetValues(con_form, XmNinitialFocus, con_in, NULL);
    con_append("SGI L2 console - type L2 commands, e.g. help, config, env summary, fan\n"
               "Up / Down recall previous commands.\n\n");
    XtManageChild(con_form);
}

/* ------------------------------------------------------------------ */
/* rack info                                                           */
/* ------------------------------------------------------------------ */

static void rackinfo_done(int ok, const char *resp, void *ud)
{
    L2ConfigEntry cfgs[L2_MAX_CONFIG];
    DBuf o;
    char line[256];
    int i, n;
    (void)ud;

    if (!ok) {
        dlg_error(resp);
        return;
    }
    n = l2_parse_config(resp, cfgs, L2_MAX_CONFIG);
    dbuf_init(&o);
    dbuf_appends(&o, "Controller  ID          Address\n");
    dbuf_appends(&o, "----------  ----------  ------------------------\n");
    for (i = 0; i < n; i++) {
        sprintf(line, "%-10.3s  %-10.23s  %.63s\n", cfgs[i].ltype, cfgs[i].id, cfgs[i].addr);
        dbuf_appends(&o, line);
    }
    sprintf(line, "\n%d L2 / L1 controllers.\n\nRaw 'config' output:\n\n", n);
    dbuf_appends(&o, line);
    dbuf_appends(&o, resp);
    dlg_text("Rack Info", o.data);
    dbuf_free(&o);
}

void dlg_rackinfo(void)
{
    l2c_exec("config", rackinfo_done, NULL);
}

/* ------------------------------------------------------------------ */
/* settings                                                            */
/* ------------------------------------------------------------------ */

static Widget st_form, st_host, st_port, st_timeout, st_refresh, st_order;
static Widget st_asc, st_custom, st_ralt, st_grey, st_confirm;

static Widget st_field(Widget grid, const char *lbl, const char *name, const char *val)
{
    Widget w;
    label(grid, "fieldLabel", lbl);
    w = XtVaCreateManagedWidget(name, xmTextFieldWidgetClass, grid,
                                XmNcolumns, 32, NULL);
    XmTextFieldSetString(w, (char *)val);
    return w;
}

static void st_get(Widget w, char *dst, size_t size)
{
    char *s = XmTextFieldGetString(w);
    s_copy(dst, s_trim(s), size);
    XtFree(s);
}

static void st_apply(int save)
{
    AppConfig c = g_cfg;
    char tmp[64];
    int reconnect;

    st_get(st_host, c.host, sizeof(c.host));
    st_get(st_port, tmp, sizeof(tmp));
    c.port = atoi(tmp) > 0 ? atoi(tmp) : CFG_DEFAULT_PORT;
    st_get(st_timeout, tmp, sizeof(tmp));
    c.timeout = atoi(tmp) > 0 ? atoi(tmp) : 30;
    st_get(st_refresh, tmp, sizeof(tmp));
    c.refresh = atoi(tmp) > 0 ? atoi(tmp) : 0;
    st_get(st_order, c.sortOrder, sizeof(c.sortOrder));
    if (XmToggleButtonGetState(st_custom)) c.sortMode = L2_SORT_CUSTOM;
    else c.sortMode = XmToggleButtonGetState(st_asc) ? L2_SORT_ASC : L2_SORT_DESC;
    c.routerAltix = XmToggleButtonGetState(st_ralt) ? 1 : 0;
    c.altix350Grey = XmToggleButtonGetState(st_grey) ? 1 : 0;
    c.confirmPower = XmToggleButtonGetState(st_confirm) ? 1 : 0;

    reconnect = strcmp(c.host, g_cfg.host) != 0 || c.port != g_cfg.port;
    g_cfg = c;
    if (save) {
        if (!cfg_save(&g_cfg, g_cfgpath)) {
            char msg[700];
            sprintf(msg, "Could not write %.600s", g_cfgpath);
            dlg_error(msg);
        } else {
            char msg[700];
            sprintf(msg, "Settings saved to %.600s", g_cfgpath);
            app_status(msg, 0);
        }
    }
    app_settings_changed(reconnect);
}

static void st_button_cb(Widget w, XtPointer cd, XtPointer cbs)
{
    int what = (int)(long)cd;   /* 0 cancel, 1 apply, 2 save */
    Widget shell = XtParent(st_form);
    (void)w;
    (void)cbs;
    if (what > 0) st_apply(what == 2);
    st_form = NULL;
    XtDestroyWidget(shell);
}

static void st_destroy_cb(Widget w, XtPointer cd, XtPointer cbs)
{
    (void)w;
    (void)cd;
    (void)cbs;
    st_form = NULL;
}

void dlg_settings(void)
{
    Widget rc, col, grid, frame, opts, save;
    char port[16], timeout[16], refresh[16];

    if (st_form) {
        XRaiseWindow(XtDisplay(st_form), XtWindow(XtParent(st_form)));
        return;
    }
    st_form = form_dialog("settingsDialog", APP_NAME " Settings", 1);
    XtAddCallback(st_form, XmNdestroyCallback, st_destroy_cb, NULL);

    rc = button_row(st_form);
    save = button(rc, "save", "Save", st_button_cb, (XtPointer)2);
    button(rc, "apply", "Apply", st_button_cb, (XtPointer)1);
    button(rc, "cancel", "Cancel", st_button_cb, (XtPointer)0);

    col = XtVaCreateManagedWidget("content", xmRowColumnWidgetClass, st_form,
                                  XmNorientation, XmVERTICAL,
                                  XmNspacing, 10,
                                  XmNtopAttachment, XmATTACH_FORM,
                                  XmNleftAttachment, XmATTACH_FORM,
                                  XmNrightAttachment, XmATTACH_FORM,
                                  XmNbottomAttachment, XmATTACH_WIDGET,
                                  XmNbottomWidget, row_separator(rc),
                                  XmNbottomOffset, 6,
                                  NULL);

    label(col, "sectionLabel", "L2 controller connection");
    grid = XtVaCreateManagedWidget("grid", xmRowColumnWidgetClass, col,
                                   XmNorientation, XmHORIZONTAL,
                                   XmNpacking, XmPACK_COLUMN,
                                   XmNnumColumns, 5,
                                   XmNisAligned, True,
                                   XmNentryAlignment, XmALIGNMENT_END,
                                   NULL);
    sprintf(port, "%d", g_cfg.port);
    sprintf(timeout, "%d", g_cfg.timeout);
    sprintf(refresh, "%d", g_cfg.refresh);
    st_host = st_field(grid, "L2 host / IP:", "host", g_cfg.host);
    st_port = st_field(grid, "L2 port (8000):", "port", port);
    st_timeout = st_field(grid, "Command timeout (s):", "timeout", timeout);
    st_refresh = st_field(grid, "Auto refresh (s, 0 = off):", "refresh", refresh);
    st_order = st_field(grid, "Custom sort order:", "sortOrder", g_cfg.sortOrder);

    frame = XtVaCreateManagedWidget("optionsFrame", xmFrameWidgetClass, col, NULL);
    opts = XtVaCreateManagedWidget("options", xmRowColumnWidgetClass, frame,
                                   XmNorientation, XmVERTICAL, NULL);
    st_asc = toggle(opts, "ascending", "Sort bricks ascending (default: descending)",
                    g_cfg.sortMode == L2_SORT_ASC);
    st_custom = toggle(opts, "customSort", "Use custom sort order",
                       g_cfg.sortMode == L2_SORT_CUSTOM);
    st_ralt = toggle(opts, "routerAltix", "Draw routers with the Altix bezel", g_cfg.routerAltix);
    st_grey = toggle(opts, "altix350Grey", "Draw Altix 350 with the grey bezel", g_cfg.altix350Grey);
    st_confirm = toggle(opts, "confirmPower", "Confirm power up / down", g_cfg.confirmPower);

    XtVaSetValues(st_form, XmNdefaultButton, save, NULL);
    XtManageChild(st_form);
}
