// lsp_wrap_expander.h — faithful port of the real LSP Expander plugin
// (lsp-expander/src/main/plug/expander.cpp, pinned release 1.0.37), stereo
// mode (EM_STEREO) / internal sidechain / feed-forward only, per
// LSP_FAITHFUL_PORT_SCOPE.md. Structural template: lsp_wrap_compressor.{h,cpp}.
//
// Self-contained: intentionally does NOT include lsp_wrap.h (same reason as
// lsp_wrap_compressor.h — independent translation unit, no shared declaration
// file to edit). jdspController.c / jamesdsp.c include this header directly.
//
// INTEGRATION NOTE: this file assumes lsp::dsp::init()'s ARM32/x86 stub
// symbols (lsp::neon_d32::dsp_init, lsp::x86::detect_cpu_features/dsp_init)
// are provided elsewhere in the link (lsp_wrap.cpp defines them), so they
// are deliberately NOT redefined here — same arrangement as the compressor
// and limiter ports.
#ifndef LSP_WRAP_EXPANDER_H
#define LSP_WRAP_EXPANDER_H

#ifdef __cplusplus
extern "C" {
#endif

void *lsp_exp_create(float sample_rate);
void  lsp_exp_destroy(void *h);
// mode: 0 = downward (real meta::expander_metadata::EM_DOWNWARD),
//       1 = upward   (real EM_UPWARD).
// threshold_db: real plugin's Attack Level port ("al", U_GAIN_AMP), in dB.
// knee: LINEAR gain (real "kn" port is U_GAIN_AMP), same convention as
//       lsp_comp_configure()'s knee argument — NOT dB.
// Control-thread safe: only sanitises and publishes (rt_config.h); the real
// update_settings() runs on the audio thread inside lsp_exp_process().
void  lsp_exp_configure(void *h, int mode, float threshold_db, float ratio, float knee,
                         float attack_ms, float release_ms, float hold_ms);
// `enabled` drives a real Bypass crossfade (~5ms, real plugin's sBypass)
// instead of a hard on/off cut — call every block regardless of enabled
// state so the crossfade-out actually runs; the function itself is a
// zero-cost no-op once fully settled disabled (same contract as
// lsp_comp_process / lsp_gate_process).
void  lsp_exp_process(void *h, float *left, float *right, unsigned n, int enabled);

// --- Telemetry (live gain change, refreshed each process block) -----------
// Downward mode: deepest VCA cut over the last block, in dB (>= 0).
// Upward mode: largest VCA boost over the last block, reported as a NEGATIVE
// "reduction" (<= 0) so the sign always reads as gain change relative to
// unity. Mirrors the real plugin's own gain meter, which switches between
// MM_ABS_MINIMUM (downward) and MM_ABS_MAXIMUM (upward). 0 when bypassed.
float lsp_exp_get_reduction_db(void *h);
int   lsp_exp_is_active(void *h);     // non-zero when |gain change| > 1 dB

// --- Config read-back (values in the exact units passed to configure) -----
void lsp_exp_get_config(void *h, int *mode, float *threshold_db, float *ratio,
                        float *knee, float *attack_ms, float *release_ms,
                        float *hold_ms);

#ifdef __cplusplus
}
#endif

#endif
