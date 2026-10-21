// lsp_wrap_expander.cpp — faithful port of the real LSP Expander plugin.
//
// Source of truth: lsp-expander/src/main/plug/expander.cpp (pinned release
// 1.0.37, git tag 1.0.37 / commit 6cb8a97)
//   (class lsp::plugins::expander — init() @145, update_sample_rate() @480,
//    update_settings() @612, process() @847)
// and its header lsp-expander/include/private/plugins/expander.h
//   (struct expander::channel_t @~120), metadata ranges/defaults in
//   lsp-expander/include/private/meta/expander.h and port symbols in
//   lsp-expander/src/main/meta/expander.cpp.
//
// Per LSP_FAITHFUL_PORT_SCOPE.md, this is a TRANSLITERATION of that real
// plugin's DSP logic into JamesDSP's calling convention, not a fresh
// reimplementation around the bare lsp::dspu::Expander primitive. The
// per-channel state below (ChannelState) mirrors expander.h's channel_t
// field-for-field for everything that is real DSP; only plug::IPort*
// members, metadata/mesh/graph/meter/UI-display machinery, and the
// premix/external-sidechain/shared-memory-link matrix are stripped, per the
// scope decision below. Structural template: lsp_wrap_compressor.cpp.
//
// Scope decision carried over from LSP_FAITHFUL_PORT_SCOPE.md: STEREO mode
// only (real plugin's EM_STEREO, i.e. expander.cpp's nMode == EM_STEREO,
// channels == 2) and INTERNAL sidechain only (real plugin's non-
// "sc_expander" variant, bSidechain == false, every channel's nScType pinned
// to SCT_INTERNAL). Concretely this means, versus the real
// process()/update_settings():
//   - update_premix()/premix_channel()/sPremix are omitted outright: with
//     bSidechain == false and no Link input (sPremix.vLink[i] == NULL),
//     every fInToSc/fInToLink/fLinkToIn/fLinkToSc/fScToIn/fScToLink premix
//     coefficient is permanently GAIN_AMP_M_INF_DB (the real plugin's own
//     default), so premix_channel() would always take its no-op fallthrough
//     path anyway (in == in_buf, sc == sc_buf == in_buf, link == NULL).
//     Skipping it is length, not behavior.
//   - EM_MONO/EM_LR/EM_MS branches of process() are omitted (scope: stereo
//     only). The EM_STEREO (non-M/S) output stage is kept.
//   - select_buffer() always returns `in` for SCT_INTERNAL (never
//     SCT_EXTERNAL/SCT_LINK — vEmptyBuffer is therefore unreachable too), so
//     it is inlined away rather than kept as dead-code plumbing.
//   - bStereoSplit / pScSpSource (stereo-split sidechain, SWITCH "ssplit"
//     default 0.0f, COMBO "sscs") is pinned to the real default (off), so
//     decode_sidechain_source(sc_src, false, i) collapses to the non-split
//     table; with SC_SOURCE_DFL == 0 that is dspu::SCS_MIDDLE for both
//     channels.
//   - Sidechain listen (bScListen / "scl", default 0.0f), shared-memory link
//     (pShmIn), inline_display(), dump(), ui_activated(), pPause/pClear/
//     bUISync and every mesh/graph/meter port (sGraph[], pGraph[], pMeter[],
//     pCurve, pReleaseOut, vCurve, vTime) are UI/host plumbing per the hard
//     rule and are stripped.
//   - Parameters the real plugin exposes as ports but that JamesDSP's
//     lsp_exp_configure() signature has no field for (release level,
//     makeup, dry/wet gain, dry/wet balance, sidechain mode/source/
//     reactivity/preamp/lookahead, HPF/LPF, input/output trim gain) are
//     wired to the REAL plugin's own documented port defaults (see
//     lsp-expander/include/private/meta/expander.h and the port macros in
//     lsp-expander/src/main/meta/expander.cpp), not guessed — each call site
//     below cites the metadata constant it mirrors. This is a narrower
//     configuration surface, not different DSP math.
//
// Everything else — the two independent per-channel Sidechain+Expander
// pairs (real design: in EM_STEREO channel 1's ports are aliased to channel
// 0's, producing linked gain change from two structurally-identical
// detectors fed the same {L,R} pair), the EXP_BUF_SIZE block-chunked
// processing loop, the lookahead/input/output/dry compensation delay chain,
// and the final Bypass dry/wet crossfade — is carried over in the real
// plugin's own order of operations.
#include "lsp_wrap_expander.h"
#include "rt_config.h"

#include <lsp-plug.in/dsp-units/const.h>
#include <lsp-plug.in/dsp-units/units.h>
#include <lsp-plug.in/dsp-units/ctl/Bypass.h>
#include <lsp-plug.in/dsp-units/dynamics/Expander.h>
#include <lsp-plug.in/dsp-units/filters/Equalizer.h>
#include <lsp-plug.in/dsp-units/filters/common.h>
#include <lsp-plug.in/dsp-units/util/Delay.h>
#include <lsp-plug.in/dsp-units/util/Sidechain.h>
#include <lsp-plug.in/dsp/dsp.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace lsp_jdsp_expander {

// lsp::dsp::init() populates lsp-dsp-lib's internal function-pointer
// dispatch table; every lsp::dsp:: call used below (mul_k3/mul3/mix2,
// used transitively by the dspu:: classes) is a null-pointer jump until
// this has run once. `static inline` (internal linkage) so this TU can be
// linked alongside lsp_wrap.cpp / lsp_wrap_compressor.cpp — which do the
// exact same thing — without an ODR conflict; calling lsp::dsp::init()
// more than once from independent statics is harmless (idempotent global
// table fill).
static inline void ensure_dsp_init() {
    static bool done = (lsp::dsp::init(), true);
    (void)done;
}

// dspu::Expander::set_threshold()/Equalizer/etc. all take *linear* gain
// (1.0 == 0dB, see lsp-dsp-units/include/.../const.h GAIN_AMP_0_DB), while
// JamesDSP's lsp_exp_configure() takes threshold in dB, matching the rest
// of this codebase's calling convention. Convert at the boundary.
static inline float db_to_linear(float db) { return powf(10.0f, db / 20.0f); }

// EXP_BUF_SIZE: real plugin's process() chunks work into blocks of this
// size (expander.cpp:36, `static constexpr size_t EXP_BUF_SIZE = 0x200`)
// — kept identical here so the chunked delay/sidechain/expander call
// pattern below is a direct mirror, not just "processes N samples somehow".
static constexpr size_t EXP_BUF_SIZE = 0x200;

// Real metadata ranges/defaults (lsp-expander/include/private/meta/expander.h,
// struct meta::expander_metadata). Linear-gain ones are quoted as the
// GAIN_AMP_* macro they expand to in lsp-dsp-units const.h.
static constexpr float ATTACK_LVL_MIN   = GAIN_AMP_M_60_DB;  // real: ATTACK_LVL_MIN  (-60 dB)
static constexpr float ATTACK_LVL_MAX   = GAIN_AMP_0_DB;     // real: ATTACK_LVL_MAX  (0 dB)
static constexpr float ATTACK_LVL_DFL   = GAIN_AMP_M_12_DB;  // real: ATTACK_LVL_DFL  (-12 dB)
static constexpr float RELEASE_LVL_DFL  = GAIN_AMP_M_INF_DB; // real: RELEASE_LVL_DFL (-inf dB == 0.0f)
static constexpr float ATTACK_TIME_MIN  = 0.0f;              // real: ATTACK_TIME_MIN
static constexpr float ATTACK_TIME_MAX  = 2000.0f;           // real: ATTACK_TIME_MAX
static constexpr float RELEASE_TIME_MIN = 0.0f;              // real: RELEASE_TIME_MIN
static constexpr float RELEASE_TIME_MAX = 5000.0f;           // real: RELEASE_TIME_MAX
static constexpr float HOLD_TIME_MIN    = 0.0f;              // real: HOLD_TIME_MIN
static constexpr float HOLD_TIME_MAX    = 1000.0f;           // real: HOLD_TIME_MAX
static constexpr float KNEE_MIN         = GAIN_AMP_M_24_DB;  // real: KNEE_MIN (-24 dB == 0.06310)
static constexpr float KNEE_MAX         = GAIN_AMP_0_DB;     // real: KNEE_MAX (0 dB)
static constexpr float MAKEUP_DFL       = GAIN_AMP_0_DB;     // real: MAKEUP_DFL
static constexpr float RATIO_MIN        = 1.0f;              // real: RATIO_MIN
static constexpr float RATIO_MAX        = 100.0f;            // real: RATIO_MAX
static constexpr float LOOKAHEAD_MAX    = 20.0f;             // real: LOOKAHEAD_MAX (ms)
static constexpr float LOOKAHEAD_DFL    = 0.0f;              // real: LOOKAHEAD_DFL (ms)
static constexpr float REACTIVITY_MAX   = 250.0f;            // real: REACTIVITY_MAX (ms)
static constexpr float REACTIVITY_DFL   = 10.0f;             // real: REACTIVITY_DFL (ms)
static constexpr float HPF_DFL          = 10.0f;             // real: HPF_DFL (Hz)
static constexpr float LPF_DFL          = 20000.0f;          // real: LPF_DFL (Hz)

// ---------------------------------------------------------------------
// ChannelState — mirrors expander.h's expander::channel_t, stripped of
// every plug::IPort*, sGraph[]/pGraph[]/pMeter[] (UI mesh/meter plumbing),
// bScListen/pShmIn/pSC (sidechain listen & shared-mem link, out of scope),
// nSync (curve-mesh UI sync) and fDotIn/fDotOut (curve-meter dot). Field
// names below match channel_t's real names 1:1 for everything that
// survives.
// ---------------------------------------------------------------------
struct ChannelState {
    lsp::dspu::Bypass       sBypass;    // real: channel_t::sBypass
    lsp::dspu::Sidechain    sSC;        // real: channel_t::sSC
    lsp::dspu::Equalizer    sSCEq;      // real: channel_t::sSCEq (sidechain HPF/LPF)
    lsp::dspu::Expander     sExp;       // real: channel_t::sExp
    lsp::dspu::Delay        sLaDelay;   // real: channel_t::sLaDelay (lookahead)
    lsp::dspu::Delay        sInDelay;   // real: channel_t::sInDelay (latency compensation)
    lsp::dspu::Delay        sOutDelay;  // real: channel_t::sOutDelay
    lsp::dspu::Delay        sDryDelay;  // real: channel_t::sDryDelay

    // real: channel_t::vIn/vOut/vSc/vEnv/vGain — scratch buffers sized to
    // EXP_BUF_SIZE, same as the real plugin's init() allocation.
    float *vIn   = nullptr;
    float *vOut  = nullptr;
    float *vSc   = nullptr;
    float *vEnv  = nullptr;
    float *vGain = nullptr;

    // real: channel_t::nScType — pinned to SCT_INTERNAL (internal sidechain
    // only, see file banner; real init() sets `c->nScType = SCT_INTERNAL`
    // and decode_sidechain_type(0) with bSidechain == false returns it).
    // Kept as a field so the mirror to real code stays readable and a
    // future link/external-sidechain port can flip it later.
    static constexpr uint32_t SCT_INTERNAL = 0; // real: expander::SCT_INTERNAL
    uint32_t nScType = SCT_INTERNAL;

    // real init() (expander.cpp:211-213): fMakeup = 1.0f, fDryGain = 1.0f,
    // fWetGain = 0.0f — note the real plugin starts fully DRY until the
    // first update_settings(); kept identical (apply() below overwrites).
    float fMakeup   = 1.0f; // real: channel_t::fMakeup
    float fDryGain  = 1.0f; // real: channel_t::fDryGain
    float fWetGain  = 0.0f; // real: channel_t::fWetGain

    void construct() {
        // real init() (expander.cpp:184-191), minus sGraph[j].construct().
        sBypass.construct();
        sSC.construct();
        sSCEq.construct();
        sExp.construct();
        sLaDelay.construct();
        sInDelay.construct();
        sOutDelay.construct();
        sDryDelay.construct();
    }

    void destroy() {
        // real do_destroy() (expander.cpp:443-470), minus sGraph[j].destroy().
        sBypass.destroy();
        sSC.destroy();
        sSCEq.destroy();
        sExp.destroy();
        sLaDelay.destroy();
        sInDelay.destroy();
        sOutDelay.destroy();
        sDryDelay.destroy();
        free(vIn); free(vOut); free(vSc); free(vEnv); free(vGain);
        vIn = vOut = vSc = vEnv = vGain = nullptr;
    }
};

} // namespace lsp_jdsp_expander

using namespace lsp_jdsp_expander;

// ---------------------------------------------------------------------
// ExpHandle — mirrors the top-level `expander` plugin object, restricted
// to nMode == EM_STEREO (real: expander.h's exp_mode_t), so `channels` is
// always 2 and vChannels is a fixed-size pair rather than an allocation.
// ---------------------------------------------------------------------
struct ExpHandle {
    ChannelState ch[2];       // real: expander::vChannels[2] (EM_STEREO)
    float        fSampleRate; // real: plug::Module::fSampleRate
    // Last-APPLIED parameters in caller units (set by expander_apply() on the
    // audio thread). Order: mode, threshold_db, ratio, knee, attack_ms,
    // release_ms, hold_ms — matches lsp_exp_configure()'s argument order.
    struct Cfg { int mode; float threshold_db, ratio, knee, attack_ms, release_ms, hold_ms; };
    Cfg          cfg;
    RtConfig<Cfg> rt;   // control->audio handoff, see rt_config.h
    // Gain change applied over the last processed block, in dB. This is
    // JamesDSP-side telemetry replacing the real plugin's pMeter[M_GAIN]/
    // sGraph[G_GAIN] UI ports (stripped per the hard rule — those are
    // host-display plumbing, not DSP), not a "guessed" feature. Sign
    // convention: see lsp_wrap_expander.h.
    float cur_reduction_db;
};

void *lsp_exp_create(float sample_rate) {
    ensure_dsp_init();
    auto *h = new ExpHandle();
    h->fSampleRate = sample_rate;

    // real: expander::update_sample_rate(sr) (expander.cpp:480), run once
    // here since JamesDSP creates one handle per fixed sample rate rather
    // than the real plugin's live-resample path:
    //   size_t max_delay = dspu::millis_to_samples(fSampleRate,
    //       meta::expander_metadata::LOOKAHEAD_MAX);
    const size_t max_delay = (size_t)lsp::dspu::millis_to_samples(sample_rate, LOOKAHEAD_MAX);

    for (int i = 0; i < 2; i++) {
        ChannelState &c = h->ch[i];
        c.construct();

        // real expander::init() (expander.cpp:196-201):
        //   if (!c->sSC.init(channels, meta::expander_metadata::REACTIVITY_MAX)) return;
        //   if (!c->sSCEq.init(2, 12)) return;
        //   c->sSCEq.set_mode(dspu::EQM_IIR);
        //   c->sSC.set_pre_equalizer(&c->sSCEq);
        c.sSC.init(2, REACTIVITY_MAX);
        c.sSCEq.init(2, 12);
        c.sSCEq.set_mode(lsp::dspu::EQM_IIR);
        c.sSC.set_pre_equalizer(&c.sSCEq);

        // real update_sample_rate() per-channel body (expander.cpp:489-497):
        //   c->sBypass.init(sr);
        //   c->sExp.set_sample_rate(sr);
        //   c->sSC.set_sample_rate(sr);
        //   c->sSCEq.set_sample_rate(sr);
        //   c->sLaDelay.init(max_delay); c->sInDelay.init(max_delay);
        //   c->sOutDelay.init(max_delay); c->sDryDelay.init(max_delay);
        //   (sGraph[j].init(...) dropped — UI history graphs.)
        c.sBypass.init((int)sample_rate);   // default 5ms switch time
        c.sExp.set_sample_rate((size_t)sample_rate);
        c.sSC.set_sample_rate((size_t)sample_rate);
        c.sSCEq.set_sample_rate((size_t)sample_rate);
        c.sLaDelay.init(max_delay);
        c.sInDelay.init(max_delay);
        c.sOutDelay.init(max_delay);
        c.sDryDelay.init(max_delay);

        // Starts fully bypassed (silent-passthrough disabled state) —
        // matches JamesDSP's zero-init "effect off until configured+
        // enabled" convention for this module (same as the compressor port).
        c.sBypass.set_bypass(true);

        // real init(): c->vIn/vOut/vSc/vEnv/vGain = advance_ptr_bytes<float>(ptr, buf_size)
        // (one aligned arena in the real plugin; separate calloc()s here —
        // allocated once at create, never on the audio thread).
        c.vIn   = (float *)calloc(EXP_BUF_SIZE, sizeof(float));
        c.vOut  = (float *)calloc(EXP_BUF_SIZE, sizeof(float));
        c.vSc   = (float *)calloc(EXP_BUF_SIZE, sizeof(float));
        c.vEnv  = (float *)calloc(EXP_BUF_SIZE, sizeof(float));
        c.vGain = (float *)calloc(EXP_BUF_SIZE, sizeof(float));
    }

    h->cfg = ExpHandle::Cfg{ 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    h->cur_reduction_db = 0.0f;
    return h;
}

void lsp_exp_destroy(void *h) {
    if (!h) return;
    auto *hd = (ExpHandle *)h;
    hd->ch[0].destroy();
    hd->ch[1].destroy();
    delete hd;
}

// Faithful port of expander::update_settings() (expander.cpp:612),
// restricted to EM_STEREO / internal sidechain. See file banner for the
// exact mapping of each real port read to either a JamesDSP configure()
// argument or the real plugin's own documented port default.
// Audio-thread half of configure(): runs from lsp_exp_process() when a new
// config was published (rt_config.h handoff).
static void expander_apply(ExpHandle *hd, int mode, float threshold_db, float ratio,
                           float knee, float attack_ms, float release_ms, float hold_ms) {

    // Hardening (same rationale as lsp_wrap_compressor.cpp's comp_apply):
    // the real plugin's host clamps every port to its metadata range;
    // JamesDSP's AIDL/Lua surface does not. knee <= 0 makes
    // dspu::Expander::update_settings() compute sExp.start = thr*0,
    // sExp.end = thr/0, hermite over logf(0)/logf(inf) -> NaN gain for every
    // sample; threshold <= 0 makes logf(fAttackThresh) = -inf the same way.
    // Clamp to the real port ranges (meta/expander.h: KNEE -24..0 dB, RATIO
    // 1..100, ATTACK_LVL -60..0 dB, ATTACK_TIME 0..2000 ms, RELEASE_TIME
    // 0..5000 ms, HOLD_TIME 0..1000 ms). `!(x >= lo)` form also catches NaN.
    if (!(knee >= KNEE_MIN)) knee = KNEE_MIN;
    if (knee > KNEE_MAX) knee = KNEE_MAX;
    if (!(ratio >= RATIO_MIN)) ratio = RATIO_MIN;
    if (ratio > RATIO_MAX) ratio = RATIO_MAX;
    if (!(threshold_db == threshold_db)) threshold_db = -12.0f; // NaN -> ATTACK_LVL_DFL (dB)
    if (threshold_db < -60.0f) threshold_db = -60.0f;           // ATTACK_LVL_MIN (dB)
    if (threshold_db > 0.0f) threshold_db = 0.0f;               // ATTACK_LVL_MAX (dB)
    if (!(attack_ms >= ATTACK_TIME_MIN)) attack_ms = ATTACK_TIME_MIN;
    if (attack_ms > ATTACK_TIME_MAX) attack_ms = ATTACK_TIME_MAX;
    if (!(release_ms >= RELEASE_TIME_MIN)) release_ms = RELEASE_TIME_MIN;
    if (release_ms > RELEASE_TIME_MAX) release_ms = RELEASE_TIME_MAX;
    if (!(hold_ms >= HOLD_TIME_MIN)) hold_ms = HOLD_TIME_MIN;
    if (hold_ms > HOLD_TIME_MAX) hold_ms = HOLD_TIME_MAX;
    mode = (mode != 0) ? 1 : 0; // COMBO "em": 0 = Down, 1 = Up

    hd->cfg = ExpHandle::Cfg{ mode, threshold_db, ratio, knee, attack_ms, release_ms, hold_ms };

    // real: `bool bypass = pBypass->value() >= 0.5f;` then, per channel,
    // `c->sBypass.set_bypass(bypass);` — JamesDSP surfaces the enable
    // state through lsp_exp_process()'s `enabled` argument instead of a
    // configure-time port, so that set_bypass() call is made there (same
    // Bypass object, same real crossfade behavior, just relocated to match
    // where this calling convention actually carries the flag).

    // real: update_premix(); — omitted, see file banner (always -inf dB).
    // real: bPause/bClear/bMSListen — UI graph controls / EM_MS only, dropped.
    // real: bStereoSplit = pStereoSplit->value() >= 0.5f; — SWITCH "ssplit"
    //   default 0.0f -> false (pinned, see banner).
    // real: fInGain = pInGain->value(); float out_gain = pOutGain->value();
    //   IN_GAIN / OUT_GAIN ports (PORTS_STEREO_PLUGIN + EXP_COMMON) default
    //   GAIN_AMP_0_DB — fInGain is applied in process(), out_gain below.
    const float out_gain = GAIN_AMP_0_DB;
    // real: size_t latency = 0;
    size_t latency = 0;

    lsp::dspu::filter_params_t fp;

    for (int i = 0; i < 2; i++) {
        ChannelState &c = hd->ch[i];

        // real: plug::IPort *sc = (bStereoSplit) ? pScSpSource : c->pScSource;
        //       size_t sc_src = (sc != NULL) ? size_t(sc->value()) : size_t(dspu::SCS_MIDDLE);
        //   COMBO("scs", ..., expander_metadata::SC_SOURCE_DFL == 0, exp_sc_sources)
        //   -> sc_src = 0.
        // real: c->nScType = decode_sidechain_type(c->pScType->value());
        //   COMBO("sci", ..., sct_dfl == 0, exp_sc_type) -> SCT_INTERNAL (pinned).
        // real: c->bScListen = c->pScListen->value() >= 0.5f; — SWITCH "scl"
        //   default 0.0f -> false, dropped.

        // real: c->sSC.set_gain(c->pScPreamp->value());
        //   AMP_GAIN100("scp", ..., GAIN_AMP_0_DB) — SC preamp default 0 dB.
        c.sSC.set_gain(GAIN_AMP_0_DB);
        // real: c->sSC.set_mode((c->pScMode != NULL) ? size_t(c->pScMode->value()) : size_t(dspu::SCM_RMS));
        //   COMBO("scm", ..., expander_metadata::SC_MODE_DFL == 1, exp_sc_modes)
        //   -> index 1 == "RMS" == dspu::SCM_RMS.
        c.sSC.set_mode((size_t)lsp::dspu::SCM_RMS);
        // real: c->sSC.set_source(decode_sidechain_source(sc_src, bStereoSplit, i));
        //   decode_sidechain_source(0, false, i) == dspu::SCS_MIDDLE.
        c.sSC.set_source((size_t)lsp::dspu::SCS_MIDDLE);
        // real: c->sSC.set_reactivity(c->pScReactivity->value());
        //   LOG_CONTROL("scr", ..., expander_metadata::REACTIVITY) ->
        //   REACTIVITY_DFL == 10 (ms).
        c.sSC.set_reactivity(REACTIVITY_DFL);
        // real: c->sSC.set_stereo_mode(((nMode == EM_MS) && (!use_sidechain(*c)))
        //   ? dspu::SCSM_MIDSIDE : dspu::SCSM_STEREO); — EM_STEREO always SCSM_STEREO.
        c.sSC.set_stereo_mode(lsp::dspu::SCSM_STEREO);

        // real: "Setup hi-pass filter for sidechain"
        //   size_t hp_slope = c->pScHpfMode->value() * 2;   // COMBO("shpm", ..., 0, exp_filter_slope) -> 0
        //   fp.nType = (hp_slope > 0) ? dspu::FLT_BT_BWC_HIPASS : dspu::FLT_NONE;
        //   fp.fFreq = c->pScHpfFreq->value();              // LOG_CONTROL("shpf", ..., HPF) -> HPF_DFL == 10 Hz
        //   fp.fFreq2 = fp.fFreq; fp.fGain = 1.0f; fp.nSlope = hp_slope; fp.fQuality = 0.0f;
        //   c->sSCEq.set_params(0, &fp);
        const size_t hp_slope = 0 * 2;
        fp.nType    = (hp_slope > 0) ? lsp::dspu::FLT_BT_BWC_HIPASS : lsp::dspu::FLT_NONE;
        fp.fFreq    = HPF_DFL;
        fp.fFreq2   = fp.fFreq;
        fp.fGain    = 1.0f;
        fp.nSlope   = hp_slope;
        fp.fQuality = 0.0f;
        c.sSCEq.set_params(0, &fp);

        // real: "Setup low-pass filter for sidechain"
        //   size_t lp_slope = c->pScLpfMode->value() * 2;   // COMBO("slpm", ..., 0, exp_filter_slope) -> 0
        //   fp.nType = (lp_slope > 0) ? dspu::FLT_BT_BWC_LOPASS : dspu::FLT_NONE;
        //   fp.fFreq = c->pScLpfFreq->value();              // LOG_CONTROL("slpf", ..., LPF) -> LPF_DFL == 20000 Hz
        //   ... c->sSCEq.set_params(1, &fp);
        const size_t lp_slope = 0 * 2;
        fp.nType    = (lp_slope > 0) ? lsp::dspu::FLT_BT_BWC_LOPASS : lsp::dspu::FLT_NONE;
        fp.fFreq    = LPF_DFL;
        fp.fFreq2   = fp.fFreq;
        fp.fGain    = 1.0f;
        fp.nSlope   = lp_slope;
        fp.fQuality = 0.0f;
        c.sSCEq.set_params(1, &fp);

        // real: "Update delay"
        //   size_t delay = dspu::millis_to_samples(fSampleRate,
        //       (c->pScLookahead != NULL) ? c->pScLookahead->value() : 0);
        //   c->sLaDelay.set_delay(delay);
        //   latency = lsp_max(latency, delay);
        //   CONTROL("sla", ..., expander_metadata::LOOKAHEAD) -> LOOKAHEAD_DFL == 0 ms.
        const size_t delay = (size_t)lsp::dspu::millis_to_samples(hd->fSampleRate, LOOKAHEAD_DFL);
        c.sLaDelay.set_delay(delay);
        latency = (latency > delay) ? latency : delay;

        // real: "Update expander settings"
        //   float attack  = c->pAttackLvl->value();          // LOG_CONTROL("al", U_GAIN_AMP) — JamesDSP threshold_db, dB->linear here
        //   float release = c->pReleaseLvl->value() * attack;// LOG_CONTROL("rrl", U_GAIN_AMP) -> RELEASE_LVL_DFL == GAIN_AMP_M_INF_DB == 0.0f
        //   bool upward   = c->pMode->value() >= 0.5f;       // COMBO("em", EM_DEFAULT) — JamesDSP `mode`
        //   NOTE "rrl" is RELATIVE to the attack level (multiplied), exactly
        //   as the real plugin does; with the real default that yields
        //   release == 0.0f * attack == 0.0f, the real out-of-the-box state.
        const float attack  = db_to_linear(threshold_db);
        const float release = RELEASE_LVL_DFL * attack;
        const bool  upward  = (float)mode >= 0.5f;

        // real: c->sExp.set_threshold(attack, release);
        c.sExp.set_threshold(attack, release);
        // real: c->sExp.set_timings(c->pAttackTime->value(), c->pReleaseTime->value());
        //   LOG_CONTROL("at", U_MSEC) / LOG_CONTROL("rt", U_MSEC) — called
        //   unconditionally (0 ms is a valid in-range value), no guard here either.
        c.sExp.set_timings(attack_ms, release_ms);
        // real: c->sExp.set_hold(c->pHoldTime->value());    // CONTROL("hold", U_MSEC)
        c.sExp.set_hold(hold_ms);
        // real: c->sExp.set_ratio(c->pRatio->value());      // LOG_CONTROL("er", U_NONE)
        c.sExp.set_ratio(ratio);
        // real: c->sExp.set_knee(c->pKnee->value());        // LOG_CONTROL("kn", U_GAIN_AMP)
        //   pKnee is already linear (U_GAIN_AMP), same units JamesDSP's
        //   `knee` argument uses at this call site, so no conversion
        //   (same convention as lsp_comp_configure()'s knee).
        c.sExp.set_knee(knee);
        // real: c->sExp.set_mode((upward) ? dspu::EM_UPWARD : dspu::EM_DOWNWARD);
        c.sExp.set_mode((upward) ? lsp::dspu::EM_UPWARD : lsp::dspu::EM_DOWNWARD);
        // real: if (c->pReleaseOut != NULL) c->pReleaseOut->set_value(release);
        //   — METER_OUT_GAIN("rl") output meter port, UI plumbing, dropped.
        // real: c->sGraph[G_GAIN].set_method((upward) ? dspu::MM_ABS_MAXIMUM : dspu::MM_ABS_MINIMUM);
        //   — UI history graph, dropped (its min/max choice is mirrored by
        //   the telemetry in lsp_exp_process()).

        // real: "Check modification flag"
        //   if (c->sExp.modified()) { c->sExp.update_settings(); c->nSync |= S_CURVE; }
        //   — nSync is curve-mesh UI sync, dropped.
        if (c.sExp.modified())
            c.sExp.update_settings();

        // real: "Update gains"
        //   const float makeup   = c->pMakeup->value();           // LOG_CONTROL("mk", U_GAIN_AMP) -> MAKEUP_DFL == GAIN_AMP_0_DB
        //   const float dry_gain = c->pDryGain->value();          // AMP_GAIN10("cdr", ..., GAIN_AMP_M_INF_DB)
        //   const float wet_gain = c->pWetGain->value() * makeup; // AMP_GAIN10("cwt", ..., GAIN_AMP_0_DB)
        //   const float drywet   = c->pDryWet->value() * 0.01f;   // PERCENTS("cdw", ..., 100.0f, 0.1f)
        //   c->fDryGain = (dry_gain * drywet + 1.0f - drywet) * out_gain;
        //   c->fWetGain = wet_gain * drywet * out_gain;
        // With every one of those at its real documented default:
        //   makeup=1, dry_gain=0, wet_gain=1, drywet=1, out_gain=1
        //   -> fDryGain = (0*1 + 1-1)*1 = 0.0f   (no dry blend-through)
        //   -> fWetGain = 1*1*1        = 1.0f   (fully wet, plain expanded output)
        const float makeup   = MAKEUP_DFL;
        const float dry_gain = GAIN_AMP_M_INF_DB;
        const float wet_gain = GAIN_AMP_0_DB * makeup;
        const float drywet   = 100.0f * 0.01f;

        c.fDryGain = (dry_gain * drywet + 1.0f - drywet) * out_gain;
        c.fWetGain = wet_gain * drywet * out_gain;

        // real: if (c->fMakeup != makeup) { c->fMakeup = makeup; c->nSync |= S_CURVE; }
        //   — nSync half dropped (UI), assignment kept.
        if (c.fMakeup != makeup)
            c.fMakeup = makeup;
    }

    // real: "Tune compensation delays" loop (expander.cpp:712-718):
    //   c->sInDelay.set_delay(latency);
    //   c->sOutDelay.set_delay(latency - c->sLaDelay.get_delay());
    //   c->sDryDelay.set_delay(latency);
    // latency stays 0 throughout (no lookahead exposed), so every one of
    // these set_delay() calls is 0 — kept for structural fidelity; this is
    // exactly where a future lookahead-ms argument would plug in.
    for (int i = 0; i < 2; i++) {
        ChannelState &c = hd->ch[i];
        c.sInDelay.set_delay(latency);
        c.sOutDelay.set_delay(latency - c.sLaDelay.get_delay());
        c.sDryDelay.set_delay(latency);
    }
    // real: set_latency(latency); — host-latency reporting, no JamesDSP
    // equivalent call site exists for this module; nothing to report since
    // latency == 0 in this scope anyway.
}

void lsp_exp_configure(void *h, int mode, float threshold_db, float ratio, float knee,
                        float attack_ms, float release_ms, float hold_ms) {
    if (!h) return;
    auto *hd = (ExpHandle *)h;
    // Sanitise + publish only (control thread). Range clamping happens in
    // expander_apply() on the audio thread so the applied values are the
    // single source of truth; here just make sure nothing non-finite is
    // published that peek() would hand back to a Lua get->modify->set loop.
    if (!(threshold_db == threshold_db)) threshold_db = -12.0f; // ATTACK_LVL_DFL (dB)
    if (!(ratio == ratio))             ratio = 4.0f;            // RATIO_DFL
    if (!(knee == knee))               knee = KNEE_MIN;         // -> clamped anyway
    if (!(attack_ms == attack_ms))     attack_ms = 20.0f;       // ATTACK_TIME_DFL
    if (!(release_ms == release_ms))   release_ms = 100.0f;     // RELEASE_TIME_DFL
    if (!(hold_ms == hold_ms))         hold_ms = 0.0f;          // HOLD_TIME_DFL
    ExpHandle::Cfg c = { (mode != 0) ? 1 : 0, threshold_db, ratio, knee, attack_ms, release_ms, hold_ms };
    hd->rt.publish(c);   // applied by expander_apply() on the audio thread's next block
}

void lsp_exp_process(void *h, float *left, float *right, unsigned n, int enabled) {
    if (!h || n == 0) return;
    auto *hd = (ExpHandle *)h;
    {
        ExpHandle::Cfg c;
        if (hd->rt.take(c))
            expander_apply(hd, c.mode, c.threshold_db, c.ratio, c.knee,
                           c.attack_ms, c.release_ms, c.hold_ms);
    }
    ChannelState &c0 = hd->ch[0];
    ChannelState &c1 = hd->ch[1];

    // real: c->sBypass.set_bypass(bypass) inside update_settings(); see
    // expander_apply()'s comment on why this lives here instead.
    const bool bypass_req = !enabled;
    c0.sBypass.set_bypass(bypass_req);
    c1.sBypass.set_bypass(bypass_req);

    // Documented zero-cost contract (lsp_wrap_expander.h): once both
    // channels' Bypass have fully settled off, skip processing entirely
    // rather than running the real plugin's full (audibly-inert)
    // expander+delay+crossfade chain every block.
    if (c0.sBypass.on() && c1.sBypass.on()) {
        hd->cur_reduction_db = 0.0f;
        return;
    }

    // real: fInGain = pInGain->value() — IN_GAIN port default GAIN_AMP_0_DB
    // (unity); no JamesDSP-side input-trim control exists at this call site.
    const float fInGain = GAIN_AMP_0_DB;

    float min_gain = 1.0f;
    float max_gain = 1.0f;
    unsigned base = 0;
    unsigned remain = n;

    // real: process()'s "Prepare audio channels" pointer setup (sPremix.vIn/
    // vOut/vSc/vLink) is premix plumbing — in_buf[i]/out_buf[i] below are
    // simply the caller's L/R pointers (see file banner).

    // real: process()'s `while (left > 0) { const size_t to_process =
    // (left > EXP_BUF_SIZE) ? EXP_BUF_SIZE : left; ... left -= to_process; }`
    // chunking loop (expander.cpp:875-983).
    while (remain > 0) {
        const unsigned to_process = (remain > EXP_BUF_SIZE) ? EXP_BUF_SIZE : remain;
        float *inL = left + base;
        float *inR = right + base;

        // real: premix_channel(i, in_buf[i], out_buf[i], sc_buf[i], shm_buf[i], to_process)
        // for i in [0,2) — see file banner; with bSidechain == false and no
        // Link input this is always a pass-through, so inL/inR stand in
        // directly for what premix_channel() would have returned.

        // real ("Prepare audio channels", EM_STEREO/EM_LR else-branch,
        // expander.cpp:895-896):
        //   dsp::mul_k3(vChannels[0].vIn, in_buf[0], fInGain, to_process);
        //   dsp::mul_k3(vChannels[1].vIn, in_buf[1], fInGain, to_process);
        lsp::dsp::mul_k3(c0.vIn, inL, fInGain, to_process);
        lsp::dsp::mul_k3(c1.vIn, inR, fInGain, to_process);

        // real: "Perform sidechain processing" (expander.cpp:899-912),
        // stripped of sGraph[G_IN]/pMeter[M_IN]:
        //   in[0] = select_buffer(*c, vChannels[0].vIn, sc_buf[0], shm_buf[0]);
        //   if (channels > 1)
        //       in[1] = select_buffer(*c, vChannels[1].vIn, sc_buf[1], shm_buf[1]);
        //   c->sSC.process(c->vSc, in, to_process);
        //   c->sExp.process(c->vGain, c->vEnv, c->vSc, to_process);
        // select_buffer() always returns `in` for SCT_INTERNAL, so the
        // sidechain input pair is {c0.vIn, c1.vIn} for BOTH channels — this
        // is what makes the two independent detectors track each other
        // (real stereo-linked expansion), not a hack.
        const float *in[2] = { c0.vIn, c1.vIn };

        c0.sSC.process(c0.vSc, in, to_process);
        c0.sExp.process(c0.vGain, c0.vEnv, c0.vSc, to_process);

        c1.sSC.process(c1.vSc, in, to_process);
        c1.sExp.process(c1.vGain, c1.vEnv, c1.vSc, to_process);

        // real: "Apply gain to each channel and process meters"
        // (expander.cpp:915-934), stripped of the sGraph/pMeter calls:
        //   c->sLaDelay.process(c->vOut, c->vIn, c->vGain, to_process);
        //   c->sInDelay.process(c->vIn, c->vIn, to_process);
        //   c->sOutDelay.process(c->vOut, c->vOut, to_process);
        for (ChannelState *c : { &c0, &c1 }) {
            c->sLaDelay.process(c->vOut, c->vIn, c->vGain, to_process);
            c->sInDelay.process(c->vIn, c->vIn, to_process);
            c->sOutDelay.process(c->vOut, c->vOut, to_process);

            // Telemetry stand-in for pMeter[M_GAIN] (abs_max of vGain) /
            // sGraph[G_GAIN] (MM_ABS_MINIMUM downward, MM_ABS_MAXIMUM upward).
            for (unsigned k = 0; k < to_process; k++) {
                const float g = c->vGain[k];
                if (g < min_gain) min_gain = g;
                if (g > max_gain) max_gain = g;
            }
        }

        // real: "Form output signal" (expander.cpp:937-971), non-EM_MS
        // else-branch, with bScListen dropped (sidechain listen, out of
        // scope — always the mix2 path):
        //   dsp::mix2(c->vOut, c->vIn, c->fWetGain, c->fDryGain, to_process);
        //   (sGraph[G_OUT]/pMeter[M_OUT] dropped.)
        lsp::dsp::mix2(c0.vOut, c0.vIn, c0.fWetGain, c0.fDryGain, to_process);
        lsp::dsp::mix2(c1.vOut, c1.vIn, c1.fWetGain, c1.fDryGain, to_process);

        // real: "Final metering" (expander.cpp:974-980) — the bypass half:
        //   c->sDryDelay.process(c->vIn, in_buf[i], to_process);
        //   c->sBypass.process(out_buf[i], c->vIn, c->vOut, to_process);
        c0.sDryDelay.process(c0.vIn, inL, to_process);
        c0.sBypass.process(inL, c0.vIn, c0.vOut, to_process);
        c1.sDryDelay.process(c1.vIn, inR, to_process);
        c1.sBypass.process(inR, c1.vIn, c1.vOut, to_process);

        base   += to_process;
        remain -= to_process;
    }
    // real: everything after the loop (mesh requests, curve output, dot
    // meter, query_display_draw()) is UI plumbing, dropped.

    // Downward: deepest cut (>= 0 dB). Upward: largest boost, reported as a
    // negative "reduction" (<= 0 dB). See lsp_wrap_expander.h.
    if (hd->cfg.mode == 0)
        hd->cur_reduction_db = (min_gain > 0.0f) ? -20.0f * log10f(min_gain) : 60.0f;
    else
        hd->cur_reduction_db = (max_gain > 0.0f) ? -20.0f * log10f(max_gain) : 0.0f;
}

float lsp_exp_get_reduction_db(void *h) {
    if (!h) return 0.0f;
    return ((ExpHandle *)h)->cur_reduction_db;
}

int lsp_exp_is_active(void *h) {
    // "Active" = gain actually moving by more than a trivial amount in
    // either direction (downward cut or upward boost). Same 1 dB hysteresis
    // idea as lsp_gate_is_closed() so it doesn't flap on noise.
    return fabsf(lsp_exp_get_reduction_db(h)) > 1.0f;
}

void lsp_exp_get_config(void *h, int *mode, float *threshold_db, float *ratio,
                        float *knee, float *attack_ms, float *release_ms,
                        float *hold_ms) {
    if (!h) {
        if (mode) *mode = 0;
        if (threshold_db) *threshold_db = 0.0f;
        if (ratio) *ratio = 0.0f;
        if (knee) *knee = 0.0f;
        if (attack_ms) *attack_ms = 0.0f;
        if (release_ms) *release_ms = 0.0f;
        if (hold_ms) *hold_ms = 0.0f;
        return;
    }
    auto *hd = (ExpHandle *)h;
    const ExpHandle::Cfg c = hd->rt.peek();   // last published, not last applied
    if (mode) *mode = c.mode;
    if (threshold_db) *threshold_db = c.threshold_db;
    if (ratio) *ratio = c.ratio;
    if (knee) *knee = c.knee;
    if (attack_ms) *attack_ms = c.attack_ms;
    if (release_ms) *release_ms = c.release_ms;
    if (hold_ms) *hold_ms = c.hold_ms;
}
