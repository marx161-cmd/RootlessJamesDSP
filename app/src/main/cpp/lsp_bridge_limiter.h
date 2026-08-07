#ifndef LSP_BRIDGE_LIMITER_H
#define LSP_BRIDGE_LIMITER_H

#ifdef __cplusplus
extern "C" {
#endif

void *lsp_limiter_create(float sample_rate);
void lsp_limiter_destroy(void *h);
void lsp_limiter_configure(void *h, float threshold_db, float attack, float release, float knee);
void lsp_limiter_process(void *h, float *left, float *right, unsigned n);

#ifdef __cplusplus
}
#endif

#endif
