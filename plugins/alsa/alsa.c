#include <errno.h>
#include <lua.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <alsa/asoundlib.h>

#include "include/plugin.h"
#include "include/plugin_logf_macros.h"
#include "include/plugin_utils.h"

#include "libls/alloc_utils.h"
#include "libls/sprintf_utils.h"

typedef struct {
    char *card;
    char *channel;
} Priv;

void
priv_destroy(Priv *p)
{
    free(p->card);
    free(p->channel);
}

LuastatusPluginInitResult
init(LuastatusPluginData *pd, lua_State *L)
{
    Priv *p = pd->priv = LS_XNEW(Priv, 1);
    *p = (Priv) {
        .card = NULL,
        .channel = NULL,
    };

    PU_MAYBE_VISIT_STR("card", s,
        p->card = ls_xstrdup(s);
    );
    if (!p->card) {
        p->card = ls_xstrdup("default");
    }

    PU_MAYBE_VISIT_STR("channel", s,
        p->channel = ls_xstrdup(s);
    );
    if (!p->channel) {
        p->channel = ls_xstrdup("Master");
    }

    return LUASTATUS_PLUGIN_INIT_RESULT_OK;

error:
    priv_destroy(p);
    free(p);
    return LUASTATUS_PLUGIN_INIT_RESULT_ERR;
}

bool
card_has_nicename(const char *realname, snd_ctl_card_info_t *info, const char *nicename)
{
    snd_ctl_t *ctl;
    if (snd_ctl_open(&ctl, realname, 0) < 0) {
        return false;
    }
    bool r = snd_ctl_card_info(ctl, info) >= 0 &&
             strcmp(snd_ctl_card_info_get_name(info), nicename) == 0;
    snd_ctl_close(ctl);
    return r;
}

char *
xalloc_card_realname(const char *nicename)
{
    snd_ctl_card_info_t *info;
    if (snd_ctl_card_info_malloc(&info) < 0) {
        ls_oom();
    }
    static const size_t BUF_SZ = 16;
    char *buf = LS_XNEW(char, BUF_SZ);

    int rcard = -1;
    while (snd_card_next(&rcard) >= 0 && rcard >= 0) {
        ls_xsnprintf(buf, BUF_SZ, "hw:%d", rcard);
        if (card_has_nicename(buf, info, nicename)) {
            goto cleanup;
        }
    }

    free(buf);
    buf = NULL;

cleanup:
    snd_ctl_card_info_free(info);
    return buf;
}

static
void
run(
    LuastatusPluginData *pd,
    LuastatusPluginCallBegin call_begin,
    LuastatusPluginCallEnd call_end)
{
    Priv *p = pd->priv;

    snd_mixer_t *mixer;
    bool mixer_opened = false;
    snd_mixer_selem_id_t *sid;
    bool sid_alloced = false;
    char *realname = NULL;

    if (!(realname = xalloc_card_realname(p->card))) {
        realname = ls_xstrdup(p->card);
    }

    // Actually, the only function that can return -EINTR is snd_mixer_wait,
    // because it uses one of the multiplexing interfaces mentioned below:
    //
    // http://man7.org/linux/man-pages/man7/signal.7.html
    //
    // > The following interfaces are never restarted after being interrupted
    // > by a signal handler, regardless of the use of SA_RESTART; they always
    // > fail with the error EINTR when interrupted by a signal handler:
    // [...]
    // >     * File descriptor multiplexing interfaces: epoll_wait(2),
    // >       epoll_pwait(2), poll(2), ppoll(2), select(2), and pselect(2).
#define ALSA_CALL(Func_, ...) \
    do { \
        int r_; \
        while ((r_ = Func_(__VA_ARGS__)) == -EINTR) {} \
        if (r_ < 0) { \
            LUASTATUS_FATALF(pd, "%s: %s", #Func_, snd_strerror(r_)); \
            goto error; \
        } \
    } while (0)

    ALSA_CALL(snd_mixer_open, &mixer, 0);
    mixer_opened = true;

    ALSA_CALL(snd_mixer_attach, mixer, realname);
    ALSA_CALL(snd_mixer_selem_register, mixer, NULL, NULL);
    ALSA_CALL(snd_mixer_load, mixer);

    ALSA_CALL(snd_mixer_selem_id_malloc, &sid);
    sid_alloced = true;

    snd_mixer_selem_id_set_name(sid, p->channel);
    snd_mixer_elem_t *elem = snd_mixer_find_selem(mixer, sid);
    if (!elem) {
        LUASTATUS_FATALF(pd, "can't find channel '%s'", p->channel);
        goto error;
    }
    while (1) {
        lua_State *L = call_begin(pd->userdata);
        lua_newtable(L); // L: table
        lua_newtable(L); // L: table table
        long pmin, pmax;
        if (snd_mixer_selem_get_playback_volume_range(elem, &pmin, &pmax) >= 0) {
            lua_pushnumber(L, pmin); // L: table table pmin
            lua_setfield(L, -2, "min"); // L: table table
            lua_pushnumber(L, pmax); // L: table table pmax
            lua_setfield(L, -2, "max"); // L: table table
        }
        long pcur;
        if (snd_mixer_selem_get_playback_volume(elem, 0, &pcur) >= 0) {
            lua_pushnumber(L, pcur); // L: table table pcur
            lua_setfield(L, -2, "cur"); // L: table table
        }
        lua_setfield(L, -2, "vol"); // L: table
        int pswitch;
        if (snd_mixer_selem_get_playback_switch(elem, 0, &pswitch) >= 0) {
            lua_pushboolean(L, !pswitch); // L: table !pswitch
            lua_setfield(L, -2, "mute"); // L: table
        }
        call_end(pd->userdata);
        ALSA_CALL(snd_mixer_wait, mixer, -1);
        ALSA_CALL(snd_mixer_handle_events, mixer);
    }
#undef ALSA_CALL
error:
    if (sid_alloced) {
        snd_mixer_selem_id_free(sid);
    }
    if (mixer_opened) {
        snd_mixer_close(mixer);
    }
    free(realname);
}

void
destroy(LuastatusPluginData *pd)
{
    priv_destroy(pd->priv);
    free(pd->priv);
}

LuastatusPluginIface luastatus_plugin_iface = {
    .init = init,
    .run = run,
    .destroy = destroy,
};
