// lsp_wrap_limiter.cpp — faithful port of the real LSP Limiter PLUGIN's DSP
// logic (lsp-limiter/src/main/plug/limiter.cpp, class def in
// lsp-limiter/include/private/plugins/limiter.h) into JamesDSP's calling
// convention, per LSP_FAITHFUL_PORT_SCOPE.md.
//
// This is NOT a wrapper around the bare lsp::dspu::Limiter primitive class.
// update_settings()/process() below are a transliteration of the real
// plugin's methods of the same name: same member names (sOver, sScOver,
// sLimit, sDataDelay, sDryDelay, sBypass, sDither), same order of
// operations, same oversampling / lookahead-delay-compensation / gain-curve
// math. Every place this file diverges from the literal real-code line is
// called out in a comment at that line, with the reason.
//
// What's stripped, per scope.md's "only strip host-plumbing/UI" rule:
//   - plug::IPort* fields and pXxx->value() reads -> replaced by this file's
//     own fixed defaults or lsp_limiter_configure()'s arguments (JamesDSP's
//     AIDL/cmd-1500 surface has no UI for lookahead, ALR, boost, stereo-link
//     %, oversampling-mode select, dithering-bits select, or gain trims —
//     see the "unexposed real-plugin parameters" block below for the exact
//     default chosen for each and why).
//   - plug::Factory / metadata registration (plugin_factory, plugin_settings,
//     meta::plugin_t) — no plugin host here.
//   - IDisplay / MeterGraph (sGraph) / Blink (sBlink) / inline_display() /
//     mesh history buffers — UI-only oscilloscope/graph code.
//   - sPremix and the SCM_EXTERNAL / SCM_LINK branches of the sidechain-mode
//     switch, and pScMode itself — per scope.md's explicit "internal
//     sidechain only" decision. Internal sidechain (SCM_INTERNAL, the
//     module's own input feeding its own detector) is the only mode ported;
//     that's the only mode JamesDSP's call surface has ever used.
//   - Mono/LR/MS variants — per scope.md's "stereo mode only" decision
//     (nChannels is hardcoded to 2 here, matching the real plugin's
//     GM_STEREO / CM_STEREO analog: `stereo` ctor arg == true).
//   - dump() (state-dumper introspection, plugin-host debugging only).
//
// What's explicitly IN scope and ported, per scope.md's own words ("real
// audio-quality DSP (true-peak limiting), not a host-plumbing feature"):
//   - sOver / sScOver (dspu::Oversampler) — signal and sidechain
//     oversampling, including the real true_peak_modes[] table and
//     get_oversampling_mode()/get_filtering() logic.
//   - sDataDelay / sDryDelay (dspu::Delay) — lookahead delay compensation
//     for the oversampled data path and the dry/bypass path respectively.
//   - sLimit (dspu::Limiter) — the actual gain-computer core.
//   - sBypass (dspu::Bypass) — sample-accurate crossfade (present, but
//     pinned permanently un-bypassed: this call surface's lsp_limiter_
//     process() has no `enabled` argument, matching its tail/last-line-of-
//     defense calling position documented in LSP_FAITHFUL_PORT_SCOPE.md).
//   - sDither (dspu::Dither) — present, defaulted to DITHER_NONE (0 bits,
//     a real no-op) matching the real plugin's own DITHER_DEFAULT.
//
// Public surface (extern "C", declared in lsp_wrap_limiter.h) is
// byte-for-byte identical to lsp_wrap.h's current Limiter section:
//   lsp_limiter_create(float sample_rate)
//   lsp_limiter_destroy(void*)
//   lsp_limiter_configure(void*, float threshold_db, float attack_ms,
//                          float release_ms, float knee_db)
//   lsp_limiter_process(void*, float *left, float *right, unsigned n)
//   lsp_limiter_get_reduction_db(void*) / lsp_limiter_is_limiting(void*)
//   lsp_limiter_get_config(void*, float*, float*, float*, float*)
// so jdspController.c's tail-position `lsp_limiter_process(jdsp->
// limiter_handle, ...)` call and jamesdsp.c's cmd-1500 handler don't need to
// change once this file replaces the old LimiterHandle/lsp_limiter_* block
// in lsp_wrap.cpp (that removal + CMakeLists.txt wiring is a separate step,
// intentionally not done by this file per the file-isolation instruction).
//
// BUILD-WIRING NOTE for whoever integrates this: lsp-dsp-units/src/main/
// util/Oversampler.cpp, Dither.cpp and Randomizer.cpp (Dither's PRNG) are
// NOT currently in CMakeLists.txt's LSP_DSP_SOURCES glob (only Compressor/
// Gate/Limiter/DynamicProcessor/Delay/Sidechain/RawRingBuffer/Bypass/
// filters are). All three need adding for this file to link — Oversampler.h
// itself was already includable (header-only visible from the existing
// include dirs) but its .cpp was never compiled in. This file also relies
// on lsp_wrap.cpp's existing ARM32/x86 dsp.cpp dispatch-table stub
// definitions (lsp::arm::/lsp::neon_d32::/lsp::x86::) — do NOT duplicate
// those here, they'd collide at link time; they're already satisfied once
// both .cpp files are linked into the same jamesdsp/jamesdsp_static target.
// Also: this file's lsp_limiter_* symbols are the same names lsp_wrap.cpp's
// old LimiterHandle section currently defines — that section (roughly its
// lines 389-520, the "// --- Limiter ---" block) must be deleted from
// lsp_wrap.cpp before adding this file to CMakeLists.txt, or the link will
// fail on duplicate symbols.

#include "lsp_wrap_limiter.h"
#include "rt_config.h"

#include <lsp-plug.in/dsp-units/util/Oversampler.h>
#include <lsp-plug.in/dsp-units/dynamics/Limiter.h>
#include <lsp-plug.in/dsp-units/util/Delay.h>
#include <lsp-plug.in/dsp-units/ctl/Bypass.h>
#include <lsp-plug.in/dsp-units/util/Dither.h>
#include <lsp-plug.in/dsp-units/units.h>
#include <lsp-plug.in/dsp-units/const.h>
#include <lsp-plug.in/common/utility.h>     // lsp::lsp_min/lsp_max
#include <lsp-plug.in/dsp/dsp.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

// lsp::dsp::init() populates the dsp:: function-pointer dispatch table
// (fill_zero, copy, mul_k3, fmmul_k3, min, max, and everything Oversampler/
// Limiter/Delay/Bypass/Dither call internally) — every pointer is NULL
// until this runs once, in ANY translation unit (it's a process-global
// table). lsp_wrap.cpp already calls this from its own ensure_dsp_init(),
// but that's a different TU with no ordering guarantee relative to this
// one's first exported call, so this file needs its own guard too (the
// underlying lsp::dsp::init() call itself is idempotent/safe to repeat).
static inline void ensure_dsp_init() {
    static bool done = (lsp::dsp::init(), true);
    (void) done;
}

namespace {

using lsp::dspu::over_mode_t;
using lsp::dspu::limiter_mode_t;

// ---------------------------------------------------------------------
// Real limiter.cpp's chunking constants (file-scope, limiter.cpp:36-37).
// LIMIT_BUFSIZE is the ceiling, in the OVERSAMPLED sample domain, that
// each of the four per-channel scratch buffers below is sized to; the
// process() loop below picks its chunk size so that ceiling is never
// exceeded regardless of the current oversampling ratio.
// ---------------------------------------------------------------------
constexpr size_t LIMIT_BUFSIZE     = 8192;
constexpr size_t LIMIT_BUFMULTIPLE = 16;

// ---------------------------------------------------------------------
// Mirrors the numeric constants of meta::limiter_metadata (lsp-limiter/
// include/private/meta/limiter.h) that update_settings()/init() actually
// need. Not pulled in via that header directly because it drags in
// lsp-plug.in/plug-fw/meta/types.h + const.h (plugin-host metadata
// registration types/MAX_SAMPLE_RATE) — exactly the host-plumbing layer
// scope.md says to strip. These are the real DSP-relevant numeric ranges,
// copied verbatim.
// ---------------------------------------------------------------------
constexpr size_t LIMITER_OVERSAMPLING_MAX = 8;      // meta::limiter_metadata::OVERSAMPLING_MAX
constexpr float  LIMITER_LOOKAHEAD_MAX    = 20.0f;  // meta::limiter_metadata::LOOKAHEAD_MAX
constexpr float  LIMITER_LOOKAHEAD_DFL    = 5.0f;   // meta::limiter_metadata::LOOKAHEAD_DFL
constexpr float  LIMITER_ALR_ATTACK_DFL   = 5.0f;   // meta::limiter_metadata::ALR_ATTACK_TIME_DFL
constexpr float  LIMITER_ALR_RELEASE_DFL  = 50.0f;  // meta::limiter_metadata::ALR_RELEASE_TIME_DFL
constexpr float  LIMITER_KNEE_SMOOTH_DFL  = -5.0f;  // meta::limiter_metadata::KNEE_SMOOTH_DFL [dB]

// meta::limiter_metadata::oversampling_mode_t, dithering_t, limiter_mode_t
// — local copies of the real enums (same names/values), since the real
// ones live behind the plug-fw metadata header pulled in above. Kept as
// full enums (not trimmed to only the values we use) so get_oversampling_
// mode()/get_filtering()/get_dithering()/get_limiter_mode() below can stay
// a verbatim port of the real switch bodies rather than a hand-simplified
// re-derivation.
enum oversampling_mode_t {
    OVS_NONE,
    OVS_HALF_2X16BIT, OVS_HALF_2X24BIT, OVS_HALF_3X16BIT, OVS_HALF_3X24BIT,
    OVS_HALF_4X16BIT, OVS_HALF_4X24BIT, OVS_HALF_6X16BIT, OVS_HALF_6X24BIT,
    OVS_HALF_8X16BIT, OVS_HALF_8X24BIT,
    OVS_FULL_2X16BIT, OVS_FULL_2X24BIT, OVS_FULL_3X16BIT, OVS_FULL_3X24BIT,
    OVS_FULL_4X16BIT, OVS_FULL_4X24BIT, OVS_FULL_6X16BIT, OVS_FULL_6X24BIT,
    OVS_FULL_8X16BIT, OVS_FULL_8X24BIT,
    OVS_TRUE_PEAK_16BIT, OVS_TRUE_PEAK_24BIT,
};

enum dithering_t {
    DITHER_NONE,
    DITHER_7BIT, DITHER_8BIT, DITHER_11BIT, DITHER_12BIT,
    DITHER_15BIT, DITHER_16BIT, DITHER_23BIT, DITHER_24BIT,
};

enum limiter_mode_sel_t {
    LOM_HERM_THIN, LOM_HERM_WIDE, LOM_HERM_TAIL, LOM_HERM_DUCK,
    LOM_EXP_THIN,  LOM_EXP_WIDE,  LOM_EXP_TAIL,  LOM_EXP_DUCK,
    LOM_LINE_THIN, LOM_LINE_WIDE, LOM_LINE_TAIL, LOM_LINE_DUCK,
};

// Real limiter.cpp's true_peak_modes[] table (:84-93), ported verbatim.
struct true_peak_mode_t {
    uint32_t   frequency;
    over_mode_t modes[2];
};

static const true_peak_mode_t true_peak_modes[] = {
    { 0,      { lsp::dspu::OM_LANCZOS_8X16BIT, lsp::dspu::OM_LANCZOS_8X24BIT } },
    { 22050,  { lsp::dspu::OM_LANCZOS_8X16BIT, lsp::dspu::OM_LANCZOS_8X24BIT } },
    { 29400,  { lsp::dspu::OM_LANCZOS_6X16BIT, lsp::dspu::OM_LANCZOS_6X24BIT } },
    { 44100,  { lsp::dspu::OM_LANCZOS_4X16BIT, lsp::dspu::OM_LANCZOS_4X24BIT } },
    { 58800,  { lsp::dspu::OM_LANCZOS_3X16BIT, lsp::dspu::OM_LANCZOS_3X24BIT } },
    { 88200,  { lsp::dspu::OM_LANCZOS_2X16BIT, lsp::dspu::OM_LANCZOS_2X24BIT } },
    { 176400, { lsp::dspu::OM_NONE,            lsp::dspu::OM_NONE            } },
};

// Real limiter::get_oversampling_mode() (limiter.cpp:410-451), ported
// verbatim (L_KEY macro included) against the local enum above. `mode` is
// always OVS_TRUE_PEAK_16BIT when called from this file today — see the
// "unexposed real-plugin parameters" note in lsp_limiter_configure() for
// why — but the full switch is kept intact rather than trimmed, both for
// mechanical fidelity to the real file and so a future oversampling-mode
// selector just has to plumb a value in here.
static over_mode_t get_oversampling_mode(size_t mode, uint32_t sample_rate) {
    #define L_KEY(x) \
        case OVS_HALF_ ## x: \
        case OVS_FULL_ ## x: \
            return lsp::dspu::OM_LANCZOS_ ## x;

    switch (mode) {
        L_KEY(2X16BIT)
        L_KEY(2X24BIT)
        L_KEY(3X16BIT)
        L_KEY(3X24BIT)
        L_KEY(4X16BIT)
        L_KEY(4X24BIT)
        L_KEY(6X16BIT)
        L_KEY(6X24BIT)
        L_KEY(8X16BIT)
        L_KEY(8X24BIT)

        case OVS_TRUE_PEAK_16BIT:
        case OVS_TRUE_PEAK_24BIT:
        {
            const size_t index = (mode == OVS_TRUE_PEAK_24BIT) ? 1 : 0;
            size_t best = 0;

            for (size_t i = 1; i < sizeof(true_peak_modes) / sizeof(true_peak_mode_t); ++i) {
                if (true_peak_modes[i].frequency <= sample_rate)
                    best = i;
            }
            return true_peak_modes[best].modes[index];
        }

        case OVS_NONE:
        default:
            return lsp::dspu::OM_NONE;
    }
    #undef L_KEY
}

// Real limiter::get_filtering() (limiter.cpp:453-456), ported verbatim.
static bool get_filtering(size_t mode) {
    return (mode >= OVS_FULL_2X16BIT) && (mode <= OVS_FULL_8X24BIT);
}

// Real limiter::get_limiter_mode() (limiter.cpp:458-493), ported verbatim.
static limiter_mode_t get_limiter_mode(size_t mode) {
    switch (mode) {
        case LOM_HERM_THIN: return lsp::dspu::LM_HERM_THIN;
        case LOM_HERM_WIDE: return lsp::dspu::LM_HERM_WIDE;
        case LOM_HERM_TAIL: return lsp::dspu::LM_HERM_TAIL;
        case LOM_HERM_DUCK: return lsp::dspu::LM_HERM_DUCK;

        case LOM_EXP_THIN: return lsp::dspu::LM_EXP_THIN;
        case LOM_EXP_WIDE: return lsp::dspu::LM_EXP_WIDE;
        case LOM_EXP_TAIL: return lsp::dspu::LM_EXP_TAIL;
        case LOM_EXP_DUCK: return lsp::dspu::LM_EXP_DUCK;

        case LOM_LINE_THIN: return lsp::dspu::LM_LINE_THIN;
        case LOM_LINE_WIDE: return lsp::dspu::LM_LINE_WIDE;
        case LOM_LINE_TAIL: return lsp::dspu::LM_LINE_TAIL;
        case LOM_LINE_DUCK: return lsp::dspu::LM_LINE_DUCK;

        default:
            break;
    }
    return lsp::dspu::LM_HERM_THIN;
}

// Real limiter::get_dithering() (limiter.cpp:495-521), ported verbatim.
static size_t get_dithering(size_t mode) {
    switch (mode) {
        case DITHER_7BIT:  return 7;
        case DITHER_8BIT:  return 8;
        case DITHER_11BIT: return 11;
        case DITHER_12BIT: return 12;
        case DITHER_15BIT: return 15;
        case DITHER_16BIT: return 16;
        case DITHER_23BIT: return 23;
        case DITHER_24BIT: return 24;

        case DITHER_NONE:
        default:
            return 0;
    }
}

// Real channel_t (limiter.h:91-125), trimmed of the plug::IPort*/vSc/
// vShmIn (external+link sidechain source pointers — out of scope) and
// sGraph/bVisible/pMeter/pGraph (UI metering/history — out of scope)
// fields. vDataBuf/vScBuf/vGainBuf/vOutBuf keep the real names and the
// real LIMIT_BUFSIZE sizing (limiter.cpp init(), :219-222).
struct channel_t {
    lsp::dspu::Bypass      sBypass;
    lsp::dspu::Oversampler sOver;      // Oversampler object for signal
    lsp::dspu::Oversampler sScOver;    // Sidechain oversampler object for signal
    lsp::dspu::Limiter     sLimit;
    lsp::dspu::Delay       sDataDelay; // Input signal delay
    lsp::dspu::Delay       sDryDelay;  // Dry delay

    float *vDataBuf = nullptr;  // Audio data buffer (oversampled)
    float *vScBuf   = nullptr;  // Sidechain buffer
    float *vGainBuf = nullptr;  // Applying gain buffer
    float *vOutBuf  = nullptr;  // Output buffer
};

} // namespace

// Real limiter class's non-port state (limiter.h:127-171), trimmed to what
// this call surface actually drives: nChannels is fixed at 2 (stereo-only,
// per scope.md), bSidechain is fixed false (internal sidechain only, per
// scope.md — SCM_EXTERNAL/SCM_LINK never reachable so nScMode isn't even
// stored), bPause/bClear/bScListen/vTime/vIDisplay/pIDisplay/bUISync/
// sPremix/pIDisplay are all UI-only and dropped entirely.
struct LimiterPortHandle {
    channel_t ch[2];
    lsp::dspu::Dither sDither;

    float fSampleRate  = 0.0f;
    float fInGain      = GAIN_AMP_0_DB;    // Input gain
    float fOutGain     = GAIN_AMP_0_DB;    // Output gain
    float fPreamp      = GAIN_AMP_0_DB;    // Sidechain pre-amplification
    float fStereoLink  = 1.0f;             // Stereo linking (0..1); fixed 100%
                                            // (LINKING_DFL) — no per-channel-
                                            // independence UI on this call
                                            // surface.

    // Last-configured parameters in caller units (dB/ms, matching what
    // lsp_limiter_configure()/lsp_limiter_get_config() pass/return).
    float cfg_threshold_db = 0.0f;
    float cfg_attack_ms    = 0.0f;
    float cfg_release_ms   = 0.0f;
    float cfg_knee_db      = 0.0f;
    struct Cfg { float threshold_db, attack_ms, release_ms, knee; };
    RtConfig<Cfg> rt;   // control->audio handoff, see rt_config.h

    // Deepest VCA gain reduction applied over the last processed block, dB.
    float cur_reduction_db = 0.0f;
};

namespace {

// Real limiter::sync_latency() (limiter.cpp:548-558), ported. The real
// version also calls plug::Module::set_latency(latency) to report
// processing delay back to the plugin host for PDC — that's the one
// genuinely host-only piece of this function (no host to report to here),
// dropped. The dry-delay compensation itself (the actual DSP reason this
// function exists) is kept and applied to both channels.
static void sync_latency(LimiterPortHandle *h) {
    channel_t &c = h->ch[0];
    size_t latency = c.sLimit.get_latency() / c.sScOver.get_oversampling()
                      + c.sScOver.latency();

    h->ch[0].sDryDelay.set_delay(latency);
    h->ch[1].sDryDelay.set_delay(latency);
}

} // namespace

void *lsp_limiter_create(float sample_rate) {
    ensure_dsp_init();

    auto *h = new LimiterPortHandle();
    h->fSampleRate = sample_rate;

    // Real init() (limiter.cpp:172-343) sizes sLimit/sDataDelay/sDryDelay
    // against a ceiling of MAX_SAMPLE_RATE (384000, the generic plugin-host
    // cap from plug-fw/const.h) * OVERSAMPLING_MAX, because a real plugin's
    // sample rate can change at any time after construction (host-driven).
    // This wrapper's sample rate is fixed for the handle's whole lifetime
    // (lsp_limiter_create's only parameter), so the ceiling is scoped to
    // THIS instance's actual sample_rate instead of the generic plugin-host
    // maximum — avoids pulling in plug-fw's MAX_SAMPLE_RATE entirely while
    // sizing every buffer at least as large as this instance will ever use.
    const size_t max_sr = (size_t) (sample_rate * (float) LIMITER_OVERSAMPLING_MAX);

    // Real lk_latency expression (limiter.cpp:205-206), ported verbatim
    // including its apparent unit quirk: samples_to_millis(sr, X) is being
    // fed OVERSAMPLING_MAX (a ratio, 8) as if it were a sample COUNT, which
    // at any real sample rate is a sub-millisecond contribution — the term
    // is dwarfed by LOOKAHEAD_MAX+1.0f either way. Flagged, not silently
    // "fixed": scope.md's hard rule is to carry the real math over as-is.
    const float lk_latency = (float) (int) lsp::dspu::samples_to_millis(
            (float) max_sr, (float) LIMITER_OVERSAMPLING_MAX)
            + LIMITER_LOOKAHEAD_MAX + 1.0f;

    bool ok = true;
    for (int i = 0; i < 2; ++i) {
        channel_t &c = h->ch[i];

        ok = ok && c.sOver.init();
        ok = ok && c.sScOver.init();
        c.sOver.set_sample_rate((size_t) sample_rate);
        c.sScOver.set_sample_rate((size_t) sample_rate);

        // Real limiter::update_sample_rate() (limiter.cpp:394) — the
        // limiter mode is set once here (not per-configure); it's real DSP
        // state (interpolation shape), left permanently at the plugin's own
        // LOM_DEFAULT rather than exposed, since lsp_limiter_configure()
        // has no mode selector.
        ok = ok && c.sLimit.init(max_sr, lk_latency);
        c.sLimit.set_mode(lsp::dspu::LM_HERM_THIN);

        // Real init() (limiter.cpp:252-255):
        //   sDataDelay.init(millis_to_samples(MAX_SR*OVERSAMPLING_MAX, lk_latency) + LIMIT_BUFSIZE)
        //   sDryDelay.init(millis_to_samples(MAX_SR, lk_latency + sOver.max_latency()))
        // — same unit-mixing quirk as lk_latency above (sOver.max_latency()
        // returns OVERSAMPLER_MAX_LATENCY, a SAMPLE count = 62, added
        // directly to a millisecond value); ported as-is, scoped to this
        // instance's max_sr/sample_rate as explained above.
        ok = ok && c.sDataDelay.init(
                (size_t) lsp::dspu::millis_to_samples((float) max_sr, lk_latency) + LIMIT_BUFSIZE);
        ok = ok && c.sDryDelay.init(
                (size_t) lsp::dspu::millis_to_samples(
                        sample_rate, lk_latency + (float) c.sOver.max_latency()));

        // sBypass.init() is called once here (not per-configure) because it
        // resets the crossfade time-constant state — real update_sample_
        // rate() (a rare host callback, only on true SR changes) calls it,
        // NOT real update_settings() (a per-parameter-change callback);
        // this instance's sample rate never changes after create(), so
        // "once at create()" is the correct analog of that distinction.
        c.sBypass.init((int) sample_rate);
        c.sBypass.set_bypass(false); // see class-level Bypass note

        c.vDataBuf = new float[LIMIT_BUFSIZE];
        c.vScBuf   = new float[LIMIT_BUFSIZE];
        c.vGainBuf = new float[LIMIT_BUFSIZE];
        c.vOutBuf  = new float[LIMIT_BUFSIZE];
        memset(c.vDataBuf, 0, LIMIT_BUFSIZE * sizeof(float));
        memset(c.vScBuf,   0, LIMIT_BUFSIZE * sizeof(float));
        memset(c.vGainBuf, 0, LIMIT_BUFSIZE * sizeof(float));
        memset(c.vOutBuf,  0, LIMIT_BUFSIZE * sizeof(float));
    }

    h->sDither.init();
    h->sDither.set_bits(0); // DITHER_NONE (meta::limiter_metadata::DITHER_DEFAULT)

    if (!ok) {
        lsp_limiter_destroy(h);
        return nullptr;
    }
    return h;
}

void lsp_limiter_destroy(void *handle) {
    if (!handle) return;
    auto *h = (LimiterPortHandle *) handle;

    for (int i = 0; i < 2; ++i) {
        channel_t &c = h->ch[i];
        c.sLimit.destroy();
        c.sOver.destroy();
        c.sScOver.destroy();
        c.sDataDelay.destroy();
        c.sDryDelay.destroy();
        c.sBypass.destroy();
        delete[] c.vDataBuf;
        delete[] c.vScBuf;
        delete[] c.vGainBuf;
        delete[] c.vOutBuf;
    }
    h->sDither.destroy();
    delete h;
}

// Real limiter::update_settings() (limiter.cpp:570-661), ported. Real
// update_premix() (limiter.cpp:560-568) is dropped entirely — it only
// reads sPremix's plug::IPort* mix-level ports, which don't exist without
// external-sidechain/link routing (out of scope, see file-header note).
//
// --- Unexposed real-plugin parameters and the fixed default chosen for
// each (all of these are real pThresh/pAttack/... sibling ports in the
// real plugin; lsp_limiter_configure()'s 4-argument signature predates
// this port and only carries threshold/attack/release/knee — matching
// that signature was a hard requirement of this task, not a scope
// decision about these parameters' importance):
//   ovs_mode   -> OVS_TRUE_PEAK_16BIT  (real default is OVS_NONE/off; this
//                 port defaults it ON instead, because scope.md is explicit
//                 that "Limiter's oversampler stays in scope — that's real
//                 DSP quality (true-peak limiting)... JamesDSP's own
//                 standing goal is pristine audio quality. Port it" — an
//                 oversampler that defaults to OM_NONE would never actually
//                 run, defeating that instruction.)
//   dithering  -> DITHER_NONE (matches real DITHER_DEFAULT exactly)
//   alr_on     -> true (real SWITCH("alr", ..., 1.0f) default -- was pinned
//                 false until 2026-10-21, which also made knee_level dead:
//                 dspu::Limiter reads fKnee only inside the ALR branch);
//                 alr_attack/alr_release -> ALR_*_DFL
//   boost      -> false (so fOutGain keeps its GAIN_AMP_0_DB default, no
//                 divide-by-threshold gain compensation)
//   fInGain / fOutGain / fPreamp -> GAIN_AMP_0_DB (unity; no gain-trim UI)
//   fStereoLink -> 1.0f (LINKING_DFL == 100%, full link)
//   lk_ahead (lookahead ms) -> LIMITER_LOOKAHEAD_DFL (5.0f, == real LOOKAHEAD_DFL)
//   op_mode (Hermite/Exp/Line x Thin/Wide/Tail/Duck) -> LM_HERM_THIN
//     (== real LOM_DEFAULT), set once in create(), not per-configure.
// Audio-thread half of configure(): the real plugin's update_settings(),
// transliterated. Runs from lsp_limiter_process() when a new config was published.
static void limiter_apply(LimiterPortHandle *h, float threshold_db, float attack_ms,
                          float release_ms, float knee_db) {

    h->cfg_threshold_db = threshold_db;
    h->cfg_attack_ms    = attack_ms;
    h->cfg_release_ms   = release_ms;
    h->cfg_knee_db      = knee_db;

    const size_t ovs_mode  = OVS_TRUE_PEAK_16BIT;
    const over_mode_t mode = get_oversampling_mode(ovs_mode, (uint32_t) h->fSampleRate);
    const bool filtering   = get_filtering(ovs_mode);
    const size_t dither    = get_dithering(DITHER_NONE);

    const bool boost          = false;
    const float thresh        = lsp::dspu::db_to_gain(threshold_db);
    const float lk_ahead      = LIMITER_LOOKAHEAD_DFL;
    const float attack        = attack_ms;
    const float release       = release_ms;
    // JamesDSP's "limiter knee" is a 0..1 slider (cmd 1500 clamps it to that;
    // 1.0 = the legacy "no knee"). The real control it drives here is the ALR
    // knee level, pKneeLevel, linear gain over KNEE_LEVEL_MIN..MAX (-12..+12
    // dB, default 0 dB): the ALR stage starts easing gain at
    // threshold * knee_level * -9 dB. Map the slider onto the lower half of
    // that range so full travel does something and the top (1.0) is the real
    // default: knee 1.0 -> 0 dB, 0.0 -> -12 dB (ALR starts 12 dB earlier).
    float knee01 = knee_db;                    // argument name is historical
    if (!(knee01 >= 0.0f)) knee01 = 0.0f;      // also catches NaN
    if (knee01 > 1.0f) knee01 = 1.0f;
    const float knee_level    = lsp::dspu::db_to_gain(-12.0f + 12.0f * knee01);
    const float knee_smooth   = lsp::dspu::db_to_gain(LIMITER_KNEE_SMOOTH_DFL);
    const bool alr_on         = true;          // real plugin default (meta: SWITCH "alr" = 1.0f)
    const float alr_attack    = LIMITER_ALR_ATTACK_DFL;
    const float alr_release   = LIMITER_ALR_RELEASE_DFL;

    h->fStereoLink = 1.0f;
    h->fOutGain    = GAIN_AMP_0_DB;
    if (boost)
        h->fOutGain /= thresh; // dead with boost fixed false; kept for fidelity
    h->fInGain = GAIN_AMP_0_DB;
    h->fPreamp = GAIN_AMP_0_DB;

    h->sDither.set_bits(dither);

    for (int i = 0; i < 2; ++i) {
        channel_t &c = h->ch[i];

        c.sBypass.set_bypass(false);
        c.sOver.set_mode(mode);
        c.sOver.set_filtering(filtering);
        if (c.sOver.modified())
            c.sOver.update_settings();

        c.sScOver.set_mode(mode);
        c.sScOver.set_filtering(false);
        if (c.sScOver.modified())
            c.sScOver.update_settings();

        const size_t real_sample_rate = c.sOver.get_oversampling() * (size_t) h->fSampleRate;

        // Update lookahead because oversampling adds extra latency
        const float lk_ahead_ch = lk_ahead
                + lsp::dspu::samples_to_millis(h->fSampleRate, (float) c.sScOver.latency());

        // Cleanup the data delay if limiter's sample rate is going to change
        if (c.sLimit.sample_rate() != real_sample_rate)
            c.sDataDelay.clear();

        c.sLimit.set_mode(get_limiter_mode(LOM_HERM_THIN));
        c.sLimit.set_sample_rate(real_sample_rate);
        c.sLimit.set_lookahead(lk_ahead_ch);
        c.sLimit.set_threshold(thresh, !boost);
        c.sLimit.set_attack(attack);
        c.sLimit.set_release(release);
        c.sLimit.set_knee(knee_level);
        c.sLimit.set_alr_knee(knee_smooth);
        c.sLimit.set_alr(alr_on);
        c.sLimit.set_alr_attack(alr_attack);
        c.sLimit.set_alr_release(alr_release);
        c.sLimit.update_settings();

        // Update the data delay
        c.sDataDelay.set_delay(c.sLimit.get_latency());
    }

    // Report latency (dry-delay compensation only; host PDC report dropped)
    sync_latency(h);
}

void lsp_limiter_configure(void *handle, float threshold_db, float attack_ms,
                            float release_ms, float knee_db) {
    if (!handle) return;
    auto *h = (LimiterPortHandle *) handle;
    LimiterPortHandle::Cfg c = { threshold_db, attack_ms, release_ms, knee_db };
    h->rt.publish(c);   // applied by limiter_apply() on the audio thread's next block
}


// Real limiter::process() (limiter.cpp:779-1016), ported. Graph/mesh
// history output (:937-1015) and query_display_draw() are dropped (UI).
// premix_channel() calls (:807-808) are dropped along with sPremix (out of
// scope); channel_t::vIn/vOut collapse to direct pointers into the
// caller's left/right buffers plus a running offset, since this call
// surface hands over the whole block up front instead of a host pulling
// per-port buffers.
void lsp_limiter_process(void *handle, float *left, float *right, unsigned n) {
    if (!handle || n == 0) return;
    auto *h = (LimiterPortHandle *) handle;
    {
        LimiterPortHandle::Cfg c;
        if (h->rt.take(c))
            limiter_apply(h, c.threshold_db, c.attack_ms, c.release_ms, c.knee);
    }

    channel_t *chans[2] = { &h->ch[0], &h->ch[1] };

    // Get oversampling times
    const size_t times    = chans[0]->sOver.get_oversampling();
    const size_t buf_size = (LIMIT_BUFSIZE / times) & ~(LIMIT_BUFMULTIPLE - 1);

    float min_gain = 1.0f;
    size_t offset  = 0;
    size_t nsamples = n;

    // Process samples
    while (nsamples > 0) {
        const size_t to_do   = lsp::lsp_min(buf_size, nsamples);
        const size_t to_doxn = to_do * times;

        float *io[2] = { left + offset, right + offset };

        // Do main stuff
        for (int i = 0; i < 2; ++i) {
            channel_t *c = chans[i];

            // Apply input gain if needed
            if (h->fInGain != GAIN_AMP_0_DB) {
                lsp::dsp::mul_k3(c->vOutBuf, io[i], h->fInGain, to_do);
                c->sOver.upsample(c->vDataBuf, c->vOutBuf, to_do);
            } else
                c->sOver.upsample(c->vDataBuf, io[i], to_do);

            // Process sidechain signal — SCM_INTERNAL only (the module's
            // own upsampled input feeds its own detector). Real process()'s
            // switch(nScMode) SCM_EXTERNAL/SCM_LINK branches are dropped
            // (out of scope); this is the switch's `default:` branch,
            // ported verbatim.
            if (h->fPreamp != GAIN_AMP_0_DB)
                lsp::dsp::mul_k3(c->vScBuf, c->vDataBuf, h->fPreamp, to_doxn);
            else
                lsp::dsp::copy(c->vScBuf, c->vDataBuf, to_doxn);

            // Perform processing by limiter
            c->sLimit.process(c->vGainBuf, c->vScBuf, to_doxn);
            c->sDataDelay.process(c->vDataBuf, c->vDataBuf, to_doxn);
        }

        // Perform stereo linking (real process()'s `if (nChannels == 2)`
        // branch — always taken here, stereo-only per scope.md)
        {
            float *cl = chans[0]->vGainBuf;
            float *cr = chans[1]->vGainBuf;

            for (size_t k = 0; k < to_doxn; ++k) {
                const float gl = cl[k];
                const float gr = cr[k];

                if (gl < gr)
                    cr[k] = gr + (gl - gr) * h->fStereoLink;
                else
                    cl[k] = gl + (gr - gl) * h->fStereoLink;
            }
        }

        // Perform downsampling and post-processing of signal and sidechain
        for (int i = 0; i < 2; ++i) {
            channel_t *c = chans[i];

            // Update output signal: adjust gain
            lsp::dsp::fmmul_k3(c->vDataBuf, c->vGainBuf, h->fOutGain, to_doxn);

            const float gain = lsp::dsp::min(c->vGainBuf, to_doxn);
            if (gain < min_gain) min_gain = gain;

            // Do Downsampling and bypassing
            c->sOver.downsample(c->vOutBuf, c->vDataBuf, to_do);         // Downsample
            h->sDither.process(c->vOutBuf, c->vOutBuf, to_do);           // Apply dithering (no-op, 0 bits)
            c->sDryDelay.process(c->vDataBuf, io[i], to_do);             // Apply dry delay
            c->sBypass.process(io[i], c->vDataBuf, c->vOutBuf, to_do);   // Pass thru bypass (pinned off)
        }

        offset   += to_do;
        nsamples -= to_do;
    }

    // Report gain reduction (real sBlink-based version replaced with the
    // same plain-minimum telemetry lsp_wrap.cpp's other handles expose)
    h->cur_reduction_db = (min_gain > 0.0f) ? -20.0f * log10f(min_gain) : 60.0f;
}

float lsp_limiter_get_reduction_db(void *handle) {
    if (!handle) return 0.0f;
    return ((LimiterPortHandle *) handle)->cur_reduction_db;
}

int lsp_limiter_is_limiting(void *handle) {
    return lsp_limiter_get_reduction_db(handle) > 0.5f;
}

void lsp_limiter_get_config(void *handle, float *threshold_db, float *attack_ms,
                             float *release_ms, float *knee_db) {
    if (!handle) {
        if (threshold_db) *threshold_db = 0.0f;
        if (attack_ms)    *attack_ms    = 0.0f;
        if (release_ms)   *release_ms   = 0.0f;
        if (knee_db)      *knee_db      = 0.0f;
        return;
    }
    auto *h = (LimiterPortHandle *) handle;
    const LimiterPortHandle::Cfg c = h->rt.peek();   // last published, not last applied
    if (threshold_db) *threshold_db = c.threshold_db;
    if (attack_ms)    *attack_ms    = c.attack_ms;
    if (release_ms)   *release_ms   = c.release_ms;
    if (knee_db)       *knee_db     = c.knee;
}
