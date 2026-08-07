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

#ifdef __cplusplus
}
#endif

#endif
