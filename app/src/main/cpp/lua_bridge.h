// lua_bridge.h — embedded Lua 5.4.8 scripting for libjamesdspaidl.so
//
// Full mpv-derived Lua API adapted for DSP: timers, property get/set,
// per-sample audio processing hooks, events, commands, utilities.

#ifndef LUA_BRIDGE_H
#define LUA_BRIDGE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Lifecycle ----------------------------------------------------------

int  lua_bridge_init(void);
void lua_bridge_destroy(void);

// Load a Lua script string (sent via AIDL or file)
int  lua_bridge_load_script(const char *script);

// --- Audio callbacks (called from DSP chain) ----------------------------

// Called at ~30Hz from the audio thread. Drives tick() + timers.
void lua_bridge_tick(const float *pcm_buffer, int num_samples);

// Per-sample process tap. Calls the script's process(spl0, spl1) if defined.
// Pure side-effect callback — audio passes through unchanged. Use this for
// monitoring, FIFO taps, metering. For audio modification use process_block().
// Call once per sample pair from the chain processing loop.
void lua_bridge_process_sample(float left, float right);

// Per-block process callback. Returns non-zero if a script defines
// process_block(). Call once per audio block before the per-sample loop.
// The script can modify the buffer in-place via jdsp.buffer_L / jdsp.buffer_R.
int lua_bridge_process_block(float *buf_L, float *buf_R, int n);

// Called on stream start/stop
void lua_bridge_event(const char *event_name);

// --- State updates (called from DSP chain before tick) ------------------

void lua_bridge_set_bands(const float *energies, int num_bands);
void lua_bridge_set_audio_level(float level);
void lua_bridge_set_postgain(float gain);
void lua_bridge_set_module_states(const int *states, int count);
void lua_bridge_set_sample_rate(int sr);

// Push a numeric telemetry value into the named-property value store,
// readable from Lua via jdsp.get_property(name) / get_property_number() /
// get_property_bool(). Unlike lua_bridge_register_property() (which wires a
// getter/setter function pair), this is a plain name→value cache that the
// DSP chain's 30Hz tick refreshes every block — the read-side path for live
// metering values that are already computed in the audio thread, so no C
// getter function per property is required. Idempotent: re-pushing a name
// overwrites in place, never grows the table.
void lua_bridge_set_property_number(const char *name, double value);

// --- Property callback bridge (called from jamesdsp.c on AIDL param set) -

// Register a named property that scripts can get/set/observe.
// getter returns the current value as a Lua number; setter writes it.
// Both return 0 on success, non-zero on error.
typedef double (*jdsp_prop_getter)(void);
typedef int    (*jdsp_prop_setter)(double value);
void lua_bridge_register_property(const char *name,
                                   jdsp_prop_getter getter,
                                   jdsp_prop_setter setter);

// --- Error reporting ----------------------------------------------------

const char *lua_bridge_get_error(void);

// --- Hook system (named chain intercepts) --------------------------------

// Fire all hooks registered for a named chain point. Called from the chain
// module wrappers immediately before each module processes. Hook callbacks
// receive the buffer (float *L, float *R, int n) and can modify it in-place.
// Scripts register hooks with jdsp.hook_add("convolver", 50, fn).
void lua_bridge_hook_fire(const char *name, float *L, float *R, int n);

// --- Command dispatch ---------------------------------------------------

// Register a handler that Lua's jdsp.command_raw() calls. handler receives
// (cmd_id, float_params, count) and returns 0 on success.
typedef int (*jdsp_cmd_handler)(int cmd, const float *params, int nparams);
void lua_bridge_set_command_handler(jdsp_cmd_handler handler);

// --- Script metadata ----------------------------------------------------

// Set by init; scripts can query via jdsp.get_script_directory()
void lua_bridge_set_script_dir(const char *dir);

#ifdef __cplusplus
}
#endif

#endif // LUA_BRIDGE_H
