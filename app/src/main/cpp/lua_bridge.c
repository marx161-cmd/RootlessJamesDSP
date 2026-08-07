// lua_bridge.c — embedded Lua scripting engine for libjamesdspaidl.so
//
// Runs user Lua scripts on a 30Hz timer inside the audio processing pipeline.
// Scripts have access to JDSP band energy and chain parameters, and can
// read/write files (for the ShaderEditor data bridge, etc.) via standard
// Lua io.open() just like mpv's audio_reactive.lua.
//
// The same script can run in either mpv (via mp.* API) or JDSP (via jdsp.*
// API) — the script checks which host is available and adapts.

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

// g_L is touched from at least two threads — the audio/timer thread
// (lua_bridge_tick / lua_bridge_event) and whatever thread handles
// parameter-set commands (lua_bridge_load_script, called from an
// EFFECT_CMD_SET_PARAM-style path). Lua states are not thread-safe for
// concurrent access; this mutex serializes every entry point that touches
// g_L so a script reload can't run concurrently with a tick() and corrupt
// the VM's internal stack.
static pthread_mutex_t g_lua_mutex = PTHREAD_MUTEX_INITIALIZER;

// --- JDSP API functions exposed to Lua ----------------------------------

// FFT band energies: jdsp.get_band_energy(n) -> 0.0..1.0
// Placeholder — wire to real FFT output when band decomposition is active.
static float g_band_energy[16] = {0};
static int g_num_bands = 0;

static int jdsp_get_band_energy(lua_State *L) {
    int n = lua_tointeger(L, 1);
    if (n < 1 || n > g_num_bands) {
        lua_pushnumber(L, 0.0);
        return 1;
    }
    lua_pushnumber(L, (double)g_band_energy[n - 1]);
    return 1;
}

// Full-spectrum audio level: jdsp.get_audio_level() -> 0.0..1.0
static float g_audio_level = 0.0f;
static int jdsp_get_audio_level(lua_State *L) {
    lua_pushnumber(L, (double)g_audio_level);
    return 1;
}

// Real-time output gain multiplier: jdsp.get_postgain() -> linear multiplier
// (1.0 = unity). Reflects AutoGain's current output, already computed by the
// chain every buffer — this is a cache, not new computation.
static float g_postgain = 1.0f;
static int jdsp_get_postgain(lua_State *L) {
    lua_pushnumber(L, (double)g_postgain);
    return 1;
}

// Module enabled flags: jdsp.get_enabled("gate") -> bool. Fixed name table,
// values set once per tick from JamesDSPProcess via lua_bridge_set_module_states().
static const char *g_module_names[] = {
    "gate", "compressor", "autogain", "reverb", "stereo", "tube",
    "convolver", "eq", "crossfeed", "ddc", "arbeq", "liveprog"
};
#define JDSP_MODULE_COUNT ((int)(sizeof(g_module_names) / sizeof(g_module_names[0])))
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

// Sample rate: jdsp.get_sample_rate() -> Hz
static int g_sample_rate = 48000;
static int jdsp_get_sample_rate(lua_State *L) {
    lua_pushinteger(L, g_sample_rate);
    return 1;
}

// Log output: jdsp.log(level, message) — writes to Android logcat
#include <android/log.h>
static int jdsp_log(lua_State *L) {
    const char *level = lua_tostring(L, 1);
    const char *msg = lua_tostring(L, 2);
    int prio = ANDROID_LOG_INFO;
    if (level && strcmp(level, "error") == 0) prio = ANDROID_LOG_ERROR;
    else if (level && strcmp(level, "warn") == 0) prio = ANDROID_LOG_WARN;
    __android_log_print(prio, "JDSP-Lua", "%s", msg ? msg : "");
    return 0;
}

static const luaL_Reg jdsp_lib[] = {
    {"get_band_energy", jdsp_get_band_energy},
    {"get_audio_level", jdsp_get_audio_level},
    {"get_postgain", jdsp_get_postgain},
    {"get_enabled", jdsp_get_enabled},
    {"get_sample_rate", jdsp_get_sample_rate},
    {"log", jdsp_log},
    {NULL, NULL}
};

// --- Lifecycle ----------------------------------------------------------

static lua_State *g_L = NULL;
static int g_script_loaded = 0;
static char g_script_error[256] = {0};

// Called once at library init
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

    // Load default data bridge script (writes audio_bridge.txt at 30Hz)
    static const char *default_script =
        "local f = '/data/data/com.termux.jdsp/files/audio_bridge.txt'\n"
        "local level = 0.0\n"
        "function tick()\n"
        "  level = level + (jdsp.get_audio_level() - level) * 0.3\n"
        "  local h = io.open(f, 'w')\n"
        "  if h then h:write(string.format('audio_level=%.4f\\naudio_peak=%.4f\\n', level, level)); h:close() end\n"
        "end\n";
    luaL_dostring(g_L, default_script);
    g_script_loaded = 1;

    pthread_mutex_unlock(&g_lua_mutex);
    return 0;
}

// Load a script from a string (sent via AIDL parameter or file path)
int lua_bridge_load_script(const char *script) {
    pthread_mutex_lock(&g_lua_mutex);

    if (!g_L) {
        pthread_mutex_unlock(&g_lua_mutex);
        return -1;
    }
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

// Called from the 30Hz timer in the audio thread.
//
// NOTE: the default script above does blocking file I/O (io.open/write/close)
// on every call. If this is genuinely invoked from the real-time audio
// render thread (effectProcessImpl's call chain), that blocking I/O belongs
// on a separate non-realtime thread instead — a stalled write here can
// produce an audible glitch/underrun. Not restructured in this pass since
// it changes the threading model rather than fixing a correctness bug;
// flagging clearly so it isn't missed before this ships.
void lua_bridge_tick(const float *pcm_buffer, int num_samples) {
    pthread_mutex_lock(&g_lua_mutex);
    if (!g_L || !g_script_loaded) {
        pthread_mutex_unlock(&g_lua_mutex);
        return;
    }

    // Update global state for script access
    // (band energies and audio_level set externally before each tick)

    // Call the script's global tick() function if defined
    lua_getglobal(g_L, "tick");
    if (lua_isfunction(g_L, -1)) {
        int rc = lua_pcall(g_L, 0, 0, 0);
        if (rc != LUA_OK) {
            const char *err = lua_tostring(g_L, -1);
            if (err) {
                __android_log_print(ANDROID_LOG_ERROR, "JDSP-Lua", "tick error: %s", err);
            }
            lua_pop(g_L, 1);
        }
    } else {
        lua_pop(g_L, 1);
    }
    pthread_mutex_unlock(&g_lua_mutex);
}

// Called when the audio stream starts or stops
void lua_bridge_event(const char *event_name) {
    pthread_mutex_lock(&g_lua_mutex);
    if (!g_L || !g_script_loaded) {
        pthread_mutex_unlock(&g_lua_mutex);
        return;
    }

    lua_getglobal(g_L, "on_event");
    if (lua_isfunction(g_L, -1)) {
        lua_pushstring(g_L, event_name);
        int rc = lua_pcall(g_L, 1, 0, 0);
        if (rc != LUA_OK) {
            const char *err = lua_tostring(g_L, -1);
            if (err) {
                __android_log_print(ANDROID_LOG_ERROR, "JDSP-Lua", "event error: %s", err);
            }
            lua_pop(g_L, 1);
        }
    } else {
        lua_pop(g_L, 1);
    }
    pthread_mutex_unlock(&g_lua_mutex);
}

// Set band energies (called from FFT processing before each tick)
void lua_bridge_set_bands(const float *energies, int num_bands) {
    if (num_bands < 0) num_bands = 0;
    int n = num_bands < 16 ? num_bands : 16;
    g_num_bands = n;
    memcpy(g_band_energy, energies, n * sizeof(float));
}

// Set audio level (from RMS or envelope tracking)
void lua_bridge_set_audio_level(float level) {
    g_audio_level = level;
}

// Set the real-time output gain multiplier
void lua_bridge_set_postgain(float gain) {
    g_postgain = gain;
}

// Set module enabled flags (fixed order, see g_module_names above)
void lua_bridge_set_module_states(const int *states, int count) {
    int n = count < JDSP_MODULE_COUNT ? count : JDSP_MODULE_COUNT;
    for (int i = 0; i < n; i++) g_module_states[i] = states[i];
}

// Set sample rate
void lua_bridge_set_sample_rate(int sr) {
    g_sample_rate = sr;
}

// Get the last script error (if any). Returns a pointer into a static
// buffer — caller must copy it out before calling load_script() again if
// it needs to survive a concurrent reload; the buffer itself stays valid
// (never freed), only its contents can change underneath a slow reader.
const char *lua_bridge_get_error(void) {
    pthread_mutex_lock(&g_lua_mutex);
    const char *result = g_script_error[0] ? g_script_error : NULL;
    pthread_mutex_unlock(&g_lua_mutex);
    return result;
}

// Cleanup
void lua_bridge_destroy(void) {
    pthread_mutex_lock(&g_lua_mutex);
    if (g_L) {
        lua_close(g_L);
        g_L = NULL;
    }
    g_script_loaded = 0;
    pthread_mutex_unlock(&g_lua_mutex);
}
