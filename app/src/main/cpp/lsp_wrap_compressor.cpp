// lsp_wrap_compressor.cpp — faithful port of the real LSP Compressor plugin.
//
// Source of truth: lsp-compressor/src/main/plug/compressor.cpp
//   (class lsp::plugins::compressor — update_settings() @605, process() @898)
// and its header lsp-compressor/include/private/plugins/compressor.h
//   (struct compressor::channel_t @119).
//
// Per LSP_FAITHFUL_PORT_SCOPE.md, this is a TRANSLITERATION of that real
// plugin's DSP logic into JamesDSP's calling convention, not a fresh
// reimplementation around the bare lsp::dspu::Compressor primitive. The
// per-channel state below (ChannelState) mirrors compressor.h's channel_t
// field-for-field for everything that is real DSP; only plug::IPort*
// members, metadata/mesh/graph/meter/UI-display machinery, and the
// premix/external-sidechain/shared-memory-link matrix are stripped, per the
// scope decision below.
//
// Scope decision carried over from LSP_FAITHFUL_PORT_SCOPE.md (compressor
// section): STEREO mode only (real plugin's CM_STEREO, i.e. compressor.cpp's
// nMode == CM_STEREO, channels == 2) and INTERNAL sidechain only (real
// plugin's non-"sc_compressor" variant, bSidechain == false, every channel's
// nScType pinned to SCT_FEED_FORWARD). Concretely this means, versus the
// real process()/update_settings():
//   - update_premix()/premix_channel()/sPremix are omitted outright: with
//     bSidechain == false and no external Link input, every fInToSc/
//     fInToLink/fLinkToIn/fLinkToSc/fScToIn/fScToLink premix coefficient
//     is permanently GAIN_AMP_M_INF_DB (the real plugin's own default), so
//     premix_channel() would always take its no-op fallthrough path anyway
//     (in == in_buf, sc == sc_buf == in_buf, link == NULL). Skipping it is
//     length, not behavior.
//   - CM_MONO/CM_LR/CM_MS branches of process() are omitted (scope: stereo
//     only). The CM_STEREO branch's mix2()-based non-M/S output stage is
//     kept.
//   - process_feedback()/the feedback bitmask switch's cases 1-3 are
//     omitted: internal sidechain implies nScType == SCT_FEED_FORWARD for
//     both channels always, so real process()'s `feedback` bitmask is
//     always 0 and only the `case 0` (process_non_feedback for both
//     channels) branch is ever reached. select_buffer() is therefore always
//     `in` (never SCT_EXTERNAL/SCT_LINK) and is inlined away rather than
//     kept as dead-code plumbing.
//   - External-sidechain ports (pSC), shared-memory link (pShmIn), sidechain
//     type/listen (nScType is pinned, bScListen dropped), inline_display(),
//     dump(), and every mesh/graph/meter port are UI/host plumbing per the
//     hard rule and are stripped.
//   - Parameters the real plugin exposes as ports but that JamesDSP's
//     lsp_comp_configure() signature has no field for (sidechain mode,
//     source, reactivity, preamp, HPF/LPF, lookahead, boost threshold/mode,
//     makeup, dry/wet gain, dry/wet balance, input/output trim gain) are
//     wired to the REAL plugin's own documented port defaults (see
//     lsp-compressor/include/private/meta/compressor.h), not guessed — each
//     call site below cites the metadata constant it mirrors. This is a
//     narrower configuration surface, not different DSP math.
//
// Everything else — the two independent per-channel Sidechain+Compressor
// pairs (real design: both channels' sidechain ports are aliased to the
// SAME source/mode/reactivity settings in CM_STEREO, producing linked
// gain reduction from two structurally-identical detectors, not one
// detector shared by copy-twice), the COMP_BUF_SIZE block-chunked
// processing loop, the lookahead/input/output/dry compensation delay
// chain, and the final Bypass dry/wet crossfade — is carried over in the
// real plugin's own order of operations.
#include "lsp_wrap_compressor.h"
#include "rt_config.h"

#include <lsp-plug.in/dsp-units/const.h>
#include <lsp-plug.in/dsp-units/units.h>
#include <lsp-plug.in/dsp-units/ctl/Bypass.h>
#include <lsp-plug.in/dsp-units/dynamics/Compressor.h>
#include <lsp-plug.in/dsp-units/filters/Equalizer.h>
#include <lsp-plug.in/dsp-units/filters/common.h>
#include <lsp-plug.in/dsp-units/util/Delay.h>
#include <lsp-plug.in/dsp-units/util/Sidechain.h>
#include <lsp-plug.in/dsp/dsp.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace lsp_jdsp_compressor {

// lsp::dsp::init() populates lsp-dsp-lib's internal function-pointer
// dispatch table; every lsp::dsp:: call used below (mul_k3/mul3/mix2,
// used transitively by the dspu:: classes) is a null-pointer jump until
// this has run once. `static inline` (internal linkage) so this TU can be
// linked alongside lsp_wrap.cpp — which does the exact same thing for
// Gate/Limiter — without an ODR conflict; calling lsp::dsp::init() twice
// from two independent statics is harmless (idempotent global table fill).
static inline void ensure_dsp_init() {
    static bool done = (lsp::dsp::init(), true);
    (void)done;
}

// dspu::Compressor::set_threshold()/Equalizer/etc. all take *linear* gain
// (1.0 == 0dB, see lsp-dsp-units/include/.../const.h GAIN_AMP_0_DB), while
// JamesDSP's lsp_comp_configure() takes threshold/knee in dB, matching the
// rest of this codebase's calling convention. Convert at the boundary.
static inline float db_to_linear(float db) { return powf(10.0f, db / 20.0f); }

// COMP_BUF_SIZE: real plugin's process() chunks work into blocks of this
// size (compressor.cpp:35, `static constexpr size_t COMP_BUF_SIZE = 0x200`)
// — kept identical here so the chunked delay/sidechain/compressor call
// pattern below is a direct mirror, not just "processes N samples somehow".
static constexpr size_t COMP_BUF_SIZE = 0x200;

// ---------------------------------------------------------------------
// ChannelState — mirrors compressor.h's compressor::channel_t, stripped of
// every plug::IPort*, sGraph[]/pGraph[]/pMeter[] (UI mesh/meter plumbing),
// bScListen/pShmIn/pSC (external-sidechain listen & shared-mem link, out of
// scope), and fFeedback (feedback-sidechain topology, out of scope — see
// file banner). Field names below match channel_t's real names 1:1 for
// everything that survives.
// ---------------------------------------------------------------------
struct ChannelState {
    lsp::dspu::Bypass       sBypass;    // real: channel_t::sBypass
    lsp::dspu::Sidechain    sSC;        // real: channel_t::sSC
    lsp::dspu::Equalizer    sSCEq;      // real: channel_t::sSCEq (sidechain HPF/LPF)
    lsp::dspu::Compressor   sComp;      // real: channel_t::sComp
    lsp::dspu::Delay        sLaDelay;   // real: channel_t::sLaDelay (lookahead)
    lsp::dspu::Delay        sInDelay;   // real: channel_t::sInDelay (latency compensation)
    lsp::dspu::Delay        sOutDelay;  // real: channel_t::sOutDelay
    lsp::dspu::Delay        sDryDelay;  // real: channel_t::sDryDelay

    // real: channel_t::vIn/vOut/vSc/vEnv/vGain — scratch buffers sized to
    // COMP_BUF_SIZE, same as the real plugin's init() allocation.
    float *vIn   = nullptr;
    float *vOut  = nullptr;
    float *vSc   = nullptr;
    float *vEnv  = nullptr;
    float *vGain = nullptr;

    // real: channel_t::nScType — pinned to SCT_FEED_FORWARD (internal
    // sidechain only, see file banner); kept as a field (rather than a
    // literal inlined at every use) so the mirror to real code stays
    // readable and so a future external-sidechain port can flip it later.
    static constexpr uint32_t SCT_FEED_FORWARD = 0; // real: compressor::SCT_FEED_FORWARD
    uint32_t nScType = SCT_FEED_FORWARD;

    float fMakeup   = 1.0f; // real: channel_t::fMakeup
    float fDryGain  = 0.0f; // real: channel_t::fDryGain
    float fWetGain  = 1.0f; // real: channel_t::fWetGain

    void construct() {
        sBypass.construct();
        sSC.construct();
        sSCEq.construct();
        sComp.construct();
        sLaDelay.construct();
        sInDelay.construct();
        sOutDelay.construct();
        sDryDelay.construct();
    }

    void destroy() {
        sBypass.destroy();
        sSC.destroy();
        sSCEq.destroy();
        sComp.destroy();
        sLaDelay.destroy();
        sInDelay.destroy();
        sOutDelay.destroy();
        sDryDelay.destroy();
        free(vIn); free(vOut); free(vSc); free(vEnv); free(vGain);
        vIn = vOut = vSc = vEnv = vGain = nullptr;
    }
};

} // namespace lsp_jdsp_compressor

using namespace lsp_jdsp_compressor;

// ---------------------------------------------------------------------
// CompHandle — mirrors the top-level `compressor` plugin object, restricted
// to nMode == CM_STEREO (real: compressor.h's c_mode_t), so `channels` is
// always 2 and vChannels is a fixed-size pair rather than an allocation.
// ---------------------------------------------------------------------
struct CompHandle {
    ChannelState ch[2];       // real: compressor::vChannels[2] (CM_STEREO)
    float        fSampleRate; // real: plug::Module::fSampleRate
    // Last-configured parameters in caller units, so the Lua property
    // setters (jdspController.c) can read back one parameter without
    // stomping the others. Order: threshold_db, ratio, knee_db, attack_ms,
    // release_ms, hold_ms — matches lsp_comp_configure()'s argument order.
    float cfg[6];
    struct Cfg { float threshold, ratio, knee, attack_ms, release_ms, hold_ms; };
    RtConfig<Cfg> rt;   // control->audio handoff, see rt_config.h
    // Deepest VCA gain reduction applied over the last processed block, in
    // dB. This is JamesDSP-side telemetry replacing the real plugin's
    // pMeter[M_GAIN]/sGraph[G_GAIN] UI ports (stripped per the hard rule —
    // those are host-display plumbing, not DSP), not a "guessed" feature.
    float cur_reduction_db;
};

void *lsp_comp_create(float sample_rate) {
    ensure_dsp_init();
    auto *h = new CompHandle();
    h->fSampleRate = sample_rate;

    // real: compressor::update_sample_rate(sr) (compressor.cpp:485), run
    // once here since JamesDSP creates one handle per fixed sample rate
    // rather than the real plugin's live-resample path.
    const float LOOKAHEAD_MAX = 20.0f; // meta::compressor_metadata::LOOKAHEAD_MAX
    const size_t max_delay = (size_t)lsp::dspu::millis_to_samples(sample_rate, LOOKAHEAD_MAX);
    const float REACTIVITY_MAX = 250.0f; // meta::compressor_metadata::REACTIVITY_MAX

    for (int i = 0; i < 2; i++) {
        ChannelState &c = h->ch[i];
        c.construct();

        // real compressor::init() (compressor.cpp:195-200):
        //   c->sSC.init(channels, meta::compressor_metadata::REACTIVITY_MAX);
        //   c->sSCEq.init(2, 12); c->sSCEq.set_mode(dspu::EQM_IIR);
        //   c->sSC.set_pre_equalizer(&c->sSCEq);
        c.sSC.init(2, REACTIVITY_MAX);
        c.sSCEq.init(2, 12);
        c.sSCEq.set_mode(lsp::dspu::EQM_IIR);
        c.sSC.set_pre_equalizer(&c.sSCEq);

        c.sComp.set_sample_rate((size_t)sample_rate);
        c.sSC.set_sample_rate((size_t)sample_rate);
        c.sSCEq.set_sample_rate((size_t)sample_rate);
        c.sLaDelay.init(max_delay);
        c.sInDelay.init(max_delay);
        c.sOutDelay.init(max_delay);
        c.sDryDelay.init(max_delay);

        // real: c->sBypass.init(sr) (default 5ms switch time).
        c.sBypass.init((int)sample_rate);
        // Starts fully bypassed (silent-passthrough disabled state) —
        // matches JamesDSP's zero-init "effect off until configured+
        // enabled" convention for this module.
        c.sBypass.set_bypass(true);

        c.vIn   = (float *)calloc(COMP_BUF_SIZE, sizeof(float));
        c.vOut  = (float *)calloc(COMP_BUF_SIZE, sizeof(float));
        c.vSc   = (float *)calloc(COMP_BUF_SIZE, sizeof(float));
        c.vEnv  = (float *)calloc(COMP_BUF_SIZE, sizeof(float));
        c.vGain = (float *)calloc(COMP_BUF_SIZE, sizeof(float));
    }

    for (int i = 0; i < 6; i++) h->cfg[i] = 0.0f;
    h->cur_reduction_db = 0.0f;
    return h;
}

void lsp_comp_destroy(void *h) {
    if (!h) return;
    auto *ch = (CompHandle *)h;
    ch->ch[0].destroy();
    ch->ch[1].destroy();
    delete ch;
}

// Faithful port of compressor::update_settings() (compressor.cpp:605),
// restricted to CM_STEREO / internal sidechain. See file banner for the
// exact mapping of each real port read to either a JamesDSP configure()
// argument or the real plugin's own documented port default.
// Audio-thread half of configure(): the real plugin's update_settings(),
// transliterated. Runs from lsp_comp_process() when a new config was published.
static void comp_apply(CompHandle *hd, float threshold, float ratio, float knee,
                       float attack_ms, float release_ms, float hold_ms) {

    // Hardening (review 2026-10-21): the real plugin's host clamps every port
    // to its metadata range; JamesDSP's AIDL/Lua surface does not, and the
    // app's knee slider goes down to 0.0. knee <= 0 makes
    // dspu::Compressor::update_settings() compute start = thr*0, end = thr/0,
    // hermite over logf(0)/logf(inf) -> NaN gain for every sample, and the
    // always-on output limiter's peak loop (NaN is never <= threshold) then
    // spins forever on the audio thread. Clamp to the real port ranges
    // (compressor.h: KNEE_MIN = -24 dB, KNEE_MAX = 0 dB, RATIO 1..100).
    // `!(x >= lo)` form also catches NaN.
    if (!(knee >= 0.0630957f)) knee = 0.0630957f;
    if (knee > 1.0f) knee = 1.0f;
    if (!(ratio >= 1.0f)) ratio = 1.0f;
    if (ratio > 100.0f) ratio = 100.0f;
    if (!(threshold == threshold)) threshold = -24.0f;
    if (!(attack_ms >= 0.0f)) attack_ms = 0.0f;
    if (!(release_ms >= 0.0f)) release_ms = 0.0f;
    if (!(hold_ms >= 0.0f)) hold_ms = 0.0f;

    hd->cfg[0] = threshold;
    hd->cfg[1] = ratio;
    hd->cfg[2] = knee;
    hd->cfg[3] = attack_ms;
    hd->cfg[4] = release_ms;
    hd->cfg[5] = hold_ms;

    // real: `bool bypass = pBypass->value() >= 0.5f;` then, per channel,
    // `c->sBypass.set_bypass(bypass);` — JamesDSP surfaces the enable
    // state through lsp_comp_process()'s `enabled` argument instead of a
    // configure-time port, so that set_bypass() call is made there (same
    // Bypass object, same real crossfade behavior, just relocated to match
    // where this calling convention actually carries the flag).

    lsp::dspu::filter_params_t fp;

    for (int i = 0; i < 2; i++) {
        ChannelState &c = hd->ch[i];

        // real: c->nScType = decode_sidechain_type(...) — pinned, see
        // ChannelState::nScType comment (internal sidechain only).

        // real: c->sSC.set_gain(c->pScPreamp->value());
        //   AMP_GAIN100("scp", ..., GAIN_AMP_0_DB) — SC preamp default 0dB.
        c.sSC.set_gain(GAIN_AMP_0_DB);
        // real: c->sSC.set_mode(size_t(c->pScMode->value()));
        //   compressor_metadata::SC_MODE_DFL == 1 == dspu::SCM_RMS.
        c.sSC.set_mode((size_t)lsp::dspu::SCM_RMS);
        // real: c->sSC.set_source(decode_sidechain_source(sc_src, ...));
        //   SC_SOURCE_DFL == 0, non-split -> dspu::SCS_MIDDLE.
        c.sSC.set_source((size_t)lsp::dspu::SCS_MIDDLE);
        // real: c->sSC.set_reactivity(c->pScReactivity->value());
        //   compressor_metadata::REACTIVITY_DFL == 10 (ms).
        c.sSC.set_reactivity(10.0f);
        // real: c->sSC.set_stereo_mode(((nMode==CM_MS)&&(!use_sidechain))
        //   ? SCSM_MIDSIDE : SCSM_STEREO); — CM_STEREO always SCSM_STEREO.
        c.sSC.set_stereo_mode(lsp::dspu::SCSM_STEREO);

        // real: sidechain HPF — COMBO("shpm", ..., 0, comp_filter_slope)
        // defaults to slope index 0 => hp_slope = 0*2 = 0 => FLT_NONE.
        // compressor_metadata::HPF_DFL == 10.0f Hz.
        fp.nType    = lsp::dspu::FLT_NONE;
        fp.fFreq    = 10.0f;
        fp.fFreq2   = fp.fFreq;
        fp.fGain    = 1.0f;
        fp.nSlope   = 0;
        fp.fQuality = 0.0f;
        c.sSCEq.set_params(0, &fp);

        // real: sidechain LPF — same default-off slope index 0.
        // compressor_metadata::LPF_DFL == 20000.0f Hz.
        fp.fFreq  = 20000.0f;
        fp.fFreq2 = fp.fFreq;
        c.sSCEq.set_params(1, &fp);

        // real: size_t delay = millis_to_samples(fSampleRate,
        //   c->pScLookahead ? value : 0); c->sLaDelay.set_delay(delay);
        //   latency = max(latency, delay);
        //   compressor_metadata::LOOKAHEAD_DFL == 0.0f -> delay == 0.
        c.sLaDelay.set_delay(0);

        // real: float attack = c->pAttackLvl->value();
        //       float release = c->pReleaseLvl->value() * attack;
        //       c->sComp.set_threshold(attack, release);
        //   JamesDSP's `threshold` (dB) maps to the real plugin's Attack
        //   Level port (pAttackLvl, U_GAIN_AMP i.e. linear gain) — convert
        //   dB->linear at the boundary same as the rest of this codebase.
        //   pReleaseLvl has no JamesDSP-side control; its real default is
        //   compressor_metadata::RELEASE_LVL_DFL == GAIN_AMP_M_INF_DB ==
        //   0.0f, so release == 0.0f * attack == 0.0f exactly like the
        //   real plugin's own out-of-the-box state.
        const float attack_lvl  = db_to_linear(threshold);
        const float release_lvl = 0.0f; // RELEASE_LVL_DFL
        const float release_thr = release_lvl * attack_lvl;
        c.sComp.set_threshold(attack_lvl, release_thr);
        // real: c->sComp.set_timings(c->pAttackTime->value(),
        //   c->pReleaseTime->value()); — called unconditionally (0ms is a
        //   valid, in-range port value meaning "instant"), so no `> 0.0f`
        //   guard here either.
        c.sComp.set_timings(attack_ms, release_ms);
        // real: c->sComp.set_hold(c->pHoldTime->value());
        c.sComp.set_hold(hold_ms);
        // real: c->sComp.set_ratio(c->pRatio->value());
        c.sComp.set_ratio(ratio);
        // real: c->sComp.set_knee(c->pKnee->value()); — pKnee is already
        //   linear (U_GAIN_AMP), same units JamesDSP's `knee` argument
        //   uses at this call site, so no conversion (matches the prior
        //   hand-wrap's treatment of this one argument).
        c.sComp.set_knee(knee);
        // real: c->sComp.set_boost_threshold((mode != CM_BOOSTING) ?
        //   c->pBThresh->value() : c->pBoost->value());
        //   mode is pinned to CM_DOWNWARD below (!= CM_BOOSTING), so this
        //   always reads pBThresh; compressor_metadata::BTH_DFL ==
        //   GAIN_AMP_M_72_DB. Only affects CM_UPWARD/CM_BOOSTING curves,
        //   so it's inert for CM_DOWNWARD, but set for fidelity.
        c.sComp.set_boost_threshold(GAIN_AMP_M_72_DB);
        // real: c->sComp.set_mode(decode_mode(c->pMode->value()));
        //   compressor_metadata::CM_DEFAULT == CM_DOWNWARD; JamesDSP's
        //   calling convention has no mode selector, so pin to the real
        //   plugin's own default (standard downward compression).
        c.sComp.set_mode(lsp::dspu::CM_DOWNWARD);

        // real: `if (c->sComp.modified()) { c->sComp.update_settings();
        //   c->nSync |= S_CURVE; }` — nSync is curve-mesh UI sync, dropped.
        if (c.sComp.modified())
            c.sComp.update_settings();

        // real: float makeup = c->pMakeup->value();               // MAKEUP_DFL == GAIN_AMP_0_DB
        //       const float dry_gain = c->pDryGain->value();       // AMP_GAIN10("cdr",...,GAIN_AMP_M_INF_DB)
        //       const float wet_gain = c->pWetGain->value()*makeup;// AMP_GAIN10("cwt",...,GAIN_AMP_0_DB)
        //       const float drywet = c->pDryWet->value()*0.01f;    // PERCENTS("cdw",...,100.0f)
        //       c->fDryGain = (dry_gain*drywet + 1.0f - drywet)*out_gain;
        //       c->fWetGain = wet_gain*drywet*out_gain;            // out_gain == pOutGain->value(), OUT_GAIN default GAIN_AMP_0_DB
        // With every one of those at its real documented default:
        //   makeup=1, dry_gain=0, wet_gain=1, drywet=1, out_gain=1
        //   -> fDryGain = (0*1 + 1-1)*1 = 0.0f   (no dry blend-through)
        //   -> fWetGain = 1*1*1        = 1.0f   (fully wet, i.e. plain
        //      compressed output — the expected default compressor
        //      behavior JamesDSP's UI already assumes).
        const float makeup   = GAIN_AMP_0_DB;
        const float dry_gain = GAIN_AMP_M_INF_DB;
        const float wet_gain = GAIN_AMP_0_DB * makeup;
        const float drywet   = 100.0f * 0.01f;
        const float out_gain = GAIN_AMP_0_DB;
        c.fDryGain = (dry_gain * drywet + 1.0f - drywet) * out_gain;
        c.fWetGain = wet_gain * drywet * out_gain;
        c.fMakeup  = makeup;
    }

    // real: "Tune compensation delays" loop (compressor.cpp:706-713).
    // latency stays 0 throughout (no lookahead exposed), so every one of
    // these set_delay() calls is 0 — kept for structural fidelity, cheap
    // enough to not special-case away, and it's exactly where a future
    // lookahead-ms argument would plug in without restructuring anything.
    for (int i = 0; i < 2; i++) {
        ChannelState &c = hd->ch[i];
        c.sInDelay.set_delay(0);
        c.sOutDelay.set_delay(0 - c.sLaDelay.get_delay()); // == 0
        c.sDryDelay.set_delay(0);
    }
    // real: set_latency(latency); — host-latency reporting, no JamesDSP
    // equivalent call site exists for this module; nothing to report since
    // latency == 0 in this scope anyway.
}

void lsp_comp_configure(void *h, float threshold, float ratio, float knee,
                         float attack_ms, float release_ms, float hold_ms) {
    if (!h) return;
    auto *hd = (CompHandle *)h;
    CompHandle::Cfg c = { threshold, ratio, knee, attack_ms, release_ms, hold_ms };
    hd->rt.publish(c);   // applied by comp_apply() on the audio thread's next block
}


void lsp_comp_process(void *h, float *left, float *right, unsigned n, int enabled) {
    if (!h || n == 0) return;
    auto *hd = (CompHandle *)h;
    {
        CompHandle::Cfg c;
        if (hd->rt.take(c))
            comp_apply(hd, c.threshold, c.ratio, c.knee, c.attack_ms, c.release_ms, c.hold_ms);
    }
    ChannelState &c0 = hd->ch[0];
    ChannelState &c1 = hd->ch[1];

    // real: c->sBypass.set_bypass(bypass) inside update_settings(); see
    // lsp_comp_configure()'s comment on why this lives here instead.
    const bool bypass_req = !enabled;
    c0.sBypass.set_bypass(bypass_req);
    c1.sBypass.set_bypass(bypass_req);

    // Documented zero-cost contract of the function this file replaces
    // (lsp_wrap.h): once both channels' Bypass have fully settled off,
    // skip processing entirely rather than running the real plugin's full
    // (audibly-inert) compressor+delay+crossfade chain every block.
    if (c0.sBypass.on() && c1.sBypass.on()) {
        hd->cur_reduction_db = 0.0f;
        return;
    }

    float min_gain = 1.0f;
    unsigned base = 0;
    unsigned remain = n;

    // real: process()'s `while (left > 0) { to_process = min(left,
    // COMP_BUF_SIZE); ... left -= to_process; }` chunking loop
    // (compressor.cpp:930-1129).
    while (remain > 0) {
        const unsigned to_process = (remain > COMP_BUF_SIZE) ? COMP_BUF_SIZE : remain;
        float *inL = left + base;
        float *inR = right + base;

        // real: premix_channel() for i in [0,1) — see file banner; with
        // bSidechain==false and no Link input this is always a pass-
        // through, so `in_buf[i]` below stands in directly for what
        // premix_channel() would have returned.

        // real (CM_STEREO branch of the input-gain switch,
        // compressor.cpp:951-953):
        //   lsp::dsp::mul_k3(vChannels[0].vIn, in_buf[0], fInGain, to_process);
        //   lsp::dsp::mul_k3(vChannels[1].vIn, in_buf[1], fInGain, to_process);
        // fInGain == pInGain->value() default GAIN_AMP_0_DB (unity); no
        // JamesDSP-side input-trim control exists at this call site.
        lsp::dsp::mul_k3(c0.vIn, inL, GAIN_AMP_0_DB, to_process);
        lsp::dsp::mul_k3(c1.vIn, inR, GAIN_AMP_0_DB, to_process);

        // real: `switch (feedback) { case 0: ... }` — feedback is always 0
        // here (internal sidechain only, see file banner), so only this
        // branch's two process_non_feedback() calls are reachable.
        // select_buffer() always returns `in` for SCT_FEED_FORWARD, so the
        // sidechain input pair is simply {c0.vIn, c1.vIn} for BOTH
        // channels — this is what makes the two independent detectors
        // track each other (real stereo-linked compression), not a hack.
        const float *scIn[2] = { c0.vIn, c1.vIn };

        // real: process_non_feedback(c, in, samples) —
        //   c->sSC.process(c->vSc, in, samples);
        //   c->sComp.process(c->vGain, c->vEnv, c->vSc, samples);
        //   lsp::dsp::mul3(c->vOut, c->vGain, c->vIn, samples);
        c0.sSC.process(c0.vSc, scIn, to_process);
        c0.sComp.process(c0.vGain, c0.vEnv, c0.vSc, to_process);
        lsp::dsp::mul3(c0.vOut, c0.vGain, c0.vIn, to_process);

        c1.sSC.process(c1.vSc, scIn, to_process);
        c1.sComp.process(c1.vGain, c1.vEnv, c1.vSc, to_process);
        lsp::dsp::mul3(c1.vOut, c1.vGain, c1.vIn, to_process);

        // real: "Apply gain to each channel, compensate latency"
        // (compressor.cpp:1062-1080), stripped of the sGraph/pMeter calls:
        //   c->sLaDelay.process(c->vOut, c->vIn, c->vGain, to_process);
        //   c->sInDelay.process(c->vIn, c->vIn, to_process);
        //   c->sOutDelay.process(c->vOut, c->vOut, to_process);
        for (ChannelState *c : { &c0, &c1 }) {
            c->sLaDelay.process(c->vOut, c->vIn, c->vGain, to_process);
            c->sInDelay.process(c->vIn, c->vIn, to_process);
            c->sOutDelay.process(c->vOut, c->vOut, to_process);

            if (to_process > 0) {
                float m = c->vGain[0];
                for (unsigned k = 1; k < to_process; k++)
                    if (c->vGain[k] < m) m = c->vGain[k];
                if (m < min_gain) min_gain = m;
            }
        }

        // real: "Form output signal" (compressor.cpp:1082-1117), CM_STEREO
        // (not-CM_MS) branch, with bScListen dropped (external-sidechain
        // listen, out of scope):
        //   lsp::dsp::mix2(c->vOut, c->vIn, c->fWetGain, c->fDryGain, to_process);
        lsp::dsp::mix2(c0.vOut, c0.vIn, c0.fWetGain, c0.fDryGain, to_process);
        lsp::dsp::mix2(c1.vOut, c1.vIn, c1.fWetGain, c1.fDryGain, to_process);

        // real: "Final metering" block's non-metering half
        // (compressor.cpp:1119-1126):
        //   c->sDryDelay.process(c->vIn, in_buf[i], to_process);
        //   c->sBypass.process(out_buf[i], c->vIn, vChannels[i].vOut, to_process);
        c0.sDryDelay.process(c0.vIn, inL, to_process);
        c0.sBypass.process(inL, c0.vIn, c0.vOut, to_process);
        c1.sDryDelay.process(c1.vIn, inR, to_process);
        c1.sBypass.process(inR, c1.vIn, c1.vOut, to_process);

        base   += to_process;
        remain -= to_process;
    }

    hd->cur_reduction_db = (min_gain > 0.0f) ? -20.0f * log10f(min_gain) : 60.0f;
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
    auto *hd = (CompHandle *)h;
    const CompHandle::Cfg c = hd->rt.peek();   // last published, not last applied
    if (threshold_db) *threshold_db = c.threshold;
    if (ratio) *ratio = c.ratio;
    if (knee_db) *knee_db = c.knee;
    if (attack_ms) *attack_ms = c.attack_ms;
    if (release_ms) *release_ms = c.release_ms;
    if (hold_ms) *hold_ms = c.hold_ms;
}
