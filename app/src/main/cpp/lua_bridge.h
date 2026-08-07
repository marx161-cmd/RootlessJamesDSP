// lua_bridge.h — public API for embedded Lua scripting in libjamesdspaidl.so

#ifndef LUA_BRIDGE_H
#define LUA_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

// Lifecycle
int  lua_bridge_init(void);
void lua_bridge_destroy(void);

// Load a Lua script from a string (sent via AIDL parameter)
int  lua_bridge_load_script(const char *script);

// Called from 30Hz audio timer
void lua_bridge_tick(const float *pcm_buffer, int num_samples);

// Called on stream start/stop
void lua_bridge_event(const char *event_name);

// Set band energies before each tick (from FFT processing)
void lua_bridge_set_bands(const float *energies, int num_bands);

// Set current full-spectrum audio level
void lua_bridge_set_audio_level(float level);

// Real-time output gain multiplier actually being applied (reflects AutoGain
// etc.) — already computed every buffer for real DSP purposes, just cached.
void lua_bridge_set_postgain(float gain);

// Module enabled flags, in the fixed order documented by jdsp_module_names
// in lua_bridge.c (gate, compressor, autogain, reverb, stereo, tube,
// convolver, eq, crossfeed, ddc, arbeq, liveprog). Already-computed ints,
// just exposed for a Lua script to branch on.
void lua_bridge_set_module_states(const int *states, int count);

// Set sample rate
void lua_bridge_set_sample_rate(int sr);

// Get last script error (NULL = no error)
const char *lua_bridge_get_error(void);

#ifdef __cplusplus
}
#endif

#endif // LUA_BRIDGE_H
