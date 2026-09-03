// lsp_wrap_limiter.h — extern "C" surface for the faithfully-ported LSP
// Limiter plugin (see lsp_wrap_limiter.cpp for the port itself and
// LSP_FAITHFUL_PORT_SCOPE.md for why this replaces the old hand-wrapped
// Limiter section of lsp_wrap.cpp).
//
// This header intentionally mirrors lsp_wrap.h's Limiter section
// signature-for-signature so jdspController.c's
// `lsp_limiter_process(jdsp->limiter_handle, ...)` call (tail/last-line-of-
// defense position) and jamesdsp.c's cmd-1500 handler do not need to change
// when this file gets wired in. Do not edit lsp_wrap.h/.cpp from this file —
// see the file-isolation note in LSP_FAITHFUL_PORT_SCOPE.md; wiring
// (removing the old LimiterHandle/lsp_limiter_* block from lsp_wrap.cpp,
// adding this file + its extra lsp-dsp-units sources to CMakeLists.txt) is a
// separate step, done by whoever integrates this port.
#ifndef LSP_WRAP_LIMITER_H
#define LSP_WRAP_LIMITER_H

#ifdef __cplusplus
extern "C" {
#endif

void *lsp_limiter_create(float sample_rate);
void  lsp_limiter_destroy(void *h);
void  lsp_limiter_configure(void *h, float threshold_db, float attack_ms,
                             float release_ms, float knee_db);
void  lsp_limiter_process(void *h, float *left, float *right, unsigned n);

// Telemetry — deepest VCA gain reduction applied over the last processed
// block, in dB (>= 0).
float lsp_limiter_get_reduction_db(void *h);
int   lsp_limiter_is_limiting(void *h); // non-zero when limiter is engaged

// Config read-back, in the exact units passed to configure().
void lsp_limiter_get_config(void *h, float *threshold_db, float *attack_ms,
                             float *release_ms, float *knee_db);

#ifdef __cplusplus
}
#endif

#endif
