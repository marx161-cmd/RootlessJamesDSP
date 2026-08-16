// lsp_wrap.cpp — extern "C" wrappers around LSP dynamics classes
//
// IMPORTANT: the LSP Compressor/Gate/Limiter classes are *sidechain gain
// computers*, not in-place per-sample audio processors. Their single-sample
// overload is `float process(float *env, float in)` — env is an OUTPUT
// (envelope trace), `in` is the sidechain INPUT, and the return value is the
// computed VCA gain. An earlier version of this file called it as
// `process(&left[i], 1)`, which wrote envelope data over the real audio
// sample and fed a constant 1.0f as input instead of the real signal,
// discarding the actual computed gain — i.e. it silently corrupted audio
// whenever compressor/gate was enabled. Fixed below to use the batched
// sidechain API correctly: build a stereo-linked sidechain signal, get one
// gain curve from it, apply that gain to both channels. Also gives Limiter
// a proper stereo link (it previously ran two independent process() calls
// per sample against a single shared-state instance, which is neither
// independent per-channel nor properly linked) and the init() lookahead
// buffer it needs (previously never called).
#include "lsp_wrap.h"
#include <lsp-plug.in/dsp-units/dynamics/Compressor.h>
#include <lsp-plug.in/dsp-units/dynamics/Gate.h>
#include <lsp-plug.in/dsp-units/dynamics/Limiter.h>
#include <lsp-plug.in/dsp-units/util/Delay.h>
#include <lsp-plug.in/dsp-units/util/Sidechain.h>
#include <lsp-plug.in/dsp-units/ctl/Bypass.h>
#include <lsp-plug.in/dsp-units/units.h>
#include <lsp-plug.in/dsp/dsp.h>
#include <cmath>
#include <cstdlib>
#include <cstring>

// lsp::dsp::init() populates the library's internal dsp:: function-pointer
// table (fill_one, fill_zero, and everything else Compressor/Gate/Limiter
// call internally) — every pointer is NULL until this runs once. Nothing
// in this codebase was calling it, so every dsp:: call was a jump through
// a null pointer; it simply hadn't been exercised until Limiter::init()'s
// lookahead-buffer setup (dsp::fill_one), which crashed with pc=0 / SIGSEGV
// null pointer dereference on every effect open. Call once, first use.
static inline void ensure_dsp_init() {
    static bool done = (lsp::dsp::init(), true);
    (void)done;
}

// All three LSP dynamics classes take thresholds as *linear* gain
// (1.0 = 0dB, per lsp-dsp-units/include/.../const.h's GAIN_AMP_0_DB), not
// dB — none of Compressor::set_threshold/Gate::set_threshold/
// Limiter::set_threshold do any internal conversion. Callers throughout
// this file (and the legacy JLimiterSetCoefficients shim in
// jdspController.c) pass dB-scale values, which previously went straight
// through unconverted: e.g. the default limiter init call passed
// -(FLT_EPSILON*10) as if it were "just under 0dBFS" in dB terms, which
// landed as a negative *linear* threshold and crashed
// Limiter::update_settings() on every effect open. Convert at the
// boundary here so every caller can keep passing dB values.
static inline float db_to_linear(float db) { return powf(10.0f, db / 20.0f); }

// ARM32 + x86 stubs (not used on aarch64, required by dsp.cpp dispatch table)
namespace lsp { namespace arm { struct cpu_features_t {}; } }
namespace lsp { namespace neon_d32 { void dsp_init(const lsp::arm::cpu_features_t *) {} } }
namespace lsp { namespace x86 { struct cpu_features_t {}; void detect_cpu_features(cpu_features_t *) {} void dsp_init(const cpu_features_t *) {} } }

namespace {

// Shared scratch-buffer holder for the three sidechain-linked wrappers below.
struct ScratchBufs {
    float *sc = nullptr;
    float *env = nullptr;
    float *gain = nullptr;
    float *dryL = nullptr;
    float *dryR = nullptr;
    unsigned cap = 0;
    unsigned dryCap = 0;

    void ensure(unsigned n) {
        if (n <= cap) return;
        sc = (float *)realloc(sc, n * sizeof(float));
        env = (float *)realloc(env, n * sizeof(float));
        gain = (float *)realloc(gain, n * sizeof(float));
        cap = n;
    }

    // Only allocated on demand — used solely during the brief Bypass
    // crossfade window, not on every block, so it stays off the "must cost
    // ~0 when disabled" hot path.
    void ensureDry(unsigned n) {
        if (n <= dryCap) return;
        dryL = (float *)realloc(dryL, n * sizeof(float));
        dryR = (float *)realloc(dryR, n * sizeof(float));
        dryCap = n;
    }

    ~ScratchBufs() { free(sc); free(env); free(gain); free(dryL); free(dryR); }
};

} // namespace

// --- Compressor -----------------------------------------------------------

struct CompHandle {
    lsp::dspu::Compressor comp;
    // Real LSP plugin (lsp-compressor/src/main/plug/compressor.cpp) feeds
    // the compressor through a dspu::Sidechain (named SidechainProcessor in
    // newer LSP — same class, ours is the pre-rename version) rather than a
    // raw mono average: proper RMS-smoothed envelope detection instead of
    // an unsmoothed per-sample (L+R)/2. Reactivity/mode/source are fixed
    // sane defaults here (RMS, middle/mono source, 10ms reactivity) rather
    // than exposed as new tunables — that needs AIDL cmd + UI plumbing,
    // separate work. Lookahead delay, MS/LR stereo modes, feedback
    // sidechain topology, dry/wet mix and makeup gain from the reference
    // are NOT ported this pass — real features, but each adds real scope;
    // this keeps the fix to "sidechain detection quality" specifically.
    lsp::dspu::Sidechain sidechain;
    // Real LSP plugins crossfade dry/wet over ~5ms on engage/disengage
    // (reference: compressor.cpp's c->sBypass) instead of a hard cut. This
    // was missing entirely — enabling/disabling the compressor from the UI
    // was an instant step in gain, audible as a click/crack.
    lsp::dspu::Bypass bypass;
    ScratchBufs bufs;
    // Last-configured parameters, cached in the exact units passed to
    // lsp_comp_configure() so callers (Lua property setters) can read back
    // and modify one parameter without re-deriving or stomping the others.
    // Order: threshold_db, ratio, knee_db, attack_ms, release_ms, hold_ms.
    float cfg[6];
    // Deepest VCA gain reduction applied over the last processed block, in
    // dB. Refreshed each lsp_comp_process() call; 0 while fully bypassed.
    float cur_reduction_db;
};

void *lsp_comp_create(float sample_rate) {
    ensure_dsp_init();
    auto *h = new CompHandle();
    h->comp.construct();
    h->comp.set_sample_rate((size_t)sample_rate);

    h->sidechain.construct();
    h->sidechain.init(2, 250.0f); // stereo, 250ms max reactivity ceiling
    h->sidechain.set_sample_rate((size_t)sample_rate);
    h->sidechain.set_reactivity(10.0f); // ms — typical compressor smoothing
    // set_mode(SCM_RMS)/set_source(SCS_MIDDLE)/set_stereo_mode(SCSM_STEREO)
    // are already Sidechain::init()'s defaults — not set explicitly.

    h->bypass.construct();
    h->bypass.init((int)sample_rate); // default 5ms switch time
    h->bypass.set_bypass(true); // starts disabled — matches mbDynEnabled's zero-init default
    for (int i = 0; i < 6; i++) h->cfg[i] = 0.0f;
    h->cur_reduction_db = 0.0f;
    return h;
}

void lsp_comp_destroy(void *h) {
    if (!h) return;
    auto *ch = (CompHandle *)h;
    ch->comp.destroy();
    ch->sidechain.destroy();
    ch->bypass.destroy();
    delete ch;
}

void lsp_comp_configure(void *h, float threshold, float ratio, float knee,
                         float attack_ms, float release_ms, float hold_ms) {
    if (!h) return;
    auto *ch = (CompHandle *)h;
    // Cache in caller units first — get_config() must always return the most
    // recently applied values, independent of any internal dB/linear mapping.
    ch->cfg[0] = threshold;
    ch->cfg[1] = ratio;
    ch->cfg[2] = knee;
    ch->cfg[3] = attack_ms;
    ch->cfg[4] = release_ms;
    ch->cfg[5] = hold_ms;
    // Second arg to set_threshold is the *release* threshold relative to
    // the attack threshold, in (0, 1.0]. 0.9 gives a modest, sane release
    // margin instead of the previous formula, which saturated to the 0.99
    // clamp for essentially every realistic negative dB threshold anyway.
    ch->comp.set_threshold(db_to_linear(threshold), 0.9f);
    ch->comp.set_ratio(ratio);
    ch->comp.set_knee(knee);
    if (attack_ms > 0.0f) ch->comp.set_attack(attack_ms);
    if (release_ms > 0.0f) ch->comp.set_release(release_ms);
    if (hold_ms > 0.0f) ch->comp.set_hold(hold_ms);
    ch->comp.update_settings();
}

void lsp_comp_process(void *h, float *left, float *right, unsigned n, int enabled) {
    if (!h || n == 0) return;
    auto *ch = (CompHandle *)h;

    ch->bypass.set_bypass(!enabled);
    if (ch->bypass.on()) {
        ch->cur_reduction_db = 0.0f;
        return; // fully settled disabled — zero cost, audio untouched
    }

    const bool blending = ch->bypass.active();
    if (blending) {
        ch->bufs.ensureDry(n);
        memcpy(ch->bufs.dryL, left, n * sizeof(float));
        memcpy(ch->bufs.dryR, right, n * sizeof(float));
    }

    ch->bufs.ensure(n);

    // RMS-smoothed sidechain envelope (real LSP Sidechain class) instead of
    // an unsmoothed per-sample mono average — see CompHandle comment.
    const float *sc_in[2] = { left, right };
    ch->sidechain.process(ch->bufs.sc, sc_in, n);

    ch->comp.process(ch->bufs.gain, ch->bufs.env, ch->bufs.sc, n);

    // Track deepest VCA cut for metering (cur_reduction_db).
    float min_gain = 1.0f;
    for (unsigned i = 0; i < n; i++) {
        if (ch->bufs.gain[i] < min_gain) min_gain = ch->bufs.gain[i];
        left[i] *= ch->bufs.gain[i];
        right[i] *= ch->bufs.gain[i];
    }
    ch->cur_reduction_db = (min_gain > 0.0f) ? -20.0f * log10f(min_gain) : 60.0f;

    if (blending) {
        // Crossfade dry/wet over the Bypass switch window instead of the
        // hard cut that used to happen the instant mbDynEnabled flipped.
        ch->bypass.process(left, ch->bufs.dryL, left, n);
        ch->bypass.process(right, ch->bufs.dryR, right, n);
    }
}

float lsp_comp_get_reduction_db(void *h) {
    if (!h) return 0.0f;
    return ((CompHandle *)h)->cur_reduction_db;
}

void lsp_comp_get_config(void *h, float *threshold_db, float *ratio,
                         float *knee_db, float *attack_ms, float *release_ms,
                         float *hold_ms) {
    if (!h) {
        if (threshold_db) *threshold_db = 0.0f;
        if (ratio) *ratio = 0.0f;
        if (knee_db) *knee_db = 0.0f;
        if (attack_ms) *attack_ms = 0.0f;
        if (release_ms) *release_ms = 0.0f;
        if (hold_ms) *hold_ms = 0.0f;
        return;
    }
    auto *ch = (CompHandle *)h;
    if (threshold_db) *threshold_db = ch->cfg[0];
    if (ratio) *ratio = ch->cfg[1];
    if (knee_db) *knee_db = ch->cfg[2];
    if (attack_ms) *attack_ms = ch->cfg[3];
    if (release_ms) *release_ms = ch->cfg[4];
    if (hold_ms) *hold_ms = ch->cfg[5];
}

// --- Gate ------------------------------------------------------------------

struct GateHandle {
    lsp::dspu::Gate gate;
    // Same real-sidechain upgrade as CompHandle — see that comment.
    // Reference: lsp-gate/src/main/plug/gate.cpp's c->sSC.
    lsp::dspu::Sidechain sidechain;
    // Same missing-Bypass issue as CompHandle — see that comment.
    // Reference: gate.cpp's c->sBypass.
    lsp::dspu::Bypass bypass;
    ScratchBufs bufs;
    // Last-configured parameters in caller units (see CompHandle::cfg).
    // Order: open_db, close_db, attack_ms, release_ms, hold_ms, reduction_db.
    float cfg[6];
    // Deepest attenuation applied over the last processed block, in dB.
    float cur_reduction_db;
};

void *lsp_gate_create(float sample_rate) {
    ensure_dsp_init();
    auto *h = new GateHandle();
    h->gate.construct();
    h->gate.set_sample_rate((size_t)sample_rate);

    h->sidechain.construct();
    h->sidechain.init(2, 250.0f);
    h->sidechain.set_sample_rate((size_t)sample_rate);
    h->sidechain.set_reactivity(10.0f);

    h->bypass.construct();
    h->bypass.init((int)sample_rate);
    h->bypass.set_bypass(true); // starts disabled — matches gateEnabled's zero-init default
    for (int i = 0; i < 6; i++) h->cfg[i] = 0.0f;
    h->cur_reduction_db = 0.0f;
    return h;
}

void lsp_gate_destroy(void *h) {
    if (!h) return;
    auto *gh = (GateHandle *)h;
    gh->gate.destroy();
    gh->sidechain.destroy();
    gh->bypass.destroy();
    delete gh;
}

void lsp_gate_configure(void *h, float open_db, float close_db,
                         float attack_ms, float release_ms, float hold_ms, float reduction_db) {
    if (!h) return;
    auto *gh = (GateHandle *)h;
    gh->cfg[0] = open_db;
    gh->cfg[1] = close_db;
    gh->cfg[2] = attack_ms;
    gh->cfg[3] = release_ms;
    gh->cfg[4] = hold_ms;
    gh->cfg[5] = reduction_db;
    gh->gate.set_open_threshold(db_to_linear(open_db));
    gh->gate.set_close_threshold(db_to_linear(close_db));
    if (attack_ms > 0.0f) gh->gate.set_attack(attack_ms);
    if (release_ms > 0.0f) gh->gate.set_release(release_ms);
    if (hold_ms > 0.0f) gh->gate.set_hold(hold_ms);
    gh->gate.set_reduction(db_to_linear(reduction_db));
    gh->gate.update_settings();
}

void lsp_gate_process(void *h, float *left, float *right, unsigned n, int enabled) {
    if (!h || n == 0) return;
    auto *gh = (GateHandle *)h;

    gh->bypass.set_bypass(!enabled);
    if (gh->bypass.on()) {
        gh->cur_reduction_db = 0.0f;
        return; // fully settled disabled — zero cost, audio untouched
    }

    const bool blending = gh->bypass.active();
    if (blending) {
        gh->bufs.ensureDry(n);
        memcpy(gh->bufs.dryL, left, n * sizeof(float));
        memcpy(gh->bufs.dryR, right, n * sizeof(float));
    }

    gh->bufs.ensure(n);

    const float *sc_in[2] = { left, right };
    gh->sidechain.process(gh->bufs.sc, sc_in, n);

    gh->gate.process(gh->bufs.gain, gh->bufs.env, gh->bufs.sc, n);

    // Track deepest attenuation for metering (cur_reduction_db).
    float min_gain = 1.0f;
    for (unsigned i = 0; i < n; i++) {
        if (gh->bufs.gain[i] < min_gain) min_gain = gh->bufs.gain[i];
        left[i] *= gh->bufs.gain[i];
        right[i] *= gh->bufs.gain[i];
    }
    gh->cur_reduction_db = (min_gain > 0.0f) ? -20.0f * log10f(min_gain) : 60.0f;

    if (blending) {
        gh->bypass.process(left, gh->bufs.dryL, left, n);
        gh->bypass.process(right, gh->bufs.dryR, right, n);
    }
}

float lsp_gate_get_reduction_db(void *h) {
    if (!h) return 0.0f;
    return ((GateHandle *)h)->cur_reduction_db;
}

int lsp_gate_is_closed(void *h) {
    // "Closed" = actively attenuating by more than a trivial amount. During
    // open/close transitions the applied gain is somewhere between unity and
    // the reduction setting, so a small threshold avoids flapping on noise.
    return lsp_gate_get_reduction_db(h) > 1.0f;
}

void lsp_gate_get_config(void *h, float *open_db, float *close_db,
                         float *attack_ms, float *release_ms, float *hold_ms,
                         float *reduction_db) {
    if (!h) {
        if (open_db) *open_db = 0.0f;
        if (close_db) *close_db = 0.0f;
        if (attack_ms) *attack_ms = 0.0f;
        if (release_ms) *release_ms = 0.0f;
        if (hold_ms) *hold_ms = 0.0f;
        if (reduction_db) *reduction_db = 0.0f;
        return;
    }
    auto *gh = (GateHandle *)h;
    if (open_db) *open_db = gh->cfg[0];
    if (close_db) *close_db = gh->cfg[1];
    if (attack_ms) *attack_ms = gh->cfg[2];
    if (release_ms) *release_ms = gh->cfg[3];
    if (hold_ms) *hold_ms = gh->cfg[4];
    if (reduction_db) *reduction_db = gh->cfg[5];
}

// --- Limiter ---------------------------------------------------------------

struct LimiterHandle {
    lsp::dspu::Limiter limiter;
    // Real LSP plugin (lsp-limiter/src/main/plug/limiter.cpp) delays the dry
    // audio by the limiter's lookahead latency before applying the computed
    // gain curve to it (sDataDelay.set_delay(sLimit.get_latency()), applied
    // via sDataDelay.process(...)). Without this the gain curve computed for
    // an upcoming peak gets applied to different, already-past audio —
    // lookahead limiting silently doesn't work: real peaks mostly pass
    // through unattenuated because the correcting gain reduction hasn't
    // ramped in yet at the peak's actual sample. One Delay line per channel,
    // matching the reference (not oversampled — see lsp_limiter_create).
    lsp::dspu::Delay delayL, delayR;
    ScratchBufs bufs;
    // Last-configured parameters in caller units (see CompHandle::cfg).
    // Order: threshold_db, attack_ms, release_ms, knee_db.
    float cfg[4];
    // Deepest VCA gain reduction applied over the last processed block, in dB.
    float cur_reduction_db;
};

void *lsp_limiter_create(float sample_rate) {
    ensure_dsp_init();
    auto *h = new LimiterHandle();
    h->limiter.construct();
    // Lookahead buffer must be sized before use — previously never called,
    // meaning the limiter's lookahead path had no allocated delay line.
    h->limiter.init((size_t)sample_rate * 4, 20.0f);
    h->limiter.set_sample_rate((size_t)sample_rate);
    h->limiter.set_lookahead(5.0f);
    // Delay lines sized to the same max-lookahead ceiling passed to
    // limiter.init() above, so they can never be asked to hold more than
    // they're initialized for regardless of what lsp_limiter_configure()
    // sets the actual lookahead to later.
    size_t max_delay_samples = (size_t)lsp::dspu::millis_to_samples(sample_rate, 20.0f);
    h->delayL.init(max_delay_samples);
    h->delayR.init(max_delay_samples);
    for (int i = 0; i < 4; i++) h->cfg[i] = 0.0f;
    h->cur_reduction_db = 0.0f;
    return h;
}

void lsp_limiter_destroy(void *h) {
    if (!h) return;
    auto *lh = (LimiterHandle *)h;
    lh->limiter.destroy();
    delete lh;
}

void lsp_limiter_configure(void *h, float threshold_db, float attack_ms,
                            float release_ms, float knee_db) {
    if (!h) return;
    auto *lh = (LimiterHandle *)h;
    lh->cfg[0] = threshold_db;
    lh->cfg[1] = attack_ms;
    lh->cfg[2] = release_ms;
    lh->cfg[3] = knee_db;
    lh->limiter.set_threshold(db_to_linear(threshold_db), false);
    if (attack_ms > 0.0f) lh->limiter.set_attack(attack_ms);
    if (release_ms > 0.0f) lh->limiter.set_release(release_ms);
    lh->limiter.set_knee(knee_db);
    lh->limiter.update_settings();

    // Reference pattern (lsp-limiter/src/main/plug/limiter.cpp:649):
    // c->sDataDelay.set_delay(c->sLimit.get_latency()); — must be re-read
    // after update_settings(), since that's what actually recomputes
    // nLookahead/latency from whatever set_lookahead()/set_sample_rate()
    // calls changed it to.
    size_t latency = lh->limiter.get_latency();
    lh->delayL.set_delay(latency);
    lh->delayR.set_delay(latency);
}

void lsp_limiter_process(void *h, float *left, float *right, unsigned n) {
    if (!h || n == 0) return;
    auto *lh = (LimiterHandle *)h;
    lh->bufs.ensure(n);

    // True stereo link: one gain curve derived from the louder of the two
    // channels per sample, applied identically to both — not two
    // independent process() calls fighting over one instance's state.
    for (unsigned i = 0; i < n; i++) {
        float absL = left[i] < 0.0f ? -left[i] : left[i];
        float absR = right[i] < 0.0f ? -right[i] : right[i];
        lh->bufs.sc[i] = absL > absR ? absL : absR;
    }

    // Gain curve computed from the CURRENT (undelayed) signal — this is
    // correct: the limiter's lookahead already looks forward internally
    // from this point, and its output gain[i] corresponds to sample i of
    // a signal that is `latency` samples behind what's fed in now.
    lh->limiter.process(lh->bufs.gain, lh->bufs.sc, n);

    // Track deepest VCA cut for metering (cur_reduction_db).
    float min_gain = 1.0f;
    for (unsigned i = 0; i < n; i++)
        if (lh->bufs.gain[i] < min_gain) min_gain = lh->bufs.gain[i];
    lh->cur_reduction_db = (min_gain > 0.0f) ? -20.0f * log10f(min_gain) : 60.0f;

    // Delay the actual audio by that same latency, then apply the gain
    // curve to the now-time-aligned result — Delay::process's 3-arg-plus-
    // gain overload does both in one pass, in place (dst==src is a
    // documented fast path in Delay::process, not a hazard).
    lh->delayL.process(left, left, lh->bufs.gain, n);
    lh->delayR.process(right, right, lh->bufs.gain, n);
}

float lsp_limiter_get_reduction_db(void *h) {
    if (!h) return 0.0f;
    return ((LimiterHandle *)h)->cur_reduction_db;
}

int lsp_limiter_is_limiting(void *h) {
    return lsp_limiter_get_reduction_db(h) > 0.5f;
}

void lsp_limiter_get_config(void *h, float *threshold_db, float *attack_ms,
                            float *release_ms, float *knee_db) {
    if (!h) {
        if (threshold_db) *threshold_db = 0.0f;
        if (attack_ms) *attack_ms = 0.0f;
        if (release_ms) *release_ms = 0.0f;
        if (knee_db) *knee_db = 0.0f;
        return;
    }
    auto *lh = (LimiterHandle *)h;
    if (threshold_db) *threshold_db = lh->cfg[0];
    if (attack_ms) *attack_ms = lh->cfg[1];
    if (release_ms) *release_ms = lh->cfg[2];
    if (knee_db) *knee_db = lh->cfg[3];
}
