## Status vs Scope — Gaps

IMPLEMENTED:
  ✅ LSP prebuilt linked (liblsp-combined.so)
  ✅ Lua 5.4.8 embedded + lua_bridge.c
  ✅ mb_dyn_process replaces Compressor + BassBoost
  ✅ Compressor/BassBoost native code removed
  ✅ AIDL per-band commands (10100 gate, 10101 compressor, 10102 EQ)
  ✅ Termux keystore signing
  ✅ Lua bridge gets band energies

NOT IMPLEMENTED:
  ❌ AutoGain module
  ❌ Standalone Gate (pre-convolver)
  ❌ Shared FFT band split with EQ (mb_dyn and EQ use separate decompositions)
  ❌ Per-bin convolver bypass
  ❌ Data Bridge file output (Lua can do it via io.open(), no native writer)
  ❌ Chain ordering system (modules hardcoded)
  ❌ NEON FFT core
  ❌ UI still has Compander + Bass cards wired to dead native code
  ❌ mb_dyn_process has no enable/disable toggle
  ❌ LSP Dynamics card exists in XML but not loaded in DspFragment
  ❌ Standalone LSP Limiter not wired (old JLimiter in use)
  ❌ 108-cell chroma band meter (scoped 2026-09-09, see CHROMA_BAND_METER_SPEC.md)

## 2026-10-21 — status correction + split decision (append-only from here on)

Status corrections to the list above (the list is stale, kept as history):
- ✅ 108-cell chroma band meter: DONE 2026-09-11, 60 Hz continuous version
  shipped in the 2026-10-10 OTA (engine ad7813e, app 5cfc097, vendor f32fd9c).
- ✅ Standalone LSP Limiter: wired as a faithful port 2026-09-04 (d19dea3).
- ✅ LSP Gate/Compressor: faithful ports since 2026-09-04, hand-wrap gone.
- ✅ NEON/SVE2 FFT core: batched dual-buffer fft_x2 in codelet.c (engine f9e782f).
- Reference clones lsp-gate/-compressor/-limiter/-mb-compressor ARE git
  submodules (pinned releases); their stale AGENT_INSTRUCTIONS.md files were
  moved to ~/homelab/archive/jdsp-agent-instructions-2026-08-05/.

Decision (user, 2026-10-21): publish the vendor audio-HAL effect
`libjamesdspaidl.so` as TWO build variants from one source tree:
1. **audio-only** (headline): faithful LSP Gate/Compressor/Limiter ports,
   limiter/post-gain tail fix, SVE2 batched FFT + fp-contract guard, realtime
   tail-worker threads, config-swap/UAF locking fixes. NO Lua interpreter,
   NO band meter compiled in. Selected by a CMake option (name TBD when
   built); jdsp_header.h struct layout stays identical in both variants so
   the AIDL wrapper never disagrees on struct size.
2. **full**: same + Lua bridge + 108-band 60 Hz meter (what the user runs).
Reason: strangers will not flash an audio HAL binary with an embedded Lua
interpreter; the audio-only diff is also the small, reviewable one.

Validation plan before publishing (in order):
- Review of the engine delta since the 2026-08-16 ultrareview, done in-house
  (no paid review): LSP ports, configure lock, meter, 60 Hz filter rewrite.
- Differential test: real LSP LV2 plugins on comrade vs our .so through
  tools/wav_effect_harness, same WAV + parameters, sample-by-sample compare.
  Scope caveat stays stated: stereo mode, internal sidechain only.
- Benchmarks vs stock crDroid .so: offline realtime factor (harness),
  on-device audioserver CPU% + battery power (vendor/JamesDSP/bench/*.sh),
  FFT cycle counts (existing simpleperf numbers).
- Upstream merge (ThePBone 2026-07-25, multirate convolver) is optional,
  decided at publish time, not before.
