#include "include/plugin.h"
#include "include/plugin_logf_macros.h"
#include "include/plugin_utils.h"

#include <lua.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>

#include "libls/alloc_utils.h"
#include "libls/errno_utils.h"
#include "libls/time_utils.h"
#include "libls/sprintf_utils.h"
#include "libls/osdep.h"
#include "libls/wakeup_fifo.h"
#include "libls/strarr.h"

#include "connect.h"
#include "mpdproto_utils.h"

typedef struct {
    char *hostname;
    int port;
    char *password;
    struct timespec timeout;
    struct timespec retry_in;
    char *retry_fifo;
} Priv;

void
priv_destroy(Priv *p)
{
    free(p->hostname);
    free(p->password);
    free(p->retry_fifo);
}

LuastatusPluginInitResult
init(LuastatusPluginData *pd, lua_State *L)
{
    Priv *p = pd->priv = LS_XNEW(Priv, 1);
    *p = (Priv) {
        .hostname = NULL,
        .port = 6600,
        .password = NULL,
        .timeout = ls_timespec_invalid,
        .retry_in = (struct timespec) {10, 0},
        .retry_fifo = NULL,
    };

    PU_MAYBE_VISIT_STR("hostname", s,
        p->hostname = ls_xstrdup(s);
    );

    PU_MAYBE_VISIT_NUM("port", n,
        if (n < 0 || n > 65535) {
            LUASTATUS_FATALF(pd, "port (%g) is not a valid port number", (double) n);
            goto error;
        }
        p->port = n;
    );

    PU_MAYBE_VISIT_STR("password", s,
        if ((strchr(s, '\n'))) {
            LUASTATUS_FATALF(pd, "password contains a line break");
            goto error;
        }
        p->password = ls_xstrdup(s);
    );

    PU_MAYBE_VISIT_NUM("timeout", n,
        if (ls_timespec_is_invalid(p->timeout = ls_timespec_from_seconds(n)) && n >= 0) {
            LUASTATUS_FATALF(pd, "'timeout' is invalid");
            goto error;
        }
    );

    PU_MAYBE_VISIT_NUM("retry_in", n,
        if (ls_timespec_is_invalid(p->retry_in = ls_timespec_from_seconds(n)) && n >= 0) {
            LUASTATUS_FATALF(pd, "'retry_in' is invalid");
            goto error;
        }
    );

    PU_MAYBE_VISIT_STR("retry_fifo", s,
        p->retry_fifo = ls_xstrdup(s);
    );

    return LUASTATUS_PLUGIN_INIT_RESULT_OK;

error:
    priv_destroy(p);
    free(p);
    return LUASTATUS_PLUGIN_INIT_RESULT_ERR;
}

// string length without trailing newlines
static inline
size_t
dollar_strlen(const char *s)
{
    size_t len = strlen(s);
    while (len && s[len - 1] == '\n') {
        --len;
    }
    return len;
}

// If line is of form "<key>: <value>\n", appends <key> and <value> to sa.
void
kv_strarr_line_append(LSStringArray *sa, const char *line)
{
    const char *colon_pos = strchr(line, ':');
    if (!colon_pos || colon_pos[1] != ' ') {
        return;
    }
    const char *value_pos = colon_pos + 2;
    ls_strarr_append(sa, line, colon_pos - line);
    ls_strarr_append(sa, value_pos, dollar_strlen(value_pos));
}

void
kv_strarr_table_assign(LSStringArray sa, lua_State *L)
{
    // L: table
    const size_t n = ls_strarr_size(sa);
    for (size_t i = 0; i < n; i += 2) {
        size_t nkey;
        const char *key = ls_strarr_at(sa, i, &nkey);
        lua_pushlstring(L, key, nkey); // L: table key
        size_t nvalue;
        const char *value = ls_strarr_at(sa, i + 1, &nvalue);
        lua_pushlstring(L, value, nvalue); // L: table key value
        lua_settable(L, -3); // L: table
    }
}

static inline
void
report_status(LuastatusPluginData *pd,
              LuastatusPluginCallBegin call_begin, LuastatusPluginCallEnd call_end,
              const char *what)
{
    lua_State *L = call_begin(pd->userdata);
    lua_newtable(L); // L: table
    lua_pushstring(L, what); // L: table what
    lua_setfield(L, -2, "what"); // L: table
    call_end(pd->userdata);
}

// Code below is pretty ugly and spaghetti-like. Rewrite it if you can.
void
interact(int fd, LuastatusPluginData *pd,
         LuastatusPluginCallBegin call_begin, LuastatusPluginCallEnd call_end)
{
    Priv *p = pd->priv;

    FILE *f = NULL;
    int fd_to_close = fd;
    char *buf = NULL;
    size_t nbuf = 0;
    LSStringArray kv_song   = LS_STRARR_INITIALIZER;
    LSStringArray kv_status = LS_STRARR_INITIALIZER;

#define GETLINE() \
    do { \
        if (getline(&buf, &nbuf, f) < 0) { \
            goto io_error; \
        } \
    } while (0)

#define WRITE(What_) \
    do { \
        fputs(What_, f); \
        fflush(f); \
        if (ferror(f)) { \
            goto io_error; \
        } \
    } while (0)

#define UNTIL_OK(...) \
    do { \
        GETLINE(); \
        const MPDProtoResponseType rt_ = mpdproto_response_type(buf); \
        if (rt_ == MPDPROTO_RESP_TYPE_OK) { \
            break; \
        } else if (rt_ == MPDPROTO_RESP_TYPE_ACK) { \
            LUASTATUS_ERRF(pd, "server said: %.*s", (int) dollar_strlen(buf), buf); \
            goto error; \
        } else { \
            __VA_ARGS__ \
        } \
    } while (/*note infinite cycle here*/ 1)

    if (!(f = fdopen(fd, "r+"))) {
        LS_WITH_ERRSTR(str, errno,
            LUASTATUS_ERRF(pd, "fdopen: %s", str);
        );
        goto error;
    }
    fd_to_close = -1;

    // read and check the greeting
    GETLINE();
    if (strncmp(buf, "OK MPD ", 7) != 0) {
        LUASTATUS_ERRF(pd, "bad greeting: %.*s", (int) dollar_strlen(buf), buf);
        goto error;
    }

    // send the password, if specified
    if (p->password) {
        fputs("password ", f);
        mpdproto_write_quoted(f, p->password);
        fputc('\n', f);
        fflush(f);
        if (ferror(f)) {
            goto io_error;
        }
        GETLINE();
        if (mpdproto_response_type(buf) != MPDPROTO_RESP_TYPE_OK) {
            LUASTATUS_ERRF(pd, "(password) server said: %.*s", (int) dollar_strlen(buf), buf);
            goto error;
        }
    }

    sigset_t allsigs;
    if (sigfillset(&allsigs) < 0) {
        LS_WITH_ERRSTR(str, errno,
            LUASTATUS_ERRF(pd, "sigfillset: %s", str);
        );
        goto error;
    }

    if (fd >= FD_SETSIZE && !ls_timespec_is_invalid(p->timeout)) {
        LUASTATUS_WARNF(pd, "connection file descriptor is too large, will not report time outs");
    }

    fd_set fds;
    FD_ZERO(&fds);

    while (1) {
        WRITE("currentsong\n");
        UNTIL_OK(
            kv_strarr_line_append(&kv_song, buf);
        );

        WRITE("status\n");
        UNTIL_OK(
            kv_strarr_line_append(&kv_status, buf);
        );

        lua_State *L = call_begin(pd->userdata);
        lua_newtable(L); // L: table

        lua_pushstring(L, "update"); // L: table "update"
        lua_setfield(L, -2, "what"); // L: table

        lua_newtable(L); // L: table table
        kv_strarr_table_assign(kv_song, L); // L: table table
        ls_strarr_clear(&kv_song);
        lua_setfield(L, -2, "song"); // L: table

        lua_newtable(L); // L: table table
        kv_strarr_table_assign(kv_status, L); // L: table table
        ls_strarr_clear(&kv_status);
        lua_setfield(L, -2, "status"); // L: table

        call_end(pd->userdata);

        WRITE("idle player mixer\n");

        if (fd < FD_SETSIZE && !ls_timespec_is_invalid(p->timeout)) {
            while (1) {
                FD_SET(fd, &fds);
                int r = pselect(fd + 1, &fds, NULL, NULL, &p->timeout, &allsigs);
                if (r < 0) {
                    LS_WITH_ERRSTR(str, errno,
                        LUASTATUS_ERRF(pd, "pselect (on connection file descriptor): %s", str);
                    );
                    goto error;
                } else if (r == 0) {
                    report_status(pd, call_begin, call_end, "timeout");
                } else {
                    break;
                }
            }
        }

        UNTIL_OK(
            // do nothing
        );
    }
#undef UNTIL_OK

io_error:
    if (feof(f)) {
        LUASTATUS_ERRF(pd, "server closed the connection");
    } else {
        LS_WITH_ERRSTR(str, errno,
            LUASTATUS_ERRF(pd, "I/O error: %s", str);
        );
    }

error:
    if (f) {
        fclose(f);
    }
    ls_close(fd_to_close);
    free(buf);
    ls_strarr_destroy(kv_song);
    ls_strarr_destroy(kv_status);
}

void
run(LuastatusPluginData *pd, LuastatusPluginCallBegin call_begin, LuastatusPluginCallEnd call_end)
{
    Priv *p = pd->priv;

    LSWakeupFifo w;
    ls_wakeup_fifo_init(&w);

    sigset_t allsigs;
    if (sigfillset(&allsigs) < 0) {
        LS_WITH_ERRSTR(s, errno,
            LUASTATUS_FATALF(pd, "sigfillset: %s", s);
        );
        goto error;
    }
    w.fifo = p->retry_fifo;
    w.timeout = &p->retry_in;
    w.sigmask = &allsigs;

    char portstr[8];
    ls_xsnprintf(portstr, sizeof(portstr), "%d", p->port);

    while (1) {
        report_status(pd, call_begin, call_end, "connecting");

        int fd = (p->hostname && p->hostname[0] == '/')
            ? socket_open(pd, p->hostname)
            : tcp_open(pd, p->hostname, portstr);
        if (fd >= 0) {
            interact(fd, pd, call_begin, call_end);
        }

        if (ls_timespec_is_invalid(p->retry_in)) {
            LUASTATUS_FATALF(pd, "an error occurred; not retrying as requested");
            goto error;
        }

        report_status(pd, call_begin, call_end, "error");

        if (ls_wakeup_fifo_open(&w) < 0) {
            LS_WITH_ERRSTR(s, errno,
                LUASTATUS_WARNF(pd, "open: %s: %s", p->retry_fifo, s);
            );
        }
        if (ls_wakeup_fifo_pselect(&w) < 0) {
            LS_WITH_ERRSTR(s, errno,
                LUASTATUS_FATALF(pd, "pselect (on retry_fifo): %s", s);
            );
            goto error;
        }
    }

error:
    ls_wakeup_fifo_destroy(&w);
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
