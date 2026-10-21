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

} // namespace

// --- Gate ------------------------------------------------------------------

// Faithful transliteration of lsp-gate/src/main/plug/gate.cpp's
// gate::update_settings() and gate::process() into JamesDSP's calling
// convention — see LSP_FAITHFUL_PORT_SCOPE.md. This is NOT a wrapper
// around the bare lsp::dspu::Gate primitive (that was the bug, five times
// over). Per that scope doc's explicit decision:
//   - GM_STEREO only (real gate.h's gate_mode_t) — JamesDSP's AIDL surface
//     never selects LR/MS/mono.
//   - Internal sidechain only (SCT_INTERNAL — the module's own input feeds
//     its own detector, matching JamesDSP's existing behavior). No premix
//     matrix (real sPremix / update_premix() / bSidechain=true path) —
//     that's real host-routing capability current JamesDSP has no UI for.
//
// Real per-channel state ported below (real gate.h's channel_t, trimmed to
// what's left once plug::IPort*/mesh/meter/premix plumbing is stripped):
//   sBypass    -> real per-channel Bypass. JamesDSP's single "enabled" flag
//                 IS c->sBypass.set_bypass(!enabled) in the real code, not
//                 an invented JamesDSP feature.
//   sSC        -> real per-channel Sidechain detector (SC_MODE_DFL=SCM_RMS,
//                 SC_SOURCE_DFL=SCS_MIDDLE, REACTIVITY_DFL=10ms, preamp
//                 0dB — all real gate_metadata defaults; JamesDSP has no
//                 UI to override any of them, same as before).
//   sGate      -> real per-channel dspu::Gate envelope/hold/knee state
//                 machine. This primitive class itself was never the bug —
//                 the bug was everything *around* it being guessed instead
//                 of ported.
//   sLaDelay/sInDelay/sOutDelay/sDryDelay -> real compensation delay chain.
//                 JamesDSP has no lookahead-ms control, so these always run
//                 with delay=0 (Delay::process() with nDelay==0 is an exact
//                 passthrough — see Delay.cpp's dst==src&&nDelay==0 fast
//                 path, and the general path reduces to the same thing).
//                 Kept rather than deleted so the real order of operations
//                 (delay-then-gain, separate in/out/dry compensation, final
//                 bypass mix) stays identical to the reference.
//   fMakeup/fDryGain/fWetGain -> JamesDSP has no makeup/dry-wet UI; pinned
//                 to makeup=1/wetGain=1/dryGain=0 (real MAKEUP_DFL is 0dB
//                 anyway) — "just gate the input", reached by porting the
//                 real mix2() mix stage with these UI-less fields pinned,
//                 not by skipping the stage.
//   sSCEq (sidechain HPF/LPF) -> not instantiated. JamesDSP exposes no HPF/
//                 LPF controls; the real plugin's own port defaults for
//                 those (mode index 0) resolve to FLT_NONE i.e. no
//                 filtering, and a Sidechain with no pre-equalizer attached
//                 is that same no-op — omitting dead code, not guessing.
//
// NOT ported (explicitly out of scope — gate.cpp's mesh/graph/meter/
// inline_display() code, stripped per scope.md's UI-only-metering rule):
// sGraph[], pMeter[], pGraph[], fDotIn/fDotOut, vCurve. cur_reduction_db
// below is JamesDSP's own metering derived from the applied vGain buffer,
// not a port of the real fDotIn/fDotOut mesh-dot tracking.
// real: gate.cpp GATE_BUF_SIZE = 0x200 -- the chunk the real plugin's process()
// loop works in. Same here so scratch is fixed-size and RT-safe.
static constexpr unsigned GATE_BUF_SIZE = 0x200;

struct GateChannel {
    lsp::dspu::Bypass    sBypass;    // real: channel_t::sBypass
    lsp::dspu::Sidechain sSC;        // real: channel_t::sSC
    lsp::dspu::Gate      sGate;      // real: channel_t::sGate
    lsp::dspu::Delay     sLaDelay;   // real: channel_t::sLaDelay (lookahead, delay pinned 0)
    lsp::dspu::Delay     sInDelay;   // real: channel_t::sInDelay (delay pinned 0)
    lsp::dspu::Delay     sOutDelay;  // real: channel_t::sOutDelay (delay pinned 0)
    lsp::dspu::Delay     sDryDelay;  // real: channel_t::sDryDelay (delay pinned 0)

    // real: channel_t::vIn/vOut/vSc/vEnv/vGain (block-scratch buffers)
    float *vIn = nullptr;
    float *vOut = nullptr;
    float *vSc = nullptr;
    float *vEnv = nullptr;
    float *vGain = nullptr;
    // GATE_BUF_SIZE floats each, allocated once in lsp_gate_create() (review
    // 2026-10-21: the previous grow-on-demand realloc ran on the audio thread
    // on the first block and on every block-size growth). The real plugin
    // processes in GATE_BUF_SIZE chunks over fixed buffers too.

    // real: channel_t::fMakeup/fDryGain/fWetGain — see class comment above
    // for why these are pinned instead of UI-driven.
    float fMakeup = 1.0f;
    float fDryGain = 0.0f;
    float fWetGain = 1.0f;

    ~GateChannel() { free(vIn); free(vOut); free(vSc); free(vEnv); free(vGain); }
};

struct GateHandle {
    // real: gate::vChannels, fixed to 2 entries (GM_STEREO — see scope.md)
    GateChannel ch[2];

    // Dry-signal snapshot for the final sBypass crossfade. Real code reads
    // this straight from in_buf[i] (the pre-premix caller buffer) since
    // premix is a no-op for internal-sidechain/no-link — here that's just
    // the raw left/right buffers captured before any in-place processing.
    float *dryL = nullptr, *dryR = nullptr;   // GATE_BUF_SIZE each, allocated in create()

    // Last-configured parameters in caller units (see CompHandle::cfg).
    // Order: open_db, close_db, attack_ms, release_ms, hold_ms, reduction_db.
    float cfg[6];
    // Deepest attenuation applied over the last processed block, in dB.
    float cur_reduction_db = 0.0f;

    ~GateHandle() { free(dryL); free(dryR); }
};

void *lsp_gate_create(float sample_rate) {
    ensure_dsp_init();
    auto *h = new GateHandle();

    // real: meta::gate_metadata::LOOKAHEAD_MAX (20ms) sizes the delay
    // ring buffers; JamesDSP has no lookahead control so delay stays 0,
    // but buffers are still sized to the real plugin's max rather than
    // starved to size 1.
    const size_t max_delay = (size_t)lsp::dspu::millis_to_samples(sample_rate, 20.0f);
    h->dryL = (float *)calloc(GATE_BUF_SIZE, sizeof(float));
    h->dryR = (float *)calloc(GATE_BUF_SIZE, sizeof(float));

    for (int i = 0; i < 2; i++) {
        GateChannel &c = h->ch[i];
        c.vIn   = (float *)calloc(GATE_BUF_SIZE, sizeof(float));
        c.vOut  = (float *)calloc(GATE_BUF_SIZE, sizeof(float));
        c.vSc   = (float *)calloc(GATE_BUF_SIZE, sizeof(float));
        c.vEnv  = (float *)calloc(GATE_BUF_SIZE, sizeof(float));
        c.vGain = (float *)calloc(GATE_BUF_SIZE, sizeof(float));

        c.sBypass.construct();
        c.sBypass.init((int)sample_rate);
        c.sBypass.set_bypass(true); // starts disabled — matches gateEnabled's zero-init default

        c.sSC.construct();
        c.sSC.init(2, 250.0f); // real: c->sSC.init(channels, gate_metadata::REACTIVITY_MAX)
        c.sSC.set_sample_rate((size_t)sample_rate);
        c.sSC.set_reactivity(10.0f);                     // real: REACTIVITY_DFL
        c.sSC.set_mode(lsp::dspu::SCM_RMS);               // real: SC_MODE_DFL
        c.sSC.set_source(lsp::dspu::SCS_MIDDLE);          // real: SC_SOURCE_DFL
        c.sSC.set_gain(1.0f);                              // real: pScPreamp default 0dB
        c.sSC.set_stereo_mode(lsp::dspu::SCSM_STEREO);    // real: nMode==GM_STEREO branch

        c.sGate.construct();
        c.sGate.set_sample_rate((size_t)sample_rate);

        c.sLaDelay.construct();
        c.sInDelay.construct();
        c.sOutDelay.construct();
        c.sDryDelay.construct();
        c.sLaDelay.init(max_delay);
        c.sInDelay.init(max_delay);
        c.sOutDelay.init(max_delay);
        c.sDryDelay.init(max_delay);
        // set_delay() never called — all stay at their constructed 0, per
        // "lookahead pinned to 0, no UI" above.
    }

    for (int i = 0; i < 6; i++) h->cfg[i] = 0.0f;
    h->cur_reduction_db = 0.0f;
    return h;
}

void lsp_gate_destroy(void *h) {
    if (!h) return;
    auto *gh = (GateHandle *)h;
    for (int i = 0; i < 2; i++) {
        GateChannel &c = gh->ch[i];
        c.sBypass.destroy();
        c.sSC.destroy();
        c.sGate.destroy();
        c.sLaDelay.destroy();
        c.sInDelay.destroy();
        c.sOutDelay.destroy();
        c.sDryDelay.destroy();
    }
    delete gh;
}

void lsp_gate_configure(void *h, float open_db, float close_db,
                         float attack_ms, float release_ms, float hold_ms, float reduction_db) {
    if (!h) return;
    auto *gh = (GateHandle *)h;
    // Hardening (review 2026-10-21): the AIDL/Lua surface is not range-limited
    // like the real plugin's host ports. NaN in -> NaN gain out, and a close
    // threshold above the open one (two free sliders here; the real plugin
    // derives close = open * ratio<=0dB) makes the gate chatter at the open
    // threshold. Sanitise to what the real algorithm was designed for.
    if (!(open_db == open_db)) open_db = -40.0f;
    if (!(close_db == close_db) || close_db > open_db) close_db = open_db;
    if (!(attack_ms >= 0.0f)) attack_ms = 0.0f;
    if (!(release_ms >= 0.0f)) release_ms = 0.0f;
    if (!(hold_ms >= 0.0f)) hold_ms = 0.0f;
    if (!(reduction_db == reduction_db)) reduction_db = 0.0f;
    gh->cfg[0] = open_db;
    gh->cfg[1] = close_db;
    gh->cfg[2] = attack_ms;
    gh->cfg[3] = release_ms;
    gh->cfg[4] = hold_ms;
    gh->cfg[5] = reduction_db;

    // real: update_settings()'s per-channel loop —
    //   bool  hyst    = c->pHyst->value() >= 0.5f;
    //   float thresh  = c->pThresh[0]->value();
    //   float hthresh = hyst ? thresh * c->pThresh[1]->value() : thresh;
    // JamesDSP always supplies distinct open/close thresholds (no "hyst
    // off" state in its config surface), so this is the hyst==true path
    // with hthresh supplied directly instead of derived as a ratio.
    const float thresh    = db_to_linear(open_db);
    const float hthresh   = db_to_linear(close_db);
    const float reduction = db_to_linear(reduction_db);

    for (int i = 0; i < 2; i++) {
        GateChannel &c = gh->ch[i];

        // real: c->sGate.set_threshold(thresh, hthresh);
        c.sGate.set_threshold(thresh, hthresh);
        // real: float zone = c->pZone[0]->value(); hzone = hyst ? pZone[1]
        // : zone. JamesDSP has no knee/zone control — real ZONE_DFL
        // (GAIN_AMP_M_6_DB, -6dB) applies to both open and close zones.
        c.sGate.set_zone(GAIN_AMP_M_6_DB, GAIN_AMP_M_6_DB);
        // real: c->sGate.set_timings(c->pAttack->value(), c->pRelease->value());
        c.sGate.set_timings(attack_ms, release_ms);
        // real: c->sGate.set_hold(c->pHold->value());
        c.sGate.set_hold(hold_ms);
        // real: c->sGate.set_reduction(c->pReduction->value());
        c.sGate.set_reduction(reduction);

        // real: if (c->sGate.modified()) { c->sGate.update_settings(); c->nSync |= S_ALL; }
        if (c.sGate.modified())
            c.sGate.update_settings();

        // real: fMakeup/fDryGain/fWetGain are recomputed here from
        // pMakeup/pDryGain/pWetGain/pDryWet/out_gain every update_settings()
        // call. JamesDSP has none of those ports, so the pinned values set
        // at construction (makeup=1, wetGain=1, dryGain=0) never change —
        // nothing to recompute.
    }
}

void lsp_gate_process(void *h, float *left, float *right, unsigned n, int enabled) {
    if (!h || n == 0) return;
    auto *gh = (GateHandle *)h;
    GateChannel &c0 = gh->ch[0];
    GateChannel &c1 = gh->ch[1];

    c0.sBypass.set_bypass(!enabled);
    c1.sBypass.set_bypass(!enabled);

    if (c0.sBypass.on() && c1.sBypass.on()) {
        gh->cur_reduction_db = 0.0f;
        return; // fully settled disabled -- zero cost, audio untouched
    }

    // real: gate.cpp process() -- `while (samples > 0) { to_process = min(samples,
    // GATE_BUF_SIZE); ... }` over fixed GATE_BUF_SIZE scratch. Same loop here;
    // every stage below is the real per-channel pipeline in the real order.
    float min_gain = 1.0f;
    unsigned base = 0;
    unsigned remain = n;
    while (remain > 0) {
        const unsigned to_process = (remain > GATE_BUF_SIZE) ? GATE_BUF_SIZE : remain;
        float *inL = left + base;
        float *inR = right + base;

        // Dry copy for the bypass/dry path (real: sDryDelay fed from in_buf).
        memcpy(gh->dryL, inL, to_process * sizeof(float));
        memcpy(gh->dryR, inR, to_process * sizeof(float));

        // real: dsp::mul_k3(vChannels[i].vIn, in_buf[i], fInGain, to_process).
        // fInGain is fixed 1.0 (no JamesDSP input-gain UI), so this is a copy.
        memcpy(c0.vIn, inL, to_process * sizeof(float));
        memcpy(c1.vIn, inR, to_process * sizeof(float));

        // real: in[0..1] = select_buffer(...) -- SCT_INTERNAL always returns
        // the channel inputs, so both sidechains see the stereo pair {vIn0, vIn1}.
        const float *sc_in[2] = { c0.vIn, c1.vIn };
        c0.sSC.process(c0.vSc, sc_in, to_process);
        c1.sSC.process(c1.vSc, sc_in, to_process);

        // real: c->sGate.process(c->vGain, c->vEnv, c->vSc, to_process);
        c0.sGate.process(c0.vGain, c0.vEnv, c0.vSc, to_process);
        c1.sGate.process(c1.vGain, c1.vEnv, c1.vSc, to_process);

        // real: lookahead-compensated gain apply + in/out delay compensation.
        c0.sLaDelay.process(c0.vOut, c0.vIn, c0.vGain, to_process);
        c0.sInDelay.process(c0.vIn, c0.vIn, to_process);
        c0.sOutDelay.process(c0.vOut, c0.vOut, to_process);
        c1.sLaDelay.process(c1.vOut, c1.vIn, c1.vGain, to_process);
        c1.sInDelay.process(c1.vIn, c1.vIn, to_process);
        c1.sOutDelay.process(c1.vOut, c1.vOut, to_process);

        // real: dsp::mix2(c->vOut, c->vIn, c->fWetGain, c->fDryGain, to_process)
        lsp::dsp::mix2(c0.vOut, c0.vIn, c0.fWetGain, c0.fDryGain, to_process);
        lsp::dsp::mix2(c1.vOut, c1.vIn, c1.fWetGain, c1.fDryGain, to_process);

        // real: c->sDryDelay.process(c->vIn, in_buf, to_process);
        //       c->sBypass.process(out_buf, c->vIn, c->vOut, to_process);
        c0.sDryDelay.process(c0.vIn, gh->dryL, to_process);
        c1.sDryDelay.process(c1.vIn, gh->dryR, to_process);
        c0.sBypass.process(inL, c0.vIn, c0.vOut, to_process);
        c1.sBypass.process(inR, c1.vIn, c1.vOut, to_process);

        for (unsigned i = 0; i < to_process; i++) {
            if (c0.vGain[i] < min_gain) min_gain = c0.vGain[i];
            if (c1.vGain[i] < min_gain) min_gain = c1.vGain[i];
        }

        base   += to_process;
        remain -= to_process;
    }
    gh->cur_reduction_db = (min_gain > 0.0f) ? -20.0f * log10f(min_gain) : 60.0f;
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

