# HANDOFF — JDSP → ShaderEditor 108-band audio pipeline

Self-contained handoff. Paste this whole file to me (or any agent) to resume
without re-deriving anything. Written 2026-09-10.

## 1. What this is

The JamesDSP engine (a vendor audio effect on the Pixel "blazer") now computes
**108 chroma band energies** (9 octaves × 12 semitones) every 30 Hz tick, and
shares them to the ShaderEditor live-wallpaper app through a memory-mapped file,
so shaders can react to *pitch* and *register*, not just a coarse loudness.

Producer: native C in the JDSP engine (runs in the audio HAL, uid 1041).
Consumer: ShaderEditor app (uid 1000) uploads the data as a `sampler2D bands`.

## 2. Data format (bands.bin)

- File: `/data/data/com.termux.jdsp/files/bands.bin`
- Size: **432 bytes = 108 × float32**, little-endian, **raw linear RMS**
  (NOT normalized — the shader does dB/floor).
- Layout: `band = row * 12 + col`, `row = 0..8` (octave), `col = 0..11`
  (pitch class). `row 0 = A0 (27.5 Hz)`, `row 8 ≈ 13 kHz`; `col 0 = A`,
  `col 11 = G#`.
- Written by the engine via `mmap` + `memcpy` every tick — no syscalls, no
  file I/O on the realtime audio thread.
- Owned `audioserver:audio` (uid 1041, gid 1005), mode 0644. **Pre-created
  by hand** because the HAL can't create files in the 755 `files/` dir.

## 3. Files (exact paths)

Native (repo `~/builds/android/JamesDSP-src`):
- `app/src/main/cpp/libjamesdsp/Main/libjamesdsp/jni/jamesdsp/jdsp/jdspController.c`
  — `band_shm_setup()` (open/ftruncate/mmap once at `JamesDSPInit`), and the
  `memcpy(g_bandShm, bandEnergies, …)` in `JamesDSPProcess`'s 30 Hz block.
- `…/jdsp/jdsp_header.h` — `#define LUA_BAND_METER_COUNT (108)` and
  `#define CHROMA_Q (17.3)`.
- `app/src/main/cpp/lua_bridge.c` — `JDSP_BAND_ENERGY_MAX (108)`,
  `g_band_energy[108]`. (This file is NOT in the submodule; the other two are.)

ShaderEditor (repo `~/builds/android/ShaderEditor`):
- `app/src/main/java/com/termux/shadereditor/opengl/GlDevice.java` —
  `uploadFloatTexture2D(...)` (R32F, NEAREST, CLAMP_TO_EDGE).
- `…/opengl/BuiltinSystemUniforms.java` — reads `bands.bin` (~30 Hz),
  uploads, binds it as uniform name **`bands`**.

Specs/notes:
- `~/builds/android/JamesDSP-src/CHROMA_BAND_METER_SPEC.md` — "FINAL
  ARCHITECTURE (2026-09-09)" section supersedes everything above it.
- `~/homelab/crDroid/device/google/blazer-secur/JDSP_AUDIO_BRIDGE_STAGING.md`
  — the OTA-tree staging/promotion TODO (loud ⚠ STAGED banner).

## 4. Shaders (in `~/builds/android/JamesDSP-src/scripts/`)

- `ball-audio-helix.glsl` — **the working PoC**: quaternion trackball ball +
  fibonacci dual-helix cells. 4 passes (`state`, `audio`, `spark`, `image`).
- `tonnetz.glsl` — faithful port of the desktop tonnetz_pins: full per-band
  cleanup (`==field==`) + **cheap flat hex-wheel** (`==image==`, no raymarch).
- `hex-circle.glsl` — earlier crude polar hex (superseded by `tonnetz.glsl`).
- `ferrofluid-ball.glsl` — raymarched spike ball; **too slow on phone** (~50 fps
  @ half-res), abandoned.
- `chroma-grid-test.glsl`, `sphere-hex-test.glsl` — raw grid/sphere test shaders.

## 5. Knobs (tunables)

Native:
- `CHROMA_Q = 17.3` — bandpass Q; derived as `Q = f0/bandwidth` for 1-semitone
  log spacing, matching EasyEffects (adjacent-semitone bleed ≈ −7 dB). Do NOT
  raise it much — settling time `τ ≈ Q/(π·f0)` kills bass transients.
- Bandpass is **peak gain = 1** (`b0 = alpha/a0` in `bandmeter_update_coeffs`).
  The old `Q·alpha` form inflated every band by ~20·log10(Q) dB and pinned the
  meter.

Shader normalization (in the `==field==`/`==audio==` pass):
- `tonnetz.glsl` uses the desktop's **`norm_db = (20·log10(x) + 60)/60`**
  (`= (6.0206·log2(x) + 60)/60`), i.e. 0 dBFS→1, −60 dB→0. The cleanup
  coefficients are tuned against THIS scale — don't swap in a different floor
  without retuning them.
- `ball-audio-helix.glsl` uses `AUDIO_FLOOR -16 / AUDIO_CEIL -2` (log2) — a
  quick-and-dirty scale, not the tuned one.

Cleanup coefficients (desktop buffer_a, in `tonnetz.glsl` `==field==`):
- Harmonic subtraction: `−0.30 ×` octave-below fundamental, `−0.15 ×` fifth-below.
- Flatness gate: `−(0.35 + 0.025·row) ×` row-mean.
- Treble boost: `1 → 2×` with row; gamma `1.2 − 0.05·row`.
- Settle: `tauUp = 0.0025 + 0.0012·row`, release ×30.
- Pull `0.03`, bridge `0.45`, drag `tau 1.4`.

Layout (tonnetz polar): `HUB_RADIUS 1.4`, `RING_PITCH 2.0`, `RING_GROWTH 0.06`,
hex size `S 0.5`. Treble at center, bass at rim (the flip is `dataRow = 8 − candRow`).

## 6. Build & deploy

Native `.so`:
1. `ninja -C app/.cxx/RelWithDebInfo/4s520615/arm64-v8a jamesdsp_static`
   (from `~/builds/android/JamesDSP-src`).
2. `cp` the `.a` → `~/homelab/crDroid/vendor/JamesDSP/proprietary/vendor/lib64/soundfx/libjamesdsp_engine_static.a`
   AND sync `jdsp_header.h` → `…/proprietary/include/jdsp/jdsp_header.h`.
3. In `~/homelab/crDroid`: `export SKIP_KERNEL_BUILD=true && source
   build/envsetup.sh && lunch lineage_blazer bp4a user && m libjamesdspaidl`.
4. APatch module zip: `module.prop` + `vendor/lib64/soundfx/libjamesdspaidl.so`
   (the built `.so`). Install via APatch Manager + reboot.

ShaderEditor:
1. `cd ~/builds/android/ShaderEditor && ./gradlew :app:assembleRelease`
   (signed with the TERMUX/`termux_platform` key from `~/.gradle/gradle.properties`).
2. `/home/comrade/bin/blazer-sysapp-update install com.termux.shadereditor
   app/build/outputs/apk/release/app-release.apk`.

`bands.bin`: pre-create on-device before first audio after a flash:
`dd if=/dev/zero of=/data/data/com.termux.jdsp/files/bands.bin bs=432 count=1;
 chown 1041:1005 …; chmod 0644 …` (as root).

## 7. Gotchas that bit us (don't re-discover)

- **Header sync is mandatory.** The engine `.a` and the AIDL wrapper compile
  against two copies of `jdsp_header.h`. If `LUA_BAND_METER_COUNT` differs,
  `EffectDSPMain` is under-allocated → heap overflow → crash on effect open
  ("can't grab session 0"). Sync the header every time it changes.
- **Peak-gain=Q saturated the meter.** See §5.
- **No heavy work / file I/O on the realtime audio thread.** A Lua tick doing
  108 `get_band_energy` + cleanup + 216 `string.format` + an 8 KB write at
  30 Hz stalled the HAL and crash-looped `audioserver` (watchdog SIGABRT).
  Hence the mmap + native write.
- **The app's paste-box overrides the auto-loaded script.** The JDSP app stores
  a Lua script in `shared_prefs/dsp_liveprog.xml` (`lua_script`) and loads it
  over AIDL cmd 10020, which beats `files/scripts/reactive.lua` auto-load.
  `reactive.lua` was removed; only the default 2-field script runs now.
- **`band_shm_setup()` is idempotent and doesn't retry.** If `bands.bin` is
  missing at init, the bridge stays off until the effect re-inits (restart
  music or reboot). Fix-on-next-rebuild: lazy/retry, or write to a dir the HAL
  can create.
- **GLSL ES 1.00 limits** (ShaderEditor compiles as ES 1.00): no `round()`
  (use `floor(x+0.5)`), no `texelFetch` (use `texture2D`), no `log10` (use
  `log2`), loop bounds must be constant (`for(i<var)` → `for(i<9){if(i>=var)break;}`),
  and the `precision` declaration must come BEFORE any function (put it at the
  top of the preamble, not per-section).

## 8. Current state (STAGED, not durable)

- Native: APatch test module `jdsp_chroma_bandmeter` v0.4; `.so` sha `031ed1fe…`.
- ShaderEditor: `/data` overlay install, versionCode 94.
- Source: uncommitted in JamesDSP-src, ShaderEditor, and vendor/JamesDSP.
- Promotion TODO is in `JDSP_AUDIO_BRIDGE_STAGING.md` (prebuilts-apk +
  APPS_MANIFEST + re-signed `.so` into the ROM tree).

## 9. Open / next

- Tune `norm_db` floor against the phone's actual band values (dump a few raw
  floats during playback first).
- `band_shm_setup` retry-robustness (see §7).
- Ferrofluid is a no-go on the phone; the fibonacci helix (ball-audio-helix)
  is the "ball with audio cells" answer.

## 10. 2026-10-21 — current state (supersedes §8)

- Native: NOT staged any more. The 60 Hz meter engine shipped in the
  2026-10-10 OTA; `/vendor/lib64/soundfx/libjamesdspaidl.so` = `e041d142…`,
  verified live on the phone 2026-10-21. APatch test module is disabled.
- Source: committed — engine `ad7813e`, app `5cfc097`, vendor `f32fd9c`.
- Next: audio-only / full build split + validation + publish, see SCOPE.md
  2026-10-21 entry.
