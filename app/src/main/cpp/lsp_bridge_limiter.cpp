#include <lsp-plug.in/dsp-units/dynamics/Limiter.h>
#include <cstdlib>
#include <cmath>
#include <algorithm>

struct lsp_limiter_t {
    lsp::dspu::Limiter limiter;
    float *sc_buf;
    float *gain_buf;
    unsigned buf_cap;
    float sample_rate;
};

extern "C" {

void *lsp_limiter_create(float sample_rate) {
    auto *h = new lsp_limiter_t();
    h->limiter.construct();
    h->limiter.init((size_t)sample_rate * 4, 20.0f);
    h->limiter.set_sample_rate((size_t)sample_rate);
    h->limiter.set_mode(lsp::dspu::LM_HERM_THIN);
    h->limiter.set_threshold(1.0f, false);
    h->limiter.set_attack(5.0f);
    h->limiter.set_release(100.0f);
    h->limiter.set_lookahead(0.1f);
    h->limiter.set_knee(1.0f);
    h->limiter.set_alr(false);
    h->limiter.update_settings();
    h->sc_buf = nullptr;
    h->gain_buf = nullptr;
    h->buf_cap = 0;
    h->sample_rate = sample_rate;
    return h;
}

void lsp_limiter_destroy(void *h) {
    if (!h) return;
    auto *l = static_cast<lsp_limiter_t *>(h);
    l->limiter.destroy();
    free(l->sc_buf);
    free(l->gain_buf);
    delete l;
}

void lsp_limiter_configure(void *h, float threshold_db, float attack, float release, float knee) {
    if (!h) return;
    auto *l = static_cast<lsp_limiter_t *>(h);
    float threshold_lin = powf(10.0f, threshold_db / 20.0f);
    l->limiter.set_threshold(threshold_lin, true);
    l->limiter.set_attack(attack);
    l->limiter.set_release(release);
    l->limiter.set_knee(knee);
    l->limiter.update_settings();
}

void lsp_limiter_process(void *h, float *left, float *right, unsigned n) {
    if (!h || n == 0) return;
    auto *l = static_cast<lsp_limiter_t *>(h);

    if (n > l->buf_cap) {
        l->sc_buf   = (float *)realloc(l->sc_buf,   n * sizeof(float));
        l->gain_buf = (float *)realloc(l->gain_buf, n * sizeof(float));
        l->buf_cap  = n;
    }

    for (unsigned i = 0; i < n; i++) {
        float absL = fabsf(left[i]);
        float absR = fabsf(right[i]);
        l->sc_buf[i] = fmaxf(absL, absR);
    }

    l->limiter.process(l->gain_buf, l->sc_buf, n);

    for (unsigned i = 0; i < n; i++) {
        left[i]  *= l->gain_buf[i];
        right[i] *= l->gain_buf[i];
    }
}

} // extern "C"
