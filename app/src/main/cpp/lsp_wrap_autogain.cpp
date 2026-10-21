// lsp_wrap_autogain.cpp — faithful port of the real LSP Autogain plugin.
//
// Source of truth: lsp-autogain/src/main/plug/autogain.cpp (pinned release
// 1.0.19, commit 0181e17, 2026-06-13)
//   (class lsp::plugins::autogain — init() @145, update_sample_rate() @352,
//    calc_gain_speed() @430, update_settings() @442, process() @547,
//    measure_input_loudness() @622, compute_gain_correction() @700,
//    apply_gain_correction() @732, update_audio_buffers() @755)
// and its header lsp-autogain/include/private/plugins/autogain.h
//   (struct autogain::channel_t, gcontrol_t), metadata ranges/defaults in
//   lsp-autogain/include/private/meta/autogain.h and port symbols in
//   lsp-autogain/src/main/meta/autogain.cpp.
//
// Per LSP_FAITHFUL_PORT_SCOPE.md, this is a TRANSLITERATION of that real
// plugin's DSP logic into JamesDSP's calling convention, not a fresh
// reimplementation around the bare lsp::dspu::AutoGain primitive (and it
// REPLACES the hand-rolled RMS integrator that used to be chain_modules.c's
// "autogain"). Structural template: lsp_wrap_expander.cpp.
//
// Scope: STEREO variant only (real meta::autogain_stereo -> nChannels == 2,
// bSidechain == false) and INTERNAL sidechain only (real enScMode pinned to
// meta::autogain::SCMODE_INTERNAL, the "scmode" port's SCMODE_DFL).
// Concretely, versus the real code:
//   - select_buffer() always returns c->vIn for SCMODE_INTERNAL (never the
//     external-sidechain vScIn or shared-memory-link vShmIn, so vEmptyBuffer
//     is unreachable) and is inlined away.
//   - measure_input_loudness()'s SCMODE_MATCH_* branch and
//     compute_gain_correction()'s SCMODE_MATCH_* branch are omitted: only
//     the `case SCMODE_INTERNAL` (default) branch is reachable.
//   - The SIDECHAIN meters sLScMeter/sSScMeter are KEPT: in SCMODE_INTERNAL
//     they are the gain path. The real measure_input_loudness() runs the
//     input meters into vLBuffer/vSBuffer (autogain.cpp:680/684) and then
//     runs the sidechain meters into the SAME buffers (autogain.cpp:689/693),
//     and compute_gain_correction() feeds sAutoGain from vLBuffer/vSBuffer
//     (autogain.cpp:719/722) — i.e. from the sidechain meters, fed by the
//     preamp-ramped vBuffer. The input meters' outputs only survive into
//     fLInGain/fSInGain (meters/graphs). Stripping the SC meters was tried
//     first and produced a measurable start-up deviation vs the real plugin
//     (first block's fOldPreamp 0->1 ramp is seen only by the SC meters).
//   - The OUTPUT meters sLOutMeter/sSOutMeter are stripped: they are
//     provably inert in this scope — apply_gain_correction() runs them into
//     vLBuffer/vSBuffer AFTER sAutoGain has consumed those buffers
//     (autogain.cpp:745/749), and the next chunk's measure_input_loudness()
//     rewrites both before anything reads them. Their only consumers are
//     pLOutGain/pSOutGain + sLOutGraph/sSOutGraph (UI meter/graph ports).
//   - sLInGraph/sSInGraph/sGainGraph & co (MeterGraph), vTimePoints,
//     vIDisplay, pIDisplay, output_meters(), output_mesh_data(),
//     inline_display(), dump() are UI/host plumbing and are stripped.
//   - Ports the real plugin exposes but lsp_autogain_configure() has no
//     field for (sidechain preamp/lookahead/mode, weighting, quick
//     amplifier, the four gain-speed numerator/time pairs) are pinned to
//     the REAL metadata defaults, each cited at its call site.
//
// Everything else — the long/short BS.1770 K-weighted input loudness
// meters feeding the real dspu::AutoGain level-normaliser, the BUFFER_SIZE
// block-chunked loop (measure -> compute -> apply -> bypass), the lookahead
// delay call, the preamp ramp, and the Bypass crossfade — is carried over
// in the real plugin's own order of operations.
#include "lsp_wrap_autogain.h"
#include "rt_config.h"

#include <lsp-plug.in/dsp-units/const.h>
#include <lsp-plug.in/dsp-units/units.h>
#include <lsp-plug.in/dsp-units/ctl/Bypass.h>
#include <lsp-plug.in/dsp-units/dynamics/AutoGain.h>
#include <lsp-plug.in/dsp-units/meters/LoudnessMeter.h>
#include <lsp-plug.in/dsp-units/misc/broadcast.h>
#include <lsp-plug.in/dsp-units/util/Delay.h>
#include <lsp-plug.in/dsp/dsp.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace lsp_jdsp_autogain {

// lsp::dsp::init() populates lsp-dsp-lib's dispatch table; same idempotent
// internal-linkage guard as the other LSP ports in this build.
static inline void ensure_dsp_init() {
    static bool done = (lsp::dsp::init(), true);
    (void)done;
}

// real: autogain.cpp:36 `static constexpr size_t BUFFER_SIZE = 0x400;`
static constexpr size_t BUFFER_SIZE = 0x400;

// real: autogain.cpp:39 "Gain numerators multiplied by 10"
//   static const uint8_t gain_numerators[] = { 1, 5, 10, 30, 60, 90, 100, 120, 150, 180, 200, 210, 240 };
static const uint8_t gain_numerators[] = { 1, 5, 10, 30, 60, 90, 100, 120, 150, 180, 200, 210, 240 };

// Real metadata ranges/defaults (lsp-autogain/include/private/meta/autogain.h,
// struct meta::autogain).
static constexpr float SC_PREAMP_DFL     = 0.0f;     // real: SC_PREAMP_DFL (dB)
static constexpr float SC_LOOKAHEAD_MAX  = 40.0f;    // real: SC_LOOKAHEAD_MAX (ms)
static constexpr float SC_LOOKAHEAD_DFL  = 0.0f;     // real: SC_LOOKAHEAD_DFL (ms)
static constexpr float LONG_PERIOD_MIN   = 100.0f;   // real: LONG_PERIOD_MIN (ms)
static constexpr float LONG_PERIOD_MAX   = 2000.0f;  // real: LONG_PERIOD_MAX (ms)
static constexpr float LONG_PERIOD_DFL   = 400.0f;   // real: LONG_PERIOD_DFL (ms)
static constexpr float SHORT_PERIOD_MIN  = 5.0f;     // real: SHORT_PERIOD_MIN (ms)
static constexpr float SHORT_PERIOD_MAX  = 100.0f;   // real: SHORT_PERIOD_MAX (ms)
static constexpr float SHORT_PERIOD_DFL  = 20.0f;    // real: SHORT_PERIOD_DFL (ms)
static constexpr float LONG_GROW_DFL     = 500.0f;   // real: LONG_GROW_DFL (ms)
static constexpr float LONG_FALL_DFL     = 500.0f;   // real: LONG_FALL_DFL (ms)
static constexpr float SHORT_GROW_DFL    = 40.0f;    // real: SHORT_GROW_DFL (ms)
static constexpr float SHORT_FALL_DFL    = 10.0f;    // real: SHORT_FALL_DFL (ms)
static constexpr float DEVIATION_MIN     = 0.0f;     // real: DEVIATION_MIN (dB)
static constexpr float DEVIATION_MAX     = 24.0f;    // real: DEVIATION_MAX (dB)
static constexpr float DEVIATION_DFL     = 12.0f;    // real: DEVIATION_DFL (dB)
static constexpr float SILENCE_MIN       = -84.0f;   // real: SILENCE_MIN (LUFS)
static constexpr float SILENCE_MAX       = -36.0f;   // real: SILENCE_MAX (LUFS)
static constexpr float SILENCE_DFL       = -72.0f;   // real: SILENCE_DFL (LUFS)
static constexpr float MAX_GAIN_MIN      = 0.0f;     // real: MAX_GAIN_MIN (dB)
static constexpr float MAX_GAIN_MAX      = 108.0f;   // real: MAX_GAIN_MAX (dB)
static constexpr float MAX_GAIN_DFL      = 36.0f;    // real: MAX_GAIN_DFL (dB)
static constexpr float LEVEL_MIN         = -60.0f;   // real: LEVEL_MIN (LUFS)
static constexpr float LEVEL_MAX         = 0.0f;     // real: LEVEL_MAX (LUFS)
static constexpr float LEVEL_DFL         = -23.0f;   // real: LEVEL_DFL (LUFS)
static constexpr size_t NUM_DFL          = 7;        // real: meta::autogain::NUM_DFL == NUM_12DB (index 7 -> 12.0 dB)
static constexpr size_t WEIGHT_DFL       = 5;        // real: meta::autogain::WEIGHT_DFL == WEIGHT_K

// ---------------------------------------------------------------------
// ChannelState — mirrors autogain.h's autogain::channel_t, stripped of
// plug::IPort* (pIn/pScIn/pShmIn/pOut) and the external-sidechain /
// shared-memory inputs (vScIn/vShmIn, out of scope). vIn/vOut are the
// caller's L/R pointers (real: bound per block in bind_audio_ports()).
// ---------------------------------------------------------------------
struct ChannelState {
    lsp::dspu::Bypass   sBypass;        // real: channel_t::sBypass
    lsp::dspu::Delay    sDelay;         // real: channel_t::sDelay (lookahead)
    float              *vIn  = nullptr; // real: channel_t::vIn  (bound per block)
    float              *vOut = nullptr; // real: channel_t::vOut (bound per block)
    float              *vBuffer = nullptr; // real: channel_t::vBuffer (BUFFER_SIZE scratch)

    void construct() {
        // real init() (autogain.cpp:217-218)
        sBypass.construct();
        sDelay.construct();
    }
    void destroy() {
        // real do_destroy() (autogain.cpp:327-328)
        sDelay.destroy();
        sBypass.destroy();
        free(vBuffer);
        vBuffer = nullptr;
    }
};

} // namespace lsp_jdsp_autogain

using namespace lsp_jdsp_autogain;

// ---------------------------------------------------------------------
// AutogainHandle — mirrors the top-level `autogain` plugin object for
// nChannels == 2 / SCMODE_INTERNAL (see file banner for what is stripped).
// ---------------------------------------------------------------------
struct AutogainHandle {
    lsp::dspu::LoudnessMeter sLInMeter;   // real: autogain::sLInMeter (long-period input loudness)
    lsp::dspu::LoudnessMeter sSInMeter;   // real: autogain::sSInMeter (short-period input loudness)
    lsp::dspu::LoudnessMeter sLScMeter;   // real: autogain::sLScMeter (long-period sidechain loudness — the gain path)
    lsp::dspu::LoudnessMeter sSScMeter;   // real: autogain::sSScMeter (short-period sidechain loudness — the gain path)
    lsp::dspu::AutoGain      sAutoGain;   // real: autogain::sAutoGain
    static constexpr size_t  nChannels = 2; // real: autogain::nChannels (stereo)
    ChannelState             vChannels[2]; // real: autogain::vChannels
    float fLInGain;     // real: autogain::fLInGain (long input loudness meter)
    float fSInGain;     // real: autogain::fSInGain (short input loudness meter)
    float fLScGain;     // real: autogain::fLScGain (long sidechain loudness meter)
    float fSScGain;     // real: autogain::fSScGain (short sidechain loudness meter)
    float fGain;        // real: autogain::fGain    (gain correction meter)
    float fOldLevel;    // real: autogain::fOldLevel
    float fLevel;       // real: autogain::fLevel
    float fOldPreamp;   // real: autogain::fOldPreamp
    float fPreamp;      // real: autogain::fPreamp
    float *vLBuffer;    // real: autogain::vLBuffer
    float *vSBuffer;    // real: autogain::vSBuffer
    float *vGainBuffer; // real: autogain::vGainBuffer
    float fSampleRate;  // real: plug::Module::fSampleRate

    struct Cfg { float level_lufs, long_period_ms, short_period_ms, drift_db,
                       silence_lufs, max_gain_db; int max_gain_on; };
    Cfg           cfg;  // last APPLIED (audio thread)
    RtConfig<Cfg> rt;   // control->audio handoff, see rt_config.h

    // Telemetry snapshot of the real plugin's "g_g" / "g_in_l" meters.
    float cur_gain_db;
    float cur_input_lufs;
};

void *lsp_autogain_create(float sample_rate) {
    ensure_dsp_init();
    auto *h = new AutogainHandle();
    h->fSampleRate = sample_rate;

    // real constructor (autogain.cpp:62-): fLInGain = fSInGain = fGain = 0;
    //   fOldLevel = fLevel = dspu::db_to_gain(meta::autogain::LEVEL_DFL);
    //   fOldPreamp = 0.0f; fPreamp = 1.0f;
    h->fLInGain   = 0.0f;
    h->fSInGain   = 0.0f;
    h->fLScGain   = 0.0f;
    h->fSScGain   = 0.0f;
    h->fGain      = 0.0f;
    h->fOldLevel  = lsp::dspu::db_to_gain(LEVEL_DFL);
    h->fLevel     = lsp::dspu::db_to_gain(LEVEL_DFL);
    h->fOldPreamp = 0.0f;
    h->fPreamp    = 1.0f;

    // real init() (autogain.cpp:172-186):
    //   sLInMeter.init(nChannels, meta::autogain::LONG_PERIOD_MAX)
    //   sSInMeter.init(nChannels, meta::autogain::SHORT_PERIOD_MAX)
    //   (sLOutMeter/sSOutMeter stripped, see banner)
    //   sLScMeter.init(nChannels, meta::autogain::LONG_PERIOD_MAX)
    //   sSScMeter.init(nChannels, meta::autogain::SHORT_PERIOD_MAX)
    //   sAutoGain.init()
    h->sLInMeter.init(AutogainHandle::nChannels, LONG_PERIOD_MAX);
    h->sSInMeter.init(AutogainHandle::nChannels, SHORT_PERIOD_MAX);
    h->sLScMeter.init(AutogainHandle::nChannels, LONG_PERIOD_MAX);
    h->sSScMeter.init(AutogainHandle::nChannels, SHORT_PERIOD_MAX);
    h->sAutoGain.init();

    // real init(): vLBuffer/vSBuffer/vGainBuffer = advance_ptr_bytes<float>(ptr, szof_buffer)
    // (one aligned arena in the real plugin; separate calloc()s here —
    // allocated once at create, never on the audio thread).
    h->vLBuffer    = (float *)calloc(BUFFER_SIZE, sizeof(float));
    h->vSBuffer    = (float *)calloc(BUFFER_SIZE, sizeof(float));
    h->vGainBuffer = (float *)calloc(BUFFER_SIZE, sizeof(float));

    // real update_sample_rate(sr) (autogain.cpp:352), run once here since
    // JamesDSP creates one handle per fixed sample rate:
    //   sLInMeter.set_sample_rate(sr); sSInMeter.set_sample_rate(sr);
    //   sLScMeter.set_sample_rate(sr); sSScMeter.set_sample_rate(sr);
    //   sAutoGain.set_sample_rate(sr);
    //   size_t max_delay = dspu::millis_to_samples(sr, meta::autogain::SC_LOOKAHEAD_MAX);
    //   per channel: c->sDelay.init(max_delay); c->sBypass.init(sr);
    h->sLInMeter.set_sample_rate((size_t)sample_rate);
    h->sSInMeter.set_sample_rate((size_t)sample_rate);
    h->sLScMeter.set_sample_rate((size_t)sample_rate);
    h->sSScMeter.set_sample_rate((size_t)sample_rate);
    h->sAutoGain.set_sample_rate((size_t)sample_rate);
    const size_t max_delay = (size_t)lsp::dspu::millis_to_samples(sample_rate, SC_LOOKAHEAD_MAX);

    for (size_t i = 0; i < AutogainHandle::nChannels; i++) {
        ChannelState &c = h->vChannels[i];
        c.construct();                                   // real init(): sBypass/sDelay.construct()
        c.vBuffer = (float *)calloc(BUFFER_SIZE, sizeof(float)); // real: c->vBuffer scratch
        c.sDelay.init(max_delay);
        c.sBypass.init((int)sample_rate);
        // Starts fully bypassed — JamesDSP's "off until configured+enabled"
        // convention for this module (same as the other LSP ports).
        c.sBypass.set_bypass(true);
    }

    h->cfg = AutogainHandle::Cfg{ 0, 0, 0, 0, 0, 0, 0 };
    h->cur_gain_db = 0.0f;
    h->cur_input_lufs = -120.0f;
    return h;
}

void lsp_autogain_destroy(void *h) {
    if (!h) return;
    auto *hd = (AutogainHandle *)h;
    // real do_destroy() (autogain.cpp:306): meters, sAutoGain, channels.
    hd->sLInMeter.destroy();
    hd->sSInMeter.destroy();
    hd->sLScMeter.destroy();
    hd->sSScMeter.destroy();
    hd->sAutoGain.destroy();
    for (size_t i = 0; i < AutogainHandle::nChannels; i++)
        hd->vChannels[i].destroy();
    free(hd->vLBuffer); free(hd->vSBuffer); free(hd->vGainBuffer);
    delete hd;
}

// real: float autogain::calc_gain_speed(gcontrol_type_t type) (autogain.cpp:430):
//   size_t numerator = size_t(gc->pValue->value());
//   size_t idx = lsp_limit(numerator, 0U, (sizeof(gain_numerators)/sizeof(gain_numerators[0])) - 1);
//   float fnum = gain_numerators[idx] * 0.1f;
//   float time = gc->pPeroid->value() * 0.001f;
//   return fnum / time; // Return value in dB/s
// The numerator port ("vgrow_l"/"vfall_l"/"vgrow_s"/"vfall_s", COMBO) and
// period port ("tgrow_l"/"tfall_l"/"tgrow_s"/"tfall_s", U_MSEC) are both
// pinned to their real defaults by the callers below.
static inline float calc_gain_speed(size_t numerator, float period_ms) {
    const size_t last = (sizeof(gain_numerators) / sizeof(gain_numerators[0])) - 1;
    const size_t idx  = (numerator > last) ? last : numerator;
    const float fnum  = gain_numerators[idx] * 0.1f;
    const float time  = period_ms * 0.001f;
    return fnum / time; // dB/s
}

// Faithful port of autogain::update_settings() (autogain.cpp:442), restricted
// to nChannels == 2 / SCMODE_INTERNAL. Audio-thread half of configure(): runs
// from lsp_autogain_process() when a new config was published.
static void autogain_apply(AutogainHandle *hd, float level_lufs, float long_period_ms,
                           float short_period_ms, float drift_db, float silence_lufs,
                           float max_gain_db, int max_gain_on) {
    // Hardening: the real host clamps every port to its metadata range;
    // JamesDSP's AIDL/Lua surface does not. Clamp to the real ranges
    // (meta/autogain.h). `!(x >= lo)` form also catches NaN.
    if (!(level_lufs >= LEVEL_MIN)) level_lufs = LEVEL_MIN;
    if (level_lufs > LEVEL_MAX) level_lufs = LEVEL_MAX;
    if (!(long_period_ms >= LONG_PERIOD_MIN)) long_period_ms = LONG_PERIOD_MIN;
    if (long_period_ms > LONG_PERIOD_MAX) long_period_ms = LONG_PERIOD_MAX;
    if (!(short_period_ms >= SHORT_PERIOD_MIN)) short_period_ms = SHORT_PERIOD_MIN;
    if (short_period_ms > SHORT_PERIOD_MAX) short_period_ms = SHORT_PERIOD_MAX;
    if (!(drift_db >= DEVIATION_MIN)) drift_db = DEVIATION_MIN;
    if (drift_db > DEVIATION_MAX) drift_db = DEVIATION_MAX;
    if (!(silence_lufs >= SILENCE_MIN)) silence_lufs = SILENCE_MIN;
    if (silence_lufs > SILENCE_MAX) silence_lufs = SILENCE_MAX;
    if (!(max_gain_db >= MAX_GAIN_MIN)) max_gain_db = MAX_GAIN_MIN;
    if (max_gain_db > MAX_GAIN_MAX) max_gain_db = MAX_GAIN_MAX;
    max_gain_on = (max_gain_on != 0) ? 1 : 0;

    hd->cfg = AutogainHandle::Cfg{ level_lufs, long_period_ms, short_period_ms,
                                   drift_db, silence_lufs, max_gain_db, max_gain_on };

    // real: bool bypass = pBypass->value() >= 0.5f; — relocated to
    //   lsp_autogain_process()'s `enabled` argument (same Bypass object).
    // real: dspu::bs::weighting_t weight = decode_weighting(pWeighting->value());
    //   COMBO("weight", ..., meta::autogain::WEIGHT_DFL == WEIGHT_K) ->
    //   decode_weighting(WEIGHT_K) == dspu::bs::WEIGHT_K.
    const lsp::dspu::bs::weighting_t weight = lsp::dspu::bs::WEIGHT_K;
    (void)WEIGHT_DFL;

    // real: "Update level"
    //   fLevel   = dspu::db_to_gain(pLevel->value());               // CONTROL("level", U_LUFS)
    //   enScMode = decode_sidechain_mode(pScMode->value());         // COMBO("scmode", SCMODE_DFL) -> SCMODE_INTERNAL (pinned)
    //   fPreamp  = dspu::db_to_gain(pScPreamp->value());            // CONTROL("preamp", U_DB) -> SC_PREAMP_DFL == 0 dB
    //   size_t lookahead = dspu::millis_to_samples(fSampleRate, pLookahead->value()); // CONTROL("lkahead", U_MSEC) -> SC_LOOKAHEAD_DFL == 0
    hd->fLevel  = lsp::dspu::db_to_gain(level_lufs);
    hd->fPreamp = lsp::dspu::db_to_gain(SC_PREAMP_DFL);
    const size_t lookahead = (size_t)lsp::dspu::millis_to_samples(hd->fSampleRate, SC_LOOKAHEAD_DFL);

    // real: "Configure autogain"
    //   sAutoGain.set_deviation(dspu::db_to_gain(pDeviation->value()));            // CONTROL("drift", U_DB)
    hd->sAutoGain.set_deviation(lsp::dspu::db_to_gain(drift_db));
    //   sAutoGain.set_long_speed(calc_gain_speed(GCT_LONG_GROW), calc_gain_speed(GCT_LONG_FALL));
    //     vgrow_l = NUM_DFL (12 dB), tgrow_l = LONG_GROW_DFL (500 ms) -> 24 dB/s
    //     vfall_l = NUM_DFL (12 dB), tfall_l = LONG_FALL_DFL (500 ms) -> 24 dB/s
    hd->sAutoGain.set_long_speed(calc_gain_speed(NUM_DFL, LONG_GROW_DFL),
                                 calc_gain_speed(NUM_DFL, LONG_FALL_DFL));
    //   sAutoGain.set_short_speed(calc_gain_speed(GCT_SHORT_GROW), calc_gain_speed(GCT_SHORT_FALL));
    //     vgrow_s = NUM_DFL (12 dB), tgrow_s = SHORT_GROW_DFL (40 ms) -> 300 dB/s
    //     vfall_s = NUM_DFL (12 dB), tfall_s = SHORT_FALL_DFL (10 ms) -> 1200 dB/s
    hd->sAutoGain.set_short_speed(calc_gain_speed(NUM_DFL, SHORT_GROW_DFL),
                                  calc_gain_speed(NUM_DFL, SHORT_FALL_DFL));
    //   sAutoGain.set_silence_threshold(dspu::db_to_gain(pSilence->value()));      // CONTROL("silence", U_LUFS)
    hd->sAutoGain.set_silence_threshold(lsp::dspu::db_to_gain(silence_lufs));
    //   sAutoGain.enable_quick_amplifier(pQAmp->value() >= 0.5f);                  // SWITCH("qamp", 0.0f) -> false
    hd->sAutoGain.enable_quick_amplifier(0.0f >= 0.5f);
    //   sAutoGain.set_max_gain(dspu::db_to_gain(pAmpGain->value()), pAmpOn->value() >= 0.5f);
    //     CONTROL("max_amp", U_DB) / SWITCH("max_on", 0.0f)
    hd->sAutoGain.set_max_gain(lsp::dspu::db_to_gain(max_gain_db), (float)max_gain_on >= 0.5f);

    // real: "Set measuring period"
    //   float l_period = pLPeriod->value();   // LOG_CONTROL("lperiod", U_MSEC)
    //   float s_period = pSPeriod->value();   // LOG_CONTROL("speriod", U_MSEC)
    //   sLInMeter.set_period(l_period); sSInMeter.set_period(s_period);
    //   sLInMeter.set_weighting(weight); sSInMeter.set_weighting(weight);
    //   (sLOutMeter/sSOutMeter: stripped, see banner)
    //   sLScMeter.set_period(l_period); sSScMeter.set_period(s_period);
    //   sLScMeter.set_weighting(weight); sSScMeter.set_weighting(weight);
    const float l_period = long_period_ms;
    const float s_period = short_period_ms;
    hd->sLInMeter.set_period(l_period);
    hd->sSInMeter.set_period(s_period);
    hd->sLInMeter.set_weighting(weight);
    hd->sSInMeter.set_weighting(weight);

    hd->sLScMeter.set_period(l_period);
    hd->sSScMeter.set_period(s_period);
    hd->sLScMeter.set_weighting(weight);
    hd->sSScMeter.set_weighting(weight);

    // real: if (nChannels > 1) { sLInMeter.set_designation(0, CHANNEL_LEFT); (1, CHANNEL_RIGHT);
    //   sSInMeter ...; (sLOutMeter/sSOutMeter stripped); sLScMeter ...; sSScMeter ...; }
    hd->sLInMeter.set_designation(0, lsp::dspu::bs::CHANNEL_LEFT);
    hd->sLInMeter.set_designation(1, lsp::dspu::bs::CHANNEL_RIGHT);
    hd->sSInMeter.set_designation(0, lsp::dspu::bs::CHANNEL_LEFT);
    hd->sSInMeter.set_designation(1, lsp::dspu::bs::CHANNEL_RIGHT);

    hd->sLScMeter.set_designation(0, lsp::dspu::bs::CHANNEL_LEFT);
    hd->sLScMeter.set_designation(1, lsp::dspu::bs::CHANNEL_RIGHT);
    hd->sSScMeter.set_designation(0, lsp::dspu::bs::CHANNEL_LEFT);
    hd->sSScMeter.set_designation(1, lsp::dspu::bs::CHANNEL_RIGHT);

    // real: for (i < nChannels) { sLInMeter.set_link(i, 1.0f); sLInMeter.set_active(i, true);
    //   sSInMeter.set_link(i, 1.0f); sSInMeter.set_active(i, true);
    //   (sLOutMeter/sSOutMeter stripped)
    //   sLScMeter.set_link(i, 1.0f); sLScMeter.set_active(i, true);
    //   sSScMeter.set_link(i, 1.0f); sSScMeter.set_active(i, true); }
    for (size_t i = 0; i < AutogainHandle::nChannels; i++) {
        hd->sLInMeter.set_link(i, 1.0f);
        hd->sLInMeter.set_active(i, true);
        hd->sSInMeter.set_link(i, 1.0f);
        hd->sSInMeter.set_active(i, true);

        hd->sLScMeter.set_link(i, 1.0f);
        hd->sLScMeter.set_active(i, true);
        hd->sSScMeter.set_link(i, 1.0f);
        hd->sSScMeter.set_active(i, true);
    }

    // real: "Update bypass"
    //   for (i < nChannels) { c->sDelay.set_delay(lookahead); c->sBypass.set_bypass(bypass); }
    //   (set_bypass relocated to lsp_autogain_process(), see above)
    for (size_t i = 0; i < AutogainHandle::nChannels; i++)
        hd->vChannels[i].sDelay.set_delay(lookahead);

    // real: set_latency(lookahead); — host latency reporting, no JamesDSP
    // equivalent; lookahead == 0 in this scope anyway.
}

void lsp_autogain_configure(void *h, float level_lufs, float long_period_ms,
                             float short_period_ms, float drift_db,
                             float silence_lufs, float max_gain_db,
                             int max_gain_on) {
    if (!h) return;
    auto *hd = (AutogainHandle *)h;
    // Sanitise + publish only (control thread); range clamping happens in
    // autogain_apply() on the audio thread. NaN -> real defaults so peek()
    // never hands a NaN back to a Lua get->modify->set loop.
    if (!(level_lufs == level_lufs))           level_lufs = LEVEL_DFL;
    if (!(long_period_ms == long_period_ms))   long_period_ms = LONG_PERIOD_DFL;
    if (!(short_period_ms == short_period_ms)) short_period_ms = SHORT_PERIOD_DFL;
    if (!(drift_db == drift_db))               drift_db = DEVIATION_DFL;
    if (!(silence_lufs == silence_lufs))       silence_lufs = SILENCE_DFL;
    if (!(max_gain_db == max_gain_db))         max_gain_db = MAX_GAIN_DFL;
    AutogainHandle::Cfg c = { level_lufs, long_period_ms, short_period_ms, drift_db,
                              silence_lufs, max_gain_db, (max_gain_on != 0) ? 1 : 0 };
    hd->rt.publish(c);   // applied by autogain_apply() on the audio thread's next block
}

// Faithful port of autogain::process() (autogain.cpp:547) and the four
// per-chunk helpers it calls, restricted to nChannels == 2 / SCMODE_INTERNAL.
void lsp_autogain_process(void *h, float *left, float *right, unsigned n, int enabled) {
    if (!h || n == 0) return;
    auto *hd = (AutogainHandle *)h;
    {
        AutogainHandle::Cfg c;
        if (hd->rt.take(c))
            autogain_apply(hd, c.level_lufs, c.long_period_ms, c.short_period_ms,
                           c.drift_db, c.silence_lufs, c.max_gain_db, c.max_gain_on);
    }
    ChannelState &c0 = hd->vChannels[0];
    ChannelState &c1 = hd->vChannels[1];

    // real: c->sBypass.set_bypass(bypass) inside update_settings(); relocated
    // here because JamesDSP carries the flag per block, not per configure.
    const bool bypass_req = !enabled;
    c0.sBypass.set_bypass(bypass_req);
    c1.sBypass.set_bypass(bypass_req);

    // Documented zero-cost contract (lsp_wrap_autogain.h): once both
    // channels' Bypass have fully settled off, skip the whole chain.
    if (c0.sBypass.on() && c1.sBypass.on()) {
        hd->cur_gain_db = 0.0f;
        hd->cur_input_lufs = -120.0f;
        return;
    }

    // real: bind_audio_ports() (autogain.cpp:572) —
    //   c->vIn = c->pIn->buffer<float>(); c->vScIn = ... (ext SC, out of
    //   scope); c->vShmIn = NULL (link, out of scope); c->vOut = c->pOut->buffer<float>();
    // JamesDSP processes in place: vIn and vOut are the same caller buffer.
    c0.vIn = left;  c0.vOut = left;
    c1.vIn = right; c1.vOut = right;

    // real: clean_meters() (autogain.cpp:590) — fLInGain = fSInGain =
    //   fLScGain = fSScGain = fGain = 0 (fLOutGain/fSOutGain: stripped meters)
    hd->fLInGain = 0.0f;
    hd->fSInGain = 0.0f;
    hd->fLScGain = 0.0f;
    hd->fSScGain = 0.0f;
    hd->fGain    = 0.0f;

    // real: for (size_t offset=0; offset < samples; ) {
    //           size_t to_do = lsp_min(samples - offset, BUFFER_SIZE);
    //           measure_input_loudness(to_do); compute_gain_correction(to_do);
    //           apply_gain_correction(to_do); update_audio_buffers(to_do);
    //           offset += to_do; }
    for (size_t offset = 0; offset < n; ) {
        const size_t to_do = ((n - offset) < BUFFER_SIZE) ? (n - offset) : BUFFER_SIZE;

        // ---- real: measure_input_loudness(to_do) (autogain.cpp:622) ----
        // "Bind channels for analysis"
        for (size_t i = 0; i < AutogainHandle::nChannels; i++) {
            ChannelState *c = &hd->vChannels[i];
            //   sLInMeter.bind(i, NULL, c->vIn, 0);
            //   sSInMeter.bind(i, NULL, c->vIn, 0);
            hd->sLInMeter.bind(i, NULL, c->vIn, 0);
            hd->sSInMeter.bind(i, NULL, c->vIn, 0);
            //   "Process sidechain signal"
            //   const float *in_buf = select_buffer(c);   // SCMODE_INTERNAL -> c->vIn
            //   dsp::lramp2(c->vBuffer, in_buf, fOldPreamp, fPreamp, samples);
            //   "Bind sidechain meters"
            //   sLScMeter.bind(i, NULL, c->vBuffer, 0);
            //   sSScMeter.bind(i, NULL, c->vBuffer, 0);
            const float *in_buf = c->vIn;
            lsp::dsp::lramp2(c->vBuffer, in_buf, hd->fOldPreamp, hd->fPreamp, to_do);
            hd->sLScMeter.bind(i, NULL, c->vBuffer, 0);
            hd->sSScMeter.bind(i, NULL, c->vBuffer, 0);
        }
        //   fOldPreamp = fPreamp;
        hd->fOldPreamp = hd->fPreamp;

        //   switch (enScMode) { ... case SCMODE_INTERNAL: default:
        //     "Process the loudnes of input signal"
        //     sLInMeter.process(vLBuffer, samples, dspu::bs::DBFS_TO_LUFS_SHIFT_GAIN);
        //     fLInGain = lsp_max(fLInGain, dsp::max(vLBuffer, samples));
        //     sLInGraph.process(vLBuffer, samples);                      // UI graph, dropped
        //     sSInMeter.process(vSBuffer, samples, dspu::bs::DBFS_TO_LUFS_SHIFT_GAIN);
        //     fSInGain = lsp_max(fSInGain, dsp::max(vSBuffer, samples));
        //     sSInGraph.process(vSBuffer, samples);                      // UI graph, dropped
        //     "Process the loudness of sidechain signal"
        //     sLScMeter.process(vLBuffer, samples, dspu::bs::DBFS_TO_LUFS_SHIFT_GAIN);  // OVERWRITES vLBuffer
        //     fLScGain = lsp_max(fLInGain, dsp::max(vLBuffer, samples));
        //     sLScGraph.process(vLBuffer, samples);                      // UI graph, dropped
        //     sSScMeter.process(vSBuffer, samples, dspu::bs::DBFS_TO_LUFS_SHIFT_GAIN);  // OVERWRITES vSBuffer
        //     fSScGain = lsp_max(fSInGain, dsp::max(vSBuffer, samples));
        //     sSScGraph.process(vSBuffer, samples); }                    // UI graph, dropped
        // NOTE: after this block vLBuffer/vSBuffer hold the SIDECHAIN meters'
        // output (the input meters' output is overwritten), and that is what
        // compute_gain_correction() feeds to sAutoGain — the SC meters are
        // the gain path in SCMODE_INTERNAL. Real order kept exactly.
        hd->sLInMeter.process(hd->vLBuffer, to_do, lsp::dspu::bs::DBFS_TO_LUFS_SHIFT_GAIN);
        {
            const float m = lsp::dsp::max(hd->vLBuffer, to_do);
            hd->fLInGain = (hd->fLInGain > m) ? hd->fLInGain : m;
        }
        hd->sSInMeter.process(hd->vSBuffer, to_do, lsp::dspu::bs::DBFS_TO_LUFS_SHIFT_GAIN);
        {
            const float m = lsp::dsp::max(hd->vSBuffer, to_do);
            hd->fSInGain = (hd->fSInGain > m) ? hd->fSInGain : m;
        }

        hd->sLScMeter.process(hd->vLBuffer, to_do, lsp::dspu::bs::DBFS_TO_LUFS_SHIFT_GAIN);
        {
            const float m = lsp::dsp::max(hd->vLBuffer, to_do);
            hd->fLScGain = (hd->fLInGain > m) ? hd->fLInGain : m;   // real: lsp_max(fLInGain, ...) — verbatim
        }
        hd->sSScMeter.process(hd->vSBuffer, to_do, lsp::dspu::bs::DBFS_TO_LUFS_SHIFT_GAIN);
        {
            const float m = lsp::dsp::max(hd->vSBuffer, to_do);
            hd->fSScGain = (hd->fSInGain > m) ? hd->fSInGain : m;   // real: lsp_max(fSInGain, ...) — verbatim
        }

        // ---- real: compute_gain_correction(to_do) (autogain.cpp:700) ----
        //   case SCMODE_INTERNAL: default:
        //     if (fOldLevel != fLevel) {
        //         dsp::lramp_set1(vGainBuffer, fOldLevel, fLevel, samples);
        //         sAutoGain.process(vGainBuffer, vLBuffer, vSBuffer, vGainBuffer, samples);
        //     } else
        //         sAutoGain.process(vGainBuffer, vLBuffer, vSBuffer, fLevel, samples);
        //   fOldLevel = fLevel;
        //   fGain = lsp_max(fGain, dsp::max(vGainBuffer, samples));
        //   sGainGraph.process(vGainBuffer, samples);                   // UI graph, dropped
        if (hd->fOldLevel != hd->fLevel) {
            lsp::dsp::lramp_set1(hd->vGainBuffer, hd->fOldLevel, hd->fLevel, to_do);
            hd->sAutoGain.process(hd->vGainBuffer, hd->vLBuffer, hd->vSBuffer, hd->vGainBuffer, to_do);
        } else
            hd->sAutoGain.process(hd->vGainBuffer, hd->vLBuffer, hd->vSBuffer, hd->fLevel, to_do);
        hd->fOldLevel = hd->fLevel;
        {
            const float m = lsp::dsp::max(hd->vGainBuffer, to_do);
            hd->fGain = (hd->fGain > m) ? hd->fGain : m;
        }

        // ---- real: apply_gain_correction(to_do) (autogain.cpp:732) ----
        //   for (i < nChannels) {
        //     c->sDelay.process(c->vBuffer, c->vBuffer, samples);  // "Apply lookahead to the delay"
        //     dsp::mul3(c->vBuffer, c->vIn, vGainBuffer, samples); // "Apply VCA control"
        //     sLOutMeter.bind(...); sSOutMeter.bind(...);          // stripped (output meters)
        //   }
        //   sLOutMeter.process(vLBuffer, ...); sSOutMeter.process(vSBuffer, ...);
        //   — stripped: provably inert here, they overwrite vLBuffer/vSBuffer
        //   AFTER sAutoGain consumed them and measure_input_loudness() rewrites
        //   both before the next read; only pLOutGain/pSOutGain + graphs read them.
        // NOTE (verbatim quirk, not "fixed"): the real plugin delays vBuffer
        // (the preamped sidechain copy) and then immediately overwrites
        // vBuffer with vIn * gain, so the lookahead delay never reaches the
        // audio path. Ported as-is per the hard rule; with lookahead pinned
        // to 0 it is inert here regardless.
        for (size_t i = 0; i < AutogainHandle::nChannels; i++) {
            ChannelState *c = &hd->vChannels[i];
            c->sDelay.process(c->vBuffer, c->vBuffer, to_do);
            lsp::dsp::mul3(c->vBuffer, c->vIn, hd->vGainBuffer, to_do);
        }

        // ---- real: update_audio_buffers(to_do) (autogain.cpp:755) ----
        //   for (i < nChannels) {
        //     c->sBypass.process(c->vOut, c->vIn, c->vBuffer, samples);  // "Apply bypass"
        //     c->vIn += samples; c->vScIn += samples; if (c->vShmIn) c->vShmIn += samples; c->vOut += samples;
        //   }
        // (vOut == vIn here: Bypass::process reads *dry before writing
        // *dst per sample, so in-place is safe.)
        for (size_t i = 0; i < AutogainHandle::nChannels; i++) {
            ChannelState *c = &hd->vChannels[i];
            c->sBypass.process(c->vOut, c->vIn, c->vBuffer, to_do);
            c->vIn  += to_do;
            c->vOut += to_do;
        }

        offset += to_do;
    }

    // real: output_meters() — pGain->set_value(fGain); pLInGain->set_value(fLInGain)
    //   (host meter ports; kept as JamesDSP telemetry instead)
    hd->cur_gain_db    = (hd->fGain > 0.0f) ? 20.0f * log10f(hd->fGain) : -120.0f;
    hd->cur_input_lufs = (hd->fLInGain > 0.0f) ? 20.0f * log10f(hd->fLInGain) : -120.0f;
    // real: output_mesh_data(); pWrapper->query_display_draw(); — UI, dropped.
}

float lsp_autogain_get_gain_db(void *h) {
    if (!h) return 0.0f;
    return ((AutogainHandle *)h)->cur_gain_db;
}

float lsp_autogain_get_input_lufs(void *h) {
    if (!h) return -120.0f;
    return ((AutogainHandle *)h)->cur_input_lufs;
}

void lsp_autogain_get_config(void *h, float *level_lufs, float *long_period_ms,
                             float *short_period_ms, float *drift_db,
                             float *silence_lufs, float *max_gain_db,
                             int *max_gain_on) {
    if (!h) {
        if (level_lufs) *level_lufs = 0.0f;
        if (long_period_ms) *long_period_ms = 0.0f;
        if (short_period_ms) *short_period_ms = 0.0f;
        if (drift_db) *drift_db = 0.0f;
        if (silence_lufs) *silence_lufs = 0.0f;
        if (max_gain_db) *max_gain_db = 0.0f;
        if (max_gain_on) *max_gain_on = 0;
        return;
    }
    auto *hd = (AutogainHandle *)h;
    const AutogainHandle::Cfg c = hd->rt.peek();   // last published, not last applied
    if (level_lufs) *level_lufs = c.level_lufs;
    if (long_period_ms) *long_period_ms = c.long_period_ms;
    if (short_period_ms) *short_period_ms = c.short_period_ms;
    if (drift_db) *drift_db = c.drift_db;
    if (silence_lufs) *silence_lufs = c.silence_lufs;
    if (max_gain_db) *max_gain_db = c.max_gain_db;
    if (max_gain_on) *max_gain_on = c.max_gain_on;
}
