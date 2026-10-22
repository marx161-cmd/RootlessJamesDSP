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

## 2026-10-21 (night) — scope widened (user: publish shifts 1-2 weeks, front-load now)

Agreed plan: (1) profile the real preset on-device (when the user is home), (2) lock-free
config handoff for the dynamics ports, (3) Expander port, (4) AutoGain port; loudness
compensation only if the profile shows headroom; multiband compressor last/if ever.

(2) DONE: rt_config.h -- configure() on the control thread only sanitises and publishes;
the audio thread try-takes and runs the real update_settings() itself (the real LSP host
structure). All jdsp_lock use on the gate/compressor/limiter path removed (chain wrappers,
JamesDSPProcess limiter call, every configure call site). Verified: difftest unchanged,
harness --stress (random reconfigures from a second thread during processing) clean.
(3) IN PROGRESS via subagent: lsp_wrap_expander.{h,cpp} transliterated from
lsp-expander (submodule pinned 1.0.37, 2026-06-13, same date as the gate pin); AIDL cmd
10103, 9 floats vsize 36 [band, enabled, mode(0 down/1 up), threshold dB, ratio, knee
linear, attack, release, hold]; chain slot right after "gate". App card
dsp_expander_preferences.xml + PREF_EXPANDER done by me.
(4) NEXT: lsp-autogain submodule added, pinned 1.0.19 (2026-06-13, matches the dsp-units
pin); port after the expander; LADSPA autogain_stereo is available on comrade for the
difftest.
Also: compander knee slider minimum raised to 0.063 (the engine clamps anyway).

## 2026-10-22 (night) — on-device bench done, numbers in release/README.md

Bench (vendor commit 6bd12f5): Pixel 10 Pro, 60 s same track, screen off, unplugged,
user preset (convolver+DDC+GEQ+LiveProg, gate/compander off, limiter on). Audio HAL
process CPU, % of one core: stock 60.9, audio-only 23.9, full 27.7. Whole-phone draw
2184 / 1764 / 1824 mW (fuel gauge, one run each). Profile (simpleperf, full): convolver
FFT 28 %, JamesDSPProcess 9 %, limiter 6 %, EEL 3 %, DDC 2.5 %, Lua+meter 2.5 %. The HAL
process also hosts VLC's own DynamicsProcessing effect (~12 % of samples), same for all
three variants.

Found on the way (not scope changes, recorded so nobody re-derives them):
- `bench/swap_effect.sh` had stacked 60+ bind-mount layers: /data and /vendor are shared
  mounts, so each `mount --bind` also propagated onto the previous source file's path,
  and `umount` of a mapped .so is EBUSY while the HAL runs. Fixed: lazy-unmount every
  layer first, push to a per-sha filename, `mount -o private none <target>` afterwards.
- The bench wait loop itself was fine; the previous night's failure was the JamesDSP
  root service not re-attaching after the HAL restart. The loop now also requires an
  active output thread and relaunches the app every 25 s while the effect is missing.
- The app APK built on 2026-10-21 was the `rootFull` flavor (Firebase/Crashlytics);
  it crashes at launch as a system app (FirebaseApp not initialised, Cache.cleanup
  Timber tree). Every install so far has been `rootFdroid`. Rolled back; the fdroid
  build with the limiter/expander/autogain cards replaces it.

Phone state: `release/full` (2fdbc008) bind-mounted for the ALR listening check; reboot
returns to the OTA .so (e041d142). Still open: the ALR listen (user), README-level
publish decisions (which repos go public), then the module list for the 1-2 week window.

## 2026-10-22 (night, later) — ALR listen passed; loudness compensation port started

User listened with release/full (ALR on) + the new app: limiter loads and sounds fine; the
one tick when toggling the output limiter on/off is audible, as documented (lookahead
drops in one step). No change requested.

Decision (user: "keep crunching", credit available today/tomorrow): module order
1) LSP loud_comp (loudness compensation) -- pre-agreed "if the profile shows headroom";
   full build sits at 27.7 % of one core, so it does;
2) multiband compressor as a faithful port (replacing the hand-wrap) -- last, as before;
3) meter decimation in the full build (~2.5 % of the HAL) -- small, quick.

loud_comp port spec: submodule app/src/main/cpp/lsp-loud-comp pinned 1.0.33 (2026-06-13,
same release set as the other pins; plugin logic identical to the 1.0.31 the comrade
reference build 1.2.29 ships). AIDL cmd 10105, 10 floats (vsize 40): [band, enabled,
input gain dB, mode 0 FFT/1 IIR, std 0 Flat/1 ISO226-2003/2 Fletcher-Munson/
3 Robinson-Dadson/4 ISO226-2023, fft rank idx 0..6 (256..16384), approx 0..4, volume
phons -83..7, hclip, hcrange dB]. Real defaults: 0 dB, FFT, ISO226-2023, 4096, Normal,
0 phon, hclip off, 6 dB; module off by default. Reference generator, meters and UI-only
ports omitted. Chain position: last module before the output limiter / post gain.
Lock-free config via rt_config.h like the others; no allocation on the audio thread.
Difftest case `loudcomp` against LADSPA loud_comp_stereo. Port executed by a subagent,
reviewed + committed by me.

## 2026-10-22 ~03:00 — loud_comp port DONE (subagent), reviewed, committed

Result: lsp_wrap_loudcomp.{h,cpp} (transliteration of loud_comp.cpp 1.0.33, real line
numbers cited), cmd 10105 as specified above, chain slot "loudcomp" registered last (after
reverb, before the Lua tap; postGain + limiter follow in JamesDSPProcess), app card after
Output control, Lua toggle_module("loudcomp"). Everything allocated once in create() for
FFT_RANK_MAX; nothing on the audio thread. Difftest on the final binaries, both variants
identical: loudcomp FFT (ISO226-2023, -30 phon, 4096) -111.6 dB; gate/comp/limiter/
expander/autogain unchanged (-110/-114/-126/-108/-89); stress 174/112 reconfigures incl.
rank and mode switches, no non-finite samples.

IIR mode: -30.2 dB only. Proven to be the real plugin's own float32 floor, not a port
error: the unmodified x86 LADSPA plugin run natively (AVX2) vs under qemu (SSE4.2) differs
from itself by -31.5 dB in IIR mode and -115 dB in FFT mode, the same pair of numbers.
Error is -22 dB at 20-28 Hz falling to -77 dB at 14-20 kHz (biquads with poles at
|z| ~ 0.997 in the 22-80 Hz sections). -fno-fast-math on the LSP objects moved it to
-32.9 dB only; reverted, -Ofast kept. Documented in make_reference.sh and the wrapper.

Deviations (deliberate, documented in the header): reference generator, meters, clip
indicator, mesh/UI ports omitted; bypass port = enabled argument; zero-cost-when-disabled
skips the FFT path once the Bypass has settled off and resets delay/FFT/EQ state on
re-enable (the real plugin's own mode-switch reset), so enabling fills in over 1<<rank
samples (~85 ms at 4096). Real-plugin quirks kept verbatim (latency read from the
previous rank; 16384-sample first delay on an 8192 line, dry path only).

Chain capacity note: g_modules[16] is now exactly full; the mb-compressor rework (already
a module) needs no new slot, anything else does.

Vendor: Soong slot = full .a with loud_comp (header + slot committed together, as on
10-21); release/full 268de2a3, release/audio 14d9d52e.

## 2026-10-22 ~03:20 — "meter decimation" replaced by Lua hook caching (full build)

The profile of the full build showed the full-only cost is not the 108-band meter (not in
the top of the profile at all) but the Lua per-sample tap: lua_bridge_process_sample did a
lua_getglobal("process") (hash lookup + strcmp) for every sample, 48000x/s, only to find
that the running script defines no process() -- lua_getglobal 1.14 % + luaV_finishget
0.27 % + lua_bridge_process_sample 0.61 % of the HAL. Decimating the meter would have gained
nothing measurable. Change: cache "script defines process() / process_block()" flags at
every script (re)load and once per tick(); the per-sample and per-block taps return
immediately when the flag is off. Behaviour unchanged for scripts that define the hooks
(a hook defined later at runtime is picked up at the next tick, <= 1/60 s). Full-build
re-bench follows; the audio-only build is unaffected (stubs).

## 2026-10-22 ~03:45 — Lua hook caching measured: 77.0 -> 75.2 % (A/B, same preset)

Bench of the full build with the user's current everything-on preset (gate, compander,
expander, autogain, loud_comp FFT 4096, convolver, DDC, GEQ, LiveProg "Joe0Bloggs DRX10K
compander-HR.eel"): previous full build 268de2a3 = 77.0 %, new build 5011f9eb = 75.2 % of one
core, screen off, unplugged, 60 s each. The 27.7 % earlier tonight was a different preset
(gainControl.eel, dynamics off), not comparable. Profile of the everything-on preset:
NSEEL_code_execute (the LiveProg EEL interpreter running the DRX10K compander script) 56 %,
convolver 14 %, everything else small; the EEL script alone costs about twice the rest of
the chain. That is the user's script choice, not an engine issue, but worth knowing when
reading HAL CPU numbers. Phone back on 5011f9eb.

## 2026-10-22 ~04:00 — multiband compressor: NEW module (not a rework), port started

Finding: there is no multiband compressor in the tree any more. The old hand-wrap
(lsp_mb_dynamics) was removed as dead code on 2026-08-16, AIDL cmd 10102 is a "MB EQ" no-op,
and the app has no card. So this is a new module like loud_comp, built from the already
present submodule lsp-mb-compressor (1.0.35, 2026-06-13).

Scope (user: "keep going"; layout decision stated, not asked): faithful stereo port of
mb_compressor, all three modes (Classic / Modern / Linear Phase), BANDS_MAX 8 with every
per-band control, 7 splits, global gains / dry-wet / reactivity / shift / envelope boost;
internal sidechain only (no external sidechain input exists in the chain); analyzer, meters,
meshes and UI-only ports omitted. AIDL cmd 10106, one fixed payload (~180 floats, layout
documented in the wrapper header). Chain slot "mbcomp" after expander, before autogain (level
after band compression); chain capacity 16 -> 24. App card after the Expander card: global
section, splits section, 8 band sections (plain seekbar/list/switch preferences, so presets
stay plain preference files). Difftest cases mbcomp (Classic) and mbcomp_lp (Linear Phase,
latency-aligned) vs LADSPA mb_compressor_stereo. Port by subagent, reviewed + committed by me.

## 2026-10-22 ~04:50 — mb_compressor port DONE (subagent), reviewed; chain order kept stock

Result: lsp_wrap_mbcomp.{h,cpp} (1564 lines, transliteration of mb_compressor.cpp 1.0.35,
stereo, internal sidechain), cmd 10106, payload 218 floats (vsize 872): [band, enabled,
10 globals (mode 0 Classic/1 Modern/2 LP, in/out/dry/wet dB, dry-wet %, reactivity, shift,
envelope boost, stereo split), 7 x (split enable, freq), 8 x 24 per-band params]; layout in
the header. Chain: registered right after expander (registration index 3); chain capacity
16 -> 24. App card after Expander: global + splits + 8 band sections (215 keys; the per-band
keys are generated strings, PreferenceCache got a string-key get()). Difftest, final binaries,
full == audio: mbcomp Classic -78.5 dB, mbcomp Linear Phase -89.5 dB; the unmodified x86
reference differs from itself (AVX2 vs SSE4.2) by -76.6 / -91.0 dB in the same two modes,
so the port sits at the reference's own float32 floor (>99.9 % of the residual below 800 Hz,
the 100/800 Hz LR4 biquads). Regression unchanged: gate -110, comp -114, limiter -126,
expander -108, autogain -88, loudcomp -112 / IIR -30. Stress 153/125 reconfigures clean.
Codegen note: two different -Ofast/LTO builds differ by ~-96 dB; treat ~1 dB as jitter.

Correction by me: the subagent rotated default_order[] to {1,2,3,0,...} to honour "before
autogain", which moved autogain from FIRST in the chain (stock behaviour, legacy integrator
at the input; the user's preset has it on) to after the expander. Reverted to registration
order: autogain, gate, expander, mbcomp, tube, compressor, ... loudcomp, lua. Engine + vendor
rebuilt after the revert; regression difftest re-run on the final binaries.

Deviations (documented in the header): sidechain type pinned internal, premix omitted,
analyzer/meters/meshes/UI ports omitted, react/shift accepted but inert (they only fed the
analyzer), cbe (split enable) INCLUDED (my omit list had it by mistake). Zero-cost-when-
disabled with the real mode-switch reset on re-enable; Compressor envelope has no reset API
(decays, as the real plugin). Known gap: Modern mode has no difftest case (stress only).

## 2026-10-22 ~12:40 — follow-ups closed: preset rows, Modern difftest, reproducible paths

Scope (from the 05:05 follow-up list, no new behaviour): 1) `presets/make_defaults_preset.py`
gains dsp_loudcomp.xml (9 keys) and dsp_mbcomp.xml (215 keys, generated from the same
split/band tables as readMbCompParams()), verified key-for-key and value-for-value against
the two preference XMLs; regenerated tar pushed to the phone's Presets dir. 2) difftest case
`mbcomp_modern` (mode 1, same band settings, lag 0): port -59.5 dB vs the real plugin; the
real plugin's own AVX2-vs-SSE4.2 self-diff in Modern mode is -61.9 dB (Classic re-measured
-76.6, as recorded), residual 99.5 % below 252 Hz -> reference float32 floor, like Classic.
3) `-ffile-prefix-map` for source, build and NDK roots in app/src/main/cpp/CMakeLists.txt
(the one `/home/...` string in the stripped release, eel2 HPFloat/print.c `__FILE__`, is
now `jamesdsp/...`); `release/*/SHA256SUMS` list bare file names. Both variants rebuilt;
regression unchanged on every case (gate -110, comp -114, limiter -126, expander -108,
autogain -89, loudcomp -112 / IIR -30, mbcomp -79 / -91), audio == full, stress clean.
Soong slot = the new full .a. Phone NOT re-swapped (still on 021d0e3a from 05:05; the new
binaries differ only by the path string); user at work, listen tests of loud_comp / mb_comp
and the publish decision still open.

## 2026-10-22 ~12:40 — app crash on the multiband card (user, "toggled on everything")

`MaterialSeekbarPreference.updateLabelValue()` spliced the unit into the `String.format`
pattern; the dry/wet seekbar's unit is `%`, so a trailing `%` threw
UnknownFormatConversionException on every bind -> the DSP page crash-looped while the
switch was on. Fix at the shared layer: format the number, then append the unit (any unit
is now safe). Unblocked live by flipping `mbcomp_enable` to false in the saved prefs, fixed
APK installed 12:38, switch restored to on, no crash after relaunch.

## 2026-10-22 ~13:00 — 108-band meter parity with the desktop feed (user: easy wins + the .so-bake part, OTA this weekend)

Why: the phone ports copy desktop movement code 1:1 but the phone meter feeds them something
else. Measured 2026-10-22: one RBJ biquad at Q 17.3 per band (neighbour semitone -7 dB, octave
-28 dB) vs the desktop's Butterworth with +-1/2-semitone edges, order 2..4 per octave (-12/-57
.. -24/-115 dB); raw linear RMS vs the desktop's norm_db 0..1 (-60..0 dB) with 60/70 ms
attack/release; RMS only vs flux/onset/centroid + named ranges; ShaderEditor reads the latest
frame at 30 Hz as a 108x1 texture although the engine publishes 60 Hz + a 32-frame ring; a
stalled file (pause) is not detected by the texture reader; no per-band group-delay alignment.

Decisions (engine, full variant only; the audio-only publish candidate has no meter):
1. Filter bank = the desktop design exactly (band_filters.py / shader_bands.cpp
   design_butterworth_bandpass): centre 27.5*2^(b/12), -3 dB at +-1/2 semitone, orders per
   octave {2,2,2,3,3,4,4,4,4} for the 9 phone octaves. 336 biquads/sample mono instead of 108;
   meter cost re-measured on the bench afterwards (was ~2 % of the HAL incl. Lua).
2. bands.bin v3: bytes [0,432) stay the latest RAW frame (old readers untouched); header
   version 3 with the ring offset as a field; NEW regions: a "cooked" 108-float frame
   (norm_db floor -60 dB + per-band attack 60 / release 70 ms envelope, the desktop's
   constants) and a feature block (rms, sub, bass, lowmid, mid, highmid, presence,
   brilliance, centroid, flux, onset) computed per 60 Hz frame by transliterating the
   desktop plugin's formulas, so u_* names mean the same on both machines. Raw ring kept.
3. ShaderEditor: read the ring at 60 Hz by seq (not the 432 bytes at 30 Hz); `bands` stays
   RAW latest (existing phone shaders keep their tuning); new samplers `bands_db` (cooked
   108x1, desktop-compatible 0..1) and `bands_hist` (raw 108x32, newest row 0); new uniforms
   u_onset, u_onset_smooth, u_onset_accum, u_flux, u_centroid and the named ranges with the
   desktop names; texture zeroed when seq stalls > 150 ms (pause); glTexSubImage2D updates.
4. OPEN (user's call, audible/visible trade-off): per-band group-delay alignment like the
   desktop's comp_rows(). Full alignment costs ~280 ms of visual latency at A0 (order 2).
   Proposal: align with a CAP (default 100 ms: everything from A1 up lines up, octave 0 stays
   up to ~180 ms late), cap in a file next to beat_offset_ms.txt; default on.
Order: engine + vendor first (bind-mount trial, then the weekend OTA bakes the full .a),
ShaderEditor second (APK, no OTA needed).

## 2026-10-22 ~13:25 — meter parity + realtime effect thread BUILT; phone swap pending

Engine (submodule): Butterworth bank transliterated from shader_bands.cpp (checked against
band_filters.design(): worst 1.8e-9 dB over all 108 bands at -12..+12 semitone probes),
named ranges, cooked frame + 11 features, bands.bin v3 (regions appended after the ring,
header version 3; BeatClock's v2 offsets untouched). Wrapper (vendor): SCHED_FIFO 5 for the
effect's process thread on first process call, measured cause: the framework thread ran
SCHED_OTHER nice -19 with 70 % involuntary preemptions at 64-84 % of a 400-730 MHz core.
Regression difftest on the new binaries: identical numbers to the morning run on every case,
audio == full, stress clean. ShaderEditor v108 (41ac7cd, installed 13:19) reads v2 and v3.
Pending: bind-mount trial (user's moment: HAL restart cuts audio), verify bands.bin v3 values
+ SCHED_FIFO in /proc, re-measure the full variant's HAL CPU with the bench at home.

## 2026-10-22 ~23:45 — "Desk send": Pixel output tap → Oppo desk speaker (user add-on before the OTA)

Goal (user, at home): hear the Pixel's audio on the desk speaker together with comrade's music,
with no extra hop. Desk speaker path today: comrade → ffmpeg UDP s16le/48k/stereo over USB tether
→ Oppo Find X2 Pro Termux (`listen-amd`: ffplay `udp://@:1236` → pulse SLES sink → Bluetooth).
Decisions:
1. Merge point = the Oppo's pulseaudio (additive mix; the comrade stream never waits on the
   phone one). NOT via comrade (user: JDSP already shapes the phone audio; one hop fewer).
   Pixel reaches the Oppo over Tailscale (user turns the Oppo's Wi-Fi on at the desk).
2. JDSP engine (full variant only, like the meter): a post-chain OUTPUT TAP after postGain +
   limiter, written as s16 interleaved stereo into a mmap'd ring file
   `/data/data/com.termux.jdsp/files/tap.bin` (same dir/sepolicy as bands.bin; pure memory
   writes on the audio thread, no syscalls). Header: magic "JDTP", version 1, seq = frames
   written (release store), ringFrames 32768 (0.68 s @ 48k), channels 2, sampleRate, flags
   (enabled, muteLocal). Cmd 10107 payload 7 floats [band=0, enabled, muteLocal, gainDb, 0,0,0]
   (vsize 28, the only free block size); Lua path `cmd == 10107`.
3. "Mute local" is part of the toggle (default ON): Android applies track volume BEFORE the
   effect, so phone volume 0 would silence the tap too; the engine zeroes its output after
   copying instead. Gain -24..+12 dB on the tap only.
4. Pixel Termux sender `jdsp-tap-send` (C, vendor `tools/`): polls tap.bin every 5 ms, sends
   240-frame (960-byte) UDP packets to the Oppo :1237; when enabled but the tap stalls (nothing
   playing) it sends SILENCE at the real rate so ffplay's buffer never drifts; when disabled it
   sends nothing. Started from `~/.termux/boot/`.
5. Oppo: `listen-pixel` = `listen-amd` twin on `udp://@:1237` + boot script `21-listen-pixel.sh`.
6. App: "Desk send" card (enable, mute local, gain) next to the other dynamics cards.
Latency for phone sounds at the speaker ≈ the comrade path's (~1.1 s, Bluetooth tail dominates);
fine for music/notifications, not for anything that must match the phone's screen.
Order: engine + vendor (bind-mount swap + listen), app APK, Pixel sender, Oppo receiver;
THEN the OTA bake (this lands in the Soong slot .a).
