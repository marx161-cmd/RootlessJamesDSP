// lsp_wrap_loudcomp.cpp — faithful port of the real LSP Loudness Compensator.
//
// Source of truth: lsp-loud-comp/src/main/plug/loud_comp.cpp (pinned release
// 1.0.33, git tag 1.0.33 / commit b38b9ac; plugin logic identical to the
// 1.0.31 shipped in the comrade reference build lsp-plugins 1.2.29)
//   (class lsp::plugins::loud_comp — init() @139, update_sample_rate() @331,
//    update_settings() @382, update_fft_curve() @496, update_iir_curve() @574,
//    process_callback() @679, process_spectrum() @686,
//    process_iir_equalizer() @748, process_audio() @779, process() @875)
// and its header lsp-loud-comp/include/private/plugins/loud_comp.h
//   (struct loud_comp::channel_t), metadata ranges/defaults in
//   lsp-loud-comp/include/private/meta/loud_comp.h, port symbols in
//   lsp-loud-comp/src/main/meta/loud_comp.cpp, and the equal-loudness
//   contour tables in lsp-loud-comp/include/generated/iso226/*.h (compiled
//   in unchanged, included below).
//
// Per LSP_FAITHFUL_PORT_SCOPE.md, this is a TRANSLITERATION of the real
// plugin's DSP logic into JamesDSP's calling convention. The dsp-units
// classes it drives (dspu::SpectralProcessor, dspu::Equalizer, dspu::Delay,
// dspu::Bypass) are the real ones, compiled from the submodule. Structural
// template: lsp_wrap_expander.cpp / lsp_wrap_autogain.cpp.
//
// Scope: STEREO variant only (real meta::loud_comp_stereo -> nChannels == 2),
// so vChannels is a fixed pair rather than an allocation. Versus the real
// code, the following is stripped or pinned — all of it host/UI plumbing or
// the reference generator, none of it the compensation DSP:
//   - Reference generator: "refer" SWITCH (dfl 0) / "reftype" COMBO pinned
//     off -> bReference == false, so process() always takes process_audio()
//     and generate_signal(), sOsc, sNoise, enGenerator, decode_generator(),
//     get_generator_amplitude() and sNoise.set_amplitude() are omitted.
//   - Meters: sInMeter/sOutMeter (dspu::LoudnessMeter), fInLufs/fOutLufs,
//     pLufsIn/pLufsOut, per-channel fInLevel/fOutLevel, pMeterIn/pMeterOut
//     are output-only meter ports (lufs_il/lufs_ol/ilm_*/olm_*) and are
//     stripped. In process_audio() the abs_max() that feeds the clip test
//     is kept where it has a DSP consequence (bHClipOn: decides whether
//     dsp::limit1() runs) and skipped when hard clip is off, where its only
//     reader was the Blink indicator.
//   - Hard-clip indicator: sClipInd (dspu::Blink), c->bHClip, pHClipInd
//     (olc_*), the "hcclean" TRIGGER / rst_clip are the indicator and its
//     reset — stripped. The hard clipping itself (fHClipLvl computation in
//     update_settings(), dsp::limit1() in process_audio()) is kept verbatim.
//   - Mesh/inline display: pMesh ("spec"), pRelative ("relspec"), bRelative,
//     bSyncMesh, vFreqMesh/vAmpMesh, generate_frequencies(), the amp-mesh
//     build at the end of update_fft_curve(), the freq_chart()/pcomplex_mod()
//     at the end of update_iir_curve(), inline_display(), ui_activated(),
//     pIDisplay, query_display_draw(), dump(). NOTE on freq_chart(): it
//     forces Equalizer::reconfigure() on channel 0 inside update_settings();
//     the per-channel sEqualizer.get_latency() that follows in the same
//     update_settings() (kept) does the same reconfigure() for both
//     channels before any sample is processed, so stripping the mesh call
//     changes no sample.
//   - set_latency() host reporting -> lsp_loudcomp_get_latency().
//   - The lsp_trace() dump of the 256-point response (debug build only).
//   - pBypass: the real plugin reads a bypass port in update_settings();
//     JamesDSP carries the enable flag through lsp_loudcomp_process(), so
//     the identical sBypass.set_bypass() call is made there (same object,
//     same crossfade; relocated, not changed).
// Everything else — curve interpolation into vFreqApply / vGains, the
// SpectralProcessor callback multiply, the BUF_SIZE chunked loop, the
// dry-path Delay for the bypass crossfade, the IIR smooth-ramp path, the
// hard clip, and the Bypass — is carried over in the real plugin's own
// order of operations, including its quirks (latency computed from the
// SpectralProcessor's PREVIOUS rank in update_settings(); a first-update
// delay of 1 << FFT_RANK_MAX on a Delay line of 1 << (FFT_RANK_MAX-1)
// samples, which Delay::set_delay() wraps modulo its size).
//
// VERIFICATION 2026-10-22 (bench/difftest, vs LADSPA loud_comp_stereo from
// lsp-plugins 1.2.29 on comrade, ISO226-2023 @ -30 phons, 4096-point):
//   FFT mode: -111.6 dB rms residual (full and audio variants alike).
//   IIR mode: -30.2 dB — this is the REFERENCE's own float32 biquad rounding
//   floor at the 22..80 Hz sections (poles at |z| ~ 0.997), not a port
//   deviation: the same x86 plugin run on the AVX2/FMA vs SSE4.2 dsp-lib
//   paths differs from itself by -31.5 dB in IIR mode (FFT mode -115 dB).
#include "lsp_wrap_loudcomp.h"
#include "rt_config.h"

#include <lsp-plug.in/common/alloc.h>
#include <lsp-plug.in/common/utility.h>
#include <lsp-plug.in/dsp-units/const.h>
#include <lsp-plug.in/dsp-units/units.h>
#include <lsp-plug.in/dsp-units/ctl/Bypass.h>
#include <lsp-plug.in/dsp-units/filters/Equalizer.h>
#include <lsp-plug.in/dsp-units/filters/common.h>
#include <lsp-plug.in/dsp-units/util/Delay.h>
#include <lsp-plug.in/dsp-units/util/SpectralProcessor.h>
#include <lsp-plug.in/dsp/dsp.h>

// real: loud_comp.cpp:31-34 — the generated equal-loudness contour tables,
// compiled in unchanged from the submodule (lsp-loud-comp/include).
#include <generated/iso226/fletcher_munson.h>
#include <generated/iso226/robinson_dadson.h>
#include <generated/iso226/iso226-2003.h>
#include <generated/iso226/iso226-2023.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace lsp_jdsp_loudcomp {

// lsp::dsp::init() populates lsp-dsp-lib's dispatch table; same idempotent
// internal-linkage guard as the other LSP ports in this build.
static inline void ensure_dsp_init() {
    static bool done = (lsp::dsp::init(), true);
    (void)done;
}

// real: loud_comp.cpp:40-46
//   static const freq_curve_t *freq_curves[] = { &iso226_2003_curve,
//       &fletcher_munson_curve, &robinson_dadson_curve, &iso226_2023_curve };
// Indexed by (nCurve - 1); nCurve == 0 is "Flat" (no table).
static const lsp::freq_curve_t *freq_curves[] =
{
    &lsp::iso226_2003_curve,
    &lsp::fletcher_munson_curve,
    &lsp::robinson_dadson_curve,
    &lsp::iso226_2023_curve,
};

// real: loud_comp.cpp:48-51
static constexpr size_t EQ_SMOOTH_STEP      = 32;
static constexpr size_t BUF_SIZE            = 0x200;
static constexpr size_t NUM_CURVES          = (sizeof(freq_curves)/sizeof(lsp::freq_curve_t *));
static constexpr float  CURVE_APPROX        = 0.0005f * M_LN10;

// real: loud_comp.cpp:53-66
typedef struct approx_preset_t
{
    uint8_t     nFilters;
    uint8_t     nSlope;
} approx_preset_t;

static const approx_preset_t approx_presets[] =
{
    { 16, 1, },
    { 24, 2, },
    { 32, 3, },
    { 48, 4, },
    { 64, 5, }
};
static constexpr size_t NUM_APPROX          = (sizeof(approx_presets)/sizeof(approx_preset_t));

// Real metadata ranges/defaults (lsp-loud-comp/include/private/meta/loud_comp.h,
// struct meta::loud_comp_metadata) and the port macros in meta/loud_comp.cpp.
static constexpr float  PHONS_MIN           = -83.0f;   // real: PHONS_MIN
static constexpr float  PHONS_MAX           = 7.0f;     // real: PHONS_MAX
static constexpr float  PHONS_DFL           = 0.0f;     // real: PHONS_DFL
static constexpr float  HCRANGE_MIN         = 0.0f;     // real: HCRANGE_MIN (dB)
static constexpr float  HCRANGE_MAX         = 24.0f;    // real: HCRANGE_MAX (dB)
static constexpr float  HCRANGE_DFL         = 6.0f;     // real: HCRANGE_DFL (dB)
static constexpr float  FREQ_MIN            = LSP_DSP_UNITS_SPEC_FREQ_MIN;  // real: FREQ_MIN = SPEC_FREQ_MIN (10 Hz)
static constexpr float  FREQ_MAX            = LSP_DSP_UNITS_SPEC_FREQ_MAX;  // real: FREQ_MAX = SPEC_FREQ_MAX (24 kHz)
static constexpr size_t FFT_RANK_MIN        = 8;        // real: FFT_RANK_MIN
static constexpr size_t FFT_RANK_MAX        = 14;       // real: FFT_RANK_MAX
static constexpr size_t FFT_RANK_IDX_DFL    = 4;        // real: FFT_RANK_IDX_DFL (-> 4096)
static constexpr size_t STD_DFL             = 4;        // real: STD_DFL (ISO226-2023)
static constexpr size_t APPROX_DFL          = 2;        // real: COMBO("approx", ..., 2, ...) -> Normal
static constexpr size_t FILTER_BANDS        = 64;       // real: FILTER_BANDS
static constexpr float  INPUT_GAIN_MAX      = GAIN_AMP_P_72_DB; // real: AMP_GAIN("input", ..., GAIN_AMP_0_DB, GAIN_AMP_P_72_DB)
// real: meta::loud_comp_metadata::mode_t
static constexpr uint32_t MODE_FFT          = 0;
static constexpr uint32_t MODE_IIR          = 1;

// ---------------------------------------------------------------------
// ChannelState — mirrors loud_comp.h's loud_comp::channel_t, stripped of
// fInLevel/fOutLevel/bHClip (meters + indicator), sClipInd (Blink) and
// every plug::IPort*. Field names match channel_t's real names 1:1 for
// everything that survives.
// ---------------------------------------------------------------------
struct ChannelState {
    float                        *vIn     = nullptr; // real: channel_t::vIn   (port buffer; here caller's L/R + offset)
    float                        *vOut    = nullptr; // real: channel_t::vOut  (same pointer: JamesDSP processes in place)
    float                        *vDry    = nullptr; // real: channel_t::vDry  (BUF_SIZE)
    float                        *vBuffer = nullptr; // real: channel_t::vBuffer (BUF_SIZE)

    lsp::dspu::Bypass             sBypass;    // real: channel_t::sBypass
    lsp::dspu::Delay              sDelay;     // real: channel_t::sDelay (dry path, for bypass)
    lsp::dspu::SpectralProcessor  sProc;      // real: channel_t::sProc
    lsp::dspu::Equalizer          sEqualizer; // real: channel_t::sEqualizer

    void construct() {
        // real init() (loud_comp.cpp:196-200), minus sClipInd.construct().
        sDelay.construct();
        sBypass.construct();
        sProc.construct();
        sEqualizer.construct();
    }

    void destroy() {
        // real do_destroy() (loud_comp.cpp:309-311).
        sDelay.destroy();
        sProc.destroy();
        sEqualizer.destroy();
        free(vDry); free(vBuffer);
        vDry = vBuffer = nullptr;
    }
};

} // namespace lsp_jdsp_loudcomp

using namespace lsp_jdsp_loudcomp;

// ---------------------------------------------------------------------
// LcHandle — mirrors the top-level `loud_comp` plugin object for
// nChannels == 2. Field names match the real members.
// ---------------------------------------------------------------------
struct LcHandle {
    ChannelState    ch[2];          // real: loud_comp::vChannels[2]
    float           fSampleRate;    // real: plug::Module::fSampleRate

    uint32_t        nMode;          // real: nMode
    uint32_t        nCurve;         // real: nCurve
    uint32_t        nRank;          // real: nRank
    uint32_t        nFilters;       // real: nFilters
    float           fGain;          // real: fGain (LINEAR input gain)
    float           fVolume;        // real: fVolume (phons)
    bool            bHClipOn;       // real: bHClipOn
    float           fHClipLvl;      // real: fHClipLvl
    float          *vTmpBuf;        // real: vTmpBuf
    float          *vFreqApply;     // real: vFreqApply (2 << FFT_RANK_MAX floats)
    bool            bSmooth;        // real: bSmooth

    float           vOldGains[FILTER_BANDS]; // real: vOldGains
    float           vGains[FILTER_BANDS];    // real: vGains

    // Last-APPLIED parameters in caller units (set by loudcomp_apply() on the
    // audio thread). Order matches lsp_loudcomp_configure()'s arguments.
    struct Cfg { float input_gain_db; int mode, std, fft_rank_idx, approx; float volume_phons; int hclip; float hcrange_db; };
    Cfg             cfg;
    RtConfig<Cfg>   rt;             // control->audio handoff, see rt_config.h

    // Set when lsp_loudcomp_process() skipped the block because both Bypass
    // objects had settled fully off; the next processed block resets the
    // processors first (see the header's contract note).
    bool            bIdleSkipped;
};

// real: loud_comp::process_callback() (loud_comp.cpp:679) +
//       loud_comp::process_spectrum() (loud_comp.cpp:686):
//   size_t count = 2 << nRank;
//   dsp::mul2(buf, vFreqApply, count);
// (The real callback ignores the `rank` argument and uses the plugin's own
// nRank — kept identical.)
static void process_callback(void *object, void *subject, float *buf, size_t rank)
{
    (void)subject; (void)rank;
    LcHandle *hd = static_cast<LcHandle *>(object);
    const size_t count = 2 << hd->nRank;
    lsp::dsp::mul2(buf, hd->vFreqApply, count);
}

void *lsp_loudcomp_create(float sample_rate) {
    ensure_dsp_init();
    auto *h = new LcHandle();
    h->fSampleRate = sample_rate;

    // real constructor (loud_comp.cpp:85-114): the start state every later
    // update_settings() diffs against.
    h->nMode        = MODE_FFT;
    h->nCurve       = 0;
    h->nRank        = FFT_RANK_MIN;
    h->nFilters     = 24;
    h->fGain        = 0.0f;
    h->fVolume      = -1.0f;
    h->bHClipOn     = false;
    h->fHClipLvl    = 1.0f;
    h->bSmooth      = false;
    lsp::dsp::fill(h->vOldGains, GAIN_AMP_0_DB, FILTER_BANDS);
    lsp::dsp::fill(h->vGains,    GAIN_AMP_0_DB, FILTER_BANDS);

    // real init() (loud_comp.cpp:167-186): buffer sizing.
    //   sz_tmpbuf = max over curves of hdots, aligned to 0x100 bytes, then
    //   lsp_max(sz_tmpbuf, BUF_SIZE*sizeof(float));
    //   sz_fft = (2 << FFT_RANK_MAX) * sizeof(float); sz_buf = BUF_SIZE*sizeof(float)
    size_t sz_tmpbuf = 0;
    for (size_t i = 0; i < NUM_CURVES; ++i) {
        const lsp::freq_curve_t *c = freq_curves[i];
        if (sz_tmpbuf < c->hdots)
            sz_tmpbuf = c->hdots;
    }
    sz_tmpbuf = lsp::align_size(sz_tmpbuf * sizeof(float), 0x100);
    const size_t sz_fft = (2 << FFT_RANK_MAX) * sizeof(float);
    const size_t sz_buf = BUF_SIZE * sizeof(float);
    sz_tmpbuf = lsp::lsp_max(sz_tmpbuf, sz_buf);

    // (one aligned arena in the real plugin; separate calloc()s here —
    // allocated once at create, never on the audio thread.)
    h->vFreqApply = (float *)calloc(sz_fft / sizeof(float), sizeof(float));
    h->vTmpBuf    = (float *)calloc(sz_tmpbuf / sizeof(float), sizeof(float));

    for (int i = 0; i < 2; i++) {
        ChannelState &c = h->ch[i];
        c.construct();

        // real init() (loud_comp.cpp:202-207):
        //   c->sDelay.init(1 << (meta::loud_comp_metadata::FFT_RANK_MAX - 1));
        //   c->sProc.init(meta::loud_comp_metadata::FFT_RANK_MAX);
        //   c->sProc.bind(process_callback, this, c);
        //   c->sProc.set_phase(0.5f * i);
        //   c->sEqualizer.init(meta::loud_comp_metadata::FILTER_BANDS, 0);
        //   c->sEqualizer.set_mode(dspu::EQM_IIR);
        c.sDelay.init(1 << (FFT_RANK_MAX - 1));
        c.sProc.init(FFT_RANK_MAX);
        c.sProc.bind(process_callback, h, &c);
        c.sProc.set_phase(0.5f * i);
        c.sEqualizer.init(FILTER_BANDS, 0);
        c.sEqualizer.set_mode(lsp::dspu::EQM_IIR);

        // real init() (loud_comp.cpp:227-234): vDry/vBuffer = BUF_SIZE floats,
        // vBuffer zeroed.
        c.vDry    = (float *)calloc(BUF_SIZE, sizeof(float));
        c.vBuffer = (float *)calloc(BUF_SIZE, sizeof(float));
        lsp::dsp::fill_zero(c.vBuffer, BUF_SIZE);

        // real update_sample_rate(sr) (loud_comp.cpp:331-347), run once here
        // since JamesDSP creates one handle per fixed sample rate (sOsc/
        // sNoise/sInMeter/sOutMeter/sClipInd dropped, see banner):
        //   c->sBypass.init(sr);
        //   c->sEqualizer.set_sample_rate(sr);
        c.sBypass.init((int)sample_rate);   // default 5 ms switch time
        c.sEqualizer.set_sample_rate((size_t)sample_rate);

        // Starts fully bypassed (silent-passthrough disabled state) — matches
        // JamesDSP's zero-init "effect off until configured+enabled"
        // convention for this module (same as the other LSP ports).
        c.sBypass.set_bypass(true);
    }

    h->cfg = LcHandle::Cfg{ 0.0f, 0, 0, 0, 0, 0.0f, 0, 0.0f };
    h->bIdleSkipped = false;
    return h;
}

void lsp_loudcomp_destroy(void *h) {
    if (!h) return;
    auto *hd = (LcHandle *)h;
    hd->ch[0].destroy();
    hd->ch[1].destroy();
    free(hd->vTmpBuf);
    free(hd->vFreqApply);
    delete hd;
}

// Faithful port of loud_comp::update_fft_curve() (loud_comp.cpp:496), minus
// the lsp_trace() dump and the generate_frequencies()/amp-mesh tail (UI).
static void update_fft_curve(LcHandle *hd)
{
    const lsp::freq_curve_t *c = ((hd->nCurve > 0) && (hd->nCurve <= NUM_CURVES)) ? freq_curves[hd->nCurve-1] : NULL;
    size_t fft_size         = 1 << hd->nRank;
    size_t fft_csize        = (fft_size >> 1) + 1;
    float *vTmpBuf          = hd->vTmpBuf;
    float *vFreqApply       = hd->vFreqApply;

    if (c != NULL)
    {
        // Get the volume
        const float vol     = lsp::lsp_limit(hd->fVolume - PHONS_MIN, c->amin, c->amax) - c->amin;

        // Compute interpolation coefficients
        float range         = c->amax - c->amin;
        const float step    = range / (c->curves - 1);
        ssize_t nc          = vol / step;
        if (nc >= ssize_t(c->curves - 1))
            --nc;
        const float k2      = CURVE_APPROX * (vol/step - nc);
        const float k1      = CURVE_APPROX - k2;

        // Interpolate curves to the temporary buffer, translate decibels to gain
        const int16_t *a    = c->data[nc];
        const int16_t *b    = c->data[nc+1];
        for (size_t i=0; i<c->hdots; ++i)
            vTmpBuf[i]      = a[i] * k1 + b[i] * k2;

        lsp::dsp::exp1(vTmpBuf, c->hdots);

        // Compute frequency response
        ssize_t idx;
        float *v            = vFreqApply;
        const float rfmin   = 1.0f / c->fmin;
        const float kf      = float(hd->fSampleRate) / float(fft_size);
        const float kdf     = (c->hdots - 1) / logf(c->fmax * rfmin);
        for (size_t i=0; i < fft_csize; ++i, v += 2)
        {
            float f     = kf * i; // Target frequency
            if (f <= c->fmin)
                idx         = 0;
            else if (f >= c->fmax)
                idx         = c->hdots - 1;
            else
                idx             = logf(f * rfmin) * kdf;

            f           = vTmpBuf[idx];
            v[0]        = f;
            v[1]        = f;
        }

        // Create reverse copy to complete the FFT response
        lsp::dsp::reverse2(&vFreqApply[fft_size+2], &vFreqApply[2], fft_size-2);

        // real: `if (fft_size == 256) { lsp_trace(...) }` — debug trace, dropped.
    }
    else
    {
        const float vol     = lsp::dspu::db_to_gain(hd->fVolume);
        lsp::dsp::fill(vFreqApply, vol, fft_size * 2);
    }

    // real: generate_frequencies(); + "Build amp mesh" loop into vAmpMesh —
    // UI mesh (pMesh/inline_display), dropped.
}

// Faithful port of loud_comp::update_iir_curve() (loud_comp.cpp:574), minus
// the generate_frequencies()/freq_chart()/pcomplex_mod() tail (UI mesh; see
// the banner for why stripping freq_chart() changes no sample).
static void update_iir_curve(LcHandle *hd, uint32_t slope)
{
    const lsp::freq_curve_t *c = ((hd->nCurve > 0) && (hd->nCurve <= NUM_CURVES)) ? freq_curves[hd->nCurve-1] : NULL;
    float *freqs            = hd->vTmpBuf;
    const size_t nFilters   = hd->nFilters;
    float *vGains           = hd->vGains;

    if (c != NULL)
    {
        // Generate the list of frequencies
        const float rfmin   = 1.0f / c->fmin;
        const float df      = logf(c->fmax * rfmin);
        const float kf      = df / (nFilters - 1);
        for (size_t i=0; i < nFilters; ++i)
            freqs[i]            = c->fmin * expf((i + 0.5f) * kf);

        // Get the volume
        const float vol     = lsp::lsp_limit(hd->fVolume - PHONS_MIN, c->amin, c->amax) - c->amin;

        // Compute interpolation coefficients
        float range         = c->amax - c->amin;
        const float step    = range / (c->curves - 1);
        ssize_t nc          = vol / step;
        if (nc >= ssize_t(c->curves - 1))
            --nc;
        const float k2      = CURVE_APPROX * (vol/step - nc);
        const float k1      = CURVE_APPROX - k2;

        // Compute frequency response
        ssize_t idx;
        const int16_t *a    = c->data[nc];
        const int16_t *b    = c->data[nc+1];
        const float kdf     = (c->hdots - 1) / df;

        for (size_t i=0; i < nFilters; ++i)
        {
            const float f       = c->fmin * expf(i * kf);
            if (f <= c->fmin)
                idx                 = 0;
            else if (f >= c->fmax)
                idx                 = c->hdots - 1;
            else
                idx                 = logf(f * rfmin) * kdf;

            vGains[i]           = expf(a[idx] * k1 + b[idx] * k2);
        }
    }
    else
    {
        const float df      = logf(FREQ_MAX / FREQ_MIN);   // real: SPEC_FREQ_MAX / SPEC_FREQ_MIN
        const float kf      = df / (nFilters - 1);
        for (size_t i=0; i < nFilters; ++i)
            freqs[i]            = FREQ_MIN * expf((i + 0.5f) * kf);

        const float vol     = lsp::dspu::db_to_gain(hd->fVolume);
        lsp::dsp::fill(vGains, vol, nFilters);
    }

    if (!hd->bSmooth)
        lsp::dsp::copy(hd->vOldGains, vGains, nFilters);

    // Configure equalizer
    lsp::dspu::filter_params_t fp;
    fp.nType            = lsp::dspu::FLT_NONE;
    fp.nSlope           = slope;
    fp.fFreq            = 0.0f;
    fp.fFreq2           = 0.0f;
    fp.fGain            = GAIN_AMP_0_DB;
    fp.fQuality         = 0.0f;

    for (size_t i=0; i < FILTER_BANDS; ++i)
    {
        if (i >= nFilters) // Disabled filter
            fp.nType        = lsp::dspu::FLT_NONE;
        else if (i == 0) // Low-shelf
        {
            fp.nType        = lsp::dspu::FLT_BT_LRX_LOSHELF;
            fp.fFreq        = freqs[0];
            fp.fFreq2       = fp.fFreq;
        }
        else if (i < nFilters - 1) // Ladder-pass
        {
            fp.nType        = lsp::dspu::FLT_BT_LRX_LADDERPASS;
            fp.fFreq        = freqs[i-1];
            fp.fFreq2       = freqs[i];
        }
        else // High-shelf
        {
            fp.nType        = lsp::dspu::FLT_BT_LRX_HISHELF;
            fp.fFreq        = freqs[i-1];
            fp.fFreq2       = fp.fFreq;
        }

        fp.fGain        = vGains[i];

        for (size_t j=0; j<2; ++j)
            hd->ch[j].sEqualizer.set_params(i, &fp);
    }

    // real: generate_frequencies(); vChannels[0]->sEqualizer.freq_chart(
    //   vFreqApply, vFreqMesh, CURVE_MESH_SIZE); dsp::pcomplex_mod(vAmpMesh, ...)
    //   — UI mesh, dropped (see banner).
}

// Faithful port of loud_comp::update_settings() (loud_comp.cpp:382).
// Audio-thread half of configure(): runs from lsp_loudcomp_process() when a
// new config was published (rt_config.h handoff).
static void loudcomp_apply(LcHandle *hd, float input_gain_db, int mode_i, int std_i,
                           int fft_rank_idx, int approx_i, float volume_phons,
                           int hclip_i, float hcrange_db)
{
    // Hardening (same rationale as the other ports): the real plugin's host
    // clamps every port to its metadata range; JamesDSP's AIDL/Lua surface
    // does not. `!(x >= lo)` form also catches NaN.
    if (!(input_gain_db == input_gain_db)) input_gain_db = 0.0f;            // NaN -> 0 dB (GAIN_AMP_0_DB)
    float in_gain = lsp::dspu::db_to_gain(input_gain_db);                   // real "input" port is LINEAR (U_GAIN_AMP)
    if (!(in_gain >= GAIN_AMP_M_INF_DB)) in_gain = GAIN_AMP_M_INF_DB;      // AMP_GAIN min = 0.0 linear
    if (in_gain > INPUT_GAIN_MAX) in_gain = INPUT_GAIN_MAX;                 // AMP_GAIN max = +72 dB
    mode_i      = (mode_i != 0) ? 1 : 0;                                    // COMBO "mode": 0 FFT, 1 IIR
    if (std_i < 0) std_i = 0;                                               // COMBO "std": 0..NUM_CURVES
    if (std_i > (int)NUM_CURVES) std_i = (int)NUM_CURVES;
    if (fft_rank_idx < 0) fft_rank_idx = 0;                                 // COMBO "fft": 0..6
    if (fft_rank_idx > (int)(FFT_RANK_MAX - FFT_RANK_MIN)) fft_rank_idx = (int)(FFT_RANK_MAX - FFT_RANK_MIN);
    if (approx_i < 0) approx_i = 0;                                         // COMBO "approx": 0..4
    if (approx_i > (int)(NUM_APPROX - 1)) approx_i = (int)(NUM_APPROX - 1);
    if (!(volume_phons == volume_phons)) volume_phons = PHONS_DFL;          // NaN -> PHONS_DFL
    if (volume_phons < PHONS_MIN) volume_phons = PHONS_MIN;
    if (volume_phons > PHONS_MAX) volume_phons = PHONS_MAX;
    hclip_i     = (hclip_i != 0) ? 1 : 0;
    if (!(hcrange_db >= HCRANGE_MIN)) hcrange_db = HCRANGE_MIN;             // also NaN
    if (hcrange_db > HCRANGE_MAX) hcrange_db = HCRANGE_MAX;

    hd->cfg = LcHandle::Cfg{ input_gain_db, mode_i, std_i, fft_rank_idx, approx_i, volume_phons, hclip_i, hcrange_db };

    // real (loud_comp.cpp:384-395):
    //   const bool rst_clip     = pHClipReset->value() >= 0.5f;   — indicator reset (TRIGGER "hcclean"), dropped
    //   const bool bypass       = pBypass->value() >= 0.5f;       — carried by lsp_loudcomp_process()'s `enabled`
    //   const uint32_t mode     = pMode->value();
    //   const uint32_t curve    = pCurve->value();
    //   uint32_t rank           = FFT_RANK_MIN + ssize_t(pRank->value());
    //   rank                    = lsp_limit(rank, FFT_RANK_MIN, FFT_RANK_MAX);
    //   const float volume      = pVolume->value();
    //   const bool relative     = pRelative->value() >= 0.5f;     — UI mesh, dropped
    //   const bool reference    = pReference->value() >= 0.5f;    — pinned false, dropped
    //   const uint32_t iapprox  = pApproximation->value();
    //   const uint32_t filters  = approx_presets[iapprox].nFilters;
    //   const uint32_t slope    = approx_presets[iapprox].nSlope;
    const uint32_t mode     = (uint32_t)mode_i;
    const uint32_t curve    = (uint32_t)std_i;
    uint32_t rank           = (uint32_t)(FFT_RANK_MIN + (ssize_t)fft_rank_idx);
    rank                    = lsp::lsp_limit(rank, (uint32_t)FFT_RANK_MIN, (uint32_t)FFT_RANK_MAX);
    const float volume      = volume_phons;
    const uint32_t iapprox  = (uint32_t)approx_i;
    const uint32_t filters  = approx_presets[iapprox].nFilters;
    const uint32_t slope    = approx_presets[iapprox].nSlope;

    // Need to update curve?  (loud_comp.cpp:398-407)
    if (mode != hd->nMode)
    {
        for (size_t i=0; i<2; ++i)
        {
            ChannelState *c = &hd->ch[i];
            c->sDelay.clear();
            c->sProc.reset();
            c->sEqualizer.reset();
        }
    }

    if (mode == MODE_FFT)   // (loud_comp.cpp:409-422)
    {
        if ((mode != hd->nMode) || (curve != hd->nCurve) || (rank != hd->nRank) || (volume != hd->fVolume))
        {
            hd->nMode           = mode;
            hd->nCurve          = curve;
            hd->nRank           = rank;
            hd->fVolume         = volume;
            // real: bSyncMesh = true; — UI, dropped
            hd->bSmooth         = false;

            update_fft_curve(hd);
        }
    }
    else // mode == MODE_IIR  (loud_comp.cpp:423-436)
    {
        if ((mode != hd->nMode) || (curve != hd->nCurve) || (filters != hd->nFilters) || (volume != hd->fVolume))
        {
            hd->bSmooth         = (hd->nMode == mode) && (hd->nCurve == curve) && (hd->nFilters == filters);
            hd->nMode           = mode;
            hd->nCurve          = curve;
            hd->nFilters        = filters;
            hd->fVolume         = volume;
            // real: bSyncMesh = true; — UI, dropped

            update_iir_curve(hd, slope);
        }
    }

    // real (loud_comp.cpp:438-454):
    //   if (reference != bReference) sOsc.reset_phase_accumulator();   — generator, dropped
    //   if (bRelative != relative) bSyncMesh = true;                    — UI, dropped
    //   if ((bypass != bBypass) || (bSyncMesh)) pWrapper->query_display_draw(); — UI, dropped
    //   fGain = pGain->value(); bHClipOn = pHClipOn->value() >= 0.5f;
    //   bBypass = bypass; bRelative = relative; bReference = reference;
    //   enGenerator = decode_generator(pGenerator->value());
    //   sNoise.set_amplitude(get_generator_amplitude(enGenerator, nChannels > 1)); — generator, dropped
    hd->fGain               = in_gain;
    hd->bHClipOn            = (hclip_i != 0);

    // (loud_comp.cpp:456-469)
    if (hd->bHClipOn)
    {
        const float range   = lsp::dspu::db_to_gain(hcrange_db);
        float min, max;
        if (hd->nMode == MODE_FFT)
        {
            lsp::dsp::abs_minmax(hd->vFreqApply, 2 << hd->nRank, &min, &max);
            hd->fHClipLvl       = range * sqrtf(min * max);
        }
        else
            hd->fHClipLvl       = range * lsp::dsp::max(hd->vGains, hd->nFilters);
    }
    else
        hd->fHClipLvl       = 1.0f;

    // (loud_comp.cpp:471-482) — note the real plugin reads sProc.latency()
    // BEFORE sProc.set_rank(rank) in the same iteration, i.e. the dry-path
    // delay follows the PREVIOUS rank until the next update_settings().
    // Kept verbatim (hard rule), not "fixed".
    for (size_t i=0; i<2; ++i)
    {
        ChannelState *c         = &hd->ch[i];
        const size_t latency    = (hd->nMode == MODE_FFT) ? c->sProc.latency() : c->sEqualizer.get_latency();

        // real: c->sBypass.set_bypass(bypass); — made in lsp_loudcomp_process().
        c->sProc.set_rank(rank);

        c->sDelay.set_delay(latency);
        // real: if (rst_clip) c->bHClip = false; — indicator, dropped
    }
}

void lsp_loudcomp_configure(void *h, float input_gain_db, int mode, int std,
                            int fft_rank_idx, int approx, float volume_phons,
                            int hclip, float hcrange_db) {
    if (!h) return;
    auto *hd = (LcHandle *)h;
    // Sanitise + publish only (control thread). Range clamping happens in
    // loudcomp_apply() on the audio thread so the applied values are the
    // single source of truth; here just make sure nothing non-finite is
    // published that peek() would hand back to a Lua get->modify->set loop.
    if (!(input_gain_db == input_gain_db)) input_gain_db = 0.0f;        // GAIN_AMP_0_DB
    if (!(volume_phons == volume_phons))   volume_phons = PHONS_DFL;
    if (!(hcrange_db == hcrange_db))       hcrange_db = HCRANGE_DFL;
    LcHandle::Cfg c = { input_gain_db, (mode != 0) ? 1 : 0, std, fft_rank_idx, approx,
                        volume_phons, (hclip != 0) ? 1 : 0, hcrange_db };
    hd->rt.publish(c);   // applied by loudcomp_apply() on the audio thread's next block
}

// Faithful port of loud_comp::process_iir_equalizer() (loud_comp.cpp:748).
static void process_iir_equalizer(LcHandle *hd, ChannelState *c, size_t samples)
{
    // Process the signal by the equalizer
    if (hd->bSmooth)
    {
        lsp::dspu::filter_params_t fp;
        const float den   = 1.0f / samples;

        // In smooth mode, we need to update filter parameters for each sample
        for (size_t offset=0; offset<samples; )
        {
            const size_t count          = lsp::lsp_min(samples - offset, EQ_SMOOTH_STEP);
            const float k               = float(offset) * den;

            // Tune filters
            for (size_t j=0; j < hd->nFilters; ++j)
            {
                c->sEqualizer.get_params(j, &fp);
                fp.fGain                    = hd->vOldGains[j] * expf(logf(hd->vGains[j] / hd->vOldGains[j])*k);
                c->sEqualizer.set_params(j, &fp);
            }

            // Apply processing
            c->sEqualizer.process(&c->vBuffer[offset], &c->vBuffer[offset], count);
            offset                     += count;
        }
    }
    else
        c->sEqualizer.process(c->vBuffer, c->vBuffer, samples);
}

void lsp_loudcomp_process(void *h, float *left, float *right, unsigned n, int enabled) {
    if (!h || n == 0) return;
    auto *hd = (LcHandle *)h;
    {
        LcHandle::Cfg c;
        if (hd->rt.take(c))
            loudcomp_apply(hd, c.input_gain_db, c.mode, c.std, c.fft_rank_idx, c.approx,
                           c.volume_phons, c.hclip, c.hcrange_db);
    }
    ChannelState &c0 = hd->ch[0];
    ChannelState &c1 = hd->ch[1];

    // real: c->sBypass.set_bypass(bypass) inside update_settings(); see
    // loudcomp_apply()'s comment on why this lives here instead.
    const bool bypass_req = !enabled;
    c0.sBypass.set_bypass(bypass_req);
    c1.sBypass.set_bypass(bypass_req);

    // Documented zero-cost contract (lsp_wrap_loudcomp.h): once both
    // channels' Bypass have fully settled off, skip processing entirely
    // rather than running the FFT/IIR path on an inaudible wet signal.
    if (c0.sBypass.on() && c1.sBypass.on()) {
        hd->bIdleSkipped = true;
        return;
    }
    if (hd->bIdleSkipped) {
        // Coming back from the skipped state: the delay line / FFT overlap
        // buffers / IIR state hold stale audio from before the skip. Reset
        // them the way the real plugin resets on a mode switch
        // (loud_comp.cpp:398-407) so the wet path refills from silence
        // instead of replaying old samples into the crossfade.
        for (ChannelState *c : { &c0, &c1 }) {
            c->sDelay.clear();
            c->sProc.reset();
            c->sEqualizer.reset();
        }
        hd->bIdleSkipped = false;
    }

    // real process() (loud_comp.cpp:875-888): bind ports (vIn/vOut pointers;
    // fInLevel/fOutLevel/fInLufs/fOutLufs meter resets dropped), then
    // `if (bReference) generate_signal(samples); else process_audio(samples);`
    // — bReference is pinned false, so this is process_audio() (loud_comp.cpp:779).
    c0.vIn = c0.vOut = left;
    c1.vIn = c1.vOut = right;

    const size_t samples = n;
    for (size_t offset = 0; offset < samples; )
    {
        size_t to_process   = lsp::lsp_min(samples - offset, BUF_SIZE);

        // Pre-process input signal  (loud_comp.cpp:787-799)
        for (size_t i=0; i<2; ++i)
        {
            ChannelState *c = &hd->ch[i];

            // Process the signal
            c->sDelay.process(c->vDry, c->vIn, to_process);

            // Apply input gain
            lsp::dsp::mul_k3(c->vBuffer, c->vIn, hd->fGain, to_process);
            // real: lvl = dsp::abs_max(c->vBuffer, to_process); c->fInLevel = lsp_max(...) — meter, dropped
        }

        // real: "Measure input loudness" sInMeter block (loud_comp.cpp:801-806) — meter, dropped

        // Do the loudness compensation  (loud_comp.cpp:808-846)
        for (size_t i=0; i<2; ++i)
        {
            ChannelState *c = &hd->ch[i];

            // Apply volume attenuation
            if (hd->nMode == MODE_FFT)
                c->sProc.process(c->vBuffer, c->vBuffer, to_process);
            else
                process_iir_equalizer(hd, c, to_process);

            // Perform clipping
            // real: lvl = dsp::abs_max(c->vBuffer, to_process); c->sClipInd.process(to_process);
            //       bool clip = lvl > fHClipLvl;
            //       if (bHClipOn) { if (clip) { lvl = fHClipLvl; c->bHClip = true;
            //           dsp::limit1(c->vBuffer, -fHClipLvl, +fHClipLvl, to_process); }
            //           c->pHClipInd->set_value(...); }
            //       else { if (clip) c->sClipInd.blink(); c->pHClipInd->set_value(...); }
            //       c->fOutLevel = ...;
            // With the indicator/meter consumers stripped, the abs_max() only
            // matters when it gates limit1(), so it is evaluated only then.
            if (hd->bHClipOn)
            {
                const float lvl = lsp::dsp::abs_max(c->vBuffer, to_process);
                const bool clip = lvl > hd->fHClipLvl;
                if (clip)
                    lsp::dsp::limit1(c->vBuffer, -hd->fHClipLvl, +hd->fHClipLvl, to_process);
            }

            // Apply bypass
            c->sBypass.process(c->vOut, c->vDry, c->vBuffer, to_process);
        }

        // Reset smooth flag for equalizer  (loud_comp.cpp:849-853)
        if (hd->bSmooth)
        {
            lsp::dsp::copy(hd->vOldGains, hd->vGains, hd->nFilters);
            hd->bSmooth         = false;
        }

        // real: "Measure output loudness" sOutMeter block (loud_comp.cpp:855-860) — meter, dropped

        // Update sample counter and pointers  (loud_comp.cpp:862-871)
        for (size_t i=0; i<2; ++i)
        {
            ChannelState *c = &hd->ch[i];
            c->vIn         += to_process;
            c->vOut        += to_process;
        }
        offset         += to_process;
    }

    // real: everything after process_audio() in process() — meter ports,
    // set_latency() (-> lsp_loudcomp_get_latency()), mesh sync — is host/UI
    // plumbing, dropped.
}

int lsp_loudcomp_get_latency(void *h) {
    if (!h) return 0;
    // real process() (loud_comp.cpp:910): set_latency(vChannels[0]->sDelay.get_delay());
    return (int)((LcHandle *)h)->ch[0].sDelay.get_delay();
}

void lsp_loudcomp_get_config(void *h, float *input_gain_db, int *mode, int *std,
                             int *fft_rank_idx, int *approx, float *volume_phons,
                             int *hclip, float *hcrange_db) {
    if (!h) {
        if (input_gain_db) *input_gain_db = 0.0f;
        if (mode) *mode = 0;
        if (std) *std = 0;
        if (fft_rank_idx) *fft_rank_idx = 0;
        if (approx) *approx = 0;
        if (volume_phons) *volume_phons = 0.0f;
        if (hclip) *hclip = 0;
        if (hcrange_db) *hcrange_db = 0.0f;
        return;
    }
    auto *hd = (LcHandle *)h;
    const LcHandle::Cfg c = hd->rt.peek();   // last published, not last applied
    if (input_gain_db) *input_gain_db = c.input_gain_db;
    if (mode) *mode = c.mode;
    if (std) *std = c.std;
    if (fft_rank_idx) *fft_rank_idx = c.fft_rank_idx;
    if (approx) *approx = c.approx;
    if (volume_phons) *volume_phons = c.volume_phons;
    if (hclip) *hclip = c.hclip;
    if (hcrange_db) *hcrange_db = c.hcrange_db;
}
