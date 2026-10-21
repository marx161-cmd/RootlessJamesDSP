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

## 2026-10-21 (later) — in-house review done, fixes applied, LSP differential test PASSED

Review = my own read + one unanchored subagent read of lsp_wrap*.cpp + call sites.
Differential test (bench/difftest in vendor/JamesDSP): gate/compressor/limiter on the
phone (real AIDL/FMQ path) vs the real LSP plugins (Fedora lsp-plugins-ladspa 1.2.29)
on comrade, same signals/settings: rms(diff)/rms(ref) = -110 dB (gate), -114 dB
(compressor), -132 dB (limiter); audio-only and full variants identical. The port is
faithful at float-noise level. First run before fixes; re-run after fixes required.

FIXED (engine 02192d9 follow-ups, all in this session):
- F1 limiter configure/process race: cmd 1500 (both shapes), Lua lim_apply, Lua cmd
  1500 now take jdsp_lock; lsp_limiter_process in JamesDSPProcess locked too.
- F2 zero-length / short vendor parameter blob null-deref in EffectDSPMainCommand
  (crashed the HAL-side process in the harness; any IEffect client could trigger it):
  size-validated before any field read.
- Compressor knee=0 (app slider minimum) -> NaN gain -> output limiter peak loop spins
  forever on the audio thread: inputs clamped to the real plugin port ranges in
  lsp_comp_configure (knee -24..0 dB linear, ratio 1..100, NaN guards).
- Gate heap realloc on the audio thread (first block + block growth): gate now
  processes in fixed GATE_BUF_SIZE=512 chunks over buffers allocated at create, like
  the real gate.cpp. Dead ScratchBufs struct removed. close_db > open_db clamped.
- Lua cmd 1500 stored postgain raw (dB value used as linear): goes through
  JamesDSPSetPostGain now.
- Harness: no dlclose at teardown (crashed after output was written).

NOT fixed, documented as known limitations (decision: out of scope for this release):
- jdsp_lock contention: gate/compressor/limiter block on the same global mutex the
  convolver IR / DDC / liveprog loaders hold -> possible underrun while loading an IR.
  Pre-existing stock design (w_conv/w_ddc/w_liveprog already did this).
- Limiter "knee" is inert: dspu::Limiter only uses it in ALR mode, which the port pins
  off (as the real plugin default). UI/Lua knee has no effect.
- Handles are created at 48 kHz and not re-created on sample-rate change; Pixel HAL
  output is fixed 48 kHz, other devices at 44.1 kHz get ~8.8 % long time constants.
- Bypassed gate/compressor freeze their detector state (real plugin keeps running it);
  documented zero-cost contract in lsp_wrap.h.
Also fixed today: CMake flags bug (flags above project() lost on fresh configure).

## 2026-10-21 (later) — limiter ALR ON (user decision)

Review found the port pinned the limiter's ALR (automatic level regulation) OFF while the
real plugin's metadata default is ON (SWITCH "alr" = 1.0f); the port's comment claiming
"off = real default" was wrong. Since dspu::Limiter only reads the knee level inside the
ALR branch, that also made JamesDSP's "limiter knee" slider inert. Decision (user, in
chat): fix before shipping, go faithful: ALR on with the real attack/release defaults
(5 ms / 50 ms); the 0..1 knee slider maps linearly onto the ALR knee level -12..0 dB
(1.0 = 0 dB = real default). Sound change for the always-on output limiter -> listening
check by the user when home, together with the on-device bench. Difftest references
regenerated with ALR on.

## 2026-10-21 (later) — output limiter becomes opt-OUT (user request)

User asked for the limiter to be toggleable like the other modules. Decision: toggle,
DEFAULT ON (stock always ran it as the clip guard after post gain; existing presets assume
it). Engine: `limiterEnabled` in JamesDSPLib (jdsp_header.h, vendor header copy synced),
cmd 1500 accepts an optional 6th float (vsize 24) = enabled; 5-float senders leave it on;
Lua cmd 1500 6th param likewise. Off = hard skip of lsp_limiter_process -> the ~5 ms
lookahead vanishes in one step = one tick at the toggle (user: "expected, fine trade").
App: SwitchPreferenceGroup on the output-control card (key limiter_enable, default true),
BaseEngine/Remote/Local setOutputControl gained the enable arg, JNI setLimiterEnabled for
the rootless path.
