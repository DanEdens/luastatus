#include <lua.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <pulse/pulseaudio.h>

#include "include/plugin_v1.h"
#include "include/sayf_macros.h"
#include "include/plugin_utils.h"

#include "libls/alloc_utils.h"

//typedef struct {
//} Priv;

static
void
destroy(LuastatusPluginData *pd)
{
    (void) pd;
    //Priv *p = pd->priv;
    //free(p);
}

static
int
init(LuastatusPluginData *pd, lua_State *L)
{
    //Priv *p = pd->priv = LS_XNEW(Priv, 1);
    //*p = (Priv) {
    //};

    lua_pushnil(L);
    if (lua_next(L, -2)) {
        LS_FATALF(pd, "no options supported");
        goto error;
    }

    return LUASTATUS_OK;

error:
    destroy(pd);
    return LUASTATUS_ERR;
}

static const uint32_t DEF_SINK_IDX = UINT32_MAX;

typedef struct {
    LuastatusPluginData *pd;
    LuastatusPluginRunFuncs funcs;
    pa_mainloop *ml;
    uint32_t def_sink_idx;
} UserData;

static
void
free_op(LuastatusPluginData *pd, pa_context *c, pa_operation *o, const char *what)
{
    if (o) {
        pa_operation_unref(o);
    } else {
        LS_ERRF(pd, "%s: %s", what, pa_strerror(pa_context_errno(c)));
    }
}

static
void
store_volume_from_sink_cb(pa_context *c, const pa_sink_info *info, int eol, void *vud)
{
    UserData *ud = vud;
    if (eol < 0) {
        if (pa_context_errno(c) == PA_ERR_NOENTITY) {
            return;
        }
        LS_ERRF(ud->pd, "PulseAudio error: %s", pa_strerror(pa_context_errno(c)));
    } else if (eol == 0) {
        if (info->index == ud->def_sink_idx) {
            lua_State *L = ud->funcs.call_begin(ud->pd->userdata);
            // L: ?
            lua_newtable(L); // L: ? table
            lua_pushinteger(L, pa_cvolume_avg(&info->volume)); // L: ? table integer
            lua_setfield(L, -2, "cur"); // L: ? table
            lua_pushinteger(L, PA_VOLUME_NORM); // L: ? table integer
            lua_setfield(L, -2, "norm"); // L: ? table
            lua_pushboolean(L, info->mute); // L: ? table boolean
            lua_setfield(L, -2, "mute"); // L: ? table
            ud->funcs.call_end(ud->pd->userdata);
        }
    }
}

static
void
store_default_sink_cb(pa_context *c, const pa_sink_info *i, int eol, void *vud)
{
    UserData *ud = vud;
    if (i) {
        if (ud->def_sink_idx != i->index) {
            // default sink changed?
            ud->def_sink_idx = i->index;
            store_volume_from_sink_cb(c, i, eol, vud);
        }
    }
}

static
void
update_default_sink(pa_context *c, void *vud)
{
    UserData *ud = vud;
    pa_operation *o = pa_context_get_sink_info_by_name(
        c, "@DEFAULT_SINK@", store_default_sink_cb, vud);
    free_op(ud->pd, c, o, "pa_context_get_sink_info_by_name");
}

static
void
subscribe_cb(pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *vud)
{
    UserData *ud = vud;

    if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) != PA_SUBSCRIPTION_EVENT_CHANGE) {
        return;
    }
    pa_subscription_event_type_t facility = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    switch (facility) {
    case PA_SUBSCRIPTION_EVENT_SERVER:
        // server change event, see if the default sink changed
        update_default_sink(c, vud);
        break;
    case PA_SUBSCRIPTION_EVENT_SINK:
        {
            pa_operation *o = pa_context_get_sink_info_by_index(c, idx, store_volume_from_sink_cb, vud);
            free_op(ud->pd, c, o, "pa_context_get_sink_info_by_index");
        }
        break;
    default:
        break;
    }
}


static
void
context_state_cb(pa_context *c, void *vud)
{
    UserData *ud = vud;
    switch (pa_context_get_state(c)) {
    case PA_CONTEXT_UNCONNECTED:
    case PA_CONTEXT_CONNECTING:
    case PA_CONTEXT_AUTHORIZING:
    case PA_CONTEXT_SETTING_NAME:
    case PA_CONTEXT_TERMINATED:
    default:
        break;

    case PA_CONTEXT_READY:
        {
            pa_context_set_subscribe_callback(c, subscribe_cb, vud);
            update_default_sink(c, vud);
            pa_operation *o = pa_context_subscribe(
                c, PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER, NULL, NULL);
            free_op(ud->pd, c, o, "pa_context_subscribe");
        }
        break;

    case PA_CONTEXT_FAILED:
        // server disconnected us
        pa_mainloop_quit(ud->ml, 1);
        break;
    }
}

static
bool
iteration(LuastatusPluginData *pd, LuastatusPluginRunFuncs funcs)
{
    bool ret = false;
    UserData ud = {.pd = pd, .funcs = funcs, .def_sink_idx = DEF_SINK_IDX};
    pa_mainloop_api *api = NULL;
    pa_context *ctx = NULL;

    if (!(ud.ml = pa_mainloop_new())) {
        LS_FATALF(pd, "pa_mainloop_new() failed");
        goto error;
    }
    if (!(api = pa_mainloop_get_api(ud.ml))) {
        LS_FATALF(pd, "pa_mainloop_get_api() failed");
        goto error;
    }
    pa_proplist *proplist = pa_proplist_new();
    pa_proplist_sets(proplist, PA_PROP_APPLICATION_NAME, "luastatus-plugin-pulse");
    pa_proplist_sets(proplist, PA_PROP_APPLICATION_ID, "io.github.shdown.luastatus");
    pa_proplist_sets(proplist, PA_PROP_APPLICATION_VERSION, "0.0.1");
    ctx = pa_context_new_with_proplist(api, "luastatus-plugin-pulse", proplist);
    pa_proplist_free(proplist);
    if (!ctx) {
        LS_FATALF(pd, "pa_context_new_with_proplist() failed");
        goto error;
    }

    pa_context_set_state_callback(ctx, context_state_cb, &ud);
    if (pa_context_connect(ctx, NULL, PA_CONTEXT_NOFAIL | PA_CONTEXT_NOAUTOSPAWN, NULL) < 0) {
        LS_FATALF(pd, "pa_context_connect: %s", pa_strerror(pa_context_errno(ctx)));
        goto error;
    }
    ret = true;

    int ignored;
    if (pa_mainloop_run(ud.ml, &ignored) < 0) {
        LS_FATALF(pd, "pa_mainloop_run: %s", pa_strerror(pa_context_errno(ctx)));
        goto error;
    }

error:
    if (ctx) {
        pa_context_unref(ctx);
    }
    if (ud.ml) {
        pa_mainloop_free(ud.ml);
    }
    return ret;
}

static
void
run(LuastatusPluginData *pd, LuastatusPluginRunFuncs funcs)
{
    while (1) {
        if (!iteration(pd, funcs)) {
            nanosleep((struct timespec[1]){{.tv_sec = 5}}, NULL);
        }
    }
}

LuastatusPluginIface luastatus_plugin_iface_v1 = {
    .init = init,
    .run = run,
    .destroy = destroy,
};
