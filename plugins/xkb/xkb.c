#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <lua.h>

#include "include/plugin_v1.h"
#include "include/sayf_macros.h"
#include "include/plugin_utils.h"

#include "libls/alloc_utils.h"
#include "libls/compdep.h"
#include "libls/strarr.h"

#include "rules_names.h"

// If this plugin is used, the whole process gets killed if a connection to the display is lost,
// because Xlib is terrible.
//
// See:
// * https://tronche.com/gui/x/xlib/event-handling/protocol-errors/XSetIOErrorHandler.html
// * https://tronche.com/gui/x/xlib/event-handling/protocol-errors/XSetErrorHandler.html

typedef struct {
    char *dpyname;
    unsigned deviceid;
} Priv;

static
void
destroy(LuastatusPluginData *pd)
{
    Priv *p = pd->priv;
    free(p->dpyname);
    free(p);
}

static
int
init(LuastatusPluginData *pd, lua_State *L)
{
    Priv *p = pd->priv = LS_XNEW(Priv, 1);
    *p = (Priv) {
        .dpyname = NULL,
        .deviceid = XkbUseCoreKbd,
    };

    PU_MAYBE_VISIT_STR("display", NULL, s,
        p->dpyname = ls_xstrdup(s);
    );

    PU_MAYBE_VISIT_NUM("device_id", NULL, n,
        if (n < 0) {
            LS_FATALF(pd, "device_id < 0");
            goto error;
        }
        if (n > UINT_MAX) {
            LS_FATALF(pd, "device_id > UINT_MAX");
            goto error;
        }
        p->deviceid = n;
    );

    void **ptr = pd->map_get(pd->userdata, "flag:library_used:x11");
    if (!*ptr) {
        if (!XInitThreads()) {
            LS_FATALF(pd, "XInitThreads failed");
            goto error;
        }
        *ptr = "yes";
    }

    return LUASTATUS_OK;

error:
    destroy(pd);
    return LUASTATUS_ERR;
}

static
Display *
open_dpy(LuastatusPluginData *pd, char *dpyname)
{
    XkbIgnoreExtension(False);
    int event_out;
    int error_out;
    int reason_out;
    int major_in_out = XkbMajorVersion;
    int minor_in_out = XkbMinorVersion;
    Display *dpy = XkbOpenDisplay(dpyname, &event_out, &error_out, &major_in_out, &minor_in_out,
                                  &reason_out);
    if (dpy && reason_out == XkbOD_Success) {
        return dpy;
    }
    const char *msg;
    switch (reason_out) {
    case XkbOD_BadLibraryVersion:
        msg = "bad XKB library version";
        break;
    case XkbOD_ConnectionRefused:
        msg = "can't open display, connection refused";
        break;
    case XkbOD_BadServerVersion:
        msg = "server has an incompatible extension version";
        break;
    case XkbOD_NonXkbServer:
        msg = "extension is not present in the server";
        break;
    default:
        msg = "unknown error";
        break;
    }
    LS_FATALF(pd, "XkbOpenDisplay failed: %s", msg);
    return NULL;
}

static
bool
query_groups(Display *dpy, LSStringArray *groups)
{
    RulesNames rn;
    if (!rules_names_load(dpy, &rn)) {
        return false;
    }

    ls_strarr_clear(groups);
    if (rn.layout) {
        // split /rn.layout/ by non-parenthesized commas
        int balance = 0;
        size_t prev = 0;
        const size_t nlayout = strlen(rn.layout);
        for (size_t i = 0; i < nlayout; ++i) {
            switch (rn.layout[i]) {
            case '(': ++balance; break;
            case ')': --balance; break;
            case ',':
                if (balance == 0) {
                    ls_strarr_append(groups, rn.layout + prev, i - prev);
                    prev = i + 1;
                }
                break;
            }
        }
        ls_strarr_append(groups, rn.layout + prev, nlayout - prev);
    }

    rules_names_destroy(&rn);
    return true;
}

static
void
run(LuastatusPluginData *pd, LuastatusPluginRunFuncs funcs)
{
    Priv *p = pd->priv;
    LSStringArray groups = ls_strarr_new();
    Display *dpy = NULL;

    if (!(dpy = open_dpy(pd, p->dpyname))) {
        goto error;
    }

    if (!query_groups(dpy, &groups)) {
        LS_FATALF(pd, "query_groups failed");
        goto error;
    }
    while (1) {
        // query current state
        XkbStateRec state;
        if (XkbGetState(dpy, p->deviceid, &state) != Success) {
            LS_FATALF(pd, "XkbGetState failed");
            goto error;
        }

        // check if group is valid and possibly requery
        int group = state.group;
        if (group < 0) {
            LS_WARNF(pd, "group ID is negative (%d)", group);
        } else if ((size_t) group >= ls_strarr_size(groups)) {
            LS_WARNF(pd, "group ID (%d) is too large, requerying", group);
            if (!query_groups(dpy, &groups)) {
                LS_FATALF(pd, "query_groups failed");
                goto error;
            }
            if ((size_t) group >= ls_strarr_size(groups)) {
                LS_WARNF(pd, "group ID is still too large");
            }
        }

        // make a call
        lua_State *L = funcs.call_begin(pd->userdata); // L: -
        lua_newtable(L); // L: table
        lua_pushinteger(L, group); // L: table n
        lua_setfield(L, -2, "id"); // L: table
        if (group >= 0 && (size_t) group < ls_strarr_size(groups)) {
            size_t nbuf;
            const char *buf = ls_strarr_at(groups, group, &nbuf);
            lua_pushlstring(L, buf, nbuf); // L: table group
            lua_setfield(L, -2, "name"); // L: table
        }
        funcs.call_end(pd->userdata);

        // wait for next event
        if (XkbSelectEventDetails(dpy, p->deviceid, XkbStateNotify, XkbAllStateComponentsMask,
                                  XkbGroupStateMask)
            == False)
        {
            LS_FATALF(pd, "XkbSelectEventDetails failed");
            goto error;
        }
        XEvent event;
        // XXX should we block all signals here to ensure /XNextEvent/ will not
        // fail with /EINTR/? Apparently not: /XNextEvent/ is untimed, so there is
        // no sense for it to use a multiplexing interface.
        XNextEvent(dpy, &event);
    }

error:
    if (dpy) {
        XCloseDisplay(dpy);
    }
    ls_strarr_destroy(groups);
}

LuastatusPluginIface luastatus_plugin_iface_v1 = {
    .init = init,
    .run = run,
    .destroy = destroy,
};
