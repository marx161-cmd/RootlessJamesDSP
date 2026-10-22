// lsp_wrap_mbcomp.cpp — faithful port of the real LSP Multiband Compressor.
//
// Source of truth: lsp-mb-compressor/src/main/plug/mb_compressor.cpp (pinned
// release 1.0.35, git tag 1.0.35 / commit 286794d; the DSP is identical to
// the 1.0.33 shipped in the comrade reference build lsp-plugins 1.2.29 —
// `git diff 1.0.33 1.0.35 -- src/main/plug/mb_compressor.cpp` is one
// size_t() cast, the metadata diff is label renumbering only)
//   (class lsp::plugins::mb_compressor — constructor @88, init() @294,
//    decode_sidechain_type() @743, update_settings() @778,
//    select_fft_rank() @1278, update_sample_rate() @1285,
//    process_band() @1393, process_input_stereo() @1410,
//    select_buffer() @1451, preprocess_channel_input() @1463,
//    premix_channel() @1499, process() @1615)
// and its header lsp-mb-compressor/include/private/plugins/mb_compressor.h
//   (comp_band_t / split_t / channel_t), metadata ranges/defaults in
//   lsp-mb-compressor/include/private/meta/mb_compressor.h, port symbols and
//   per-band defaults in lsp-mb-compressor/src/main/meta/mb_compressor.cpp
//   (MB_COMMON / MB_SPLIT / MB_BAND_COMMON / MB_STEREO_BAND and the
//   mb_compressor_stereo_ports[] table).
//
// Per LSP_FAITHFUL_PORT_SCOPE.md, this is a TRANSLITERATION of the real
// plugin's DSP logic into JamesDSP's calling convention. The dsp-units
// classes it drives (dspu::Compressor, Sidechain, Equalizer, Filter,
// DynamicFilters, FFTCrossover, Delay, Bypass) are the real ones, compiled
// from the submodule. Structural template: lsp_wrap_loudcomp.cpp.
//
// Scope: STEREO variant only (meta::mb_compressor_stereo -> nMode ==
// MBCM_STEREO, channels == 2, bSidechain == false), so vChannels is a fixed
// pair and the MBCM_MONO / MBCM_LR / MBCM_MS branches are not carried.
// Versus the real code the following is stripped or pinned — host/UI
// plumbing or inputs that do not exist in the JamesDSP chain, none of it the
// compression DSP:
//   - Sidechain type ("sce_N", Internal/Link): decode_sidechain_type() is
//     pinned to SCT_INTERNAL (the real default). The shared-memory link
//     (pShmIn / core::AudioBuffer, vShmBuffer, sEnvBoost[2].process(),
//     bUseShmLink) has no source in JamesDSP, so bUseShmLink is permanently
//     false and select_buffer() always returns vExtScBuffer (its
//     SCT_INTERNAL && !bSidechain result) — kept as the function it is.
//   - Premix ("in2lk"/"lk2in"/"lk2sc", AMP_GAIN10 dfl GAIN_AMP_M_INF_DB):
//     with bSidechain == false and vLink == NULL, premix_channel() reduces
//     to binding c->vIn/vOut/vScIn (= vIn)/vShmIn (= NULL) and advancing the
//     pointers; update_premix()/sPremix coefficients are omitted, the
//     binding is kept inline in process().
//   - Analyzer (sAnalyzer, vAnalyze[], bInFft/bOutFft, "ife_*"/"ofe_*",
//     "react"/"shift" consumers, vFreqs/vIndexes, get_frequencies()),
//     sCounter (mesh refresh gate), all meshes ("ag_*", "ifg_*", "ofg_*",
//     "bfc_N", "ccg_N": vTr/vPFc/vRFc/vTrMem/vCurve/b->vSc/b->vTr, nSync,
//     freq_chart()/pcomplex_mod()/pcomplex_mul2() calls), inline_display(),
//     pIDisplay, ui_activated(), dump(), "zoom"/fZoom, "bsel", "flt", "hue_N".
//   - Meters: pInLvl/pOutLvl (and their abs_max()), pEnvLvl/pCurveLvl/
//     pMeterGain (and the abs_max(vEnv)/curve()/reduction() that feed them),
//     pRelLevelOut, pFreqEnd (an output port; where update_settings() reads
//     it back via pFreqEnd->value() the value is b->fFreqEnd it had just
//     set), b->fGainLevel (only read by the mesh code).
//   - set_latency() host reporting -> lsp_mbcomp_get_latency().
//   - pBypass: the real plugin reads a bypass port in update_settings();
//     JamesDSP carries the enable flag through lsp_mbcomp_process(), so the
//     identical sBypass.set_bypass() call is made there (same object, same
//     crossfade; relocated, not changed).
//   - lsp_trace() / LSP_TRACE debug dumps.
// Everything else — the split-plan rebuild and sort, the per-band sidechain
// EQ -> Sidechain -> lookahead Delay -> Compressor -> VCA chain, the three
// crossover topologies (Modern DynamicFilters ladder, Classic
// pass/reject/all-pass cascade, Linear Phase FFTCrossover), the dry all-pass
// chain and dry/wet mix, the lookahead compensation delays, the MBC_BUFFER_
// SIZE chunked loop and the Bypass crossfade — is carried over in the real
// plugin's own order of operations, including its quirks (the plan rebuild's
// O(n^2) swap sort; DynamicFilters::set_filter_active() ignoring its
// argument; solo forcing bMute on the non-solo bands every update).
//
// VERIFICATION 2026-10-22: see bench/difftest (cases "mbcomp" Classic and
// "mbcomp_lp" Linear Phase vs LADSPA mb_compressor_stereo, lsp-plugins 1.2.29).
#include "lsp_wrap_mbcomp.h"
#include "rt_config.h"

#include <lsp-plug.in/common/alloc.h>
#include <lsp-plug.in/common/bits.h>
#include <lsp-plug.in/common/utility.h>
#include <lsp-plug.in/dsp-units/const.h>
#include <lsp-plug.in/dsp-units/units.h>
#include <lsp-plug.in/dsp-units/ctl/Bypass.h>
#include <lsp-plug.in/dsp-units/dynamics/Compressor.h>
#include <lsp-plug.in/dsp-units/filters/DynamicFilters.h>
#include <lsp-plug.in/dsp-units/filters/Equalizer.h>
#include <lsp-plug.in/dsp-units/filters/Filter.h>
#include <lsp-plug.in/dsp-units/filters/common.h>
#include <lsp-plug.in/dsp-units/util/Delay.h>
#include <lsp-plug.in/dsp-units/util/FFTCrossover.h>
#include <lsp-plug.in/dsp-units/util/Sidechain.h>
#include <lsp-plug.in/dsp/dsp.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace lsp_jdsp_mbcomp {

// lsp::dsp::init() populates lsp-dsp-lib's dispatch table; same idempotent
// internal-linkage guard as the other LSP ports in this build.
static inline void ensure_dsp_init() {
    static bool done = (lsp::dsp::init(), true);
    (void)done;
}

// real: mb_compressor.cpp:33  #define MBC_BUFFER_SIZE 0x200U
static constexpr size_t MBC_BUFFER_SIZE     = 0x200U;

// real: meta::mb_compressor_metadata (include/private/meta/mb_compressor.h)
static constexpr size_t BANDS_MAX           = 8;        // real: BANDS_MAX
static constexpr size_t BANDS_DFL           = 4;        // real: BANDS_DFL
static constexpr float  REACTIVITY_MIN      = 0.0f;     // real: REACTIVITY_MIN (ms)
static constexpr float  REACTIVITY_MAX      = 250.0f;   // real: REACTIVITY_MAX (ms)
static constexpr float  REACTIVITY_DFL      = 10.0f;    // real: REACTIVITY_DFL (ms)
static constexpr float  LOOKAHEAD_MIN       = 0.0f;     // real: LOOKAHEAD_MIN (ms)
static constexpr float  LOOKAHEAD_MAX       = 20.0f;    // real: LOOKAHEAD_MAX (ms)
static constexpr float  LOOKAHEAD_DFL       = 0.0f;     // real: LOOKAHEAD_DFL (ms)
static constexpr float  ATTACK_LVL_MIN      = GAIN_AMP_M_60_DB;   // real: ATTACK_LVL_MIN
static constexpr float  ATTACK_LVL_MAX      = GAIN_AMP_0_DB;      // real: ATTACK_LVL_MAX
static constexpr float  ATTACK_LVL_DFL_DB   = -12.0f;             // real: ATTACK_LVL_DFL = GAIN_AMP_M_12_DB
static constexpr float  RELEASE_LVL_MIN     = GAIN_AMP_M_INF_DB;  // real: RELEASE_LVL_MIN
static constexpr float  RELEASE_LVL_MAX     = GAIN_AMP_P_36_DB;   // real: RELEASE_LVL_MAX
static constexpr float  ATTACK_TIME_MIN     = 0.0f;     // real: ATTACK_TIME_MIN
static constexpr float  ATTACK_TIME_MAX     = 2000.0f;  // real: ATTACK_TIME_MAX
static constexpr float  ATTACK_TIME_DFL     = 20.0f;    // real: ATTACK_TIME_DFL
static constexpr float  RELEASE_TIME_MIN    = 0.0f;     // real: RELEASE_TIME_MIN
static constexpr float  RELEASE_TIME_MAX    = 5000.0f;  // real: RELEASE_TIME_MAX
static constexpr float  RELEASE_TIME_DFL    = 100.0f;   // real: RELEASE_TIME_DFL
static constexpr float  HOLD_TIME_MIN       = 0.0f;     // real: HOLD_TIME_MIN
static constexpr float  HOLD_TIME_MAX       = 1000.0f;  // real: HOLD_TIME_MAX
static constexpr float  HOLD_TIME_DFL       = 0.0f;     // real: HOLD_TIME_DFL
static constexpr float  KNEE_MIN            = GAIN_AMP_M_24_DB;   // real: KNEE_MIN
static constexpr float  KNEE_MAX            = GAIN_AMP_0_DB;      // real: KNEE_MAX
static constexpr float  KNEE_DFL_DB         = -6.0f;              // real: KNEE_DFL = GAIN_AMP_M_6_DB
static constexpr float  BTH_MIN             = GAIN_AMP_M_120_DB;  // real: BTH_MIN
static constexpr float  BTH_MAX             = GAIN_AMP_M_60_DB;   // real: BTH_MAX
static constexpr float  BTH_DFL_DB          = -72.0f;             // real: BTH_DFL = GAIN_AMP_M_72_DB
static constexpr float  BSA_MIN             = GAIN_AMP_M_72_DB;   // real: BSA_MIN
static constexpr float  BSA_MAX             = GAIN_AMP_P_72_DB;   // real: BSA_MAX
static constexpr float  BSA_DFL_DB          = 6.0f;               // real: BSA_DFL = GAIN_AMP_P_6_DB
static constexpr float  MAKEUP_MIN          = GAIN_AMP_M_60_DB;   // real: MAKEUP_MIN
static constexpr float  MAKEUP_MAX          = GAIN_AMP_P_60_DB;   // real: MAKEUP_MAX
static constexpr float  MAKEUP_DFL_DB       = 0.0f;               // real: MAKEUP_DFL = GAIN_AMP_0_DB
static constexpr float  RATIO_MIN           = 1.0f;     // real: RATIO_MIN
static constexpr float  RATIO_MAX           = 100.0f;   // real: RATIO_MAX
static constexpr float  RATIO_DFL           = 1.0f;     // real: RATIO_DFL
static constexpr float  FREQ_MIN            = 10.0f;    // real: FREQ_MIN
static constexpr float  FREQ_MAX            = 20000.0f; // real: FREQ_MAX
static constexpr float  FREQ_BOOST_MIN      = 10.0f;    // real: FREQ_BOOST_MIN
static constexpr float  REACT_TIME_MIN      = 0.0f;     // real: REACT_TIME_MIN
static constexpr float  REACT_TIME_MAX      = 1.0f;     // real: REACT_TIME_MAX
static constexpr float  REACT_TIME_DFL      = 0.2f;     // real: REACT_TIME_DFL
static constexpr size_t SC_MODE_DFL         = 1;        // real: SC_MODE_DFL (RMS)
static constexpr size_t SC_SOURCE_DFL       = 0;        // real: SC_SOURCE_DFL (Middle)
static constexpr size_t FFT_XOVER_RANK_MIN  = 12;       // real: FFT_XOVER_RANK_MIN
static constexpr size_t FFT_XOVER_FREQ_MIN  = 44100;    // real: FFT_XOVER_FREQ_MIN
// real: AMP_GAIN("g_in"/"g_out"/"g_dry"/"g_wet", ..., 10.0f) -> 0..10 linear;
//       AMP_GAIN("shift", 1.0f, 100.0f) and AMP_GAIN100("scp") -> 0..100 linear
static constexpr float  GAIN10_MAX          = 10.0f;
static constexpr float  GAIN100_MAX         = 100.0f;
// real: COMBO("mode", ..., 1, mb_global_comp_modes) -> Modern
static constexpr int    MODE_DFL            = 1;
// real: COMBO("envb", ..., FB_DEFAULT = FB_BT_3DB)
static constexpr int    ENVB_DFL            = 1;
// real: per-band defaults of MB_SPLIT / MB_STEREO_BAND in
// mb_compressor_stereo_ports[] (meta/mb_compressor.cpp:352-367)
static const float      SPLIT_ENABLE_DFL[LSP_MBCOMP_SPLITS] = { 0, 1, 0, 1, 0, 1, 0 };
static const float      SPLIT_FREQ_DFL[LSP_MBCOMP_SPLITS]   = { 40.0f, 100.0f, 252.0f, 632.0f, 1587.0f, 3984.0f, 10000.0f };
static const float      BAND_LCF_DFL[LSP_MBCOMP_BANDS]      = { 10.0f, 40.0f, 100.0f, 252.0f, 632.0f, 1587.0f, 3984.0f, 10000.0f };
static const float      BAND_HCF_DFL[LSP_MBCOMP_BANDS]      = { 40.0f, 100.0f, 252.0f, 632.0f, 1587.0f, 3984.0f, 10000.0f, 20000.0f };

// real: mb_compressor::xover_mode_t
enum xover_mode_t { XOVER_CLASSIC, XOVER_MODERN, XOVER_LINEAR_PHASE };
// real: mb_compressor::sc_type_t (only SCT_INTERNAL is reachable here)
enum sc_type_t { SCT_INTERNAL, SCT_EXTERNAL, SCT_LINK };
// real: meta::mb_compressor_metadata::boost_t
enum boost_t { FB_OFF, FB_BT_3DB, FB_MT_3DB, FB_BT_6DB, FB_MT_6DB };
// real: meta::mb_compressor_metadata::mode_t
enum cmode_t { CM_DOWNWARD, CM_UPWARD, CM_BOOSTING };

// Indices into the configuration block (see lsp_wrap_mbcomp.h).
enum { G_MODE, G_IN_DB, G_OUT_DB, G_DRY_DB, G_WET_DB, G_DRYWET, G_REACT, G_SHIFT_DB, G_ENVB, G_SSPLIT };
enum { B_CE, B_BS, B_BM, B_CM, B_SCM, B_SCS, B_SSCS, B_SLA, B_SCR, B_SCP_DB, B_SCLC, B_SCHC,
       B_SCLF, B_SCHF, B_AL_DB, B_AT, B_RRL_DB, B_RT, B_CR, B_KN_DB, B_MK_DB, B_BTH_DB, B_BSA_DB, B_HT };
static_assert(LSP_MBCOMP_GLOBALS == G_SSPLIT + 1, "globals layout");
static_assert(LSP_MBCOMP_BAND_PARAMS == B_HT + 1, "band layout");

// ---------------------------------------------------------------------
// Port-value sanitation (audio thread). The real plugin's host clamps every
// port to its metadata range; JamesDSP's AIDL/Lua surface does not.
// `!(x >= lo)` form also catches NaN.
// ---------------------------------------------------------------------
static inline float clampf(float v, float lo, float hi, float dfl) {
    if (!(v == v)) v = dfl;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}
static inline int clampi(float v, int lo, int hi, int dfl) {
    if (!(v == v)) return dfl;
    int i = (int)lroundf(v);
    if (i < lo) i = lo;
    if (i > hi) i = hi;
    return i;
}
static inline bool sw(float v) { return (v == v) && (v >= 0.5f); }
// dB (caller units) -> the real port's linear U_GAIN_AMP, clamped to the real
// range. <= LSP_MBCOMP_DB_MINUS_INF means -inf dB (GAIN_AMP_M_INF_DB == 0.0).
static inline float lin_from_db(float db, float lo, float hi, float dfl_db) {
    if (!(db == db)) db = dfl_db;
    float g = (db <= LSP_MBCOMP_DB_MINUS_INF) ? GAIN_AMP_M_INF_DB : lsp::dspu::db_to_gain(db);
    if (!(g >= lo)) g = lo;
    if (g > hi) g = hi;
    return g;
}

// Real port values (what the real update_settings() reads through
// plug::IPort::value()), decoded from the configuration block.
struct BandPorts {
    int     scs, sscs, scm, cm;
    float   sla, scr, scp, sclf, schf;
    bool    sclc, schc, ce, bs, bm;
    float   al, at, rrl, rt, ht, cr, kn, bth, bsa, mk;
};
struct SplitPorts { bool cbe; float sf; };
struct Ports {
    int         mode;
    float       g_in, g_out, g_dry, g_wet, drywet, react, shift;
    int         envb;
    bool        ssplit;
    SplitPorts  split[LSP_MBCOMP_SPLITS];
    BandPorts   band[LSP_MBCOMP_BANDS];
};

static void decode_ports(const float *p, Ports &P)
{
    P.mode      = clampi(p[G_MODE], 0, 2, MODE_DFL);
    P.g_in      = lin_from_db(p[G_IN_DB],  GAIN_AMP_M_INF_DB, GAIN10_MAX, 0.0f);
    P.g_out     = lin_from_db(p[G_OUT_DB], GAIN_AMP_M_INF_DB, GAIN10_MAX, 0.0f);
    P.g_dry     = lin_from_db(p[G_DRY_DB], GAIN_AMP_M_INF_DB, GAIN10_MAX, LSP_MBCOMP_DB_MINUS_INF);
    P.g_wet     = lin_from_db(p[G_WET_DB], GAIN_AMP_M_INF_DB, GAIN10_MAX, 0.0f);
    P.drywet    = clampf(p[G_DRYWET], 0.0f, 100.0f, 100.0f);
    P.react     = clampf(p[G_REACT], REACT_TIME_MIN, REACT_TIME_MAX, REACT_TIME_DFL);
    P.shift     = lin_from_db(p[G_SHIFT_DB], GAIN_AMP_M_INF_DB, GAIN100_MAX, 0.0f);
    P.envb      = clampi(p[G_ENVB], 0, 4, ENVB_DFL);
    P.ssplit    = sw(p[G_SSPLIT]);
    for (size_t s = 0; s < LSP_MBCOMP_SPLITS; ++s) {
        const float *q = &p[LSP_MBCOMP_SPLIT_BASE + 2 * s];
        P.split[s].cbe  = sw(q[0]);
        P.split[s].sf   = clampf(q[1], FREQ_MIN, FREQ_MAX, SPLIT_FREQ_DFL[s]);
    }
    for (size_t b = 0; b < LSP_MBCOMP_BANDS; ++b) {
        const float *q = &p[LSP_MBCOMP_BAND_BASE + LSP_MBCOMP_BAND_PARAMS * b];
        BandPorts &B = P.band[b];
        B.ce    = sw(q[B_CE]);
        B.bs    = sw(q[B_BS]);
        B.bm    = sw(q[B_BM]);
        B.cm    = clampi(q[B_CM], 0, 2, CM_DOWNWARD);
        B.scm   = clampi(q[B_SCM], 0, 3, (int)SC_MODE_DFL);
        B.scs   = clampi(q[B_SCS], 0, 5, (int)SC_SOURCE_DFL);
        B.sscs  = clampi(q[B_SSCS], 0, 5, 0);
        B.sla   = clampf(q[B_SLA], LOOKAHEAD_MIN, LOOKAHEAD_MAX, LOOKAHEAD_DFL);
        B.scr   = clampf(q[B_SCR], REACTIVITY_MIN, REACTIVITY_MAX, REACTIVITY_DFL);
        B.scp   = lin_from_db(q[B_SCP_DB], GAIN_AMP_M_INF_DB, GAIN100_MAX, 0.0f);
        B.sclc  = sw(q[B_SCLC]);
        B.schc  = sw(q[B_SCHC]);
        B.sclf  = clampf(q[B_SCLF], FREQ_MIN, FREQ_MAX, BAND_LCF_DFL[b]);
        B.schf  = clampf(q[B_SCHF], FREQ_MIN, FREQ_MAX, BAND_HCF_DFL[b]);
        B.al    = lin_from_db(q[B_AL_DB], ATTACK_LVL_MIN, ATTACK_LVL_MAX, ATTACK_LVL_DFL_DB);
        B.at    = clampf(q[B_AT], ATTACK_TIME_MIN, ATTACK_TIME_MAX, ATTACK_TIME_DFL);
        B.rrl   = lin_from_db(q[B_RRL_DB], RELEASE_LVL_MIN, RELEASE_LVL_MAX, LSP_MBCOMP_DB_MINUS_INF);
        B.rt    = clampf(q[B_RT], RELEASE_TIME_MIN, RELEASE_TIME_MAX, RELEASE_TIME_DFL);
        B.cr    = clampf(q[B_CR], RATIO_MIN, RATIO_MAX, RATIO_DFL);
        B.kn    = lin_from_db(q[B_KN_DB], KNEE_MIN, KNEE_MAX, KNEE_DFL_DB);
        B.mk    = lin_from_db(q[B_MK_DB], MAKEUP_MIN, MAKEUP_MAX, MAKEUP_DFL_DB);
        B.bth   = lin_from_db(q[B_BTH_DB], BTH_MIN, BTH_MAX, BTH_DFL_DB);
        B.bsa   = lin_from_db(q[B_BSA_DB], BSA_MIN, BSA_MAX, BSA_DFL_DB);
        B.ht    = clampf(q[B_HT], HOLD_TIME_MIN, HOLD_TIME_MAX, HOLD_TIME_DFL);
    }
}

// ---------------------------------------------------------------------
// comp_band_t — mirrors mb_compressor.h's comp_band_t, stripped of vSc/vTr
// (mesh transfer functions), fGainLevel (mesh), nSync (mesh sync) and every
// plug::IPort*. Field names match the real names 1:1.
// ---------------------------------------------------------------------
struct comp_band_t {
    lsp::dspu::Sidechain    sSC;            // real: comp_band_t::sSC
    lsp::dspu::Equalizer    sEQ[2];         // real: sEQ[2] (sidechain equalizers)
    lsp::dspu::Compressor   sComp;          // real: sComp
    lsp::dspu::Filter       sPassFilter;    // real: sPassFilter ('classic' mode)
    lsp::dspu::Filter       sRejFilter;     // real: sRejFilter ('classic' mode)
    lsp::dspu::Filter       sAllFilter;     // real: sAllFilter (phase compensation)
    lsp::dspu::Delay        sScDelay;       // real: sScDelay (sidechain lookahead)

    float                  *vBuffer;        // real: vBuffer (crossover band data)
    float                  *vVCA;           // real: vVCA
    float                   fScPreamp;      // real: fScPreamp

    float                   fFreqStart;     // real: fFreqStart
    float                   fFreqEnd;       // real: fFreqEnd

    float                   fFreqHCF;       // real: fFreqHCF
    float                   fFreqLCF;       // real: fFreqLCF
    float                   fMakeup;        // real: fMakeup
    uint32_t                nLookahead;     // real: nLookahead

    bool                    bEnabled;       // real: bEnabled
    bool                    bCustHCF;       // real: bCustHCF
    bool                    bCustLCF;       // real: bCustLCF
    bool                    bMute;          // real: bMute
    bool                    bSolo;          // real: bSolo
    uint32_t                nScType;        // real: nScType (always SCT_INTERNAL here)
    uint32_t                nFilterID;      // real: nFilterID
};

// real: mb_compressor.h split_t (minus plug::IPort*)
struct split_t {
    bool                    bEnabled;       // real: bEnabled
    float                   fFreq;          // real: fFreq
};

// real: mb_compressor.h channel_t, minus analyzer fields (vInAnalyze is kept:
// it is the input-gain-scaled signal the DSP consumes, not analysis),
// vScBuffer (bSidechain == false -> NULL in the real plugin, so the
// sEnvBoost[0].process() guarded by `if (c->vScBuffer != NULL)` never runs),
// vShmBuffer (link, see banner), vTr/vTrMem (mesh) and every plug::IPort*.
struct channel_t {
    lsp::dspu::Bypass       sBypass;        // real: sBypass
    lsp::dspu::Filter       sEnvBoost[3];   // real: sEnvBoost[3] (envelope boost filters)
    lsp::dspu::Delay        sDelay;         // real: sDelay (lookahead compensation)
    lsp::dspu::Delay        sDryDelay;      // real: sDryDelay (dry signal)
    lsp::dspu::Delay        sXOverDelay;    // real: sXOverDelay (crossover)
    lsp::dspu::Equalizer    sDryEq;         // real: sDryEq
    lsp::dspu::FFTCrossover sFFTXOver;      // real: sFFTXOver (linear phase)

    comp_band_t             vBands[BANDS_MAX];      // real: vBands
    split_t                 vSplit[BANDS_MAX-1];    // real: vSplit
    comp_band_t            *vPlan[BANDS_MAX];       // real: vPlan
    size_t                  nPlanSize;              // real: nPlanSize

    float                  *vIn;            // real: vIn (caller's L/R + offset)
    float                  *vOut;           // real: vOut (same pointer: in place)
    float                  *vScIn;          // real: vScIn (== vIn: no sidechain input)

    float                  *vInAnalyze;     // real: vInAnalyze
    float                  *vInBuffer;      // real: vInBuffer
    float                  *vBuffer;        // real: vBuffer
    float                  *vExtScBuffer;   // real: vExtScBuffer
};

} // namespace lsp_jdsp_mbcomp

using namespace lsp_jdsp_mbcomp;

// ---------------------------------------------------------------------
// MbcHandle — mirrors the top-level `mb_compressor` plugin object for
// nMode == MBCM_STEREO / bSidechain == false. Field names match the real
// members.
// ---------------------------------------------------------------------
struct MbcHandle {
    float                   fSampleRate;    // real: plug::Module::fSampleRate

    lsp::dspu::DynamicFilters sFilters;     // real: sFilters ('modern' mode)
    bool                    bEnvUpdate;     // real: bEnvUpdate
    xover_mode_t            enXOver;        // real: enXOver
    bool                    bStereoSplit;   // real: bStereoSplit
    uint32_t                nEnvBoost;      // real: nEnvBoost
    channel_t               vChannels[2];   // real: vChannels (allocated pair)
    float                   fInGain;        // real: fInGain
    float                   fDryGain;       // real: fDryGain
    float                   fWetGain;       // real: fWetGain
    uint8_t                *pData;          // real: pData (one aligned arena)
    float                  *vSc[2];         // real: vSc[2]
    float                  *vBuffer;        // real: vBuffer
    float                  *vEnv;           // real: vEnv
    size_t                  nLatency;       // real: set_latency() argument

    // Last-PUBLISHED configuration block in caller units.
    struct Cfg { float v[LSP_MBCOMP_NPARAMS]; };
    Cfg                     cfg;
    RtConfig<Cfg>           rt;             // control->audio handoff, see rt_config.h

    // Set when lsp_mbcomp_process() skipped the block because both Bypass
    // objects had settled fully off; the next processed block resets the
    // processors first (see the header's contract note).
    bool                    bIdleSkipped;
};

// real: mb_compressor::decode_mode() (mb_compressor.cpp:171)
static lsp::dspu::compressor_mode_t decode_mode(int mode)
{
    switch (mode)
    {
        case CM_DOWNWARD: return lsp::dspu::CM_DOWNWARD;
        case CM_UPWARD:   return lsp::dspu::CM_UPWARD;
        case CM_BOOSTING: return lsp::dspu::CM_BOOSTING;
        default: break;
    }
    return lsp::dspu::CM_DOWNWARD;
}

// real: mb_compressor::decode_sidechain_source() (mb_compressor.cpp:183)
static lsp::dspu::sidechain_source_t decode_sidechain_source(int source, bool split, size_t channel)
{
    if (!split)
    {
        switch (source)
        {
            case 0: return lsp::dspu::SCS_MIDDLE;
            case 1: return lsp::dspu::SCS_SIDE;
            case 2: return lsp::dspu::SCS_LEFT;
            case 3: return lsp::dspu::SCS_RIGHT;
            case 4: return lsp::dspu::SCS_AMIN;
            case 5: return lsp::dspu::SCS_AMAX;
            default: break;
        }
    }

    if (channel == 0)
    {
        switch (source)
        {
            case 0: return lsp::dspu::SCS_LEFT;
            case 1: return lsp::dspu::SCS_RIGHT;
            case 2: return lsp::dspu::SCS_MIDDLE;
            case 3: return lsp::dspu::SCS_SIDE;
            case 4: return lsp::dspu::SCS_AMIN;
            case 5: return lsp::dspu::SCS_AMAX;
            default: break;
        }
    }
    else
    {
        switch (source)
        {
            case 0: return lsp::dspu::SCS_RIGHT;
            case 1: return lsp::dspu::SCS_LEFT;
            case 2: return lsp::dspu::SCS_SIDE;
            case 3: return lsp::dspu::SCS_MIDDLE;
            case 4: return lsp::dspu::SCS_AMIN;
            case 5: return lsp::dspu::SCS_AMAX;
            default: break;
        }
    }

    return lsp::dspu::SCS_MIDDLE;
}

// real: mb_compressor::compare_bands_for_sort() (mb_compressor.cpp:164)
static bool compare_bands_for_sort(const comp_band_t *b1, const comp_band_t *b2)
{
    if (b1->fFreqStart != b2->fFreqStart)
        return (b1->fFreqStart > b2->fFreqStart);
    return b1 < b2;
}

// real: mb_compressor::decode_sidechain_type() (mb_compressor.cpp:743), with
// bSidechain == false and the "sce_N" port pinned to its default 0:
//   case 0: return SCT_INTERNAL;
static inline uint32_t decode_sidechain_type() { return SCT_INTERNAL; }

// real: mb_compressor::select_fft_rank() (mb_compressor.cpp:1278)
static size_t select_fft_rank(size_t sample_rate)
{
    const size_t k = (sample_rate + FFT_XOVER_FREQ_MIN/2) / FFT_XOVER_FREQ_MIN;
    const size_t n = lsp::int_log2(k);
    return FFT_XOVER_RANK_MIN + n;
}

// real: mb_compressor::process_band() (mb_compressor.cpp:1393) — FFTCrossover
// band sink: store data to band's buffer.
static void process_band(void *object, void *subject, size_t band, const float *data, size_t sample, size_t count)
{
    (void)object;
    channel_t *c            = static_cast<channel_t *>(subject);
    comp_band_t *b          = &c->vBands[band];

    lsp::dsp::copy(&b->vBuffer[sample], data, count);
}

// real: mb_compressor::process_input_stereo() (mb_compressor.cpp:1410), the
// nMode != MBCM_MS branch. Inputs are never NULL here (JamesDSP always hands
// both channels), the NULL branches are kept for fidelity.
static void process_input_stereo(MbcHandle *hd, float *l_out, float *r_out, const float *l_in, const float *r_in, size_t count)
{
    if (l_in != NULL)
        lsp::dsp::mul_k3(l_out, l_in, hd->fInGain, count);
    else
        lsp::dsp::fill_zero(l_out, count);

    if (r_in != NULL)
        lsp::dsp::mul_k3(r_out, r_in, hd->fInGain, count);
    else
        lsp::dsp::fill_zero(r_out, count);
}

// real: mb_compressor::select_buffer() (mb_compressor.cpp:1451):
//   case SCT_INTERNAL: return (bSidechain) ? channel->vScBuffer : channel->vExtScBuffer;
static inline const float *select_buffer(const comp_band_t *band, const channel_t *channel)
{
    (void)band; // nScType is always SCT_INTERNAL (see banner)
    return channel->vExtScBuffer;
}

// real: mb_compressor::preprocess_channel_input() (mb_compressor.cpp:1463),
// channels > 1, bUseShmLink == false, vScBuffer == NULL.
static void preprocess_channel_input(MbcHandle *hd, size_t count)
{
    channel_t *l = &hd->vChannels[0];
    channel_t *r = &hd->vChannels[1];

    process_input_stereo(hd, l->vInAnalyze, r->vInAnalyze, l->vIn, r->vIn, count);
    process_input_stereo(hd, l->vExtScBuffer, r->vExtScBuffer, l->vScIn, r->vScIn, count);
    // real: if (bUseShmLink) process_input_stereo(l->vShmBuffer, ...) — link pinned off

    // Do frequency boost and input channel analysis
    for (size_t i=0; i<2; ++i)
    {
        channel_t *c        = &hd->vChannels[i];
        // real: if (c->vScBuffer != NULL) c->sEnvBoost[0].process(c->vScBuffer, c->vInAnalyze, count); — NULL (bSidechain == false)
        c->sEnvBoost[1].process(c->vExtScBuffer, c->vExtScBuffer, count);
        // real: if (bUseShmLink) c->sEnvBoost[2].process(c->vShmBuffer, c->vShmBuffer, count); — link pinned off
    }
}

void lsp_mbcomp_default_config(float *p)
{
    if (!p) return;
    p[G_MODE]       = (float)MODE_DFL;
    p[G_IN_DB]      = 0.0f;                     // IN_GAIN_DFL = 1.0 linear
    p[G_OUT_DB]     = 0.0f;                     // OUT_GAIN_DFL = 1.0 linear
    p[G_DRY_DB]     = LSP_MBCOMP_DB_MINUS_INF;  // AMP_GAIN("g_dry", ..., 0.0f, 10.0f)
    p[G_WET_DB]     = 0.0f;                     // AMP_GAIN("g_wet", ..., 1.0f, 10.0f)
    p[G_DRYWET]     = 100.0f;                   // PERCENTS("drywet", ..., 100.0f, 0.1f)
    p[G_REACT]      = REACT_TIME_DFL;
    p[G_SHIFT_DB]   = 0.0f;                     // AMP_GAIN("shift", ..., 1.0f, 100.0f)
    p[G_ENVB]       = (float)ENVB_DFL;
    p[G_SSPLIT]     = 0.0f;                     // SWITCH("ssplit", ..., 0.0f)
    for (size_t s = 0; s < LSP_MBCOMP_SPLITS; ++s) {
        p[LSP_MBCOMP_SPLIT_BASE + 2*s + 0] = SPLIT_ENABLE_DFL[s];
        p[LSP_MBCOMP_SPLIT_BASE + 2*s + 1] = SPLIT_FREQ_DFL[s];
    }
    for (size_t b = 0; b < LSP_MBCOMP_BANDS; ++b) {
        float *q = &p[LSP_MBCOMP_BAND_BASE + LSP_MBCOMP_BAND_PARAMS * b];
        q[B_CE]     = 1.0f;                     // SWITCH("ce", ..., 1.0f)
        q[B_BS]     = 0.0f;
        q[B_BM]     = 0.0f;
        q[B_CM]     = (float)CM_DOWNWARD;       // CM_DEFAULT
        q[B_SCM]    = (float)SC_MODE_DFL;
        q[B_SCS]    = (float)SC_SOURCE_DFL;
        q[B_SSCS]   = 0.0f;
        q[B_SLA]    = LOOKAHEAD_DFL;
        q[B_SCR]    = REACTIVITY_DFL;
        q[B_SCP_DB] = 0.0f;                     // AMP_GAIN100("scp", ..., GAIN_AMP_0_DB)
        q[B_SCLC]   = 0.0f;
        q[B_SCHC]   = 0.0f;
        q[B_SCLF]   = BAND_LCF_DFL[b];
        q[B_SCHF]   = BAND_HCF_DFL[b];
        q[B_AL_DB]  = ATTACK_LVL_DFL_DB;
        q[B_AT]     = ATTACK_TIME_DFL;
        q[B_RRL_DB] = LSP_MBCOMP_DB_MINUS_INF;  // RELEASE_LVL_DFL = GAIN_AMP_M_INF_DB
        q[B_RT]     = RELEASE_TIME_DFL;
        q[B_CR]     = RATIO_DFL;
        q[B_KN_DB]  = KNEE_DFL_DB;
        q[B_MK_DB]  = MAKEUP_DFL_DB;
        q[B_BTH_DB] = BTH_DFL_DB;
        q[B_BSA_DB] = BSA_DFL_DB;
        q[B_HT]     = HOLD_TIME_DFL;
    }
}

static void mbcomp_apply(MbcHandle *hd, const MbcHandle::Cfg &cfg);

void *lsp_mbcomp_create(float sample_rate) {
    ensure_dsp_init();
    auto *hd = new MbcHandle();
    hd->fSampleRate = sample_rate;

    // real constructor (mb_compressor.cpp:88-157): the start state every
    // later update_settings() diffs against (analyzer/premix/UI members dropped).
    hd->bEnvUpdate      = true;
    hd->enXOver         = XOVER_MODERN;
    hd->bStereoSplit    = false;
    hd->nEnvBoost       = FB_BT_3DB;                // meta::mb_compressor_metadata::FB_DEFAULT
    hd->fInGain         = GAIN_AMP_0_DB;
    hd->fDryGain        = GAIN_AMP_M_INF_DB;
    hd->fWetGain        = GAIN_AMP_0_DB;
    hd->pData           = NULL;
    hd->vSc[0]          = NULL;
    hd->vSc[1]          = NULL;
    hd->vBuffer         = NULL;
    hd->vEnv            = NULL;
    hd->nLatency        = 0;

    // real init() (mb_compressor.cpp:294-530), channels == 2, bSidechain == false.
    const size_t channels = 2;

    // Allocate float buffer data — the real arena minus the mesh/analyzer
    // (vTr/vPFc/vRFc/vFreqs/vCurve/vIndexes, per-channel vTr/vTrMem, per-band
    // vSc/vTr), premix temporaries and the link/sidechain buffers that are
    // never read here (see banner). One aligned allocation, create-time only.
    size_t to_alloc =
            MBC_BUFFER_SIZE * sizeof(float) + // Global vBuffer for band signal processing
            MBC_BUFFER_SIZE * sizeof(float) + // Global vEnv for band signal processing
            (
                MBC_BUFFER_SIZE * sizeof(float) + // Global vSc[] for each channel
                MBC_BUFFER_SIZE * sizeof(float) + // vInAnalyze for each channel
                MBC_BUFFER_SIZE * sizeof(float) + // vInBuffer for each channel
                MBC_BUFFER_SIZE * sizeof(float) + // vBuffer for each channel
                MBC_BUFFER_SIZE * sizeof(float) + // vExtScBuffer for each channel
                (
                    MBC_BUFFER_SIZE * sizeof(float) + // vBuffer of each band
                    MBC_BUFFER_SIZE * sizeof(float)   // vVCA of each band
                ) * BANDS_MAX
            ) * channels;

    uint8_t *ptr    = lsp::alloc_aligned<uint8_t>(hd->pData, to_alloc);
    if (ptr == NULL) {
        delete hd;
        return NULL;
    }
    lsp::dsp::fill_zero(reinterpret_cast<float *>(ptr), to_alloc / sizeof(float));

    hd->vSc[0]      = lsp::advance_ptr_bytes<float>(ptr, MBC_BUFFER_SIZE * sizeof(float));
    hd->vSc[1]      = lsp::advance_ptr_bytes<float>(ptr, MBC_BUFFER_SIZE * sizeof(float));
    hd->vBuffer     = lsp::advance_ptr_bytes<float>(ptr, MBC_BUFFER_SIZE * sizeof(float));
    hd->vEnv        = lsp::advance_ptr_bytes<float>(ptr, MBC_BUFFER_SIZE * sizeof(float));

    // Initialize filters according to number of bands
    //   real: if (sFilters.init(BANDS_MAX * channels) != STATUS_OK) return;
    hd->sFilters.init(BANDS_MAX * channels);
    size_t filter_cid = 0;

    // Initialize channels
    for (size_t i=0; i<channels; ++i)
    {
        channel_t *c    = &hd->vChannels[i];

        c->sBypass.construct();
        c->sEnvBoost[0].construct();
        c->sEnvBoost[1].construct();
        c->sEnvBoost[2].construct();
        c->sDelay.construct();
        c->sDryDelay.construct();
        c->sXOverDelay.construct();
        c->sDryEq.construct();
        c->sFFTXOver.construct();

        c->sEnvBoost[0].init(NULL);
        c->sEnvBoost[1].init(NULL);
        c->sEnvBoost[2].init(NULL);

        c->sDryEq.init(BANDS_MAX-1, 0);
        c->sDryEq.set_mode(lsp::dspu::EQM_IIR);

        c->nPlanSize    = 0;
        c->vIn          = NULL;
        c->vOut         = NULL;
        c->vScIn        = NULL;

        c->vInAnalyze   = lsp::advance_ptr_bytes<float>(ptr, MBC_BUFFER_SIZE * sizeof(float));
        c->vInBuffer    = lsp::advance_ptr_bytes<float>(ptr, MBC_BUFFER_SIZE * sizeof(float));
        c->vBuffer      = lsp::advance_ptr_bytes<float>(ptr, MBC_BUFFER_SIZE * sizeof(float));
        // real: c->vScBuffer = (bSidechain) ? ... : NULL;
        c->vExtScBuffer = lsp::advance_ptr_bytes<float>(ptr, MBC_BUFFER_SIZE * sizeof(float));
        // real: c->vShmBuffer / c->vTr / c->vTrMem — link + mesh, dropped

        // Initialize bands
        for (size_t j=0; j<BANDS_MAX; ++j)
        {
            comp_band_t *b  = &c->vBands[j];

            b->sSC.init(channels, REACTIVITY_MAX);
            b->sPassFilter.init(NULL);
            b->sRejFilter.init(NULL);
            b->sAllFilter.init(NULL);

            // Initialize sidechain equalizers
            b->sEQ[0].init(2, 6);
            b->sEQ[0].set_mode(lsp::dspu::EQM_IIR);
            b->sEQ[1].init(2, 6);
            b->sEQ[1].set_mode(lsp::dspu::EQM_IIR);

            b->vBuffer      = lsp::advance_ptr_bytes<float>(ptr, MBC_BUFFER_SIZE * sizeof(float));
            b->vVCA         = lsp::advance_ptr_bytes<float>(ptr, MBC_BUFFER_SIZE * sizeof(float));
            // real: b->vSc / b->vTr — mesh transfer functions, dropped

            b->fScPreamp    = GAIN_AMP_0_DB;

            b->fFreqStart   = 0.0f;
            b->fFreqEnd     = 0.0f;

            b->fFreqHCF     = 0.0f;
            b->fFreqLCF     = 0.0f;
            b->fMakeup      = GAIN_AMP_0_DB;
            // real: b->fGainLevel = GAIN_AMP_0_DB; — mesh, dropped
            b->bEnabled     = j < BANDS_DFL;
            b->bCustHCF     = false;
            b->bCustLCF     = false;
            b->bMute        = false;
            b->bSolo        = false;
            b->nScType      = SCT_INTERNAL;
            // real: b->nSync = S_ALL; — mesh, dropped
            b->nFilterID    = filter_cid++;
        }

        // Initialize split
        for (size_t j=0; j<BANDS_MAX-1; ++j)
        {
            split_t *s      = &c->vSplit[j];

            s->bEnabled     = false;
            s->fFreq        = 0.0f;
        }
    }

    // real: port binding (BIND_PORT/SKIP_PORT, mb_compressor.cpp:532-735) and
    // the vCurve log grid (:737-740) — host/UI, dropped.

    // real update_sample_rate(sr) (mb_compressor.cpp:1285-1352), run once
    // here since JamesDSP creates one handle per fixed sample rate
    // (sAnalyzer/sCounter dropped, see banner).
    {
        const size_t sr         = (size_t)sample_rate;
        const size_t fft_rank   = select_fft_rank(sr);
        const size_t bins       = 1 << fft_rank;
        const size_t max_delay  = bins + lsp::dspu::millis_to_samples(sample_rate, LOOKAHEAD_MAX);

        hd->sFilters.set_sample_rate(sr);
        hd->bEnvUpdate          = true;

        for (size_t i=0; i<channels; ++i)
        {
            channel_t *c = &hd->vChannels[i];
            c->sBypass.init(sr);
            c->sDelay.init(max_delay);
            c->sDryDelay.init(max_delay);
            c->sXOverDelay.init(max_delay);
            c->sDryEq.set_sample_rate(sr);

            // Need to re-initialize FFT crossover?  (always, first call)
            if (fft_rank != c->sFFTXOver.rank())
            {
                c->sFFTXOver.init(fft_rank, BANDS_MAX);
                for (size_t j=0; j<BANDS_MAX; ++j)
                    c->sFFTXOver.set_handler(j, process_band, hd, c);
                c->sFFTXOver.set_rank(fft_rank);
                c->sFFTXOver.set_phase(float(i) / float(channels));
            }
            c->sFFTXOver.set_sample_rate(sr);

            // Update bands
            for (size_t j=0; j<BANDS_MAX; ++j)
            {
                comp_band_t *b  = &c->vBands[j];

                b->sSC.set_sample_rate(sr);
                b->sComp.set_sample_rate(sr);
                b->sScDelay.init(max_delay);

                b->sPassFilter.set_sample_rate(sr);
                b->sRejFilter.set_sample_rate(sr);
                b->sAllFilter.set_sample_rate(sr);

                b->sEQ[0].set_sample_rate(sr);
                b->sEQ[1].set_sample_rate(sr);
            }

            c->nPlanSize        = 0; // Force to rebuild plan

            // Starts fully bypassed (silent-passthrough disabled state) —
            // matches JamesDSP's zero-init "effect off until configured+
            // enabled" convention for this module (same as the other LSP ports).
            c->sBypass.set_bypass(true);
        }
    }

    // The real host calls update_settings() once before the first process();
    // do the same here with the real defaults so the plan/filters exist
    // before any sample is processed (never run with nPlanSize == 0).
    lsp_mbcomp_default_config(hd->cfg.v);
    hd->rt.publish(hd->cfg);        // so peek()/get_config() read the defaults too
    { MbcHandle::Cfg c; hd->rt.take(c); mbcomp_apply(hd, c); }
    hd->bIdleSkipped = false;
    return hd;
}

void lsp_mbcomp_destroy(void *h) {
    if (!h) return;
    auto *hd = (MbcHandle *)h;
    // real do_destroy() (mb_compressor.cpp:235-292), minus analyzer/display.
    for (size_t i=0; i<2; ++i)
    {
        channel_t *c    = &hd->vChannels[i];

        c->sEnvBoost[0].destroy();
        c->sEnvBoost[1].destroy();
        c->sEnvBoost[2].destroy();
        c->sDelay.destroy();
        c->sDryDelay.destroy();
        c->sXOverDelay.destroy();
        c->sDryEq.destroy();
        c->sFFTXOver.destroy();

        c->vBuffer      = NULL;

        for (size_t j=0; j<BANDS_MAX; ++j)
        {
            comp_band_t *b  = &c->vBands[j];

            b->sEQ[0].destroy();
            b->sEQ[1].destroy();
            b->sSC.destroy();
            b->sScDelay.destroy();

            b->sPassFilter.destroy();
            b->sRejFilter.destroy();
            b->sAllFilter.destroy();
        }
    }

    // Destroy dynamic filters
    hd->sFilters.destroy();

    // Destroy data
    if (hd->pData != NULL)
        lsp::free_aligned(hd->pData);

    delete hd;
}

// Faithful port of mb_compressor::update_settings() (mb_compressor.cpp:778).
// Audio-thread half of configure(): runs from lsp_mbcomp_process() when a
// new config was published (rt_config.h handoff), and once from create().
static void mbcomp_apply(MbcHandle *hd, const MbcHandle::Cfg &cfg)
{
    hd->cfg = cfg;
    Ports P;
    decode_ports(cfg.v, P);

    lsp::dspu::filter_params_t fp;

    // real: update_premix(); — pinned -inf dB, omitted (see banner)

    // Determine number of channels
    const size_t channels   = 2;
    // real: int active_channels = 0; — analyzer, dropped
    size_t env_boost        = (size_t)P.envb;

    // Determine work mode: classic, modern or linear phase
    xover_mode_t xover      = xover_mode_t(P.mode);
    if (xover != hd->enXOver)
    {
        hd->enXOver             = xover;
        for (size_t i=0; i<channels; ++i)
        {
            hd->vChannels[i].nPlanSize      = 0;
            hd->vChannels[i].sXOverDelay.clear();
        }
    }
    hd->bStereoSplit        = P.ssplit;         // real: (pStereoSplit != NULL) ? pStereoSplit->value() >= 0.5f : false

    // Store gain
    const float out_gain    = P.g_out;
    const float drywet      = P.drywet * 0.01f;
    const float dry_gain    = P.g_dry;
    const float wet_gain    = P.g_wet;

    hd->fInGain             = P.g_in;
    hd->fDryGain            = (dry_gain * drywet + 1.0f - drywet) * out_gain;
    hd->fWetGain            = wet_gain * drywet * out_gain;
    // real: fZoom = pZoom->value(); — UI, dropped
    // real: bUseShmLink = false; — permanently false here

    // Configure channels
    for (size_t i=0; i<channels; ++i)
    {
        channel_t *c    = &hd->vChannels[i];

        // real: c->sBypass.set_bypass(pBypass->value()); — made in lsp_mbcomp_process()

        // Update frequency split bands
        for (size_t j=0; j<BANDS_MAX-1; ++j)
        {
            split_t *s      = &c->vSplit[j];

            bool enabled    = s->bEnabled;
            s->bEnabled     = P.split[j].cbe;
            if (enabled != s->bEnabled)
                c->nPlanSize    = 0;

            float v         = s->fFreq;
            s->fFreq        = P.split[j].sf;
            if (v != s->fFreq)
                c->nPlanSize    = 0;
        }

        // real: analyzer settings (bInFft/bOutFft, sAnalyzer.enable_channel(),
        // active_channels) — dropped

        // Update envelope boost filters
        if ((env_boost != hd->nEnvBoost) || (hd->bEnvUpdate))
        {
            fp.fFreq        = FREQ_BOOST_MIN;
            fp.fFreq2       = 0.0f;
            fp.fGain        = 1.0f;
            fp.fQuality     = 0.0f;

            switch (env_boost)
            {
                case FB_BT_3DB:
                    fp.nType        = lsp::dspu::FLT_BT_RLC_ENVELOPE;
                    fp.nSlope       = 1;
                    break;
                case FB_MT_3DB:
                    fp.nType        = lsp::dspu::FLT_MT_RLC_ENVELOPE;
                    fp.nSlope       = 1;
                    break;
                case FB_BT_6DB:
                    fp.nType        = lsp::dspu::FLT_BT_RLC_ENVELOPE;
                    fp.nSlope       = 2;
                    break;
                case FB_MT_6DB:
                    fp.nType        = lsp::dspu::FLT_MT_RLC_ENVELOPE;
                    fp.nSlope       = 2;
                    break;
                case FB_OFF:
                default:
                    fp.nType        = lsp::dspu::FLT_NONE;
                    fp.nSlope       = 1;
                    break;
            }

            c->sEnvBoost[0].update(hd->fSampleRate, &fp);
            c->sEnvBoost[1].update(hd->fSampleRate, &fp);
            c->sEnvBoost[2].update(hd->fSampleRate, &fp);
        }
    }

    // real: sAnalyzer.set_reactivity(pReactivity->value()); sAnalyzer.set_shift(
    //   pShiftGain->value() * 100.0f); set_activity(); reconfigure();
    //   get_frequencies() — the ONLY consumers of "react"/"shift", dropped
    //   with the analyzer (P.react / P.shift are stored, not used).

    size_t latency = 0;
    bool solo_on = false;

    // Configure channels
    for (size_t i=0; i<channels; ++i)
    {
        channel_t *c    = &hd->vChannels[i];

        // Update compressor bands
        for (size_t j=0; j<BANDS_MAX; ++j)
        {
            comp_band_t *b  = &c->vBands[j];
            const BandPorts &B = P.band[j];     // stereo: channel 1's ports are aliased to channel 0's

            float attack    = B.al;
            float release   = B.rrl * attack;
            float makeup    = B.mk;
            lsp::dspu::compressor_mode_t mode = decode_mode(B.cm);
            bool enabled    = B.ce;
            if (enabled && (j > 0))
                enabled         = c->vSplit[j-1].bEnabled;
            bool cust_lcf   = B.sclc;
            bool cust_hcf   = B.schc;
            float sc_gain   = B.scp;
            bool mute       = B.bm;
            bool solo       = (enabled) && (B.bs);
            size_t sc_src   = (hd->bStereoSplit) ? size_t(B.sscs) : size_t(B.scs);   // real: (bStereoSplit) ? pScSpSource : pScSource

            // real: b->pRelLevelOut->set_value(release); — meter, dropped

            b->nScType      = decode_sidechain_type();
            // real: if (b->nScType == SCT_LINK) bUseShmLink = true; — unreachable

            b->sSC.set_mode((size_t)B.scm);
            b->sSC.set_reactivity(B.scr);
            b->sSC.set_stereo_mode(lsp::dspu::SCSM_STEREO);     // real: (nMode == MBCM_MS) ? SCSM_MIDSIDE : SCSM_STEREO
            b->sSC.set_source(decode_sidechain_source((int)sc_src, hd->bStereoSplit, i));

            if (sc_gain != b->fScPreamp)
            {
                b->fScPreamp    = sc_gain;
                // real: b->nSync |= S_EQ_CURVE; — mesh, dropped
            }

            b->sComp.set_mode(mode);
            b->sComp.set_threshold(attack, release);
            b->sComp.set_timings(B.at, B.rt);
            b->sComp.set_hold(B.ht);
            b->sComp.set_ratio(B.cr);
            b->sComp.set_knee(B.kn);
            b->sComp.set_boost_threshold((mode != lsp::dspu::CM_BOOSTING) ? B.bth : B.bsa);

            if (b->sComp.modified())
            {
                b->sComp.update_settings();
                // real: b->nSync |= S_COMP_CURVE;
            }
            if (b->fMakeup != makeup)
            {
                b->fMakeup      = makeup;
            }
            if (b->bEnabled != enabled)
            {
                b->bEnabled     = enabled;
                if (!enabled)
                    b->sScDelay.clear(); // Clear delay buffer from artifacts
            }
            if (b->bSolo != solo)
            {
                b->bSolo        = solo;
            }
            if (b->bMute != mute)
            {
                b->bMute        = mute;
            }
            if (b->bCustLCF != cust_lcf)
            {
                b->bCustLCF     = cust_lcf;
                c->nPlanSize    = 0;
            }
            if (b->bCustHCF != cust_hcf)
            {
                b->bCustHCF     = cust_hcf;
                c->nPlanSize    = 0;
            }
            if (cust_lcf)
            {
                float lcf       = B.sclf;
                if (lcf != b->fFreqLCF)
                {
                    b->fFreqLCF     = lcf;
                    c->nPlanSize    = 0;
                }
            }
            if (cust_hcf)
            {
                float hcf       = B.schf;
                if (hcf != b->fFreqHCF)
                {
                    b->fFreqHCF     = hcf;
                    c->nPlanSize    = 0;
                }
            }

            if (b->bSolo)
                solo_on         = true;

            // Estimate lookahead buffer size
            b->nLookahead   = lsp::dspu::millis_to_samples(hd->fSampleRate, B.sla);
        }
    }

    for (size_t i=0; i<channels; ++i)
    {
        channel_t *c    = &hd->vChannels[i];

        // Check muting option
        for (size_t j=0; j<BANDS_MAX; ++j)
        {
            comp_band_t *b      = &c->vBands[j];
            if ((!b->bMute) && (solo_on))
                b->bMute    = !b->bSolo;
        }

        // Rebuild compression plan
        if (c->nPlanSize <= 0)
        {
            c->nPlanSize                = 0;
            c->vBands[0].fFreqStart     = 0;
            c->vPlan[c->nPlanSize++]    = &c->vBands[0];

            for (size_t j=0; j<BANDS_MAX-1; ++j)
            {
                comp_band_t *b      = &c->vBands[j+1];
                b->fFreqStart       = c->vSplit[j].fFreq;

                if (c->vSplit[j].bEnabled)
                    c->vPlan[c->nPlanSize++]    = b;
            }

            // Do simple sort of PLAN items by frequency
            if (c->nPlanSize > 1)
            {
                // Sort in ascending order
                for (size_t si=0; si < c->nPlanSize-1; ++si)
                    for (size_t sj=si+1; sj < c->nPlanSize; ++sj)
                        if (compare_bands_for_sort(c->vPlan[si], c->vPlan[sj]))
                        {
                            comp_band_t *tmp = c->vPlan[si];
                            c->vPlan[si]    = c->vPlan[sj];
                            c->vPlan[sj]    = tmp;
                        }

                for (size_t j=1; j<c->nPlanSize; ++j)
                    c->vPlan[j-1]->fFreqEnd     = c->vPlan[j]->fFreqStart;
            }
            c->vPlan[c->nPlanSize-1]->fFreqEnd       = (size_t(hd->fSampleRate) >> 1);   // real: (fSampleRate >> 1), fSampleRate is integral

            // Configure equalizers
            for (size_t j=0; j<c->nPlanSize; ++j)
            {
                comp_band_t *b  = c->vPlan[j];
                size_t band     = b - c->vBands;
                const BandPorts &B = P.band[band];
                // real: b->pFreqEnd->set_value(b->fFreqEnd); b->nSync |= S_EQ_CURVE | S_BAND_CURVE; — meter/mesh

                // Configure equalizer for the sidechain
                for (size_t k=0; k<channels; ++k)
                {
                    // Configure lo-pass filter
                    fp.nType        = ((j != (c->nPlanSize-1)) || (b->bCustHCF)) ? lsp::dspu::FLT_BT_LRX_LOPASS : lsp::dspu::FLT_NONE;
                    fp.fFreq        = (b->bCustHCF) ? B.schf : b->fFreqEnd;   // real: pScHcfFreq->value() : pFreqEnd->value() (just set to fFreqEnd)
                    fp.fFreq2       = fp.fFreq;
                    fp.fQuality     = 0.0f;
                    fp.fGain        = 1.0f;
                    fp.fQuality     = 0.0f;
                    fp.nSlope       = 2;

                    b->sEQ[k].set_params(0, &fp);

                    // Configure hi-pass filter
                    fp.nType        = ((j != 0) || (b->bCustLCF)) ? lsp::dspu::FLT_BT_LRX_HIPASS : lsp::dspu::FLT_NONE;
                    fp.fFreq        = (b->bCustLCF) ? B.sclf : b->fFreqStart;
                    fp.fFreq2       = fp.fFreq;
                    fp.fQuality     = 0.0f;
                    fp.fGain        = 1.0f;
                    fp.fQuality     = 0.0f;
                    fp.nSlope       = 2;

                    b->sEQ[k].set_params(1, &fp);
                }

                // real: b->sEQ[0].freq_chart(b->vSc, vFreqs, FFT_MESH_POINTS);
                //       dsp::pcomplex_mod(b->vSc, b->vSc, FFT_MESH_POINTS); — mesh, dropped

                // Update filter parameters, depending on operating mode
                if (hd->enXOver == XOVER_MODERN)
                {
                    // Configure filter for band
                    if (j <= 0)
                    {
                        fp.nType        = (c->nPlanSize > 1) ? lsp::dspu::FLT_BT_LRX_LOSHELF : lsp::dspu::FLT_BT_AMPLIFIER;
                        fp.fFreq        = b->fFreqEnd;
                        fp.fFreq2       = b->fFreqEnd;
                    }
                    else if (j >= (c->nPlanSize - 1))
                    {
                        fp.nType        = lsp::dspu::FLT_BT_LRX_HISHELF;
                        fp.fFreq        = b->fFreqStart;
                        fp.fFreq2       = b->fFreqStart;
                    }
                    else
                    {
                        fp.nType        = lsp::dspu::FLT_BT_LRX_LADDERPASS;
                        fp.fFreq        = b->fFreqStart;
                        fp.fFreq2       = b->fFreqEnd;
                    }

                    fp.fGain        = 1.0f;
                    fp.nSlope       = 2;
                    fp.fQuality     = 0.0;

                    hd->sFilters.set_params(b->nFilterID, &fp);
                }
                else if (hd->enXOver == XOVER_CLASSIC)
                {
                    fp.fGain        = 1.0f;
                    fp.nSlope       = 2;
                    fp.fQuality     = 0.0;
                    fp.fFreq        = b->fFreqEnd;
                    fp.fFreq2       = b->fFreqEnd;

                    // We're going from low frequencies to high frequencies
                    if (j >= (c->nPlanSize - 1))
                    {
                        fp.nType    = lsp::dspu::FLT_NONE;
                        b->sPassFilter.update(hd->fSampleRate, &fp);
                        b->sRejFilter.update(hd->fSampleRate, &fp);
                        b->sAllFilter.update(hd->fSampleRate, &fp);
                    }
                    else
                    {
                        fp.nType    = lsp::dspu::FLT_BT_LRX_LOPASS;
                        b->sPassFilter.update(hd->fSampleRate, &fp);
                        fp.nType    = lsp::dspu::FLT_BT_LRX_HIPASS;
                        b->sRejFilter.update(hd->fSampleRate, &fp);
                        fp.nType    = (j == 0) ? lsp::dspu::FLT_NONE : lsp::dspu::FLT_BT_LRX_ALLPASS;
                        b->sAllFilter.update(hd->fSampleRate, &fp);
                    }
                }
                else // enXOver == XOVER_LINEAR_PHASE
                {
                    if (j > 0)
                    {
                        c->sFFTXOver.enable_hpf(band, true);
                        c->sFFTXOver.set_hpf_frequency(band, b->fFreqStart);
                        c->sFFTXOver.set_hpf_slope(band, -48.0f);
                    }
                    else
                        c->sFFTXOver.disable_hpf(band);

                    if (j < (c->nPlanSize-1))
                    {
                        c->sFFTXOver.enable_lpf(band, true);
                        c->sFFTXOver.set_lpf_frequency(band, b->fFreqEnd);
                        c->sFFTXOver.set_lpf_slope(band, -48.0f);
                    }
                    else
                        c->sFFTXOver.disable_lpf(band);
                }
            }
        } // nPlanSize

        // Enable/disable dynamic filters and bands
        for (size_t j=0; j<BANDS_MAX; ++j)
        {
            comp_band_t *b  = &c->vBands[j];
            size_t band     = b - c->vBands;
            hd->sFilters.set_filter_active(b->nFilterID, b->bEnabled);
            c->sFFTXOver.enable_band(j, (band > 0) ? c->vSplit[band-1].bEnabled : true);
        }

        // Set-up all-pass filters for the 'dry' chain which can be mixed with the 'wet' chain.
        for (size_t j=0; j<BANDS_MAX-1; ++j)
        {
            comp_band_t *b  = (j < (c->nPlanSize-1)) ? c->vPlan[j] : NULL;
            fp.nType        = (b != NULL) ? lsp::dspu::FLT_BT_LRX_ALLPASS : lsp::dspu::FLT_NONE;
            fp.fFreq        = (b != NULL) ? b->fFreqEnd : 0.0f;
            fp.fFreq2       = fp.fFreq;
            fp.fQuality     = 0.0f;
            fp.fGain        = 1.0f;
            fp.fQuality     = 0.0f;
            fp.nSlope       = 2;

            c->sDryEq.set_params(j, &fp);
        }

        // Calculate latency
        for (size_t j=0; j<c->nPlanSize; ++j)
        {
            comp_band_t *b  = c->vPlan[j];
            latency         = lsp::lsp_max(latency, size_t(b->nLookahead));
        }
    }

    // Update latency
    size_t xover_latency = (hd->enXOver == XOVER_LINEAR_PHASE) ? hd->vChannels[0].sFFTXOver.latency() : 0;

    hd->nLatency = latency + xover_latency;      // real: set_latency(latency + xover_latency);
    for (size_t i=0; i<channels; ++i)
    {
        channel_t *c    = &hd->vChannels[i];

        // Update latency
        for (size_t j=0; j<c->nPlanSize; ++j)
        {
            comp_band_t *b  = c->vPlan[j];
            b->sScDelay.set_delay(latency + xover_latency - b->nLookahead);
        }
        c->sDelay.set_delay(latency);
        c->sDryDelay.set_delay(latency + xover_latency);
        // real: sAnalyzer.set_channel_delay(c->nAnInChannel, xover_latency); — analyzer, dropped
        c->sXOverDelay.set_delay(xover_latency);
    }

    hd->nEnvBoost   = env_boost;
    hd->bEnvUpdate  = false;
}

void lsp_mbcomp_configure(void *h, const float *params, unsigned count) {
    if (!h || !params || count < LSP_MBCOMP_NPARAMS) return;
    auto *hd = (MbcHandle *)h;
    // Sanitise + publish only (control thread). Range clamping happens in
    // decode_ports() on the audio thread so the applied values are the single
    // source of truth; here just make sure nothing non-finite is published
    // that peek() would hand back to a Lua get->modify->set loop.
    MbcHandle::Cfg c;
    float dfl[LSP_MBCOMP_NPARAMS];
    lsp_mbcomp_default_config(dfl);
    for (size_t i = 0; i < LSP_MBCOMP_NPARAMS; ++i)
        c.v[i] = (params[i] == params[i]) ? params[i] : dfl[i];
    hd->rt.publish(c);   // applied by mbcomp_apply() on the audio thread's next block
}

// Port-side reset used when coming back from the settled-off skip (see the
// header's contract note): the real plugin's mode-switch reset (nPlanSize =
// 0, sXOverDelay.clear()) plus every delay line / filter memory holding
// audio from before the skip.
static void mbcomp_idle_reset(MbcHandle *hd)
{
    lsp::dspu::filter_params_t none;
    none.nType = lsp::dspu::FLT_NONE; none.nSlope = 1; none.fFreq = 0.0f; none.fFreq2 = 0.0f;
    none.fGain = 1.0f; none.fQuality = 0.0f;
    for (size_t i=0; i<2; ++i)
    {
        channel_t *c = &hd->vChannels[i];
        c->nPlanSize = 0;                   // real mode-switch reset (plan + filters rebuilt by mbcomp_apply)
        c->sXOverDelay.clear();             // real mode-switch reset
        c->sDelay.clear();
        c->sDryDelay.clear();
        c->sFFTXOver.clear();
        c->sDryEq.reset();
        for (size_t k=0; k<3; ++k)
            c->sEnvBoost[k].clear();
        for (size_t j=0; j<BANDS_MAX; ++j)
        {
            comp_band_t *b = &c->vBands[j];
            b->sScDelay.clear();
            b->sSC.clear();
            b->sEQ[0].reset();
            b->sEQ[1].reset();
            b->sPassFilter.clear();
            b->sRejFilter.clear();
            b->sAllFilter.clear();
            // DynamicFilters only clears its biquad memory on a filter TYPE
            // change (set_params(): nType differs -> bClearMem). Re-arm with
            // FLT_NONE so the plan rebuild's set_params() triggers that clear.
            hd->sFilters.set_params(b->nFilterID, &none);
        }
    }
    mbcomp_apply(hd, hd->cfg);
}

void lsp_mbcomp_process(void *h, float *left, float *right, unsigned n, int enabled) {
    if (!h || n == 0) return;
    auto *hd = (MbcHandle *)h;
    {
        MbcHandle::Cfg c;
        if (hd->rt.take(c))
            mbcomp_apply(hd, c);
    }
    channel_t &c0 = hd->vChannels[0];
    channel_t &c1 = hd->vChannels[1];

    // real: c->sBypass.set_bypass(pBypass->value()) inside update_settings();
    // see mbcomp_apply()'s comment on why this lives here instead.
    const bool bypass_req = !enabled;
    c0.sBypass.set_bypass(bypass_req);
    c1.sBypass.set_bypass(bypass_req);

    // Documented zero-cost contract (lsp_wrap_mbcomp.h): once both channels'
    // Bypass have fully settled off, skip processing entirely rather than
    // running the crossover/sidechain/compressor path on an inaudible wet
    // signal.
    if (c0.sBypass.on() && c1.sBypass.on()) {
        hd->bIdleSkipped = true;
        return;
    }
    if (hd->bIdleSkipped) {
        mbcomp_idle_reset(hd);
        hd->bIdleSkipped = false;
    }

    // real process() (mb_compressor.cpp:1615-1632): bind input signal —
    // sPremix.vIn/vOut = port buffers, sPremix.vSc = vIn (pScIn == NULL),
    // sPremix.vLink = NULL (no shared-memory link).
    float *premix_vIn[2]  = { left, right };
    float *premix_vOut[2] = { left, right };     // JamesDSP processes in place

    const size_t samples  = n;
    const size_t channels = 2;

    // Do processing
    for (size_t offset = 0; offset < samples; )
    {
        // Determine buffer size for processing
        const size_t to_process = lsp::lsp_min(MBC_BUFFER_SIZE, samples - offset);

        // Premix and measure input signal level
        for (size_t i=0; i<channels; ++i)
        {
            channel_t *c        = &hd->vChannels[i];

            // real: premix_channel(i, to_process) with bSidechain == false,
            // link == NULL and every premix coefficient at -inf dB: bind the
            // pointers and advance them (the fmadd/mul paths are all skipped).
            c->vIn              = premix_vIn[i];
            c->vOut             = premix_vOut[i];
            c->vScIn            = premix_vIn[i];        // sPremix.vSc[i] == sPremix.vIn[i]
            premix_vIn[i]      += to_process;
            premix_vOut[i]     += to_process;
            // real: const float level = dsp::abs_max(c->vIn, to_process) * fInGain;
            //       c->pInLvl->set_value(level); — meter, dropped
        }

        // Pre-process channel data
        preprocess_channel_input(hd, to_process);

        // MAIN PLUGIN STUFF
        for (size_t i=0; i<channels; ++i)
        {
            channel_t *c        = &hd->vChannels[i];

            for (size_t j=0; j<c->nPlanSize; ++j)
            {
                comp_band_t *b      = c->vPlan[j];

                // Prepare sidechain signal with band equalizers
                b->sEQ[0].process(hd->vSc[0], select_buffer(b, &hd->vChannels[0]), to_process);
                b->sEQ[1].process(hd->vSc[1], select_buffer(b, &hd->vChannels[1]), to_process);

                // Preprocess VCA signal
                b->sSC.process(hd->vBuffer, const_cast<const float **>(hd->vSc), to_process); // Band now contains processed by sidechain signal
                b->sScDelay.process(hd->vBuffer, hd->vBuffer, b->fScPreamp, to_process); // Apply sidechain preamp and lookahead delay

                if (b->bEnabled)
                {
                    b->sComp.process(b->vVCA, hd->vEnv, hd->vBuffer, to_process); // Output
                    lsp::dsp::mul_k2(b->vVCA, b->fMakeup, to_process); // Apply makeup gain

                    // real: "Output curve level" abs_max(vEnv) -> pEnvLvl/pMeterGain/
                    //       pCurveLvl, and b->fGainLevel = b->vVCA[to_process-1]
                    //       — meters/mesh, dropped

                    // Check muting option
                    if (b->bMute)
                        lsp::dsp::fill(b->vVCA, GAIN_AMP_M_36_DB, to_process);
                }
                else
                {
                    lsp::dsp::fill(b->vVCA, (b->bMute) ? GAIN_AMP_M_36_DB : GAIN_AMP_0_DB, to_process);
                    // real: b->fGainLevel = GAIN_AMP_0_DB; — mesh, dropped
                }
            }

            // real: "Output curve parameters for disabled compressors" — meters, dropped
        }

        // Here, we apply VCA to input signal dependent on the input
        if (hd->enXOver == XOVER_MODERN) // 'Modern' mode
        {
            // Apply VCA control
            for (size_t i=0; i<channels; ++i)
            {
                channel_t *c        = &hd->vChannels[i];
                c->sDelay.process(c->vInBuffer, c->vInAnalyze, to_process); // Apply delay to compensate lookahead feature

                // Process first band
                comp_band_t *b      = c->vPlan[0];
                hd->sFilters.process(b->nFilterID, c->vBuffer, c->vInBuffer, b->vVCA, to_process);

                // Process other bands
                for (size_t j=1; j<c->nPlanSize; ++j)
                {
                    b                   = c->vPlan[j];
                    hd->sFilters.process(b->nFilterID, c->vBuffer, c->vBuffer, b->vVCA, to_process);
                }
            }
        }
        else if (hd->enXOver == XOVER_CLASSIC) // 'Classic' mode
        {
            // Apply VCA control
            for (size_t i=0; i<channels; ++i)
            {
                channel_t *c        = &hd->vChannels[i];

                // Originally, there is no signal
                c->sDelay.process(c->vInBuffer, c->vInAnalyze, to_process); // Apply delay to compensate lookahead feature, store into vBuffer

                // First step
                comp_band_t *b      = c->vPlan[0];
                // Filter frequencies from input
                b->sPassFilter.process(hd->vEnv, c->vInBuffer, to_process);
                // Apply VCA gain and add to the channel buffer
                lsp::dsp::mul3(c->vBuffer, hd->vEnv, b->vVCA, to_process);
                // Filter frequencies from input
                b->sRejFilter.process(hd->vBuffer, c->vInBuffer, to_process);

                // All other steps
                for (size_t j=1; j<c->nPlanSize; ++j)
                {
                    b                   = c->vPlan[j];
                    // Process the signal with all-pass
                    b->sAllFilter.process(c->vBuffer, c->vBuffer, to_process);
                    // Filter frequencies from input
                    b->sPassFilter.process(hd->vEnv, hd->vBuffer, to_process);
                    // Apply VCA gain and add to the channel buffer
                    lsp::dsp::fmadd3(c->vBuffer, hd->vEnv, b->vVCA, to_process);
                    // Filter frequencies from input
                    b->sRejFilter.process(hd->vBuffer, hd->vBuffer, to_process);
                }
            }
        }
        else // enXOver == XOVER_LINEAR_PHASE
        {
            // Apply VCA control
            for (size_t i=0; i<channels; ++i)
            {
                channel_t *c        = &hd->vChannels[i];

                // Apply delay to compensate lookahead feature
                c->sDelay.process(c->vBuffer, c->vInAnalyze, to_process);
                // Apply delay to unprocessed signal to compensate lookahead + crossover delay
                c->sXOverDelay.process(c->vInBuffer, c->vBuffer, to_process);
                c->sFFTXOver.process(c->vBuffer, to_process);

                // First step
                comp_band_t *b      = c->vPlan[0];
                lsp::dsp::mul3(c->vBuffer, b->vVCA, b->vBuffer, to_process);

                // All other steps
                for (size_t j=1; j<c->nPlanSize; ++j)
                {
                    b                   = c->vPlan[j];
                    lsp::dsp::fmadd3(c->vBuffer, b->vVCA, b->vBuffer, to_process);
                }
            }
        }

        // MAIN PLUGIN STUFF END

        // real: "Do input and output channel analysis" sAnalyzer block — dropped
        // real: "Post-process data" ms_to_lr — nMode != MBCM_MS, not taken

        // Final metering
        for (size_t i=0; i<channels; ++i)
        {
            channel_t *c        = &hd->vChannels[i];

            // Apply dry/wet balance
            if (hd->enXOver == XOVER_MODERN)
                lsp::dsp::mix2(c->vBuffer, c->vInBuffer, hd->fWetGain, hd->fDryGain, to_process);
            else if (hd->enXOver == XOVER_CLASSIC)
            {
                c->sDryEq.process(hd->vBuffer, c->vInBuffer, to_process);
                lsp::dsp::mix2(c->vBuffer, hd->vBuffer, hd->fWetGain, hd->fDryGain, to_process);
            }
            else // enXOver == XOVER_LINEAR_PHASE
                lsp::dsp::mix2(c->vBuffer, c->vInBuffer, hd->fWetGain, hd->fDryGain, to_process);

            // real: float level = dsp::abs_max(c->vBuffer, to_process); c->pOutLvl->set_value(level); — meter, dropped

            // Apply bypass
            c->sDryDelay.process(hd->vBuffer, c->vIn, to_process);
            c->sBypass.process(c->vOut, hd->vBuffer, c->vBuffer, to_process);
        }
        offset     += to_process;
    }

    // real: sCounter.submit(); transfer-function / mesh / FFT-curve output
    // for each channel and band; query_display_draw(); sCounter.commit()
    // — UI, dropped.
}

int lsp_mbcomp_get_latency(void *h) {
    if (!h) return 0;
    return (int)((MbcHandle *)h)->nLatency;
}

void lsp_mbcomp_get_config(void *h, float *params) {
    if (!params) return;
    if (!h) {
        lsp_mbcomp_default_config(params);
        return;
    }
    auto *hd = (MbcHandle *)h;
    const MbcHandle::Cfg c = hd->rt.peek();   // last published, not last applied
    memcpy(params, c.v, sizeof(c.v));
}
