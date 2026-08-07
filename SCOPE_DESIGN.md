<!--
Recovered 2026-08-06 from opencode session ses_02ffb5936ffegxAE2H4LMfKoYo
(message prt_fd3f1f319001d4NJhgsIZUHVft, sent 2026-08-05 22:01:16, user -> deepseek-v4-pro).
That session's title ("Optimize compute in Plenum (Agora fork)") is unrelated —
this got pasted into a repurposed session. Saved here verbatim so it's never
"took ages to find" again. SCOPE.md stays as the short status/gap checklist;
this is the full design it's checked against.
-->

<!--
REVISION 2026-08-06 (evening, post-AIDL-crash-fix session). The original
scope below is kept verbatim for history — do not delete it, it's the doc
that "took ages to find" once already. The section immediately below this
comment is the CURRENT plan and supersedes the original's "Module
architecture: shared FFT band decomposition" and "Removed modules" sections
specifically (the shared-FFT EQ+Gate+Compressor merge is dropped — see
below for why). Everything else in the original (signal chain modules not
named below, Data Bridge, NEON FFT optimisation note, OTA deployment
strategy) still stands unchanged.
-->

## Status: 2026-08-06 revision — read this before the original scope below

**Standing goals** (restated because they've had to be re-explained too many
times — treat as durable, not per-session context): pristine audio quality,
low compute, flexibility, steerable/scriptable via Lua. Compute matters
concretely: on the old unoptimized code, running Convolver + EQ together was
already close to the device's ceiling. The point of this rebuild is to open
headroom to run *more* effects at once, not just replace one implementation
with an equivalent one.

**Root cause of this revision**: earlier ports of Limiter/Compressor/Gate did
not port the real, self-contained LSP plugin modules — they were hand-rolled
wrapper code (`lsp_wrap.cpp`) written around the bare `lsp-dsp-units` classes,
built from instructions that said "read the reference, write a new minimal
wrapper" rather than "port the reference." That produced a string of basic,
avoidable bugs (uninitialized `lsp::dsp::init()`, dB-vs-linear unit
mismatches, missing lookahead-delay compensation, double-processing) that a
real port of the reference `plug/*.cpp` files would not have had, because
those files already handle all of it correctly. Don't repeat the pattern:
port the real thing, not a reimplementation of a slice of it.

### Module sourcing (verified against EasyEffects' actual upstream, not assumed)

EasyEffects (the reference this project models "EasyEffects/LSP Dynamics"
after) has migrated most of its dynamics effects from Calf to LSP over time.
Verified via its GitHub changelog/history 2026-08-06 — not from memory:

| Module | Real source | Status |
|---|---|---|
| Limiter | LSP (`lsp-limiter`, migrated from Calf) | **Port real `plug/limiter.cpp`**, replacing `lsp_wrap.cpp`'s hand-rolled version. Must include the lookahead-delay compensation (`dspu::Delay`, `set_delay(get_latency())`) the current wrapper is missing. |
| Compressor | LSP (`lsp-compressor`, single-band) | **Port real `plug/*.cpp`.** Single-band is the default/primary — see below, this is a deliberate choice, not a shortcut. |
| Gate | LSP (`lsp-gate`, migrated from Calf) | **Port real `plug/*.cpp`.** Currently has **no enable toggle at all** (`chain_register("gate", w_gate_lsp, NULL)` in `chain_modules.c` — hardcoded `NULL`, runs unconditionally on every block regardless of UI state). Must get a real enable flag as part of this work. |
| AutoGain | **Not LSP** — EasyEffects' own `src/autogain.cpp`, built on `libebur128` (real ITU-R BS.1770 / EBU R128 loudness, the actual broadcast/streaming standard) | **Port `libebur128` + adapt `autogain.cpp`'s logic.** Portable ANSI C, MIT, ~224KB, no heavy deps — cheap (K-weighting is a couple of biquads, not FFT), so this doesn't cost the low-compute goal anything. Also currently has no enable toggle (same `NULL` bug as Gate) — fix as part of this port, not separately. |
| Multiband Compressor | LSP (`lsp-mb-compressor`, migrated from Calf) | Stays **optional and separate**, not a replacement for single-band. See rationale below. |

### Single-band vs. multiband compressor — deliberate, not a compute shortcut

Multiband compression is a genuine EasyEffects trade-off, not a strict
upgrade over single-band: it prevents cross-band "pumping" (a bass hit
ducking the vocals too) but costs real CPU (its own FFT band decomposition +
one compressor instance per band) and can add latency. EasyEffects' own
guidance is not to blanket-replace single-band compressors with multiband
ones — use multiband when you specifically need per-band control.
Additionally, the old Dynamic Range Compander this was meant to replace was
almost never actually used. So: single-band LSP Compressor is the
default/primary path (driving the rebuilt Compressor card, below). Multiband
Compressor stays available as a distinct, separately-toggleable module for
when per-band control is actually wanted — never on by default, never
merged into the main signal path.

This supersedes the original scope's "Module architecture: shared FFT band
decomposition" section entirely — the shared single-FFT-serves-EQ+Gate+
Compressor design is dropped. It was the more ambitious, more expensive
option, and doesn't fit the low-compute goal as the default path.

### Confirmed untouched (checked, not assumed)

- **Convolver** (`convolver1D.c`): already a two-stage partitioned FFT
  convolver — same core technique as `zita-convolver` (short head partitions
  for latency, longer tail partitions for efficiency), not an inferior
  algorithm. Evidence: an open EasyEffects GitHub issue is literally titled
  "...for JamesDSP parity" — their community asking to catch up to JDSP's
  convolver, not the reverse. Not touching it. (The convolver-distortion
  symptom reported this session traces to the Limiter's missing
  delay-compensation, not the convolver — see Limiter above.)
- **Crossfeed**: already uses `libbs2b`, the same library EasyEffects uses
  for this. No change.
- **Reverb**, **StereoWiden**: explicitly deprioritized this pass, not
  because they're confirmed fine — just not investigated. Leave as-is.

### UI: one real card, not two

The old "Compander" preference card (`time constant`/`granularity`/
`TF transforms`/per-band `response`) and the newer "LSP Dynamics" card
(`threshold`/`ratio`/`attack`/`release`, mode dropdown) currently coexist —
the Compander card is dead (feeds cmd 115/1200 into removed native code),
and the LSP Dynamics card only ever sends the compressor command regardless
of its Limiter/Gate mode dropdown options (those do nothing). Intent was
always to repoint the *existing* Compander card at the new engine, not add a
parallel one. Fix: delete the old Compander fields, delete the separate LSP
Dynamics card, rebuild **one** card in the Compander's slot with the real
LSP compressor parameters (threshold, ratio, knee, attack, release, hold).

### Explicitly deferred, not forgotten

- A queryable/accessible debug-dump mechanism for the engine (state +
  recent events, in the shape of Cybersyn's `EXPLAIN` endpoint) — proposed
  2026-08-06, declined for this pass ("don't like that shape"). Revisit
  later if wanted; don't reintroduce that specific shape without asking again.
- Reverb/StereoWiden real-source investigation (Calf reverb is a candidate,
  unconfirmed whether it's worth it).

# JamesDSP Native Rebuild — Scope

Target: replace the AIDL audio effect library (`libjamesdspaidl.so`) with an
enhanced version built from the fork at `app/src/main/cpp/`. Same AIDL interface,
extended parameter codes for new features. Ship in crDroid OTA.

## Signal chain

```
Input (AudioFlinger PCM float)
  → [AutoGain]              — no FFT, cheap RMS integration
  → [Gate]                  — no FFT, cheap RMS threshold
  → [Arbitrary Response EQ] — FIR via NEON FFT convolution (AutoEQ profiles)
  → [Convolver]             — partitioned NEON FFT, per-bin wet/dry bypass
  → [ViPER-DDC]             — text-based SOS IIR
  → [Liveprog / EEL2]       — Nullsoft VM, per-sample bytecode
  → [Multimodal EQ + Gate + LSP Multiband Compressor]
                             — ONE FFT, shared band decomposition
                             — three layers per band: EQ → Gate → Compressor
  → [Tube]                  — harmonic exciter
  → [Crossfeed]             — BS2B IIR + HRTF NEON FFT convolution
  → [Reverb]                — algorithmic stereo reverb
  → [StereoWiden]           — warped PFB 5-band
  → [Limiter]               — brickwall soft-clamp, last line of defense
  → [Data Bridge]           — writes per-block metrics to shared file
  → Output (back to AudioFlinger)
```

## Module architecture: shared FFT band decomposition

The Multimodal EQ, Gate, and LSP Multiband Compressor all operate on the same
frequency decomposition. A single FFT pass splits the signal into N configurable
bands (typically 8-15, matching the EQ's band layout). Each band then passes
through three processing layers:

```
FFT once → per-band energy ──┬── EQ layer:     apply static per-band gain curve
                              ├── Gate layer:   if energy < noise_floor → 0
                              └── Comp layer:   LSP multiband dynamics
                                                (threshold, ratio, knee,
                                                 attack, release, lookahead,
                                                 sidechain boost, stereo split)

IFFT once → output
```

The band definitions (crossover frequencies, filter shapes) are configured once
and shared by all three layers. The EQ drives the frequency response curve,
the Gate controls noise floor per band, and the LSP Compressor handles
dynamic gain reduction per band.

### LSP Multiband Compressor (replaces Dynamic Range Compander + Dynamic Bass Boost)

Sourced from Linux Studio Plugins (lsp-plug.in), LGPL-licensed, pure C++ DSP.
Linear Phase mode uses FFT-based FIR crossover filters — same FFT engine
as our NEON-optimized `codelet.c`, zero redundancy.

Key features:
- Up to 8 bands, flexible crossover frequencies
- Linear Phase mode (FFT FIR crossover, zero phase distortion)
- Sidechain boost (pink/brown noise compensation for per-band weighting)
- Lookahead per band (predictive gain control)
- Two-stage release (envelope-dependent release timing)
- Independent stereo channel processing

## New modules

### AutoGain

No FFT. Cheap RMS + gated EBU R128 loudness integration. Slowly adjusts input
gain toward a target integrated loudness. Keeps the downstream compressor in
its sweet spot regardless of track mastering level.

### Gate (pre-convolver)

No FFT. Cheap full-spectrum RMS energy threshold. Kills sub-noise-floor
content before it hits the convolver (silence shouldn't give reverb).

### Gate layer (post-convolver, shared FFT)

Per-band noise gate inside the shared FFT band decomposition. Same RMS
threshold logic, per-band.

### Limiter

Standalone, last in chain. Brickwall soft-clamp:
`clamp(sample, -1.0, 1.0)` with configurable knee width. No overshoot possible.

### Data Bridge

Shared file: `/data/data/com.termux.jdsp/files/audio_bridge.txt`

Multi-line key=value format, atomic write per audio block. Read by
ShaderEditor (`BuiltinSystemUniforms.java`) and other `com.termux.*` apps
via direct file read (zero-sandbox, shared UID 1000).

```text
audio_level=0.4567   # instantaneous envelope (beat-reactive, ~30Hz)
audio_mood=0.3210    # integrated loudness (atmosphere, slow)
audio_peak=0.8912    # per-block peak (visualisation)
```

For mpv-only playback, the existing `audio_reactive.lua` →
`/data/data/com.termux.mpv/files/audio_level.txt` path still works alongside.
When JamesDSP processes system-wide audio, the bridge file provides the richer
three-value format.

## Optimisation: NEON FFT core

All FFT code in `generalDSP/codelet.c` (4641 lines of hand-unrolled scalar
radix-2 FHT) gets a NEON/SVE2 rewrite targeting Tensor G5 (ARMv9-A, 8 cores).

**Impact**: every FFT-using module gets ~3-5x faster:
- LSP Multiband Compressor (Linear Phase mode)
- Convolver (per-block forward FFT + per-segment MAC)
- Multimodal EQ FIR mode
- Arbitrary Response EQ FIR generation + convolution
- Crossfeed HRTF convolution

This is the single highest-impact optimisation in the project and should
be done first.

## Existing modules (keep, no changes planned)

| Module | Status |
|--------|--------|
| Arbitrary Response EQ | FIR via NEON FFT. Works. |
| ViPER-DDC | Text SOS IIR. Works. |
| Convolver | Partitioned FFT, two-stage. NEON-accelerated. Per-bin bypass added. |
| Liveprog / EEL2 | Nullsoft VM. Per-sample bytecode. Trace-JIT deferred. |
| Multimodal EQ | 15-band, FIR+IIR modes. Shared FFT with compressor. |
| Tube | Polynomial exciter. Functional. |
| Crossfeed | BS2B IIR + HRTF convolution. Functional. |
| Reverb | sndfilter, 19 presets. Functional. |
| StereoWiden | Warped PFB 5-band. Works. |

## Removed modules

| Module | Replaced by |
|--------|-------------|
| Dynamic Range Compander | LSP Multiband Compressor (shared FFT chain) |
| Dynamic Bass Boost | LSP Multiband Compressor (covers bass dynamics) |

## Build signpost

Current `rootFullRelease` variant at `com.termux.jdsp`, native libs via CMake.
Signing reads from `~/.gradle/gradle.properties` (Termux keystore, same as
Plenum/Agora) after swapping `signingConfigs` before `buildTypes` in
`android {}` block.

LSP Dynamics Kotlin/Java code removed from app layer. LSP `.so` prebuilts
(`liblsp-combined.so`, `liblsp-dynamics-bridge.so`) still bundled in APK
but inert — strip when LSP sources are compiled directly into the native
library via CMake.

## OTA deployment

Two-OTA strategy for the package rename (`james.dsp` → `com.termux.jdsp`):
1. **OTA 1**: Remove `james.dsp` from product image
2. **OTA 2**: Add `com.termux.jdsp` to product image with updated
   `audio_effects_config.xml`, privapp whitelist, APPS_MANIFEST.md

Both packages cannot coexist — they would fight over the same audio effect
UUID. The native library is rebuilt from this source tree and deployed to
`/vendor/lib64/soundfx/libjamesdspaidl.so`.

## REVISION 2026-08-07: Limiter/Compressor/Gate — faithful port, not a wrapper

Root cause of a recurring pattern this project kept hitting: `lsp_wrap.cpp`
was never actually a port of the real LSP plugins. It was hand-derived
glue code around bare `dspu::` primitives (Compressor, Gate, Limiter,
Sidechain, Delay), reinventing the reference plugins' control flow instead
of transcribing it — which is exactly why pieces kept turning up missing
one bug at a time (`lsp::dsp::init()`, dB/linear unit mismatches, missing
lookahead-delay compensation, double-processing, and now this round: real
`Bypass` crossfade, and a hardcoded Limiter attack constant that was
actually the reference plugin's *minimum* value, not its default).

This revision checked the actual reference sources
(`/usr/src/debug/lsp-plugins-1.2.26-1.fc43.x86_64/modules/lsp-plugins-{limiter,compressor,gate}/src/main/plug/*.cpp`
and their `meta/*.h`) directly, not memory/inference, and transcribed the
real signal-flow pieces that matter for our simplified (no UI, no
metering, no oversampling) build.

**Ported this round (verified against real reference source):**

- **Limiter attack**: was hardcoded `0.25ms` in `JLimiterSetCoefficients`
  (`jdspController.c`) — that is `meta::limiter_metadata::ATTACK_TIME_MIN`,
  the most aggressive setting the algorithm allows, not
  `ATTACK_TIME_DFL` (5.0ms). Every limiter instance has been running the
  fastest, least stable attack the class supports. Fixed to use the real
  default (5.0ms). Release now clamped to the reference's actual tested
  range (0.25–20ms per `RELEASE_TIME_MIN/MAX`) instead of passing through
  the UI's old legacy range (1.5–500ms) unclamped; UI range updated to
  match (default 60ms → 5ms, matching `RELEASE_TIME_DFL`).
- **Compressor/Gate real `Bypass`** (`lsp::dspu::Bypass`, from
  `lsp-dsp-units/src/main/ctl/Bypass.cpp` — already vendored in-tree,
  previously unused): both reference plugins crossfade dry/wet over ~5ms
  on engage/disengage (`c->sBypass` in both `compressor.cpp`/`gate.cpp`).
  Our `chain_process()` did a hard `continue` when disabled — an instant
  step in gain, audible as a click every time either was toggled from the
  UI. Now each module owns a real `Bypass` internally
  (`CompHandle`/`GateHandle` in `lsp_wrap.cpp`) and `chain_register()`
  passes `NULL` for their toggle so `chain_process()` always calls them —
  the module's own `Bypass` state decides whether to actually run the
  DSP (skipped entirely once fully settled disabled — same "cost ~0 when
  off" contract as before, just enforced one level down).
- Removed `lsp_bridge_gate.h/.cpp` — an orphaned, never-built duplicate
  Gate wrapper defining the same public symbol names as the real one in
  `lsp_wrap.cpp`. Dead code, not a working alternative; deleted rather
  than left to confuse a future session.

**Investigated and ruled out** (checked against real source, not assumed):
Limiter's `knee` parameter (`fKnee`) only ever feeds the ALR gain-curve
computation (`update_settings()`'s `UP_ALR` block); ALR is off in our
build, so `fKnee` is inert regardless of its value — the `1.0f` we pass is
in fact `KNEE_LEVEL_DFL` from the real metadata, i.e. already correct.
`Delay::clear()` (called by the reference on sample-rate change only) does
not apply to us — our sample rate is fixed at runtime, that branch never
fires either way. Confirmed by reading `Limiter::process()`'s actual gain-
curve algorithm directly: the `gain[i]` it returns already corresponds to
sidechain input delayed by `nLookahead` samples, which is exactly the
contract our external `Delay`-based compensation assumes — that part of
the original port was already correct.

**Explicit drop-list (intentionally not ported, so this doesn't get
silently rediscovered as "missing" again):**

- Oversampling (`sOver`/`sScOver`, Lanczos variants) — real feature, real
  CPU cost, deliberately skipped per the low-compute goal.
- ALR (Automatic Level Regulation) on the Limiter — real, cheap, possibly
  worth adding later, not done this round.
- Limiter dual-mono + explicit `fStereoLink` gain-blend — we use a single
  combined `max(|L|,|R|)` sidechain feeding one gain curve applied to both
  channels instead. Simpler, always-fully-linked; matches an earlier
  session decision, not revisited here.
- Compressor MS/mid-side mode, feedback sidechain topology, external
  sidechain input, per-channel dry/wet mix knob (`fWetGain`/`fDryGain`) —
  internal-sidechain, always-100%-wet is all our UI exposes.
- Gate/Compressor user-facing lookahead (`sLaDelay`) — `dspu::Compressor`
  and `dspu::Gate` have zero built-in latency of their own (unlike
  Limiter, which does); the reference's `sLaDelay` only matters if a
  lookahead *parameter* is exposed, which ours isn't, so it's correctly a
  no-op for us rather than a missing feature.
- All metering/graph/mesh output, UI port binding, `IStateDumper` dump
  support — host/UI plumbing, not audio-path.
- Limiter has no `Bypass` — unlike Compressor/Gate, it has no user-facing
  enable/disable at all (always-on output stage), so there is no
  transition for a crossfade to ever smooth. Not an oversight.

Still not started: AutoGain → real `libebur128` port (currently patched
in place, not ported — see `project_jdsp_dsp_engine_goals` memory).
`/vendor/lib64/soundfx/libjamesdspaidl.so`.
