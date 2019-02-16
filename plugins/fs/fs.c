#include <errno.h>
#include <stdbool.h>
#include <lua.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>
#include <sys/statvfs.h>

#include "include/plugin_v1.h"
#include "include/sayf_macros.h"
#include "include/plugin_utils.h"

#include "libls/alloc_utils.h"
#include "libls/lua_utils.h"
#include "libls/vector.h"
#include "libls/time_utils.h"
#include "libls/cstring_utils.h"
#include "libls/wakeup_fifo.h"

typedef struct {
    LS_VECTOR_OF(char *) paths;
    struct timespec period;
    char *fifo;
} Priv;

static
void
destroy(LuastatusPluginData *pd)
{
    Priv *p = pd->priv;
    for (size_t i = 0; i < p->paths.size; ++i) {
        free(p->paths.data[i]);
    }
    LS_VECTOR_FREE(p->paths);
    free(p->fifo);
    free(p);
}

static
int
init(LuastatusPluginData *pd, lua_State *L)
{
    Priv *p = pd->priv = LS_XNEW(Priv, 1);
    *p = (Priv) {
        .paths = LS_VECTOR_NEW(),
        .period = (struct timespec) {10, 0},
        .fifo = NULL,
    };

    PU_VISIT_TABLE_FIELD(-1, "paths", "'paths'",
        PU_CHECK_TYPE(LS_LUA_KEY, "'paths' key", LUA_TNUMBER);
        PU_VISIT_STR(LS_LUA_VALUE, "'paths' element", s,
            LS_VECTOR_PUSH(p->paths, ls_xstrdup(s));
        );
    );
    if (!p->paths.size) {
        LS_WARNF(pd, "paths are empty");
    }

    PU_MAYBE_VISIT_NUM_FIELD(-1, "period", "'period'", n,
        if (ls_timespec_is_invalid(p->period = ls_timespec_from_seconds(n))) {
            LS_FATALF(pd, "invalid 'period' value");
            goto error;
        }
    );

    PU_MAYBE_VISIT_STR_FIELD(-1, "fifo", "'fifo'", s,
        p->fifo = ls_xstrdup(s);
    );

    return LUASTATUS_OK;

error:
    destroy(pd);
    return LUASTATUS_ERR;
}

static
bool
push_for(LuastatusPluginData *pd, lua_State *L, const char *path)
{
    struct statvfs st;
    if (statvfs(path, &st) < 0) {
        LS_WARNF(pd, "statvfs: %s: %s", path, ls_strerror_onstack(errno));
        return false;
    }
    lua_createtable(L, 0, 3); // L: table
    lua_pushnumber(L, (double) st.f_frsize * st.f_blocks); // L: table n
    lua_setfield(L, -2, "total"); // L: table
    lua_pushnumber(L, (double) st.f_frsize * st.f_bfree); // L: table n
    lua_setfield(L, -2, "free"); // L: table
    lua_pushnumber(L, (double) st.f_frsize * st.f_bavail); // L: table n
    lua_setfield(L, -2, "avail"); // L: table
    return true;
}

static
void
run(LuastatusPluginData *pd, LuastatusPluginRunFuncs funcs)
{
    Priv *p = pd->priv;

    LSWakeupFifo w;
    ls_wakeup_fifo_init(&w, p->fifo, p->period, NULL);

    while (1) {
        // make a call
        lua_State *L = funcs.call_begin(pd->userdata);
        lua_createtable(L, 0, p->paths.size);
        for (size_t i = 0; i < p->paths.size; ++i) {
            const char *path = p->paths.data[i];
            if (push_for(pd, L, path)) {
                lua_setfield(L, -2, path);
            }
        }
        funcs.call_end(pd->userdata);
        // wait
        if (ls_wakeup_fifo_open(&w) < 0) {
            LS_WARNF(pd, "ls_wakeup_fifo_open: %s: %s", p->fifo, ls_strerror_onstack(errno));
        }
        if (ls_wakeup_fifo_wait(&w) < 0) {
            LS_FATALF(pd, "ls_wakeup_fifo_wait: %s: %s", p->fifo, ls_strerror_onstack(errno));
            goto error;
        }
    }

error:
    ls_wakeup_fifo_destroy(&w);
}

LuastatusPluginIface luastatus_plugin_iface_v1 = {
    .init = init,
    .run = run,
    .destroy = destroy,
};
