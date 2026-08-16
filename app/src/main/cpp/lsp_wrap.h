// lsp_wrap.h — extern "C" wrappers for LSP dynamics classes
#ifndef LSP_WRAP_H
#define LSP_WRAP_H

#ifdef __cplusplus
extern "C" {
#endif

// Compressor
void *lsp_comp_create(float sample_rate);
void  lsp_comp_destroy(void *h);
void  lsp_comp_configure(void *h, float threshold, float ratio, float knee,
                          float attack_ms, float release_ms, float hold_ms);
// `enabled` drives a real Bypass crossfade (~5ms) instead of a hard on/off
// cut — call every block regardless of enabled state so the crossfade-out
// actually runs; the function itself is a zero-cost no-op once fully
// settled disabled.
void  lsp_comp_process(void *h, float *left, float *right, unsigned n, int enabled);

// Gate
void *lsp_gate_create(float sample_rate);
void  lsp_gate_destroy(void *h);
void  lsp_gate_configure(void *h, float open_db, float close_db,
                          float attack_ms, float release_ms, float hold_ms, float reduction_db);
void  lsp_gate_process(void *h, float *left, float *right, unsigned n, int enabled);

// Limiter
void *lsp_limiter_create(float sample_rate);
void  lsp_limiter_destroy(void *h);
void  lsp_limiter_configure(void *h, float threshold_db, float attack_ms,
                             float release_ms, float knee_db);
void  lsp_limiter_process(void *h, float *left, float *right, unsigned n);

// --- Telemetry (live gain reduction, refreshed each process block) --------
// Gain reduction is the deepest VCA cut applied over the last processed
// block, in dB (>= 0; 0 when the module is bypassed/settled-disabled).
float lsp_comp_get_reduction_db(void *h);
float lsp_gate_get_reduction_db(void *h);
int   lsp_gate_is_closed(void *h);      // non-zero when gate is actively attenuating
float lsp_limiter_get_reduction_db(void *h);
int   lsp_limiter_is_limiting(void *h); // non-zero when limiter is engaged

// --- Config read-back (values in the exact units passed to configure) -----
// Used by the Lua property setters so a single-parameter change can re-apply
// the full bundle without stomping the other parameters.
void lsp_comp_get_config(void *h, float *threshold_db, float *ratio,
                         float *knee_db, float *attack_ms, float *release_ms,
                         float *hold_ms);
void lsp_gate_get_config(void *h, float *open_db, float *close_db,
                         float *attack_ms, float *release_ms, float *hold_ms,
                         float *reduction_db);
void lsp_limiter_get_config(void *h, float *threshold_db, float *attack_ms,
                            float *release_ms, float *knee_db);

#ifdef __cplusplus
}
#endif

#endif
