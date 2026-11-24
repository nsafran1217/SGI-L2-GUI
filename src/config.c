/*
 * config.c - read / write ~/.sgil2guirc
 *
 * Format: one "key = value" per line, '#' starts a comment.
 *
 *   host          = 192.168.0.100
 *   port          = 8000
 *   timeout       = 30
 *   sort          = desc | asc | custom
 *   sortorder     = 001c01 001c02 001r20
 *   routerstyle   = default | altix
 *   altix350style = yellow | grey
 *   refresh       = 0
 *   confirmpower  = 1
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "l2parse.h"
#include "util.h"

void cfg_defaults(AppConfig *c)
{
    memset(c, 0, sizeof(*c));
    c->port = CFG_DEFAULT_PORT;
    c->timeout = 30;
    c->sortMode = L2_SORT_DESC;
    c->refresh = 0;
    c->confirmPower = 1;
}

const char *cfg_default_path(void)
{
    static char path[512];
    const char *home = getenv("HOME");
    if (!home || !*home) home = ".";
    s_copy(path, home, sizeof(path));
    s_cat(path, "/.sgil2guirc", sizeof(path));
    return path;
}

static void set_key(AppConfig *c, const char *key, const char *val)
{
    if (s_equal_ci(key, "host")) s_copy(c->host, val, sizeof(c->host));
    else if (s_equal_ci(key, "port")) c->port = atoi(val) > 0 ? atoi(val) : CFG_DEFAULT_PORT;
    else if (s_equal_ci(key, "timeout")) c->timeout = atoi(val) > 0 ? atoi(val) : 30;
    else if (s_equal_ci(key, "sort")) {
        if (s_equal_ci(val, "asc")) c->sortMode = L2_SORT_ASC;
        else if (s_equal_ci(val, "custom")) c->sortMode = L2_SORT_CUSTOM;
        else c->sortMode = L2_SORT_DESC;
    } else if (s_equal_ci(key, "sortorder")) s_copy(c->sortOrder, val, sizeof(c->sortOrder));
    else if (s_equal_ci(key, "routerstyle")) c->routerAltix = s_equal_ci(val, "altix");
    else if (s_equal_ci(key, "altix350style")) c->altix350Grey = s_equal_ci(val, "grey");
    else if (s_equal_ci(key, "refresh")) c->refresh = atoi(val) > 0 ? atoi(val) : 0;
    else if (s_equal_ci(key, "confirmpower")) c->confirmPower = atoi(val) != 0;
    else fprintf(stderr, "sgil2gui: unknown config key '%s'\n", key);
}

int cfg_load(AppConfig *c, const char *path)
{
    FILE *f;
    char line[1024], *p, *eq, *key, *val;

    f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        p = s_trim(line);
        if (!*p || *p == '#') continue;
        eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        key = s_trim(p);
        val = s_trim(eq + 1);
        set_key(c, key, val);
    }
    fclose(f);
    return 1;
}

int cfg_save(const AppConfig *c, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "# SGI-L2-GUI settings\n");
    fprintf(f, "host          = %s\n", c->host);
    fprintf(f, "port          = %d\n", c->port);
    fprintf(f, "timeout       = %d\n", c->timeout);
    fprintf(f, "sort          = %s\n",
            c->sortMode == L2_SORT_ASC ? "asc" :
            (c->sortMode == L2_SORT_CUSTOM ? "custom" : "desc"));
    fprintf(f, "sortorder     = %s\n", c->sortOrder);
    fprintf(f, "routerstyle   = %s\n", c->routerAltix ? "altix" : "default");
    fprintf(f, "altix350style = %s\n", c->altix350Grey ? "grey" : "yellow");
    fprintf(f, "refresh       = %d\n", c->refresh);
    fprintf(f, "confirmpower  = %d\n", c->confirmPower);
    return fclose(f) == 0;
}
