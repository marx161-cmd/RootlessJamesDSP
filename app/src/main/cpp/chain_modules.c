// chain_modules.c — AutoGain, Gate, chain ordering for libjamesdsp.so

#include <math.h>
#include <string.h>
#include <stdlib.h>

// ============================================================
// Chain ordering system
// ============================================================

typedef struct {
    const char *name;
    void (*process)(float *L, float *R, unsigned n);
    int *enabled;
} ChainModule;

// Capacity raised 16 -> 24 (2026-10-22): the table was exactly full with 16
// modules when the LSP Multiband Compressor ("mbcomp") was added.
#define CHAIN_MAX_MODULES 24
static ChainModule g_modules[CHAIN_MAX_MODULES];
static int g_module_count = 0;
static int g_chain_order[CHAIN_MAX_MODULES];
static int g_chain_len = 0;

void chain_register(const char *name, void (*fn)(float*,float*,unsigned), int *toggle) {
    if (g_module_count >= CHAIN_MAX_MODULES) return;
    g_modules[g_module_count++] = (ChainModule){name, fn, toggle};
}

void chain_reset(void) {
    g_module_count = 0;
    g_chain_len = 0;
}

int chain_set_order(int *indices, int len) {
    if (len > CHAIN_MAX_MODULES) len = CHAIN_MAX_MODULES;
    g_chain_len = len;
    for (int i = 0; i < len; i++) g_chain_order[i] = indices[i];
    return 0;
}

void chain_process(float *L, float *R, unsigned n) {
    for (int i = 0; i < g_chain_len; i++) {
        int idx = g_chain_order[i];
        if (idx < 0 || idx >= g_module_count) continue;
        ChainModule *m = &g_modules[idx];
        if (m->enabled && !*m->enabled) continue;
        if (m->process) m->process(L, R, n);
    }
}

// ============================================================
// Gate — RMS threshold + hysteresis (open/close)
// ============================================================

typedef struct {
    float open_db, close_db, attack_s, release_s, hold_s;
    float current, atk_coef, rel_coef;
    int hold_samples, enabled;
    float fs;
} Gate;

void gate_init(Gate *g, float open_db, float close_db, float attack_s,
               float release_s, float hold_s, float fs) {
    g->open_db = open_db; g->close_db = close_db;
    g->attack_s = attack_s; g->release_s = release_s; g->hold_s = hold_s;
    g->current = 0.0f; g->hold_samples = 0; g->enabled = 1; g->fs = fs;
    g->atk_coef = 1.0f - expf(-1.0f / (attack_s * fs));
    g->rel_coef = 1.0f - expf(-1.0f / (release_s * fs));
}

void gate_process(Gate *g, float *L, float *R, unsigned n) {
    if (!g->enabled) return;
    float open_lin  = powf(10.0f, g->open_db / 20.0f);
    float close_lin = powf(10.0f, g->close_db / 20.0f);
    for (unsigned i = 0; i < n; i++) {
        float rms = fabsf((L[i] + R[i]) * 0.5f);
        if (g->hold_samples > 0) {
            g->hold_samples--;
        } else if (g->current < 0.5f && rms > open_lin) {
            // opening
        } else if (g->current > 0.5f && rms < close_lin) {
            g->hold_samples = (int)(g->hold_s * g->fs);
        }
        float target = (g->current > 0.5f && g->hold_samples > 0) ? g->current :
                       (rms > open_lin) ? 1.0f : 0.0f;
        if (g->hold_samples > 0 && target < 0.5f) target = 0.0f;
        float rate = (target > g->current) ? g->atk_coef : g->rel_coef;
        g->current += (target - g->current) * rate;
        L[i] *= g->current; R[i] *= g->current;
    }
}

// ============================================================
// AutoGain — the legacy slow-RMS-integrator module that lived here was
// replaced 2026-10-21 by the faithful LSP Autogain port
// (lsp_wrap_autogain.cpp, BS.1770 K-weighted loudness -> dspu::AutoGain).
// jdspController.c's w_autogain() and the autogain.current_gain_db Lua
// telemetry now call lsp_autogain_process()/lsp_autogain_get_gain_db().
// ============================================================
