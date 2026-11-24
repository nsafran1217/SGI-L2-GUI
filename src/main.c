/*
 * main.c - SGI-L2-GUI: native IRIX / Motif front end for the SGI L2
 *          system controller (port of SGI-L2-WUI)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <X11/cursorfont.h>
#include <Xm/Xm.h>
#include <Xm/MainW.h>
#include <Xm/Form.h>
#include <Xm/RowColumn.h>
#include <Xm/CascadeB.h>
#include <Xm/PushB.h>
#include <Xm/Separator.h>
#include <Xm/Label.h>
#include <Xm/Frame.h>
#include <Xm/Protocols.h>

#include "app.h"
#include "l2client.h"
#include "l2parse.h"
#include "rack.h"
#include "l2net.h"
#include "demo.h"
#include "util.h"

XtAppContext g_app;
Widget       g_toplevel;
AppConfig    g_cfg;
char         g_cfgpath[512];
int          g_demo;

static Widget       status_label;
static Cursor       watch_cursor;
static XtIntervalId auto_timer;
static int          auto_timer_active;

/* ------------------------------------------------------------------ */
/* X resources                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    char *lcdFont;
    char *labelFont;
    char *sysidFont;
} AppRes;

static AppRes app_res;

static XtResource resources[] = {
    { "lcdFont", "LcdFont", XtRString, sizeof(char *),
      XtOffsetOf(AppRes, lcdFont), XtRString, NULL },
    { "labelFont", "LabelFont", XtRString, sizeof(char *),
      XtOffsetOf(AppRes, labelFont), XtRString, NULL },
    { "sysidFont", "SysidFont", XtRString, sizeof(char *),
      XtOffsetOf(AppRes, sysidFont), XtRString, NULL }
};

/* used when no app-defaults file is installed */
static String fallback[] = {
    "SgiL2Gui*sgiMode: true",
    "SgiL2Gui*useSchemes: all",
    "SgiL2Gui*outputText.fontList: fixed",
    "SgiL2Gui*consoleOut.fontList: fixed",
    "SgiL2Gui*consoleIn.fontList: fixed",
    "SgiL2Gui*brickInfo.fontList: fixed",
    "SgiL2Gui*brickTitle.fontList: -*-helvetica-bold-r-normal--14-*-*-*-*-*-iso8859-1",
    "SgiL2Gui*lcdLine.fontList: -*-courier-bold-r-normal--14-*-*-*-*-*-iso8859-1",
    "SgiL2Gui*lcdFrame*background: #a4b47c",
    "SgiL2Gui*lcdLine.foreground: #101010",
    "SgiL2Gui*statusLabel.alignment: XmALIGNMENT_BEGINNING",
    NULL
};

/* ------------------------------------------------------------------ */
/* status                                                              */
/* ------------------------------------------------------------------ */

void app_status(const char *msg, int busy)
{
    XmString x;
    if (msg && status_label) {
        x = XmStringCreateLocalized((char *)msg);
        XtVaSetValues(status_label, XmNlabelString, x, NULL);
        XmStringFree(x);
    }
    if (g_toplevel && XtIsRealized(g_toplevel)) {
        if (busy) XDefineCursor(XtDisplay(g_toplevel), XtWindow(g_toplevel), watch_cursor);
        else XUndefineCursor(XtDisplay(g_toplevel), XtWindow(g_toplevel));
        XFlush(XtDisplay(g_toplevel));
    }
}

static void status_cb(const char *msg, int busy)
{
    app_status(msg, busy);
    /* make the message visible before a blocking connect */
    if (busy && status_label && XtIsRealized(status_label))
        XmUpdateDisplay(status_label);
}

L2ParseOpts app_parse_opts(void)
{
    L2ParseOpts o;
    o.routerAltix = g_cfg.routerAltix;
    o.altix350Grey = g_cfg.altix350Grey;
    return o;
}

/* ------------------------------------------------------------------ */
/* rack refresh                                                        */
/* ------------------------------------------------------------------ */

static char *rf_display;
static int   rf_running;

static void rf_display_done(int ok, const char *resp, void *ud)
{
    (void)ud;
    free(rf_display);
    rf_display = ok ? s_dup(resp) : NULL;
}

static void rf_brick_done(int ok, const char *resp, void *ud)
{
    static L2Brick b[L2_MAX_BRICKS];
    static L2Display d[L2_MAX_BRICKS];
    L2ParseOpts o = app_parse_opts();
    int nb, nd, i;
    char msg[200], tbuf[32];
    time_t now;
    (void)ud;

    rf_running = 0;
    if (!ok || !rf_display) {
        if (rack_count() == 0) rack_set_message(ok ? "No answer from the L2" : resp);
        return;
    }
    nb = l2_parse_brick(resp, &o, b, L2_MAX_BRICKS);
    nd = l2_parse_display(rf_display, d, L2_MAX_BRICKS);
    l2_merge_display(b, nb, d, nd);
    l2_sort(b, nb, g_cfg.sortMode, g_cfg.sortOrder);
    if (nb == 0) rack_set_message("The L2 reported no bricks");
    rack_set_bricks(b, nb);
    for (i = 0; i < nb; i++) brickdlg_update(&b[i]);

    now = time(NULL);
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", localtime(&now));
    sprintf(msg, "%s%.100s: %d bricks - updated %s", g_demo ? "[demo] " : "",
            g_demo ? "simulator" : g_cfg.host, nb, tbuf);
    app_status(msg, 0);
}

void app_refresh_rack(void)
{
    if (rf_running) return;
    if (!g_demo && !g_cfg.host[0]) {
        rack_set_message("No L2 host configured - see File > Settings");
        return;
    }
    rf_running = 1;
    l2c_exec("display", rf_display_done, NULL);
    l2c_exec("brick", rf_brick_done, NULL);
}

/* ------------------------------------------------------------------ */
/* single brick refresh                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    char  id[16];
    char *display;
} BrickJob;

static void br_display_done(int ok, const char *resp, void *ud)
{
    BrickJob *j = (BrickJob *)ud;
    j->display = ok ? s_dup(resp) : NULL;
}

static void br_brick_done(int ok, const char *resp, void *ud)
{
    BrickJob *j = (BrickJob *)ud;
    L2Brick b[4];
    L2Display d[4];
    L2ParseOpts o = app_parse_opts();
    int nb, nd, i;

    brickdlg_busy(j->id, 0);
    if (ok && j->display) {
        nb = l2_parse_brick(resp, &o, b, 4);
        nd = l2_parse_display(j->display, d, 4);
        l2_merge_display(b, nb, d, nd);
        for (i = 0; i < nb; i++) {
            if (strcmp(b[i].id, j->id) == 0) {
                rack_update_brick(&b[i]);
                brickdlg_update(&b[i]);
            }
        }
    }
    free(j->display);
    free(j);
}

void app_refresh_brick(const char *id)
{
    BrickJob *j = (BrickJob *)xmalloc(sizeof(BrickJob));
    char cmd[64];
    s_copy(j->id, id, sizeof(j->id));
    j->display = NULL;
    brickdlg_busy(id, 1);
    sprintf(cmd, "%.20s display", id);
    l2c_exec(cmd, br_display_done, j);
    sprintf(cmd, "%.20s brick", id);
    l2c_exec(cmd, br_brick_done, j);
}

/* ------------------------------------------------------------------ */
/* power + LEDs                                                        */
/* ------------------------------------------------------------------ */

static void delayed_rack_refresh(XtPointer cd, XtIntervalId *id)
{
    (void)cd;
    (void)id;
    app_refresh_rack();
}

static void delayed_brick_refresh(XtPointer cd, XtIntervalId *id)
{
    char *bid = (char *)cd;
    (void)id;
    if (rack_find(bid)) app_refresh_brick(bid);
    free(bid);
}

static void power_done(int ok, const char *resp, void *ud)
{
    char *target = (char *)ud;
    char msg[300];
    if (!ok) {
        dlg_error(resp);
    } else {
        sprintf(msg, "%.40s: power command sent", target ? target : "rack");
        app_status(msg, 0);
    }
    if (target) {
        brickdlg_busy(target, 0);
        XtAppAddTimeOut(g_app, 1000, delayed_brick_refresh, target);
    } else {
        XtAppAddTimeOut(g_app, 2000, delayed_rack_refresh, NULL);
    }
}

void app_power(const char *target, int up)
{
    char cmd[64];
    if (target && *target) {
        sprintf(cmd, "%.20s power %s", target, up ? "up" : "down");
        brickdlg_busy(target, 1);
        l2c_exec(cmd, power_done, s_dup(target));
    } else {
        l2c_exec(up ? "power up" : "power down", power_done, NULL);
    }
}

static void power_confirmed(const char *arg, int iarg)
{
    app_power(arg, iarg);
}

void app_power_confirm(const char *target, int up)
{
    char msg[200];
    if (!g_cfg.confirmPower) {
        app_power(target, up);
        return;
    }
    if (target && *target)
        sprintf(msg, "%s %.20s?", up ? "Power up" : "Power down", target);
    else
        sprintf(msg, "%s all bricks in the rack?", up ? "Power up" : "Power down");
    dlg_confirm(up ? "Power up" : "Power down", msg, power_confirmed, target, up);
}

static void led_done(int ok, const char *resp, void *ud)
{
    char *id = (char *)ud;
    if (!ok) dlg_error(resp);
    brickdlg_busy(id, 0);
    app_refresh_brick(id);
    free(id);
}

void app_set_led(const char *id, int failure, int on)
{
    char cmd[80];
    sprintf(cmd, "%.20s display %s %s", id, failure ? "failure" : "attention", on ? "on" : "off");
    brickdlg_busy(id, 1);
    l2c_exec(cmd, led_done, s_dup(id));
}

/* ------------------------------------------------------------------ */
/* command output dialogs                                              */
/* ------------------------------------------------------------------ */

static void show_done(int ok, const char *resp, void *ud)
{
    char *title = (char *)ud;
    if (ok) dlg_text(title, resp);
    else dlg_error(resp);
    free(title);
}

void app_show_command(const char *title, const char *cmd)
{
    l2c_exec(cmd, show_done, s_dup(title));
}

/* ------------------------------------------------------------------ */
/* auto refresh                                                        */
/* ------------------------------------------------------------------ */

static void auto_refresh(XtPointer cd, XtIntervalId *id)
{
    (void)cd;
    (void)id;
    auto_timer_active = 0;
    if (!l2c_pending()) app_refresh_rack();
    if (g_cfg.refresh > 0) {
        auto_timer = XtAppAddTimeOut(g_app, (unsigned long)g_cfg.refresh * 1000UL,
                                     auto_refresh, NULL);
        auto_timer_active = 1;
    }
}

static void restart_auto_refresh(void)
{
    if (auto_timer_active) XtRemoveTimeOut(auto_timer);
    auto_timer_active = 0;
    if (g_cfg.refresh > 0) {
        auto_timer = XtAppAddTimeOut(g_app, (unsigned long)g_cfg.refresh * 1000UL,
                                     auto_refresh, NULL);
        auto_timer_active = 1;
    }
}

void app_settings_changed(int reconnect)
{
    if (reconnect) l2c_disconnect();
    restart_auto_refresh();
    app_refresh_rack();
}

/* ------------------------------------------------------------------ */
/* menus                                                               */
/* ------------------------------------------------------------------ */

enum {
    M_SETTINGS, M_DISCONNECT, M_QUIT,
    M_REFRESH, M_PWR_UP, M_PWR_DOWN, M_RACKINFO, M_ENV, M_FAN, M_ROUTER,
    M_CONSOLE, M_ABOUT
};

static void menu_cb(Widget w, XtPointer cd, XtPointer cbs)
{
    (void)w;
    (void)cbs;
    switch ((int)(long)cd) {
    case M_SETTINGS:   dlg_settings(); break;
    case M_DISCONNECT: l2c_disconnect(); break;
    case M_QUIT:       l2c_disconnect(); exit(0); break;
    case M_REFRESH:    app_refresh_rack(); break;
    case M_PWR_UP:     app_power_confirm(NULL, 1); break;
    case M_PWR_DOWN:   app_power_confirm(NULL, 0); break;
    case M_RACKINFO:   dlg_rackinfo(); break;
    case M_ENV:        app_show_command("Environment Summary", "env summary"); break;
    case M_FAN:        app_show_command("Fan Status", "fan"); break;
    case M_ROUTER:     app_show_command("Router", "router"); break;
    case M_CONSOLE:    dlg_console(); break;
    case M_ABOUT:      dlg_about(); break;
    }
}

static Widget menu(Widget bar, const char *name, const char *label, char mnemonic)
{
    Widget pd, cb;
    XmString x = XmStringCreateLocalized((char *)label);
    char pdname[64];
    sprintf(pdname, "%.50sMenu", name);
    pd = XmCreatePulldownMenu(bar, pdname, NULL, 0);
    cb = XtVaCreateManagedWidget(name, xmCascadeButtonWidgetClass, bar,
                                 XmNlabelString, x,
                                 XmNmnemonic, (KeySym)mnemonic,
                                 XmNsubMenuId, pd,
                                 NULL);
    XmStringFree(x);
    if (strcmp(name, "help") == 0) XtVaSetValues(bar, XmNmenuHelpWidget, cb, NULL);
    return pd;
}

static void item(Widget pd, const char *name, const char *label, char mnemonic,
                 const char *accel, const char *accelText, int id)
{
    Widget w;
    XmString x = XmStringCreateLocalized((char *)label);
    w = XtVaCreateManagedWidget(name, xmPushButtonWidgetClass, pd,
                                XmNlabelString, x,
                                XmNmnemonic, (KeySym)mnemonic,
                                NULL);
    XmStringFree(x);
    if (accel) {
        x = XmStringCreateLocalized((char *)accelText);
        XtVaSetValues(w, XmNaccelerator, accel, XmNacceleratorText, x, NULL);
        XmStringFree(x);
    }
    XtAddCallback(w, XmNactivateCallback, menu_cb, (XtPointer)(long)id);
}

static void separator(Widget pd)
{
    XtVaCreateManagedWidget("sep", xmSeparatorWidgetClass, pd, NULL);
}

static Widget build_menubar(Widget mainw)
{
    Widget bar, pd;
    bar = XmCreateMenuBar(mainw, "menuBar", NULL, 0);

    pd = menu(bar, "file", "File", 'F');
    item(pd, "settings", "Settings...", 'S', NULL, NULL, M_SETTINGS);
    item(pd, "disconnect", "Disconnect", 'D', NULL, NULL, M_DISCONNECT);
    separator(pd);
    item(pd, "quit", "Quit", 'Q', "Ctrl<Key>q", "Ctrl+Q", M_QUIT);

    pd = menu(bar, "rack", "Rack", 'R');
    item(pd, "refresh", "Refresh", 'R', "Ctrl<Key>r", "Ctrl+R", M_REFRESH);
    separator(pd);
    item(pd, "powerUp", "Power Up Rack...", 'U', NULL, NULL, M_PWR_UP);
    item(pd, "powerDown", "Power Down Rack...", 'D', NULL, NULL, M_PWR_DOWN);
    separator(pd);
    item(pd, "rackInfo", "Rack Info", 'I', NULL, NULL, M_RACKINFO);
    item(pd, "env", "Environment Summary", 'E', NULL, NULL, M_ENV);
    item(pd, "fan", "Fan Status", 'F', NULL, NULL, M_FAN);
    item(pd, "router", "Router Info", 'o', NULL, NULL, M_ROUTER);

    pd = menu(bar, "tools", "Tools", 'T');
    item(pd, "console", "L2 Console...", 'C', "Ctrl<Key>l", "Ctrl+L", M_CONSOLE);

    pd = menu(bar, "help", "Help", 'H');
    item(pd, "about", "About...", 'A', NULL, NULL, M_ABOUT);

    XtManageChild(bar);
    return bar;
}

static void toolbar_button(Widget rc, const char *name, const char *label, int id)
{
    Widget w;
    XmString x = XmStringCreateLocalized((char *)label);
    w = XtVaCreateManagedWidget(name, xmPushButtonWidgetClass, rc,
                                XmNlabelString, x, NULL);
    XmStringFree(x);
    XtAddCallback(w, XmNactivateCallback, menu_cb, (XtPointer)(long)id);
}

static void brick_clicked(const L2Brick *b)
{
    brickdlg_show(b);
    /* fresh LED state for the dialog, like the WUI does on demand */
}

static void wm_quit(Widget w, XtPointer cd, XtPointer cbs)
{
    (void)w;
    (void)cd;
    (void)cbs;
    l2c_disconnect();
    exit(0);
}

/* ------------------------------------------------------------------ */
/* command line mode (no X)                                            */
/* ------------------------------------------------------------------ */

static int cli_connect(void)
{
    char err[256];
    if (g_demo) return 1;
    if (l2n_connect(g_cfg.host, g_cfg.port, 5000, err, sizeof(err)) < 0) {
        fprintf(stderr, "sgil2gui: %s:%d: %s\n", g_cfg.host, g_cfg.port, err);
        return 0;
    }
    fprintf(stderr, "sgil2gui: connected to %s:%d\n", g_cfg.host, g_cfg.port);
    return 1;
}

static char *cli_exec(const char *cmd)
{
    char *out = NULL, err[256];
    if (g_demo) return demo_exec(cmd);
    if (l2n_exec(cmd, g_cfg.timeout * 1000, &out, err, sizeof(err)) < 0) {
        fprintf(stderr, "sgil2gui: '%s' failed: %s\n", cmd, err);
        return NULL;
    }
    return out;
}

static int cli_main(const char *cmd, int summary)
{
    char *disp, *brk;
    static L2Brick b[L2_MAX_BRICKS];
    static L2Display d[L2_MAX_BRICKS];
    L2ParseOpts o = app_parse_opts();
    int nb, nd, i;

    if (!cli_connect()) return 1;
    if (!summary) {
        disp = cli_exec(cmd);
        if (!disp) return 1;
        fputs(disp, stdout);
        free(disp);
        l2n_close();
        return 0;
    }
    disp = cli_exec("display");
    brk = disp ? cli_exec("brick") : NULL;
    if (!disp || !brk) return 1;
    nb = l2_parse_brick(brk, &o, b, L2_MAX_BRICKS);
    nd = l2_parse_display(disp, d, L2_MAX_BRICKS);
    l2_merge_display(b, nb, d, nd);
    l2_sort(b, nb, g_cfg.sortMode, g_cfg.sortOrder);
    printf("%-7s %-12s %-24s %-3s %-3s %-3s %-14s %s\n",
           "ID", "SKIN", "LABEL", "PWR", "ATT", "FLT", "LCD 1", "LCD 2");
    for (i = 0; i < nb; i++) {
        printf("%-7s %-12s %-24s %-3s %-3s %-3s %-14s %s\n",
               b[i].id, l2_skin_name(b[i].skin), b[i].productLabel,
               b[i].display.pwr ? "on" : "-", b[i].display.service ? "on" : "-",
               b[i].display.fault ? "on" : "-", b[i].display.line1, b[i].display.line2);
    }
    free(disp);
    free(brk);
    if (!g_demo) l2n_close();
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(void)
{
    fprintf(stderr,
        "usage: sgil2gui [options] [X toolkit options]\n"
        "  -host <name>     L2 controller host name or IP\n"
        "  -port <n>        the L2's client port (default 8000)\n"
        "  -config <file>   settings file (default ~/.sgil2guirc)\n"
        "  -demo            run against the built-in L2 simulator\n"
        "  -cmd <command>   run one L2 command, print the output, exit (no X)\n"
        "  -summary         print the parsed rack summary and exit (no X)\n"
        "  -help            this text\n");
}

int main(int argc, char **argv)
{
    Widget mainw, form, toolbar, frame, rackw;
    Atom wm_delete;
    char err[256];
    const char *cli_cmd = NULL;
    int cli_summary = 0;
    int i, j, w, h, sh;
    char *host = NULL, *port = NULL;
    RackFonts fonts;
    Arg a[4];
    int n;

    /* our options first; the rest goes to Xt */
    s_copy(g_cfgpath, cfg_default_path(), sizeof(g_cfgpath));
    for (i = 1, j = 1; i < argc; i++) {
        if (strcmp(argv[i], "-host") == 0 && i + 1 < argc) host = argv[++i];
        else if (strcmp(argv[i], "-port") == 0 && i + 1 < argc) port = argv[++i];
        else if (strcmp(argv[i], "-config") == 0 && i + 1 < argc)
            s_copy(g_cfgpath, argv[++i], sizeof(g_cfgpath));
        else if (strcmp(argv[i], "-cmd") == 0 && i + 1 < argc) cli_cmd = argv[++i];
        else if (strcmp(argv[i], "-summary") == 0) cli_summary = 1;
        else if (strcmp(argv[i], "-demo") == 0) g_demo = 1;
        else if (strcmp(argv[i], "-help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else argv[j++] = argv[i];
    }
    argc = j;
    argv[argc] = NULL;

    cfg_defaults(&g_cfg);
    cfg_load(&g_cfg, g_cfgpath);
    if (host) s_copy(g_cfg.host, host, sizeof(g_cfg.host));
    if (port) g_cfg.port = atoi(port) > 0 ? atoi(port) : CFG_DEFAULT_PORT;

    if (cli_cmd || cli_summary) return cli_main(cli_cmd, cli_summary);

    g_toplevel = XtAppInitialize(&g_app, APP_CLASS, NULL, 0, &argc, argv,
                                 fallback, NULL, 0);
    if (argc > 1) {
        fprintf(stderr, "sgil2gui: unknown option %s\n", argv[1]);
        usage();
        return 1;
    }
    XtGetApplicationResources(g_toplevel, &app_res, resources,
                              XtNumber(resources), NULL, 0);
    if (!rack_init_visual(XtDisplay(g_toplevel),
                          DefaultScreen(XtDisplay(g_toplevel)), err, sizeof(err))) {
        fprintf(stderr, "sgil2gui: %s\n", err);
        return 1;
    }
    watch_cursor = XCreateFontCursor(XtDisplay(g_toplevel), XC_watch);
    l2c_init(g_app, &g_cfg, g_demo, status_cb);

    XtVaSetValues(g_toplevel, XmNtitle, g_demo ? APP_NAME " [demo]" : APP_NAME,
                  XmNiconName, "L2 GUI", NULL);

    mainw = XtVaCreateManagedWidget("main", xmMainWindowWidgetClass, g_toplevel, NULL);
    build_menubar(mainw);

    form = XtVaCreateManagedWidget("form", xmFormWidgetClass, mainw, NULL);

    toolbar = XtVaCreateManagedWidget("toolbar", xmRowColumnWidgetClass, form,
                                      XmNorientation, XmHORIZONTAL,
                                      XmNspacing, 6,
                                      XmNtopAttachment, XmATTACH_FORM,
                                      XmNleftAttachment, XmATTACH_FORM,
                                      XmNrightAttachment, XmATTACH_FORM,
                                      NULL);
    toolbar_button(toolbar, "refreshButton", "Refresh", M_REFRESH);
    toolbar_button(toolbar, "powerUpButton", "Power Up Rack", M_PWR_UP);
    toolbar_button(toolbar, "powerDownButton", "Power Down Rack", M_PWR_DOWN);
    toolbar_button(toolbar, "consoleButton", "L2 Console", M_CONSOLE);
    toolbar_button(toolbar, "settingsButton", "Settings", M_SETTINGS);

    frame = XtVaCreateManagedWidget("statusFrame", xmFrameWidgetClass, form,
                                    XmNshadowType, XmSHADOW_IN,
                                    XmNleftAttachment, XmATTACH_FORM,
                                    XmNrightAttachment, XmATTACH_FORM,
                                    XmNbottomAttachment, XmATTACH_FORM,
                                    NULL);
    status_label = XtVaCreateManagedWidget("statusLabel", xmLabelWidgetClass, frame,
                                           XmNalignment, XmALIGNMENT_BEGINNING,
                                           NULL);
    app_status("Ready", 0);

    fonts.lcdFont = app_res.lcdFont;
    fonts.labelFont = app_res.labelFont;
    fonts.sysidFont = app_res.sysidFont;
    n = 0;
    XtSetArg(a[n], XmNtopAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(a[n], XmNtopWidget, toolbar); n++;
    XtSetArg(a[n], XmNbottomAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(a[n], XmNbottomWidget, frame); n++;
    rackw = rack_create(form, "rack", a, (Cardinal)n, &fonts);
    XtVaSetValues(rackw, XmNleftAttachment, XmATTACH_FORM,
                  XmNrightAttachment, XmATTACH_FORM, NULL);
    rack_set_click_callback(brick_clicked);

    /* initial size: full rack width, up to most of the screen height */
    rack_preferred_size(&w, &h);
    sh = DisplayHeight(XtDisplay(g_toplevel), DefaultScreen(XtDisplay(g_toplevel)));
    h = 700;
    if (h > sh - 120) h = sh - 120;
    XtVaSetValues(rackw, XmNwidth, (Dimension)w, XmNheight, (Dimension)h, NULL);

    wm_delete = XmInternAtom(XtDisplay(g_toplevel), "WM_DELETE_WINDOW", False);
    XmAddWMProtocolCallback(g_toplevel, wm_delete, wm_quit, NULL);

    XtRealizeWidget(g_toplevel);
    rack_realize(g_toplevel);

    if (g_demo || g_cfg.host[0]) {
        rack_set_message("Connecting ...");
        app_refresh_rack();
    } else {
        rack_set_message("No L2 host configured - see File > Settings");
        dlg_settings();
    }
    restart_auto_refresh();

    XtAppMainLoop(g_app);
    return 0;
}
