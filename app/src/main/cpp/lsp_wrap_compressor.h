// lsp_wrap_compressor.h — faithful port of the real LSP Compressor plugin
// (lsp-compressor/src/main/plug/compressor.cpp), stereo mode / internal
// sidechain only, per LSP_FAITHFUL_PORT_SCOPE.md.
//
// Self-contained: intentionally does NOT include lsp_wrap.h, so it can be
// developed and compiled independently of lsp_wrap.cpp/.h while another
// agent is concurrently editing those files for Gate. The extern "C"
// function names/signatures below are an exact match of lsp_wrap.h's
// current Compressor section (lsp_comp_create/destroy/configure/process/
// get_reduction_db/get_config) so that jdspController.c's w_mb_dyn() and
// jamesdsp.c's cmd-10101 handlers can be repointed at this file without any
// call-site changes.
//
// INTEGRATION NOTE (for whoever wires this in): once this file replaces
// lsp_wrap.cpp's CompHandle/lsp_comp_* section, that section (and this
// header's duplicate declarations) must not both be linked into the same
// binary, or the extern "C" symbols will collide. This file also assumes
// lsp::dsp::init()'s ARM32/x86 stub symbols (lsp::neon_d32::dsp_init,
// lsp::x86::detect_cpu_features/dsp_init) are provided elsewhere in the
// link — currently lsp_wrap.cpp defines them for Gate/Limiter's benefit —
// so they are deliberately NOT redefined here to avoid a multiple-definition
// link error once both translation units coexist.
#ifndef LSP_WRAP_COMPRESSOR_H
#define LSP_WRAP_COMPRESSOR_H

#ifdef __cplusplus
extern "C" {
#endif

void *lsp_comp_create(float sample_rate);
void  lsp_comp_destroy(void *h);
void  lsp_comp_configure(void *h, float threshold, float ratio, float knee,
                          float attack_ms, float release_ms, float hold_ms);
// `enabled` drives a real Bypass crossfade (~5ms, real plugin's sBypass)
// instead of a hard on/off cut — call every block regardless of enabled
// state so the crossfade-out actually runs; the function itself is a
// zero-cost no-op once fully settled disabled (matches the documented
// contract of the function it replaces in lsp_wrap.h).
void  lsp_comp_process(void *h, float *left, float *right, unsigned n, int enabled);

float lsp_comp_get_reduction_db(void *h);

void lsp_comp_get_config(void *h, float *threshold_db, float *ratio,
                         float *knee_db, float *attack_ms, float *release_ms,
                         float *hold_ms);

#ifdef __cplusplus
}
#endif

#endif
