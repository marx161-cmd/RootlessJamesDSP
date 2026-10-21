// lsp_wrap_autogain.h — faithful port of the real LSP Autogain plugin
// (lsp-autogain/src/main/plug/autogain.cpp, pinned release 1.0.19), stereo
// variant (autogain_stereo, nChannels == 2) / internal sidechain
// (SCMODE_INTERNAL) only, per LSP_FAITHFUL_PORT_SCOPE.md. Structural
// template: lsp_wrap_expander.{h,cpp}. Replaces the legacy RMS-integrator
// "autogain" chain module that used to live in chain_modules.c.
//
// Self-contained: does NOT include lsp_wrap.h. jdspController.c / jamesdsp.c
// include this header directly. Assumes lsp::dsp::init()'s ARM32/x86 stub
// symbols are provided by lsp_wrap.cpp (not redefined here).
#ifndef LSP_WRAP_AUTOGAIN_H
#define LSP_WRAP_AUTOGAIN_H

#ifdef __cplusplus
extern "C" {
#endif

void *lsp_autogain_create(float sample_rate);
void  lsp_autogain_destroy(void *h);
// All arguments are in the real plugin's own port units (no conversion at
// this boundary; dB/LUFS -> linear happens inside, exactly where the real
// update_settings() does it):
//   level_lufs      real "level"   (U_LUFS) desired loudness, -60..0, dfl -23
//   long_period_ms  real "lperiod" (U_MSEC) 100..2000, dfl 400
//   short_period_ms real "speriod" (U_MSEC) 5..100, dfl 20
//   drift_db        real "drift"   (U_DB)   allowed deviation, 0..24, dfl 12
//   silence_lufs    real "silence" (U_LUFS) -84..-36, dfl -72
//   max_gain_db     real "max_amp" (U_DB)   0..108, dfl 36
//   max_gain_on     real "max_on"  (SWITCH) 0/1, dfl 0
// Control-thread safe: only sanitises and publishes (rt_config.h); the real
// update_settings() runs on the audio thread inside lsp_autogain_process().
void  lsp_autogain_configure(void *h, float level_lufs, float long_period_ms,
                              float short_period_ms, float drift_db,
                              float silence_lufs, float max_gain_db,
                              int max_gain_on);
// `enabled` drives the real Bypass crossfade (~5ms, real plugin's sBypass);
// call every block regardless of enabled state so the crossfade-out runs;
// zero-cost no-op once fully settled disabled (same contract as the other
// LSP ports).
void  lsp_autogain_process(void *h, float *left, float *right, unsigned n, int enabled);

// --- Telemetry (refreshed each process block) -----------------------------
// Current gain correction in dB (real plugin's "g_g" meter: max of the VCA
// buffer over the block, as dB). 0 when bypassed/settled-disabled.
float lsp_autogain_get_gain_db(void *h);
// Long-period input loudness in LUFS (real plugin's "g_in_l" meter).
// -120 when bypassed or silent.
float lsp_autogain_get_input_lufs(void *h);

// --- Config read-back (values in the exact units passed to configure) -----
void lsp_autogain_get_config(void *h, float *level_lufs, float *long_period_ms,
                             float *short_period_ms, float *drift_db,
                             float *silence_lufs, float *max_gain_db,
                             int *max_gain_on);

#ifdef __cplusplus
}
#endif

#endif
