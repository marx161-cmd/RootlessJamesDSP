# Chroma band-meter (108-cell) — implementation spec

Not yet implemented. Written 2026-09-09 as a handoff spec (e.g. for
DeepSeek to implement). Approach scoped/approved 2026-09-09 — see
"Settled decisions" below — read this alongside `SCOPE.md` before
starting.

## Goal

Extend JDSP's existing Lua-exposed band meter from 5 coarse
bass-to-treble taps to a 108-cell pitch-aware grid: 9 octaves × 12
semitones (pitch classes), so `reactive.lua` (or a new script) can
drive genuinely pitch/register-aware behavior instead of just coarse
energy bands. This mirrors the (row=register, col=pitch-class) 9×12
convention already proven on the desktop side (comrade's
`shader-backdrop` project, `ROWS=9 COLS=12`), so the two projects share
the same mental model even though they don't share code.

## Current architecture (verified against source, 2026-09-09)

Three files, three things to change. All paths relative to
`app/src/main/cpp/`.

**1. `jdsp_header.h:18`**
```c
#define LUA_BAND_METER_COUNT (5)
```
Sizes everything downstream. This is the only place the band count is
declared — bump it and steps 2-3 mostly follow automatically since the
processing loop is already written generically against this macro.

**2. `libjamesdsp/Main/libjamesdsp/jni/jamesdsp/jdsp/jdspController.c`**
- Lines 249-254: hardcoded center-frequency/Q tables —
  ```c
  static const double g_bandMeterCenterHz[LUA_BAND_METER_COUNT] = {
      90.0, 250.0, 700.0, 2000.0, 6000.0
  };
  static const double g_bandMeterQ[LUA_BAND_METER_COUNT] = {
      0.9, 1.0, 1.1, 1.2, 1.2
  };
  ```
- Lines 257-266: `bandmeter_update_coeffs()` — computes RBJ
  constant-skirt-gain bandpass biquad coefficients (Direct Form II
  Transposed) per band, per the standard Q/center-Hz.
- Lines 469-511: `JamesDSPProcess()`'s 30Hz Lua tick. Runs the whole
  chain, then — gated by `luaTickCounter`, not every sample buffer —
  loops over `n` samples, running all `LUA_BAND_METER_COUNT` biquads on
  a **downmixed copy** of the post-chain signal (comment at line 242-245
  confirms it never writes back into the real signal path — analysis
  only, can't affect playback), then pushes the per-band RMS energies
  via `lua_bridge_set_bands()`.

This whole loop already scales with the macro — no code-shape change
needed here beyond the center-Hz/Q table (see "What changes" below).

**3. `lua_bridge.c`**
- Line 111: `static float g_band_energy[16] = {0};` — **hardcoded to
  16, independent of `LUA_BAND_METER_COUNT`.** This is the second real
  cap and must be bumped separately.
- Lines 114-119: `jdsp_get_band_energy(n)` — Lua-facing getter, already
  bounds-checks against `g_num_bands` dynamically, no change needed.
- Lines 1266-1271: `lua_bridge_set_bands()` —
  ```c
  void lua_bridge_set_bands(const float *energies, int num_bands) {
      if (num_bands < 0) num_bands = 0;
      int n = num_bands < 16 ? num_bands : 16;   // <-- silently truncates past 16
      g_num_bands = n;
      memcpy(g_band_energy, energies, n * sizeof(float));
  }
  ```

**Lua side** (`scripts/reactive.lua`): only ever calls
`jdsp.get_band_energy(1)` through `(5)` today (lines 101, 122-125) —
pure consumer, no native-facing config API exists (`jdsp.command()`/
`jdsp.set_property()` don't expose band count or center frequencies).

## Confirmed: this requires a native rebuild

The 5 taps are **not** FFT-derived — no raw FFT bin array exists
anywhere in this path for Lua to re-bucket itself. They're 5 dedicated
per-sample IIR biquads computed natively. There is no way to get to 108
bands from Lua/`reactive.lua`/ShaderEditor alone; the center
frequencies, Q, and band count are all compile-time/native-only. This
needs an NDK rebuild + the existing OTA deploy chain (see
`BUILD_ROOT.md`), not a scripting change.

## What changes

> **DECIDED 2026-09-09** (supersedes the deconvolution discussion in
> items 2 and 5 below): go with a single uniform `CHROMA_Q ≈ 17.3`
> (mirrors EasyEffects' per-band `Q = f0/bandwidth`), no deconvolution,
> no per-row Q curve. Rationale in "Settled decisions" at the bottom.
> The deconvolution / `B⁺` and per-row-`CHROMA_Q(row)` text that follows
> is retained as escalation paths only.

1. **`jdsp_header.h`**: `LUA_BAND_METER_COUNT` → `108`, and add
   `#define CHROMA_Q (17.3)` alongside it (see Settled decisions).

2. **`jdspController.c`**: replace the 5-entry literal tables with a
   108-entry chroma grid. Recommend computing this procedurally rather
   than hand-typing 108 literals — e.g. at the same point
   `bandmeter_update_coeffs()` is already called in a loop (line
   482-483, only runs when `fs` changes, so cost is a non-issue):
   ```c
   // row = octave (0=lowest .. 8=highest), col = pitch class (0..11)
   // matches shader-backdrop's ROWS=9 COLS=12 convention, base A0=27.5Hz
   for (int row = 0; row < 9; row++) {
       for (int col = 0; col < 12; col++) {
           int b = row * 12 + col;
           double semitone = (double)(row * 12 + col);
           double centerHz = 27.5 * pow(2.0, semitone / 12.0);
           bandmeter_update_coeffs(jdsp->fs, centerHz, CHROMA_Q, b);
       }
   }
   ```
   Range: 27.5 Hz (row 0, col 0) to ~13.3 kHz (row 8, col 11) — 9
   octaves, same top-of-range reasoning as the desktop project (their
   `ROWS=9`, not the full 10, because the 10th octave read as dead/flat
   there — that reason doesn't necessarily apply to this native path
   since it's not derived from the same FFT/mic-input pipeline, but 9
   octaves keeps both projects' conventions aligned; going to 10 is a
   free choice if the low end matters less than reaching higher).

   **`CHROMA_Q` is the one real design parameter, not a copy of the
   existing 0.9-1.2 values** (those were tuned for 5 *widely spaced*
   taps meant to overlap generously). Unlike the old taps, this can be
   solved analytically rather than tuned by ear — the RBJ
   constant-skirt bandpass's exact normalized response is
   `|H(f)|² = 1/(1+δ²)`, `δ = Q·(f/f0 − f0/f)`. For a neighbor `n`
   semitones away, `f/f0 = r^n` with `r = 2^(1/12) ≈ 1.05946`, so
   bleed from a neighbor `n` semitones away, in dB, is:
   ```
   δ(n, Q)      = Q · (r^n − r^-n)
   bleed_dB(n,Q) = 10·log10(1 + δ²)
   ```
   Exact values (not approximations):

   | Q  | 1 semitone away | 2 semitones away |
   |----|------------------|-------------------|
   | 8  | −2.7 dB          | −6.5 dB           |
   | 17.3 | −7.0 dB        | −12.3 dB          |
   | 25 | −9.7 dB          | −15.4 dB          |
   | 35 | −12.4 dB         | −18.2 dB          |
   | 50 | −15.4 dB         | −21.3 dB          |

   (Corrected 2026-09-09: the Q=8 / 2-semitone cell read −8.5 dB; the
   exact value is −6.5 dB — an arithmetic slip. Every other cell checks
   out against `10·log10(1 + [Q·(r^n − r^-n)]²)`.)

   Pick `CHROMA_Q` by deciding a target bleed spec (e.g. "adjacent
   semitone must be ≥12dB down" → `Q≈35`) and solving/interpolating
   from the table above, rather than tuning by ear.

   **The same math exposes a real tradeoff against a high, uniform
   Q**: a resonant biquad's settling time scales as `τ ≈ Q/(π·f0)`. At
   `Q=35`, row 0 (27.5Hz) settles in **~400ms** — far too slow to track
   a bass transient (a kick drum has come and gone by then). The same
   `Q=35` at 440Hz (mid-grid) settles in **~25ms**, fine. So a single
   fixed *high* `CHROMA_Q` for all 108 bands forces a bad choice: tight
   enough for real pitch isolation up top, or fast enough to track bass
   transients, not both.

   **Preferred fix: keep Q LOW everywhere (fast, cheap, good transient
   tracking, including at bass), and remove the resulting bleed
   mathematically afterward instead of filtering it out physically.**
   Since `b = row·12 + col` is one continuous chromatic index (not
   wrapping per octave), the bleed between band `i` and band `j`
   depends only on `|i−j|` — the "bleed matrix" `B` where
   `B[i][j] = |H_i(f_j)|² = 1/(1+δ(|i−j|,Q)²)` is **Toeplitz**: fully
   described by 108 numbers (one per semitone-distance), not 108×108
   independent entries. Given `measured = B·true` (the raw low-Q,
   bleedy readings are the bleed matrix times the real per-semitone
   energies), recovering `true ≈ B⁻¹·measured` is a **fixed** linear
   correction — compute `B⁻¹` once, OFFLINE (e.g. numpy), bake it in as
   a constant table, apply it live as a single matrix-vector multiply
   (~12k multiply-adds at 30Hz — trivial, cheaper than the biquads
   themselves).

   Real caveat, not a footnote: at low Q the bands overlap heavily,
   which pushes `B` toward singular — a raw inverse can amplify noise
   instead of cleaning the signal. Use a regularized inverse instead,
   `B⁺ = (BᵀB + λI)⁻¹Bᵀ` (Tikhonov/ridge), with `λ` picked to trade off
   correction sharpness against noise amplification — still solved
   once, offline.

   This also changes where the correction can live: since it's just a
   fixed-coefficient weighted sum over the 108 already-exposed values,
   the deblend step doesn't need to be native code at all — it can run
   in `reactive.lua` (or a new script), tunable/iterable by editing
   `λ`/the baked table without a native rebuild, while the native side
   only needs to expose 108 *low-Q* bands (much simpler than solving a
   per-row Q-scaling curve). Fallback if deconvolution turns out to be
   more than wanted: scale `CHROMA_Q` up with row instead (a 9-entry
   `CHROMA_Q(row)` table, looser/faster at the bottom rows, tighter at
   the top) — simpler, no matrix math, but re-introduces the bass
   settle-time-vs-isolation tradeoff the deconvolution approach avoids.

3. **`jdspController.c` line ~504-508**: `bandEnergies[LUA_BAND_METER_COUNT]`
   already scales with the macro — no shape change, just confirm the
   stack array (108 floats) and the per-tick cost are fine. Back-of-
   envelope: 108 biquads × buffer-size multiply-adds, once per 30Hz
   tick — trivial next to what the rest of the chain already does per
   sample; still worth confirming empirically after building rather
   than assuming.

4. **`lua_bridge.c`**:
   - `g_band_energy[16]` → `g_band_energy[108]`.
   - `lua_bridge_set_bands()`'s `num_bands < 16 ? ... : 16` → `< 108 ? ... : 108`.

5. **Lua side**: `jdsp.get_band_energy(1..108)` works as-is once the
   above ships (1-based, `index = row*12 + col + 1`). Worth adding a
   small helper in `reactive.lua` (or a new script) so callers write
   `band(row, col)` instead of hand-rolling the index arithmetic every
   time. Also consider whether 108 individual `get_band_energy()` calls
   per Lua tick are fine as-is (almost certainly yes at 30Hz) or whether
   a bulk array-returning getter is worth adding — not required, just
   an option if 108 individual C-call round-trips ever look like a
   measurable cost, which is unlikely.

   If going with the deconvolution approach above: this is also where
   the deblend step lives — pull all 108 raw values, apply the baked
   `B⁺` matrix (a 108-entry Toeplitz kernel, not a full 108×108 table,
   per the structure above) as a weighted sum per output band, entirely
   in Lua. No native involvement beyond exposing the 108 raw low-Q
   values in the first place.

6. **Build/deploy** (corrected 2026-09-09 — `BUILD_ROOT.md` is upstream's
   rootless/rootfull install doc, NOT this project's chain):

   The engine ships on-device as a single self-contained vendor
   audio-effect `.so`, `/vendor/lib64/soundfx/libjamesdspaidl.so`, built
   in two stages. All three files changed here (`jdsp_header.h`,
   `jdspController.c`, `lua_bridge.c`) are engine sources under
   `app/src/main/cpp/`, so this is an engine rebuild — the app APK and
   its JNI wrapper don't change.

   1. Rebuild the engine STATIC lib from Gradle/CMake in
      `/home/comrade/builds/android/JamesDSP-src`: the `jamesdsp_static`
      target (additive STATIC+PIC target added 2026-08-15/16, same
      sources as the `jamesdsp` SHARED target the standalone app uses).
      Configure via `./gradlew`, then ninja `jamesdsp_static` →
      `app/.cxx/<cfg>/arm64-v8a/libjamesdsp_engine_static.a`. (The
      `lsp-*` submodules must be initialized — they already are, from
      the 2026-09-04 build.)
   2. Stage the `.a` into the ROM vendor tree:
      `~/homelab/crDroid/vendor/JamesDSP/proprietary/vendor/lib64/soundfx/libjamesdsp_engine_static.a`.
   3. Rebuild the AIDL wrapper in Soong (crDroid checkout, `lunch
      lineage_blazer bp4a user`): `m libjamesdspaidl` →
      `out/target/product/blazer/vendor/lib64/soundfx/libjamesdspaidl.so`.
   4. Verify before shipping: `readelf -d` (no `libjamesdsp_engine.so`
      in NEEDED), `nm -D --defined-only` (symbols present), record the
      sha256 — see `HEALTH_CHECK.md` §2 and
      `vendor/JamesDSP/PROJECT_SUMMARY.md` "single-file merge".
   5. **Test loop — APatch module overlay, NOT a full OTA.** A vendor
      `libjamesdspaidl.so` change can't be tested by `adb push` + restart:
      the file is held by `vendor.audio-hal-aidl` (not `audioserver`, which
      only restarts the effect factory), and `/vendor` is read-only on a
      dm-verity-backed block device. Package the freshly built `.so` as an
      APatch module and overlay it at boot:

      - Stage a module dir:
        ```
        module.prop                                  # id/name/version/versionCode/author/description
        vendor/lib64/soundfx/libjamesdspaidl.so
        ```
        (top-level `vendor/` inside the module maps onto `/vendor/` — the
        same convention as the SystemUI module's top-level `system_ext/`
        in `DOCKED_PIP.md`.)
      - Zip it flat: `cd <staging-dir> && zip -r ../jdsp-chroma-test.zip .`
        (`module.prop` at zip root; APatch accepts both flat and
        single-top-level-dir layouts).
      - Install via the APatch manager, then reboot — overlayfs mounts at
        early boot, so reboot is required both to apply and to revert.
      - Verify: `dumpsys android.hardware.audio.effect.IFactory/default`
        still lists `libjamesdspaidl.so`, and a Lua probe / `reactive.lua`
        reads the new 108 bands during real playback. `HEALTH_CHECK.md` §2's
        sha256 gate doesn't apply to an overlay (the on-disk `/vendor` file
        is unchanged) — confirm the *overlaid* file is what loaded instead
        (a new unique symbol, size, or `dumpsys` UUID match).
      - Revert: disable/remove the module + reboot. Not durable by design.

      (Historical no-reboot alternative — the 2026-08-06 bind-mount
      hot-swap: `adb push` the `.so` to `/data/local/tmp/`, `chcon
      u:object_r:vendor_file:s0`, `mount --bind`, then `setprop ctl.restart
      audioserver`. Caused a lockscreen-hang incident via a
      stop→immediate-umount race against `vendor.audio-hal-aidl`'s
      auto-respawn — see the 2026-08-06 JDSP capsule. Prefer the module
      overlay.)

   6. **Promote** via the gated ROM/OTA flow, NOT adb-install-durable:
      `blazer-ota-build` → boot-chain-patch → `ota_from_target_files` →
      flash; then run `HEALTH_CHECK.md` in full (§2 sha256, §4
      reactive.lua liveness / audioserver read perms). See
      `docs-consolidated/70-crdroid-rom.md` and `APPS_MANIFEST.md`.

   7. Lua-side changes (`reactive.lua` + `band(row,col)` helper) need NO
      native rebuild or OTA — they live at
      `/data/data/com.termux.jdsp/files/scripts/` on-device, tunable
      live, subject to the audioserver-permission rules in
      `HEALTH_CHECK.md` §4.

## Settled decisions (2026-09-09)

Recorded after source verification + a "keep it reactive and lightweight"
directive. These supersede the questions below.

1. **9 octaves** — `ROWS=9 × COLS=12 = 108`, base A0 = 27.5 Hz → top
   ≈ 13.3 kHz. Confirmed by the operator; mirrors `shader-backdrop`'s
   `ROWS=9` (the 10th octave reads dead there and isn't wanted).

2. **Uniform `CHROMA_Q` mirroring the EasyEffects reference — no
   deconvolution, no per-row Q curve.** The bleed characteristic should
   match comrade's EasyEffects `shader_bands` filter bank exactly, since
   that's the proven desktop reference this mirrors. EasyEffects derives
   Q per band as `Q = f0 / bandwidth`; for log-spaced bands one semitone
   apart (`ratio = 2^(1/12)`), that's a constant
   `sqrt(ratio)/(ratio − 1) ≈ 17.3` regardless of row.
   - So `#define CHROMA_Q (17.3)` (or compute it as
     `sqrt(ratio)/(ratio−1)` to mirror EasyEffects' code shape).
   - Relative bleed (identical to EasyEffects): adjacent semitone
     ≈ −7.0 dB, two semitones ≈ −12.3 dB. Settling τ = Q/(π·f0):
     ≈ 200 ms @ 27.5 Hz, ≈ 100 ms @ 55 Hz, ≈ 12.5 ms @ 440 Hz — the
     bottom row's 200 ms settle is the same contained tradeoff
     EasyEffects already accepted (see its `shader_bands.cpp` LOG_LO
     note); at 55 Hz and up it's comfortably fast for the 30 Hz tick.
   - Q=8 was considered first (≈ −2.7 dB adjacent, ≈ 93 ms @ 27.5 Hz)
     but is ~4 dB fuzzier than the reference for near-zero reactivity
     gain above the bottom row — rejected in favour of matching
     EasyEffects. Keep it in mind only if the bottom octave's tracking
     ever feels slow.
   - `CHROMA_Q` is still a one-line tune + native rebuild.
   - Escalation paths (retained, not chosen): per-row `CHROMA_Q(row)`
     (native-side) or low-Q + offline Toeplitz `B⁺` deconvolution
     (Lua-side) — both documented above if ±1-semitone fuzz ever proves
     insufficient.

3. **Indices 1..108 are pure chroma; `reactive.lua` is updated, not the
   native mapping.** `index = row*12 + col + 1`, so 1..5 becomes
   A0/A#0/B0/C1/C#1 — sub-bass, which would silently break
   `reactive.lua`'s current `1..5 = bass/lowmid/mid/highmid/treble`
   assumption. Fix is Lua-side (no native rebuild): aggregate octave
   rows into the 5 coarse bands, with ranges chosen so each old tap's
   center (90/250/700/2000/6000 Hz) lands inside its band — bass = rows
   0-1, lowmid = 2-3, mid = 4, highmid = 5-6, treble = 7-8. (Initial
   draft put treble at row 8 = 7-13 kHz; corrected 2026-09-09 because
   the old treble tap was 6 kHz, which lives in row 7, and the air
   octave alone read much quieter.) Add a `band(row, col)` helper so no
   caller hand-rolls `row*12+col+1`.

4. **Bulk getter is deferred.** 108 individual `get_band_energy()` calls
   per 30 Hz tick are almost certainly fine; only add a bulk
   array-returning getter if that ever measures as a real cost.

Still open (post-first-build tuning, not blockers): the exact `CHROMA_Q`
value, and empirically confirming per-tick cost + that the meter tracks
real polyphonic material acceptably at the chosen Q.

## Open questions (resolved 2026-09-09 — kept for history; superseded by "Settled decisions" above)

- Whether to actually go with the deconvolution approach (low Q +
  offline-computed `B⁺` correction, Lua-side, no per-row Q curve
  needed) or the simpler fallback (`CHROMA_Q(row)` scaled up with
  row, native-side, re-introduces the settle-time-vs-isolation
  tradeoff). Deconvolution is the better answer if the linear
  energy-mixing approximation holds up in practice (worth a real check
  against actual polyphonic material, not just assumed) — see both
  options above.
- If deconvolution: the regularization `λ` (correction sharpness vs.
  noise amplification) and the flat low `CHROMA_Q` to build `B` from —
  this is solvable analytically, not a tune-by-ear question
  anymore, but the target dB/response-time balance is still a real
  choice to make.
- 9 vs 10 octaves (see above — free choice here, not forced).
- Whether to keep the old 5-tap `get_band_energy(1..5)` behavior
  side-by-side (e.g. as the first 5 of the new 108, or removed
  entirely) — `reactive.lua` currently depends on exactly `1..5`
  meaning bass/lowmid/mid/highmid/treble; changing what those indices
  mean without updating that script would silently break it.

---

# FINAL ARCHITECTURE (2026-09-09 — supersedes everything above)

The Lua-side band-dump + cleanup approach above was abandoned in practice: a
Lua `tick()` that does 108 `get_band_energy` round-trips + cleanup math +
216 `string.format`s + an ~8 KB file write, 30×/sec on the realtime audio
thread, stalled the audio HAL and made `audioserver` crash-loop (watchdog
SIGABRT). Lesson: do no heavy work and no file I/O on the realtime thread.

Final design (agreed with the operator 2026-09-09):

- **Native** (`jdspController.c`) `mmap`s a fixed **432-byte** region
  (108 × `float32`) once at init, and each 30 Hz tick `memcpy`s the raw
  linear RMS band energies into it. No dB, no normalization, no floor, no
  formatting, no per-tick syscalls — pure capture. This is the whole native
  job.
- **Lua engine untouched.** `lua_bridge.c` and the tick machinery stay
  exactly as-is. Only the `reactive.lua` script (the bridge-file writer) is
  removed.
- **File**: `/data/data/com.termux.jdsp/files/bands.bin` (432 bytes, `0644`).
  `audio_bridge.txt` and its `key=value` format go away. No backwards
  compat — shaders are adapted.
- **ShaderEditor** reads the region and uploads it as a 1D `sampler2D bands`
  texture (108 texels, `R32F`; single-band access =
  `texelFetch(bands, ivec2(i,0), 0).r`).
- **Loudness normalization is shader-side and phone-specific.** The JDSP
  effect sits at a different point in the audio chain than EasyEffects on the
  desktop, so the desktop's fixed `-60 dB` floor does NOT apply. After the
  mmap + texture is live, first step is to dump real raw band values during
  playback and measure the actual range, then set the shader's
  normalization/floor from that.

Explicitly out of scope (all done later in shaders, no native changes):
dB normalization, harmonic subtraction, flatness gate, treble boost, settle
filter, layout (square grid / hexagon circle / fibonacci sphere), coarse
bass/treble derivation.

Build/deploy for the native change: same chain as before — rebuild the engine
static lib, sync it + the header to the vendor tree, `m libjamesdspaidl`,
verify, then the APatch overlay module. Lua script removal is a data-dir
edit, no rebuild.
