// lua_bridge.c — embedded Lua 5.4.8 scripting engine for libjamesdspaidl.so
//
// Full mpv-derived Lua API adapted for DSP use.
//
// Registered as `jdsp.*` and `jdsp.utils.*` globals. Scripts define:
//   function tick()              — called ~30Hz from audio thread
//   function process(spl0, spl1) — called per-sample (return modified values)
//   function process_block(n)    — called per-block (modify jdsp.buffer_L/R)
//   function on_event(name)      — called on stream start/stop
//
// Standard Lua io.open/write/close works (luaL_openlibs loaded) —
// scripts can write to FIFOs, files, etc. for data bridging.

#include "lua_bridge.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <android/log.h>
#include <errno.h>
#include <math.h>

// ============================================================================
// Thread safety: g_L is touched from the audio thread (tick, process, events)
// and from whatever thread handles AIDL parameter commands (load_script).
// This mutex serializes every entry point.
// ============================================================================
static pthread_mutex_t g_lua_mutex = PTHREAD_MUTEX_INITIALIZER;

// Forward declarations — defined in Lifecycle section below, referenced by
// hook_fire, process_sample, and other "public" API functions that sit above
// the lifecycle block in this file.
static lua_State *g_L = NULL;
static int g_script_loaded = 0;

// ============================================================================
// Timer system
// ============================================================================

typedef struct jdsp_timer {
    struct jdsp_timer *next;
    double interval;        // seconds between fires (0 = one-shot timeout)
    double next_fire;       // absolute monotonic time of next fire
    int    lua_ref;         // registry ref to the Lua callback function
    int    active;
} jdsp_timer_t;

static jdsp_timer_t *g_timers = NULL;
static double        g_last_tick_time = 0.0;

// ============================================================================
// Event queue (simple ring buffer)
// ============================================================================

#define JDSP_EVENT_QUEUE_SIZE 32
static char g_event_queue[JDSP_EVENT_QUEUE_SIZE][128];
static int  g_event_read  = 0;
static int  g_event_write = 0;
static int  g_event_count = 0;

// ============================================================================
// Property registry
// ============================================================================

#define JDSP_MAX_PROPS 64

typedef struct {
    char             name[64];
    jdsp_prop_getter getter;
    jdsp_prop_setter setter;
    int              observer_ref;  // Lua registry ref to observer fn, LUA_NOREF if none
} jdsp_prop_t;

static jdsp_prop_t g_props[JDSP_MAX_PROPS];
static int         g_prop_count = 0;

// Numeric telemetry value store. Populated from the DSP audio thread's 30Hz
// tick via lua_bridge_set_property_number() and read by the get_property*
// accessors below as a fallback when the name has no registered getter. This
// is the read-side path for live metering values (gain reduction, band
// energies, etc.) that the audio thread already computes — separate from the
// getter/setter pair registry above, which is for script-controllable params.
#define JDSP_MAX_PROP_VALUES 128
typedef struct {
    char   name[64];
    double value;
} jdsp_prop_value_t;
static jdsp_prop_value_t g_prop_values[JDSP_MAX_PROP_VALUES];
static int                g_prop_value_count = 0;

// ============================================================================
// Script metadata
// ============================================================================

static char g_script_dir[512] = {0};

// ============================================================================
// JDSP API functions exposed to Lua
// ============================================================================

// --- Band energy --------------------------------------------------------
// Must match jdsp_header.h's LUA_BAND_METER_COUNT (the native meter writes
// this many taps per tick). Kept as a local constant because lua_bridge.c
// does not include jdsp_header.h (separate build unit from the engine).
#define JDSP_BAND_ENERGY_MAX (108)

static float g_band_energy[JDSP_BAND_ENERGY_MAX] = {0};
static int   g_num_bands = 0;

static int jdsp_get_band_energy(lua_State *L) {
    int n = lua_tointeger(L, 1);
    if (n < 1 || n > g_num_bands) { lua_pushnumber(L, 0.0); return 1; }
    lua_pushnumber(L, (double)g_band_energy[n - 1]);
    return 1;
}

// --- Audio level --------------------------------------------------------

static float g_audio_level = 0.0f;
static int jdsp_get_audio_level(lua_State *L) {
    lua_pushnumber(L, (double)g_audio_level);
    return 1;
}

// --- Post-gain ----------------------------------------------------------

static float g_postgain = 1.0f;
static int jdsp_get_postgain(lua_State *L) {
    lua_pushnumber(L, (double)g_postgain);
    return 1;
}

// --- Module states ------------------------------------------------------

static const char *g_module_names[] = {
    "gate", "compressor", "autogain", "reverb", "stereo", "tube",
    "convolver", "eq", "crossfeed", "ddc", "arbeq", "liveprog", "lua",
    "expander", // appended (index 13) -- jdspController.c moduleStates[] must match
    "loudcomp"  // appended (index 14)
};
#define JDSP_MODULE_COUNT ((int)(sizeof(g_module_names)/sizeof(g_module_names[0])))
static int g_module_states[JDSP_MODULE_COUNT] = {0};

static int jdsp_get_enabled(lua_State *L) {
    const char *name = lua_tostring(L, 1);
    if (name) {
        for (int i = 0; i < JDSP_MODULE_COUNT; i++) {
            if (strcmp(name, g_module_names[i]) == 0) {
                lua_pushboolean(L, g_module_states[i]);
                return 1;
            }
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

// --- Sample rate --------------------------------------------------------

static int g_sample_rate = 48000;
static int jdsp_get_sample_rate(lua_State *L) {
    lua_pushinteger(L, g_sample_rate);
    return 1;
}

// --- Logging ------------------------------------------------------------

static int jdsp_log(lua_State *L) {
    const char *level = lua_tostring(L, 1);
    const char *msg   = lua_tostring(L, 2);
    int prio = ANDROID_LOG_INFO;
    if (level) {
        if (strcmp(level, "error") == 0) prio = ANDROID_LOG_ERROR;
        else if (strcmp(level, "warn") == 0) prio = ANDROID_LOG_WARN;
        else if (strcmp(level, "debug") == 0) prio = ANDROID_LOG_DEBUG;
    }
    __android_log_print(prio, "JDSP-Lua", "%s", msg ? msg : "");
    return 0;
}

// --- Monotonic time (seconds, double precision) -------------------------

static int jdsp_get_time(lua_State *L) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    lua_pushnumber(L, (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9);
    return 1;
}

// --- Timers -------------------------------------------------------------

static void timer_free_ref(lua_State *L, jdsp_timer_t *t) {
    if (t->lua_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, t->lua_ref);
        t->lua_ref = LUA_NOREF;
    }
}

static int jdsp_add_timeout(lua_State *L) {
    double seconds = luaL_checknumber(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    jdsp_timer_t *t = (jdsp_timer_t*)malloc(sizeof(jdsp_timer_t));
    memset(t, 0, sizeof(*t));
    t->interval  = 0.0;  // one-shot
    t->lua_ref   = LUA_NOREF;
    t->active    = 1;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t->next_fire = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9 + seconds;

    lua_pushvalue(L, 2);
    t->lua_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // Insert sorted by next_fire
    jdsp_timer_t **pp = &g_timers;
    while (*pp && (*pp)->next_fire <= t->next_fire) pp = &(*pp)->next;
    t->next = *pp;
    *pp = t;

    lua_pushlightuserdata(L, t);
    return 1;
}

static int jdsp_add_periodic_timer(lua_State *L) {
    double seconds = luaL_checknumber(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    jdsp_timer_t *t = (jdsp_timer_t*)malloc(sizeof(jdsp_timer_t));
    memset(t, 0, sizeof(*t));
    t->interval  = seconds;
    t->lua_ref   = LUA_NOREF;
    t->active    = 1;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t->next_fire = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9 + seconds;

    lua_pushvalue(L, 2);
    t->lua_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    jdsp_timer_t **pp = &g_timers;
    while (*pp && (*pp)->next_fire <= t->next_fire) pp = &(*pp)->next;
    t->next = *pp;
    *pp = t;

    lua_pushlightuserdata(L, t);
    return 1;
}

static int jdsp_cancel_timer(lua_State *L) {
    jdsp_timer_t *t = (jdsp_timer_t*)lua_touserdata(L, 1);
    if (!t) return 0;
    t->active = 0;
    return 0;
}

// --- Properties ---------------------------------------------------------

// Look up a telemetry value in the value store (the tick-pushed cache).
// Returns 1 and stores the value in *out if found, 0 otherwise.
static int prop_value_lookup(const char *name, double *out) {
    for (int i = 0; i < g_prop_value_count; i++) {
        if (strcmp(name, g_prop_values[i].name) == 0) {
            if (out) *out = g_prop_values[i].value;
            return 1;
        }
    }
    return 0;
}

static int jdsp_get_property(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    for (int i = 0; i < g_prop_count; i++) {
        if (strcmp(name, g_props[i].name) == 0) {
            if (g_props[i].getter) {
                lua_pushnumber(L, (double)g_props[i].getter());
                return 1;
            }
            lua_pushnil(L);
            lua_pushstring(L, "no getter registered");
            return 2;
        }
    }
    double v;
    if (prop_value_lookup(name, &v)) {
        lua_pushnumber(L, v);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, "unknown property");
    return 2;
}

static int jdsp_get_property_bool(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    for (int i = 0; i < g_prop_count; i++) {
        if (strcmp(name, g_props[i].name) == 0) {
            if (g_props[i].getter) {
                lua_pushboolean(L, g_props[i].getter() != 0.0);
                return 1;
            }
            lua_pushboolean(L, 0);
            return 1;
        }
    }
    double v;
    if (prop_value_lookup(name, &v)) {
        lua_pushboolean(L, v != 0.0);
        return 1;
    }
    lua_pushboolean(L, 0);
    return 1;
}

static int jdsp_get_property_number(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    for (int i = 0; i < g_prop_count; i++) {
        if (strcmp(name, g_props[i].name) == 0) {
            if (g_props[i].getter) {
                lua_pushnumber(L, (double)g_props[i].getter());
                return 1;
            }
            lua_pushnumber(L, 0.0);
            return 1;
        }
    }
    double v;
    if (prop_value_lookup(name, &v)) {
        lua_pushnumber(L, v);
        return 1;
    }
    lua_pushnumber(L, 0.0);
    return 1;
}

static int jdsp_set_property(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    double value = luaL_checknumber(L, 2);
    for (int i = 0; i < g_prop_count; i++) {
        if (strcmp(name, g_props[i].name) == 0) {
            if (g_props[i].setter) {
                int rc = g_props[i].setter(value);
                if (rc == 0) {
                    // Fire observer
                    if (g_props[i].observer_ref != LUA_NOREF) {
                        lua_rawgeti(L, LUA_REGISTRYINDEX, g_props[i].observer_ref);
                        if (lua_isfunction(L, -1)) {
                            lua_pushstring(L, name);
                            lua_pushnumber(L, value);
                            lua_pcall(L, 2, 0, 0);
                        } else {
                            lua_pop(L, 1);
                        }
                    }
                    lua_pushboolean(L, 1);
                    return 1;
                }
            }
            lua_pushnil(L);
            lua_pushstring(L, "setter failed");
            return 2;
        }
    }
    lua_pushnil(L);
    lua_pushstring(L, "unknown property");
    return 2;
}

static int jdsp_set_property_bool(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    double value = lua_toboolean(L, 2) ? 1.0 : 0.0;
    for (int i = 0; i < g_prop_count; i++) {
        if (strcmp(name, g_props[i].name) == 0) {
            if (g_props[i].setter) {
                g_props[i].setter(value);
                lua_pushboolean(L, 1);
                return 1;
            }
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

static int jdsp_set_property_number(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    double value = luaL_checknumber(L, 2);
    for (int i = 0; i < g_prop_count; i++) {
        if (strcmp(name, g_props[i].name) == 0) {
            if (g_props[i].setter) {
                g_props[i].setter(value);
                lua_pushboolean(L, 1);
                return 1;
            }
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

static int jdsp_del_property(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    for (int i = 0; i < g_prop_count; i++) {
        if (strcmp(name, g_props[i].name) == 0) {
            if (g_props[i].setter) {
                g_props[i].setter(0.0); // reset to default via setter
            }
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

static int jdsp_observe_property(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    for (int i = 0; i < g_prop_count; i++) {
        if (strcmp(name, g_props[i].name) == 0) {
            if (g_props[i].observer_ref != LUA_NOREF)
                luaL_unref(L, LUA_REGISTRYINDEX, g_props[i].observer_ref);
            if (lua_type(L, 2) == LUA_TFUNCTION) {
                lua_pushvalue(L, 2);
                g_props[i].observer_ref = luaL_ref(L, LUA_REGISTRYINDEX);
            } else {
                g_props[i].observer_ref = LUA_NOREF;
            }
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

static int jdsp_unobserve_property(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    for (int i = 0; i < g_prop_count; i++) {
        if (strcmp(name, g_props[i].name) == 0) {
            if (g_props[i].observer_ref != LUA_NOREF) {
                luaL_unref(L, LUA_REGISTRYINDEX, g_props[i].observer_ref);
                g_props[i].observer_ref = LUA_NOREF;
            }
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

// --- Events -------------------------------------------------------------

static int jdsp_request_event(lua_State *L) {
    // Event subscription — no-op for now (events are always delivered).
    // The script receives all events via on_event().
    lua_pushboolean(L, 1);
    return 1;
}

// Returns the next queued event string, or nil if queue is empty.
static int jdsp_wait_event(lua_State *L) {
    (void)L;
    if (g_event_count > 0) {
        lua_pushstring(L, g_event_queue[g_event_read]);
        g_event_read = (g_event_read + 1) % JDSP_EVENT_QUEUE_SIZE;
        g_event_count--;
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

// --- Commands -----------------------------------------------------------

static jdsp_cmd_handler g_cmd_handler = NULL;

void lua_bridge_set_command_handler(jdsp_cmd_handler handler) {
    g_cmd_handler = handler;
}

// jdsp.command_raw(cmd, p1, p2, ...) — raw AIDL command dispatch
static int jdsp_command_raw(lua_State *L) {
    if (!g_cmd_handler) { lua_pushboolean(L, 0); return 1; }

    int cmd = (int)luaL_checkinteger(L, 1);
    int n = lua_gettop(L) - 1;
    float params[32];
    if (n > 32) n = 32;
    for (int i = 0; i < n; i++)
        params[i] = (float)lua_tonumber(L, i + 2);

    int rc = g_cmd_handler(cmd, n > 0 ? params : NULL, n);
    lua_pushboolean(L, rc == 0);
    return 1;
}

// Plain C toggle helper — does the actual module enable/disable dispatch.
// Shared by jdsp_toggle_module / jdsp_command / jdsp_commandv so the
// name→command mapping lives in exactly one place. Returns 0 on success,
// -1 for unknown module, or the handler's non-zero error code.
static int toggle_module_dispatch(const char *name, int enable) {
    if (!g_cmd_handler) return -1;

    // Module name → command ID mapping (matches jamesdsp.c dispatch). These
    // are pure enable/disable commands (params[0] = 0/1); gate/compressor are
    // handled separately below because their param bundles are not enable-only.
    static const struct { const char *name; int cmd; } map[] = {
        {"bassboost",     1201},
        {"equalizer",     1202},
        {"reverb",        1203},
        {"stereo",        1204},
        {"convolver",     1205},
        {"tube",          1206},
        {"autogain",      1207},
        {"crossfeed",     1208},
        {"arbeq",         1210},
        {"arb_eq",        1210}, // legacy alias
        {"ddc",           1212},
        {"liveprog",      1213},
        {NULL, 0}
    };

    for (int i = 0; map[i].name; i++) {
        if (strcmp(name, map[i].name) == 0) {
            float p = (float)enable;
            return g_cmd_handler(map[i].cmd, &p, 1);
        }
    }

    // Gate/compressor: enable flag lives on the jdsp struct and drives the
    // LSP wrapper's internal Bypass crossfade each block — flipping it alone
    // is sufficient to toggle, and re-sending a fixed param preset here would
    // silently stomp whatever the user actually configured. Send enable-only
    // (nparams==1); the dispatch handler flips the flag without reconfiguring.
    if (strcmp(name, "gate") == 0) {
        float p = (float)enable;
        return g_cmd_handler(10100, &p, 1);
    }
    if (strcmp(name, "compressor") == 0) {
        float p = (float)enable;
        return g_cmd_handler(10101, &p, 1);
    }
    if (strcmp(name, "expander") == 0) {
        float p = (float)enable;
        return g_cmd_handler(10103, &p, 1);
    }
    if (strcmp(name, "loudcomp") == 0) {
        float p = (float)enable;
        return g_cmd_handler(10105, &p, 1);
    }

    return -1; // unknown module
}

// jdsp.toggle_module(name, enable) — convenient module on/off
static int jdsp_toggle_module(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    int enable = lua_toboolean(L, 2);
    int rc = toggle_module_dispatch(name, enable);
    if (rc < 0) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "unknown module");
        return 2;
    }
    lua_pushboolean(L, rc == 0);
    return 1;
}

// Legacy command() — parses "name value" string
static int jdsp_command(lua_State *L) {
    const char *cmd = luaL_checkstring(L, 1);
    // Try toggle: "equalizer enable" / "gate disable"
    char mod[64] = {0};
    char action[16] = {0};
    if (sscanf(cmd, "%63s %15s", mod, action) == 2) {
        int enable = (strcmp(action, "enable") == 0 || strcmp(action, "1") == 0 ||
                      strcmp(action, "on") == 0 || strcmp(action, "true") == 0);
        // Previously this called jdsp_toggle_module(L) directly, which reads
        // its args from absolute Lua indices 1/2 — the original single string
        // arg, not the parsed mod/action. Route through the plain C helper so
        // the parsed values are actually used.
        int rc = toggle_module_dispatch(mod, enable);
        if (rc < 0) {
            lua_pushboolean(L, 0);
            lua_pushstring(L, "unknown module");
            return 2;
        }
        lua_pushboolean(L, rc == 0);
        return 1;
    }
    // Fallback: raw dispatch via g_cmd_handler if present
    if (g_cmd_handler) {
        int id = atoi(cmd);
        if (id > 0) { g_cmd_handler(id, NULL, 0); lua_pushboolean(L, 1); return 1; }
    }
    lua_pushboolean(L, 1);
    return 1;
}

// jdsp.commandv(...) — varargs command
static int jdsp_commandv(lua_State *L) {
    int n = lua_gettop(L);
    if (n < 1) { lua_pushboolean(L, 0); return 1; }
    const char *first = lua_tostring(L, 1);
    if (first && strcmp(first, "toggle") == 0 && n >= 3) {
        // usage: commandv("toggle", "equalizer", "enable")
        const char *mod = lua_tostring(L, 2);
        const char *act = lua_tostring(L, 3);
        if (mod && act) {
            int en = (strcmp(act, "enable") == 0 || strcmp(act, "on") == 0 || strcmp(act, "1") == 0);
            int rc = toggle_module_dispatch(mod, en);
            if (rc < 0) {
                lua_pushboolean(L, 0);
                lua_pushstring(L, "unknown module");
                return 2;
            }
            lua_pushboolean(L, rc == 0);
            return 1;
        }
    }
    lua_pushboolean(L, 1);
    return 1;
}

// ============================================================================
// Hook system (named chain intercepts)
// ============================================================================

#define JDSP_MAX_HOOKS 128

typedef struct jdsp_hook {
    struct jdsp_hook *next;
    char  name[64];
    int   priority;
    int   lua_ref;     // registry ref to callback(L, R, n)
    int   active;
} jdsp_hook_t;

static jdsp_hook_t *g_hooks = NULL;
static int          g_hook_count = 0;

static int jdsp_hook_add(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    int priority = (int)luaL_optinteger(L, 2, 50);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    if (g_hook_count >= JDSP_MAX_HOOKS) {
        lua_pushnil(L);
        lua_pushstring(L, "too many hooks");
        return 2;
    }

    jdsp_hook_t *h = (jdsp_hook_t*)malloc(sizeof(jdsp_hook_t));
    memset(h, 0, sizeof(*h));
    strncpy(h->name, name, sizeof(h->name) - 1);
    h->name[sizeof(h->name) - 1] = '\0';
    h->priority = priority;
    h->active   = 1;
    h->lua_ref  = LUA_NOREF;
    h->next     = NULL;

    lua_pushvalue(L, 3);
    h->lua_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // Insert sorted by priority (higher = earlier)
    jdsp_hook_t **pp = &g_hooks;
    while (*pp && (*pp)->priority >= priority) pp = &(*pp)->next;
    h->next = *pp;
    *pp = h;
    g_hook_count++;

    lua_pushlightuserdata(L, h);
    return 1;
}

static int jdsp_hook_continue(lua_State *L) {
    (void)L;
    // In JDSP hooks are synchronous and fire sequentially — no explicit
    // continue signal needed. Provided for API compatibility with mpv scripts.
    lua_pushboolean(L, 1);
    return 1;
}

static int jdsp_hook_remove(lua_State *L) {
    jdsp_hook_t *h = (jdsp_hook_t*)lua_touserdata(L, 1);
    if (!h) return 0;
    h->active = 0;
    return 0;
}

// Fire all active hooks registered for a given chain point name.
// Called from chain module wrappers before each module's processing.
void lua_bridge_hook_fire(const char *name, float *Lbuf, float *Rbuf, int n) {
    if (!g_L || !g_script_loaded || !g_hooks) return;

    for (jdsp_hook_t *h = g_hooks; h; h = h->next) {
        if (!h->active) continue;
        if (strcmp(h->name, name) != 0) continue;

        lua_rawgeti(g_L, LUA_REGISTRYINDEX, h->lua_ref);
        if (!lua_isfunction(g_L, -1)) { lua_pop(g_L, 1); continue; }

        // Push buffer tables
        lua_getglobal(g_L, "jdsp_buffer_L");
        for (int i = 0; i < n; i++) {
            lua_pushnumber(g_L, (double)Lbuf[i]);
            lua_rawseti(g_L, -2, i + 1);
        }

        lua_getglobal(g_L, "jdsp_buffer_R");
        for (int i = 0; i < n; i++) {
            lua_pushnumber(g_L, (double)Rbuf[i]);
            lua_rawseti(g_L, -2, i + 1);
        }

        // Call hook(L, R, n)
        lua_pushinteger(g_L, n);
        if (lua_pcall(g_L, 1, 0, 0) == LUA_OK) {
            // Read modified buffers back
            lua_getglobal(g_L, "jdsp_buffer_L");
            for (int i = 0; i < n; i++) {
                lua_rawgeti(g_L, -1, i + 1);
                Lbuf[i] = (float)lua_tonumber(g_L, -1);
                lua_pop(g_L, 1);
            }
            lua_pop(g_L, 1);

            lua_getglobal(g_L, "jdsp_buffer_R");
            for (int i = 0; i < n; i++) {
                lua_rawgeti(g_L, -1, i + 1);
                Rbuf[i] = (float)lua_tonumber(g_L, -1);
                lua_pop(g_L, 1);
            }
            lua_pop(g_L, 1);
        } else {
            const char *err = lua_tostring(g_L, -1);
            if (err) __android_log_print(ANDROID_LOG_ERROR, "JDSP-Lua",
                                          "hook '%s' error: %s", name, err);
            lua_pop(g_L, 1);
        }
    }
}

// --- Config / paths -----------------------------------------------------

static int jdsp_find_config_file(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    char path[512];
    snprintf(path, sizeof(path), "/data/data/com.termux.jdsp/files/%s", name);
    FILE *f = fopen(path, "r");
    if (f) { fclose(f); lua_pushstring(L, path); return 1; }
    lua_pushnil(L);
    return 1;
}

static int jdsp_get_script_directory(lua_State *L) {
    if (g_script_dir[0]) {
        lua_pushstring(L, g_script_dir);
        return 1;
    }
    lua_pushstring(L, "/data/data/com.termux.jdsp/files/scripts");
    return 1;
}

// --- Enable messages ----------------------------------------------------

static int g_msg_level = 3; // info by default
static int jdsp_enable_messages(lua_State *L) {
    const char *level = luaL_checkstring(L, 1);
    if (strcmp(level, "error") == 0) g_msg_level = 1;
    else if (strcmp(level, "warn") == 0) g_msg_level = 2;
    else if (strcmp(level, "info") == 0) g_msg_level = 3;
    else if (strcmp(level, "debug") == 0) g_msg_level = 4;
    else if (strcmp(level, "trace") == 0) g_msg_level = 5;
    lua_pushboolean(L, 1);
    return 1;
}

// --- jdsp module registration table --------------------------------------

static const luaL_Reg jdsp_lib[] = {
    // DSP metrics (existing)
    {"get_band_energy", jdsp_get_band_energy},
    {"get_audio_level", jdsp_get_audio_level},
    {"get_postgain", jdsp_get_postgain},
    {"get_enabled", jdsp_get_enabled},
    {"get_sample_rate", jdsp_get_sample_rate},

    // Logging
    {"log", jdsp_log},
    {"enable_messages", jdsp_enable_messages},

    // Time
    {"get_time", jdsp_get_time},

    // Timers
    {"add_timeout", jdsp_add_timeout},
    {"add_periodic_timer", jdsp_add_periodic_timer},
    {"cancel_timer", jdsp_cancel_timer},

    // Properties
    {"get_property", jdsp_get_property},
    {"get_property_bool", jdsp_get_property_bool},
    {"get_property_number", jdsp_get_property_number},
    {"set_property", jdsp_set_property},
    {"set_property_bool", jdsp_set_property_bool},
    {"set_property_number", jdsp_set_property_number},
    {"del_property", jdsp_del_property},
    {"observe_property", jdsp_observe_property},
    {"unobserve_property", jdsp_unobserve_property},

    // Events
    {"request_event", jdsp_request_event},
    {"wait_event", jdsp_wait_event},

    // Commands
    {"command", jdsp_command},
    {"commandv", jdsp_commandv},
    {"command_raw", jdsp_command_raw},
    {"toggle_module", jdsp_toggle_module},

    // Hooks
    {"hook_add", jdsp_hook_add},
    {"hook_continue", jdsp_hook_continue},
    {"hook_remove", jdsp_hook_remove},

    // Paths
    {"find_config_file", jdsp_find_config_file},
    {"get_script_directory", jdsp_get_script_directory},

    {NULL, NULL}
};

// ============================================================================
// jdsp.utils module
// ============================================================================

static int utils_readdir(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    DIR *d = opendir(path);
    if (!d) { lua_pushnil(L); lua_pushstring(L, strerror(errno)); return 2; }
    lua_newtable(L);
    int idx = 1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        lua_pushstring(L, ent->d_name);
        lua_rawseti(L, -2, idx++);
    }
    closedir(d);
    return 1;
}

static int utils_file_info(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    struct stat st;
    if (stat(path, &st) != 0) { lua_pushnil(L); lua_pushstring(L, strerror(errno)); return 2; }
    lua_newtable(L);
    lua_pushinteger(L, st.st_size);        lua_setfield(L, -2, "size");
    lua_pushinteger(L, (int)st.st_mode);    lua_setfield(L, -2, "mode");
    lua_pushinteger(L, (int)st.st_mtime);   lua_setfield(L, -2, "mtime");
    lua_pushboolean(L, S_ISDIR(st.st_mode)); lua_setfield(L, -2, "is_dir");
    lua_pushboolean(L, S_ISREG(st.st_mode)); lua_setfield(L, -2, "is_file");
    return 1;
}

static int utils_split_path(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    const char *slash = strrchr(path, '/');
    if (slash) {
        lua_pushlstring(L, path, (size_t)(slash - path)); // directory
        lua_pushstring(L, slash + 1);                      // filename
    } else {
        lua_pushstring(L, "");  // no directory
        lua_pushstring(L, path); // filename is the whole thing
    }
    return 2;
}

static int utils_join_path(lua_State *L) {
    int n = lua_gettop(L);
    char buf[1024] = {0};
    int off = 0;
    for (int i = 1; i <= n; i++) {
        const char *seg = luaL_checkstring(L, i);
        int seglen = (int)strlen(seg);
        if (seglen == 0) continue;
        if (off > 0 && buf[off - 1] != '/') buf[off++] = '/';
        if ((off + seglen) >= (int)sizeof(buf) - 1) break;
        strcpy(buf + off, seg);
        off += seglen;
    }
    lua_pushstring(L, buf);
    return 1;
}

static int utils_parse_json(lua_State *L) {
    size_t len;
    const char *json = luaL_checklstring(L, 1, &len);
    // Simple JSON -> Lua table using load() trick: "return " .. json
    char *code = (char*)malloc(len + 16);
    if (!code) { lua_pushnil(L); return 1; }
    snprintf(code, len + 16, "return %s", json);
    int rc = luaL_loadstring(L, code);
    free(code);
    if (rc != LUA_OK) {
        lua_pop(L, 1);
        lua_pushnil(L);
        return 1;
    }
    lua_pcall(L, 0, 1, 0);
    return 1;
}

// Use a recursive Lua function for format_json for reliability.
// Embedded as a string; installed during init.
// Lua's string.format('%q', v) escapes for Lua *source* syntax, not JSON:
// control bytes become decimal escapes like \9 or \0, which every strict
// JSON parser (jq, Python json, JS JSON.parse) rejects. json_escape below
// hex-encodes 0x00-0x1F as \uXXXX per RFC 8259 instead. Quote/backslash
// chars are built via string.char() rather than written literally so this
// Lua source can be embedded as a plain C string with no nested-quote
// escaping.
static const char *format_json_lua =
    "local QUOTE = string.char(34)\n"
    "local BACKSLASH = string.char(92)\n"
    "local function json_escape(s)\n"
    "  local out = {}\n"
    "  for i = 1, #s do\n"
    "    local b = s:byte(i)\n"
    "    if b == 34 then out[#out+1] = BACKSLASH .. QUOTE\n"
    "    elseif b == 92 then out[#out+1] = BACKSLASH .. BACKSLASH\n"
    "    elseif b == 8 then out[#out+1] = BACKSLASH .. 'b'\n"
    "    elseif b == 12 then out[#out+1] = BACKSLASH .. 'f'\n"
    "    elseif b == 10 then out[#out+1] = BACKSLASH .. 'n'\n"
    "    elseif b == 13 then out[#out+1] = BACKSLASH .. 'r'\n"
    "    elseif b == 9 then out[#out+1] = BACKSLASH .. 't'\n"
    "    elseif b < 0x20 then out[#out+1] = string.format(BACKSLASH .. 'u%04x', b)\n"
    "    else out[#out+1] = s:sub(i, i)\n"
    "    end\n"
    "  end\n"
    "  return QUOTE .. table.concat(out) .. QUOTE\n"
    "end\n"
    "local function format(v)\n"
    "  local t = type(v)\n"
    "  if t == 'nil' then return 'null'\n"
    "  elseif t == 'boolean' then return v and 'true' or 'false'\n"
    "  elseif t == 'number' then return tostring(v)\n"
    "  elseif t == 'string' then return json_escape(v)\n"
    "  elseif t == 'table' then\n"
    "    local parts = {}\n"
    "    local is_array = true\n"
    "    for k in pairs(v) do if type(k) ~= 'number' then is_array = false; break end end\n"
    "    if is_array then\n"
    "      for i = 1, #v do parts[#parts+1] = format(v[i]) end\n"
    "      return '[' .. table.concat(parts, ',') .. ']'\n"
    "    else\n"
    "      for k, val in pairs(v) do\n"
    "        if type(k) == 'string' then\n"
    "          parts[#parts+1] = json_escape(k) .. ':' .. format(val)\n"
    "        end\n"
    "      end\n"
    "      return '{' .. table.concat(parts, ',') .. '}'\n"
    "    end\n"
    "  end\n"
    "  return 'null'\n"
    "end\n"
    "jdsp_utils_format_json = format\n";

static int utils_format_json(lua_State *L) {
    lua_getglobal(L, "jdsp_utils_format_json");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        lua_pushstring(L, "{}");
        return 1;
    }
    lua_pushvalue(L, 1); // the table argument
    lua_pcall(L, 1, 1, 0);
    return 1;
}

static const luaL_Reg jdsp_utils_lib[] = {
    {"readdir", utils_readdir},
    {"file_info", utils_file_info},
    {"split_path", utils_split_path},
    {"join_path", utils_join_path},
    {"parse_json", utils_parse_json},
    {"format_json", utils_format_json},
    {NULL, NULL}
};

// ============================================================================
// Lifecycle
// ============================================================================

static char      g_script_error[256] = {0};

int lua_bridge_init(void) {
    pthread_mutex_lock(&g_lua_mutex);

    g_L = luaL_newstate();
    if (!g_L) {
        pthread_mutex_unlock(&g_lua_mutex);
        return -1;
    }
    luaL_openlibs(g_L);

    // Register jdsp.* module
    luaL_newlib(g_L, jdsp_lib);
    lua_setglobal(g_L, "jdsp");

    // Register jdsp.utils.* module
    lua_getglobal(g_L, "jdsp");
    luaL_newlib(g_L, jdsp_utils_lib);
    lua_setfield(g_L, -2, "utils");
    lua_pop(g_L, 1); // pop jdsp table

    // Install format_json helper
    luaL_dostring(g_L, format_json_lua);

    // Per-block buffer tables (set by process_block caller)
    lua_newtable(g_L);
    lua_setglobal(g_L, "jdsp_buffer_L");
    lua_newtable(g_L);
    lua_setglobal(g_L, "jdsp_buffer_R");

    // Default data bridge script
    static const char *default_script =
        "function tick()\n"
        "  local f = '/data/data/com.termux.jdsp/files/audio_bridge.txt'\n"
        "  local level = jdsp.get_audio_level() or 0\n"
        "  local h = io.open(f, 'w')\n"
        "  if h then\n"
        "    h:write(string.format('audio_level=%.4f\\naudio_peak=%.4f\\n', level, level))\n"
        "    h:close()\n"
        "  end\n"
        "end\n";
    luaL_dostring(g_L, default_script);
    g_script_loaded = 1;

    // Reset timer list
    while (g_timers) {
        jdsp_timer_t *next = g_timers->next;
        timer_free_ref(g_L, g_timers);
        free(g_timers);
        g_timers = next;
    }
    g_timers = NULL;

    // Reset events
    memset(g_event_queue, 0, sizeof(g_event_queue));
    g_event_read = g_event_write = g_event_count = 0;

    // Reset hooks
    while (g_hooks) {
        jdsp_hook_t *next = g_hooks->next;
        if (g_hooks->lua_ref != LUA_NOREF)
            luaL_unref(g_L, LUA_REGISTRYINDEX, g_hooks->lua_ref);
        free(g_hooks);
        g_hooks = next;
    }
    g_hook_count = 0;

    // Reset observer refs in property table
    for (int i = 0; i < g_prop_count; i++) {
        g_props[i].observer_ref = LUA_NOREF;
    }

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_last_tick_time = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;

    pthread_mutex_unlock(&g_lua_mutex);
    return 0;
}

int lua_bridge_load_script(const char *script) {
    pthread_mutex_lock(&g_lua_mutex);
    if (!g_L) { pthread_mutex_unlock(&g_lua_mutex); return -1; }

    g_script_loaded = 0;
    g_script_error[0] = '\0';

    int rc = luaL_dostring(g_L, script);
    if (rc != LUA_OK) {
        const char *err = lua_tostring(g_L, -1);
        if (err) {
            strncpy(g_script_error, err, sizeof(g_script_error) - 1);
            g_script_error[sizeof(g_script_error) - 1] = '\0';
            __android_log_print(ANDROID_LOG_ERROR, "JDSP-Lua", "load error: %s", err);
        }
        lua_pop(g_L, 1);
        pthread_mutex_unlock(&g_lua_mutex);
        return rc;
    }

    g_script_loaded = 1;
    pthread_mutex_unlock(&g_lua_mutex);
    return 0;
}

// ============================================================================
// Audio callbacks
// ============================================================================

// Process timers and call tick()
void lua_bridge_tick(const float *pcm_buffer, int num_samples) {
    (void)pcm_buffer; (void)num_samples;
    pthread_mutex_lock(&g_lua_mutex);
    if (!g_L || !g_script_loaded) { pthread_mutex_unlock(&g_lua_mutex); return; }

    double now;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;

    // Fire due timers
    jdsp_timer_t **pp = &g_timers;
    while (*pp) {
        jdsp_timer_t *t = *pp;
        if (!t->active) {
            // Remove inactive timer
            *pp = t->next;
            timer_free_ref(g_L, t);
            free(t);
            continue;
        }
        if (t->next_fire > now) { pp = &(*pp)->next; continue; }

        // Fire
        lua_rawgeti(g_L, LUA_REGISTRYINDEX, t->lua_ref);
        if (lua_isfunction(g_L, -1)) {
            if (lua_pcall(g_L, 0, 0, 0) != LUA_OK) {
                const char *err = lua_tostring(g_L, -1);
                if (err) __android_log_print(ANDROID_LOG_ERROR, "JDSP-Lua", "timer error: %s", err);
                lua_pop(g_L, 1);
            }
        } else {
            lua_pop(g_L, 1);
        }

        if (t->interval > 0.0) {
            // Periodic: advance to next fire
            t->next_fire += t->interval;
            if (t->next_fire <= now) t->next_fire = now + t->interval;
            pp = &(*pp)->next;
        } else {
            // One-shot: remove
            *pp = t->next;
            timer_free_ref(g_L, t);
            free(t);
        }
    }

    // Call tick()
    lua_getglobal(g_L, "tick");
    if (lua_isfunction(g_L, -1)) {
        if (lua_pcall(g_L, 0, 0, 0) != LUA_OK) {
            const char *err = lua_tostring(g_L, -1);
            if (err) __android_log_print(ANDROID_LOG_ERROR, "JDSP-Lua", "tick error: %s", err);
            lua_pop(g_L, 1);
        }
    } else {
        lua_pop(g_L, 1);
    }

    g_last_tick_time = now;
    pthread_mutex_unlock(&g_lua_mutex);
}

// Per-sample process tap — the FIFO monitoring entry point.
// Calls script's process(spl0, spl1) as pure side-effect; audio passes
// through unchanged. For audio modification, use process_block() instead.
void lua_bridge_process_sample(float left, float right) {
    if (!g_L || !g_script_loaded) return;
    lua_getglobal(g_L, "process");
    if (!lua_isfunction(g_L, -1)) { lua_pop(g_L, 1); return; }
    lua_pushnumber(g_L, (double)left);
    lua_pushnumber(g_L, (double)right);
    if (lua_pcall(g_L, 2, 0, 0) != LUA_OK) {
        const char *err = lua_tostring(g_L, -1);
        if (err) __android_log_print(ANDROID_LOG_ERROR, "JDSP-Lua", "process error: %s", err);
        lua_pop(g_L, 1);
    }
}

// Per-block process callback
int lua_bridge_process_block(float *buf_L, float *buf_R, int n) {
    if (!g_L || !g_script_loaded) return 0;

    lua_getglobal(g_L, "process_block");
    if (!lua_isfunction(g_L, -1)) { lua_pop(g_L, 1); return 0; }

    // Push buffer tables into Lua (jdsp.buffer_L, jdsp.buffer_R)
    lua_getglobal(g_L, "jdsp_buffer_L");
    for (int i = 0; i < n; i++) {
        lua_pushnumber(g_L, (double)buf_L[i]);
        lua_rawseti(g_L, -2, i + 1);
    }

    lua_getglobal(g_L, "jdsp_buffer_R");
    for (int i = 0; i < n; i++) {
        lua_pushnumber(g_L, (double)buf_R[i]);
        lua_rawseti(g_L, -2, i + 1);
    }

    // Call process_block(n)
    lua_pushinteger(g_L, n);
    if (lua_pcall(g_L, 1, 0, 0) != LUA_OK) {
        const char *err = lua_tostring(g_L, -1);
        if (err) __android_log_print(ANDROID_LOG_ERROR, "JDSP-Lua", "process_block error: %s", err);
        lua_pop(g_L, 1);
        return 0;
    }

    // Read modified buffers back
    lua_getglobal(g_L, "jdsp_buffer_L");
    for (int i = 0; i < n; i++) {
        lua_rawgeti(g_L, -1, i + 1);
        buf_L[i] = (float)lua_tonumber(g_L, -1);
        lua_pop(g_L, 1);
    }
    lua_pop(g_L, 1); // jdsp_buffer_L

    lua_getglobal(g_L, "jdsp_buffer_R");
    for (int i = 0; i < n; i++) {
        lua_rawgeti(g_L, -1, i + 1);
        buf_R[i] = (float)lua_tonumber(g_L, -1);
        lua_pop(g_L, 1);
    }
    lua_pop(g_L, 1); // jdsp_buffer_R

    return 1;
}

// Event callback
void lua_bridge_event(const char *event_name) {
    pthread_mutex_lock(&g_lua_mutex);
    if (!g_L || !g_script_loaded) { pthread_mutex_unlock(&g_lua_mutex); return; }

    // Push to event queue
    if (g_event_count < JDSP_EVENT_QUEUE_SIZE) {
        strncpy(g_event_queue[g_event_write], event_name, sizeof(g_event_queue[0]) - 1);
        g_event_queue[g_event_write][sizeof(g_event_queue[0]) - 1] = '\0';
        g_event_write = (g_event_write + 1) % JDSP_EVENT_QUEUE_SIZE;
        g_event_count++;
    }

    // Also call on_event() immediately
    lua_getglobal(g_L, "on_event");
    if (lua_isfunction(g_L, -1)) {
        lua_pushstring(g_L, event_name);
        if (lua_pcall(g_L, 1, 0, 0) != LUA_OK) {
            const char *err = lua_tostring(g_L, -1);
            if (err) __android_log_print(ANDROID_LOG_ERROR, "JDSP-Lua", "event error: %s", err);
            lua_pop(g_L, 1);
        }
    } else {
        lua_pop(g_L, 1);
    }
    pthread_mutex_unlock(&g_lua_mutex);
}

// ============================================================================
// State setters
// ============================================================================

void lua_bridge_set_bands(const float *energies, int num_bands) {
    if (num_bands < 0) num_bands = 0;
    int n = num_bands < JDSP_BAND_ENERGY_MAX ? num_bands : JDSP_BAND_ENERGY_MAX;
    g_num_bands = n;
    memcpy(g_band_energy, energies, n * sizeof(float));
}

void lua_bridge_set_audio_level(float level) {
    g_audio_level = level;
}

void lua_bridge_set_postgain(float gain) {
    g_postgain = gain;
}

void lua_bridge_set_module_states(const int *states, int count) {
    int n = count < JDSP_MODULE_COUNT ? count : JDSP_MODULE_COUNT;
    for (int i = 0; i < n; i++) g_module_states[i] = states[i];
}

void lua_bridge_set_sample_rate(int sr) {
    g_sample_rate = sr;
}

// ============================================================================
// Property registry
// ============================================================================

void lua_bridge_register_property(const char *name,
                                   jdsp_prop_getter getter,
                                   jdsp_prop_setter setter) {
    if (!name) return;
    // Idempotent: re-registering an existing name replaces its getter/setter
    // in place rather than appending a duplicate entry (which would overflow
    // the fixed table across re-inits).
    for (int i = 0; i < g_prop_count; i++) {
        if (strcmp(name, g_props[i].name) == 0) {
            g_props[i].getter = getter;
            g_props[i].setter = setter;
            return;
        }
    }
    if (g_prop_count >= JDSP_MAX_PROPS) return;
    jdsp_prop_t *p = &g_props[g_prop_count++];
    strncpy(p->name, name, sizeof(p->name) - 1);
    p->name[sizeof(p->name) - 1] = '\0';
    p->getter = getter;
    p->setter = setter;
    p->observer_ref = LUA_NOREF;
}

void lua_bridge_set_property_number(const char *name, double value) {
    if (!name) return;
    for (int i = 0; i < g_prop_value_count; i++) {
        if (strcmp(name, g_prop_values[i].name) == 0) {
            g_prop_values[i].value = value;
            return;
        }
    }
    if (g_prop_value_count >= JDSP_MAX_PROP_VALUES) return;
    jdsp_prop_value_t *v = &g_prop_values[g_prop_value_count++];
    strncpy(v->name, name, sizeof(v->name) - 1);
    v->name[sizeof(v->name) - 1] = '\0';
    v->value = value;
}

// ============================================================================
// Error / metadata
// ============================================================================

const char *lua_bridge_get_error(void) {
    pthread_mutex_lock(&g_lua_mutex);
    const char *result = g_script_error[0] ? g_script_error : NULL;
    pthread_mutex_unlock(&g_lua_mutex);
    return result;
}

void lua_bridge_set_script_dir(const char *dir) {
    if (dir) {
        strncpy(g_script_dir, dir, sizeof(g_script_dir) - 1);
        g_script_dir[sizeof(g_script_dir) - 1] = '\0';
    }
}

// ============================================================================
// Cleanup
// ============================================================================

void lua_bridge_destroy(void) {
    pthread_mutex_lock(&g_lua_mutex);

    // Free all timers
    while (g_timers) {
        jdsp_timer_t *next = g_timers->next;
        if (g_L) timer_free_ref(g_L, g_timers);
        free(g_timers);
        g_timers = next;
    }

    // Free observer refs
    if (g_L) {
        for (int i = 0; i < g_prop_count; i++) {
            if (g_props[i].observer_ref != LUA_NOREF) {
                luaL_unref(g_L, LUA_REGISTRYINDEX, g_props[i].observer_ref);
                g_props[i].observer_ref = LUA_NOREF;
            }
        }
        // Free hooks
        while (g_hooks) {
            jdsp_hook_t *next = g_hooks->next;
            if (g_hooks->lua_ref != LUA_NOREF)
                luaL_unref(g_L, LUA_REGISTRYINDEX, g_hooks->lua_ref);
            free(g_hooks);
            g_hooks = next;
        }
        g_hook_count = 0;
        lua_close(g_L);
        g_L = NULL;
    }

    g_script_loaded = 0;
    g_last_tick_time = 0.0;

    pthread_mutex_unlock(&g_lua_mutex);
}
