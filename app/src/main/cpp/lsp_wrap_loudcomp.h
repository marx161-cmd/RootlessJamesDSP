// lsp_wrap_loudcomp.h — faithful port of the real LSP Loudness Compensator
// plugin (lsp-loud-comp/src/main/plug/loud_comp.cpp, pinned release 1.0.33),
// stereo variant (loud_comp_stereo, nChannels == 2), per
// LSP_FAITHFUL_PORT_SCOPE.md. Structural template: lsp_wrap_expander.{h,cpp}
// / lsp_wrap_autogain.{h,cpp}.
//
// Self-contained: does NOT include lsp_wrap.h. jdspController.c / jamesdsp.c
// include this header directly. Assumes lsp::dsp::init()'s ARM32/x86 stub
// symbols are provided by lsp_wrap.cpp (not redefined here).
//
// ALLOCATION: the real plugin's init() allocates everything for
// FFT_RANK_MAX (14 -> 16384-point FFT) once: sProc.init(FFT_RANK_MAX)
// (5 x 16384 floats), sDelay.init(1 << 13), vFreqApply (2 << 14 floats),
// vTmpBuf, vDry/vBuffer; update_settings() only calls sProc.set_rank(),
// which re-dimensions pointers inside that preallocated arena
// (SpectralProcessor::update_settings(), no malloc). The Equalizer is
// init()'d for all 64 bands and its set_params()/reconfigure() recompute
// in place. So, exactly like the real plugin, this port allocates only in
// lsp_loudcomp_create() and NEVER on the audio thread, for any FFT rank or
// approximation preset.
#ifndef LSP_WRAP_LOUDCOMP_H
#define LSP_WRAP_LOUDCOMP_H

#ifdef __cplusplus
extern "C" {
#endif

void *lsp_loudcomp_create(float sample_rate);
void  lsp_loudcomp_destroy(void *h);
// All arguments map 1:1 onto the real plugin's ports (lsp-loud-comp/src/main/
// meta/loud_comp.cpp, LOUD_COMP_COMMON), except that the input gain is taken
// in dB here and converted to the real "input" port's linear U_GAIN_AMP
// inside (range 0 .. +72 dB linear, i.e. -inf .. +72 dB):
//   input_gain_db  real "input"   AMP_GAIN, dfl 0 dB (GAIN_AMP_0_DB)
//   mode           real "mode"    COMBO: 0 = FFT, 1 = IIR (dfl 0)
//   std            real "std"     COMBO: 0 Flat, 1 ISO226-2003,
//                                 2 Fletcher-Munson, 3 Robinson-Dadson,
//                                 4 ISO226-2023 (dfl STD_DFL = 4)
//   fft_rank_idx   real "fft"     COMBO: 0..6 = 256,512,...,16384
//                                 (dfl FFT_RANK_IDX_DFL = 4 -> 4096)
//   approx         real "approx"  COMBO: 0 Fastest (16 filters, slope 1),
//                                 1 Low (24/2), 2 Normal (32/3), 3 High (48/4),
//                                 4 Best (64/5) (dfl 2)
//   volume_phons   real "volume"  CONTROL U_DB, PHONS_MIN -83 .. PHONS_MAX 7,
//                                 dfl 0  (the listening level in phons; the
//                                 selected equal-loudness contour at that
//                                 level is applied as the EQ curve)
//   hclip          real "hclip"   SWITCH, dfl 0
//   hcrange_db     real "hcrange" CONTROL U_DB, HCRANGE 0..24, dfl 6
// Omitted on purpose (pinned to the real defaults): "refer"/"reftype"
// (reference generator, off), UI/meter ports (spec, relspec, lufs_il/ol,
// ilm/olm, olc blink, hcclean trigger). See the .cpp banner.
// Control-thread safe: only sanitises and publishes (rt_config.h); the real
// update_settings() runs on the audio thread inside lsp_loudcomp_process().
void  lsp_loudcomp_configure(void *h, float input_gain_db, int mode, int std,
                             int fft_rank_idx, int approx, float volume_phons,
                             int hclip, float hcrange_db);
// `enabled` drives the real Bypass crossfade (~5 ms, real plugin's per-channel
// sBypass) — call every block regardless of enabled state so the crossfade
// runs; zero-cost no-op once fully settled disabled (same contract as the
// other LSP ports). NOTE the one deviation this contract forces: the real
// plugin keeps its FFT/IIR path streaming while bypassed, this port does
// not, so when re-enabled from the settled-off state the processors are
// reset first (the real plugin's own mode-switch reset: sDelay.clear(),
// sProc.reset(), sEqualizer.reset()) and the wet path fills in over
// 1 << rank samples, exactly as the real plugin does after a mode switch.
void  lsp_loudcomp_process(void *h, float *left, float *right, unsigned n, int enabled);

// --- Telemetry --------------------------------------------------------------
// Processing latency in samples as the real plugin reports it to the host
// (process(): set_latency(vChannels[0]->sDelay.get_delay())): 1 << rank in
// FFT mode, the Equalizer's latency (0 for IIR) in IIR mode.
int   lsp_loudcomp_get_latency(void *h);

// --- Config read-back (values in the exact units passed to configure) -------
void  lsp_loudcomp_get_config(void *h, float *input_gain_db, int *mode, int *std,
                              int *fft_rank_idx, int *approx, float *volume_phons,
                              int *hclip, float *hcrange_db);

#ifdef __cplusplus
}
#endif

#endif
