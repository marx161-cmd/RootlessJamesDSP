// lsp_wrap_mbcomp.h — faithful port of the real LSP Multiband Compressor
// plugin (lsp-mb-compressor/src/main/plug/mb_compressor.cpp, pinned release
// 1.0.35; DSP identical to the 1.0.33 shipped in comrade's reference build
// lsp-plugins 1.2.29, see the .cpp banner), STEREO variant
// (meta::mb_compressor_stereo -> nMode == MBCM_STEREO, channels == 2,
// bSidechain == false), per LSP_FAITHFUL_PORT_SCOPE.md. Structural template:
// lsp_wrap_loudcomp.{h,cpp}.
//
// Self-contained: does NOT include lsp_wrap.h. jdspController.c / jamesdsp.c
// include this header directly. Assumes lsp::dsp::init()'s ARM32/x86 stub
// symbols are provided by lsp_wrap.cpp (not redefined here).
//
// ALLOCATION: the real plugin allocates everything in init() (one aligned
// arena: per-channel/per-band MBC_BUFFER_SIZE buffers; sFilters.init(
// BANDS_MAX * channels); per band sSC.init(2, REACTIVITY_MAX), three
// dspu::Filter::init(NULL), two sEQ.init(2, 6); per channel sDryEq.init(7, 0),
// three sEnvBoost.init(NULL)) and in update_sample_rate() (the four Delay
// lines per channel + sScDelay per band at max_delay = (1 << fft_rank) +
// LOOKAHEAD_MAX samples, and sFFTXOver.init(fft_rank, BANDS_MAX) where
// fft_rank = select_fft_rank(sr) = 12 for 44.1/48 kHz, 13 for 88.2/96 kHz,
// 14 for 176.4/192 kHz). JamesDSP creates one handle per fixed sample rate,
// so this port runs both of those once inside lsp_mbcomp_create() and NEVER
// allocates on the audio thread: update_settings() only calls
// Compressor/Sidechain/Filter/Equalizer/DynamicFilters::set_params/update
// (in-place recompute, checked), FFTCrossover::set_*/enable_* (flags; the
// per-band FFT curves are regenerated in place inside its preallocated arena
// on the next process()) and Delay::set_delay(); every mode (Classic,
// Modern, Linear Phase) works inside the buffers sized at create time.
#ifndef LSP_WRAP_MBCOMP_H
#define LSP_WRAP_MBCOMP_H

#ifdef __cplusplus
extern "C" {
#endif

// --- Payload layout (AIDL cmd 10106 / Lua cmd 10106) -------------------------
// ONE fixed layout, LSP_MBCOMP_PAYLOAD_FLOATS floats (vsize = 4 * that):
//   [0]  band (ignored, always 0)
//   [1]  enabled (0/1)
//   [2 .. 2+LSP_MBCOMP_NPARAMS-1]  the configuration block below, which is
//        exactly what lsp_mbcomp_configure() takes (params[0] == payload[2]).
// Configuration block (index into `params`):
//   GLOBALS (LSP_MBCOMP_GLOBALS = 10), real port in quotes, real range/default:
//     [0] mode            "mode"   COMBO 0 Classic / 1 Modern / 2 Linear Phase (dfl 1)
//     [1] input gain dB   "g_in"   AMP_GAIN 0..10 linear = -inf..+20 dB (dfl 0 dB)
//     [2] output gain dB  "g_out"  AMP_GAIN 0..10 linear (dfl 0 dB)
//     [3] dry gain dB     "g_dry"  AMP_GAIN 0..10 linear (dfl 0.0 linear = -inf dB)
//     [4] wet gain dB     "g_wet"  AMP_GAIN 0..10 linear (dfl 0 dB)
//     [5] dry/wet %       "drywet" PERCENTS 0..100 (dfl 100)
//     [6] reactivity      "react"  LOG_CONTROL REACT_TIME 0..1 (dfl 0.2). NOTE: in the
//                                  real plugin this feeds ONLY sAnalyzer.set_reactivity()
//                                  (the FFT display) which is omitted here -> accepted,
//                                  stored, read back, no effect on the audio.
//     [7] shift dB        "shift"  AMP_GAIN 0..100 linear (dfl 0 dB). Same: feeds only
//                                  sAnalyzer.set_shift() -> no effect on the audio.
//     [8] envelope boost  "envb"   COMBO 0 None / 1 Pink BT / 2 Pink MT / 3 Brown BT /
//                                  4 Brown MT (dfl 1 = FB_DEFAULT)
//     [9] stereo split    "ssplit" SWITCH (dfl 0): sidechain source combo per band is
//                                  "scs" (off) or "sscs" (on), real update_settings()
//   SPLITS (LSP_MBCOMP_SPLITS = 7), split s = 0..6 is the real "_{s+1}" split
//   (the boundary between band s and band s+1), at [10 + 2*s]:
//     [+0] enable         "cbe_N"  SWITCH (dfl 0,1,0,1,0,1,0)
//     [+1] frequency Hz   "sf_N"   LOG_CONTROL FREQ 10..20000 (dfl 40,100,252,632,1587,3984,10000)
//   BANDS (LSP_MBCOMP_BANDS = 8), band b = 0..7 is the real "_b" band, at
//   [24 + LSP_MBCOMP_BAND_PARAMS*b] (LSP_MBCOMP_BAND_PARAMS = 24):
//     [+0]  enabled              "ce"   SWITCH (dfl 1)
//     [+1]  solo                 "bs"   SWITCH (dfl 0)
//     [+2]  mute                 "bm"   SWITCH (dfl 0)
//     [+3]  compressor mode      "cm"   COMBO 0 Down / 1 Up / 2 Boost (dfl 0)
//     [+4]  sidechain mode       "scm"  COMBO 0 Peak / 1 RMS / 2 LPF / 3 SMA (dfl 1)
//     [+5]  sidechain source     "scs"  COMBO 0 Middle / 1 Side / 2 Left / 3 Right /
//                                       4 Min / 5 Max (dfl 0) -- used when ssplit == 0
//     [+6]  split sc source      "sscs" COMBO 0 Left/Right / 1 Right/Left / 2 Mid/Side /
//                                       3 Side/Mid / 4 Min / 5 Max (dfl 0) -- when ssplit == 1
//     [+7]  sc lookahead ms      "sla"  CONTROL LOOKAHEAD 0..20 (dfl 0)
//     [+8]  sc reactivity ms     "scr"  LOG_CONTROL REACTIVITY 0..250 (dfl 10)
//     [+9]  sc preamp dB         "scp"  AMP_GAIN100 0..100 linear = -inf..+40 dB (dfl 0 dB)
//     [+10] sc custom lo-cut on  "sclc" SWITCH (dfl 0)
//     [+11] sc custom hi-cut on  "schc" SWITCH (dfl 0)
//     [+12] sc lo-cut freq Hz    "sclf" LOG_CONTROL FREQ 10..20000 (dfl 10,40,100,252,632,1587,3984,10000)
//     [+13] sc hi-cut freq Hz    "schf" LOG_CONTROL FREQ 10..20000 (dfl 40,100,252,632,1587,3984,10000,20000)
//     [+14] attack level dB      "al"   LOG_CONTROL ATTACK_LVL -60..0 dB (dfl -12)
//     [+15] attack time ms       "at"   LOG_CONTROL ATTACK_TIME 0..2000 (dfl 20)
//     [+16] release level dB     "rrl"  LOG_CONTROL RELEASE_LVL -inf..+36 dB (dfl -inf);
//                                       relative to the attack level, like the real port
//     [+17] release time ms      "rt"   LOG_CONTROL RELEASE_TIME 0..5000 (dfl 100)
//     [+18] ratio                "cr"   LOG_CONTROL RATIO 1..100 (dfl 1)
//     [+19] knee dB              "kn"   LOG_CONTROL KNEE -24..0 dB (dfl -6)
//     [+20] makeup dB            "mk"   LOG_CONTROL MAKEUP -60..+60 dB (dfl 0)
//     [+21] boost threshold dB   "bth"  EXT_LOG_CONTROL BTH -120..-60 dB (dfl -72)
//     [+22] boost signal dB      "bsa"  EXT_LOG_CONTROL BSA -72..+72 dB (dfl +6)
//     [+23] hold ms              "ht"   CONTROL HOLD_TIME 0..1000 (dfl 0)
//   Total: 10 + 14 + 192 = 216 config floats; 218 payload floats; vsize 872.
// Every value whose real port is U_GAIN_AMP is taken in dB here and converted
// to the real linear gain inside; a dB value <= LSP_MBCOMP_DB_MINUS_INF is
// -inf dB (0.0 linear, the real "g_dry"/"rrl" defaults). All values are
// clamped to the real metadata ranges on the audio thread, NaN -> default.
// Pinned to the real defaults and NOT in the payload (documented deviations):
//   "sce_N" sidechain type (Internal/Link) -> Internal: the shared-memory link
//   does not exist in the JamesDSP chain, nor do external sidechain inputs
//   (the sc_* plugin variants); premix "in2lk"/"lk2in"/"lk2sc" -> -inf dB
//   (link off); UI-only "showpmx"/"zoom"/"bsel"/"flt"/"ife_*"/"ofe_*"/"hue_N";
//   all meters and meshes.
#define LSP_MBCOMP_BANDS            8
#define LSP_MBCOMP_SPLITS           7
#define LSP_MBCOMP_GLOBALS          10
#define LSP_MBCOMP_BAND_PARAMS      24
#define LSP_MBCOMP_SPLIT_BASE       (LSP_MBCOMP_GLOBALS)                                  /* 10 */
#define LSP_MBCOMP_BAND_BASE        (LSP_MBCOMP_SPLIT_BASE + 2 * LSP_MBCOMP_SPLITS)       /* 24 */
#define LSP_MBCOMP_NPARAMS          (LSP_MBCOMP_BAND_BASE + LSP_MBCOMP_BANDS * LSP_MBCOMP_BAND_PARAMS) /* 216 */
#define LSP_MBCOMP_PAYLOAD_FLOATS   (2 + LSP_MBCOMP_NPARAMS)                              /* 218 */
#define LSP_MBCOMP_DB_MINUS_INF     (-150.0f)

void *lsp_mbcomp_create(float sample_rate);
void  lsp_mbcomp_destroy(void *h);

// Fills `params` (LSP_MBCOMP_NPARAMS floats) with the real plugin's metadata
// defaults in the layout above (module disabled is the caller's flag).
void  lsp_mbcomp_default_config(float *params);

// `params` is the configuration block above (LSP_MBCOMP_NPARAMS floats;
// `count` must be >= LSP_MBCOMP_NPARAMS, extra values are ignored, fewer is
// a no-op). Control-thread safe: only sanitises NaNs and publishes
// (rt_config.h); the real update_settings() runs on the audio thread inside
// lsp_mbcomp_process().
void  lsp_mbcomp_configure(void *h, const float *params, unsigned count);

// `enabled` drives the real per-channel Bypass crossfade (~5 ms, real
// plugin's sBypass) — call every block regardless of enabled state so the
// crossfade runs; zero-cost no-op once both channels' Bypass have settled
// off (same contract as the other LSP ports). The one deviation this
// contract forces: the real plugin keeps its crossover/sidechain/delay path
// streaming while bypassed, this port does not, so when re-enabled from the
// settled-off state the port first performs the real plugin's own
// mode-switch reset (update_settings(): nPlanSize = 0 -> plan/filters
// rebuilt, sXOverDelay.clear()) and additionally clears every delay line /
// filter memory that would otherwise replay stale audio into the crossfade
// (sDelay, sDryDelay, sFFTXOver, sDryEq, sEnvBoost[], per band sScDelay,
// sSC, sEQ[], sPassFilter/sRejFilter/sAllFilter, and the Modern-mode
// DynamicFilters bank via a FLT_NONE re-arm so its set_params() path clears
// its biquad memory). dspu::Compressor has no reset API: its envelope state
// is left to decay at the configured release time, exactly as in the real
// plugin after a mode switch.
void  lsp_mbcomp_process(void *h, float *left, float *right, unsigned n, int enabled);

// --- Telemetry --------------------------------------------------------------
// Processing latency in samples as the real plugin reports it to the host
// (update_settings(): set_latency(latency + xover_latency)): the largest
// enabled band lookahead, plus the FFT crossover latency (1 << fft_rank,
// 4096 samples at 48 kHz) in Linear Phase mode.
int   lsp_mbcomp_get_latency(void *h);

// --- Config read-back (LSP_MBCOMP_NPARAMS floats, exact caller units; the
// last PUBLISHED values, not the last applied ones) ------------------------
void  lsp_mbcomp_get_config(void *h, float *params);

#ifdef __cplusplus
}
#endif

#endif
