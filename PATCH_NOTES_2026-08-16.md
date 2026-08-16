# JDSP Patch Notes — 2026-08-16

## Summary

Same-day sequence: shipped the mpv-style Lua property tree (telemetry +
control) via a full OTA, discovered and fixed a real crash-loop bug that
shipped in that same OTA, then discovered a second, still-open bug (Gate
hang) while verifying the audio-reactive shader bridge. Gate is currently
**disabled** on-device as a stopgap.

## 1. Shipped: mpv-style Lua property tree

`JamesDSP-src@89d627b` (private/master) + `libjamesdsp@0f4d8a6` (its own
private remote, branch `fft-x2-batched-convolver`). Read side: gate/
compressor/limiter gain-reduction-dB + engaged flags, autogain current gain,
convolver active/impulse-length, stereo mix, tube drive, crossfeed mode,
bassboost gain, 15 EQ band gains, band meter bumped 1→5 log-spaced taps —
all pushed into the property store every 30Hz tick. Write side: real
per-parameter setters (threshold/ratio/attack/release/hold/knee for
compressor+gate+limiter, tube drive) via a read-modify-write config cache so
one Lua-set parameter never stomps the others. See
`~/.claude/projects/-home-comrade-homelab/memory/project_jdsp_lsp_port_repo.md`
for the full port history.

## 2. FIXED: vendor-tree header ABI mismatch (the crash-loop bug)

**Symptom:** every single JDSP effect construction crash-looped
`android.hardware.audio.service-aidl.aoc` — `EffectDSPMainConstructor+72`,
`SIGSEGV`/`SEGV_ACCERR` (MTE tag-check failure = real heap overflow), on
every attempt, immediately after installing the first `2026-08-16` OTA.

**Root cause:** the crDroid vendor tree
(`vendor/JamesDSP/proprietary/include/jdsp/jdsp_header.h`) keeps its own
**separate copy** of `jdsp_header.h` for the AIDL wrapper's Soong build
(`libjamesdsp_engine_headers` → `JamesDspAidlEffect.cpp`). That copy was
8 days stale (`LUA_BAND_METER_COUNT (1)`) while the real submodule header
used to build the static engine (`libjamesdsp_engine_static.a`) had been
bumped to `(5)` as part of tonight's band-meter work (§1). The AIDL
wrapper's `calloc(1, sizeof(EffectDSPMain))` therefore allocated a smaller
struct than the engine code (compiled against the *real* header) actually
wrote into — `bandMeterState1[]`/`bandMeterState2[]` overflowed the
allocation by 4 floats each, every construction, unconditionally.

**Fix:** synced the vendored header to match the real one (single diff, just
the `LUA_BAND_METER_COUNT` value + its comment). Rebuilt `libjamesdspaidl.so`
(`m libjamesdspaidl`), verified via `nm`/`readelf` and a live health check
(stable PID since boot, zero tombstones, zero JamesDSP/EffectDSPMain errors
in logcat, engine confirmed mapped in the process).

**Lesson for next time a struct crossing the app↔vendor boundary changes:**
`proprietary/include/jdsp/jdsp_header.h` in the vendor tree is a **duplicate**
of the real submodule header, not a symlink — it will not update itself.
Anything that changes `EffectDSPMain`'s layout (band meter count, new struct
fields, etc.) needs this file synced by hand before the vendor engine is
rebuilt, or this exact bug reproduces. Worth turning into a symlink or a
build-time copy step so this class of bug can't recur silently.

**Deployed via:** a second boot-chain-patched OTA
(`crDroidAndroid-16.0-20260816-jdspfix-blazer-v12.11-known-good-bootchain-vbmeta.zip`,
sha256 `70dc03ace5e87fae30f0f010afc255cbf049a6a379d3c7a9f9bde920ee4ab64d`),
built via `target-files-package` directly (not `mka bacon` — see
`OTA_UPDATE_HOWTO.md`'s new "Preferred Flow When You Already Know Boot-Chain
Patching Is Needed" section), pushed to the device via scp
(`mcp__homelab__pixel_push`, verified byte-identical sha256 on-device),
installed by the user via the crDroid updater. All boot-chain AVB fields
verified against the documented known-good values before handoff.

## 3. OPEN, NOT FIXED: Gate hangs the real-time audio thread

**Symptom:** during real playback (not synthetic/idle testing), the native
audio processing thread got stuck inside `lsp::dspu::Gate::process()`
(confirmed via `debuggerd -b` backtrace on the vendor HAL process),
burning continuous CPU (`utime` far exceeding any other thread in the
process) without returning. Playback froze (position stopped advancing).
The app's own health-check binder call (`JamesDspRemoteEngine.checkEngine()`
→ `isPidValid()` → `AudioEffect.getParameter()`, a periodic liveness check
triggered from a broadcast receiver) blocked on that same wedged native
thread for 20+ seconds → real ANR → Android killed and restarted the app
process (confirmed via the ANR trace's Java stack + `am_anr`/`am_proc_died`
logcat lines). No crash/tombstone on the native side — it never actually
faulted, it just never returned.

**Immediate mitigation applied:** bounced `vendor.audio-hal-aidl`
(`stop`/`start`, not a device reboot) to clear the wedged thread. **User has
also disabled the Gate module on-device** as of 20:30 tonight as a further
stopgap.

**Not yet root-caused.** Leading suspect: this session's Gate config
read-modify-write path (`gate_config()`/`gate_apply()` in `jamesdsp.c`,
feeding the Lua property setters via `lsp_gate_get_config`/
`lsp_gate_configure`) — it's the one new Gate-adjacent code path that
shipped in tonight's OTA. Not confirmed; could equally be a pre-existing
LSP `Gate::process()` bug newly exercised by real playback + a settings
change (the hang coincided with the user navigating JDSP's
`LiveprogParamsActivity`/`MainActivity` while music played). **Do not
re-enable Gate without investigating this first** — the current on-device
state (Gate off) is the safe baseline until it's actually fixed.

**Next steps to root-cause:**
1. Check `gate_config()`/`gate_apply()` (`jamesdsp.c`) and
   `lsp_gate_get_config()`/`lsp_gate_configure()` (`lsp_wrap.cpp`) for an
   uninitialized-read-before-first-write in the config cache — if `hold`
   (or any parameter that bounds an internal loop/buffer in
   `Gate::process()`) can read as garbage before the user ever touches a
   gate control, that would explain a pathologically long single call.
2. Reproduce deliberately (enable Gate, play something, watch
   `/proc/<pid>/task/*/stat` for a thread pinned in `R` state with runaway
   `utime`) rather than waiting for it to recur incidentally.
3. If the config-cache theory doesn't hold, look at LSP's own
   `Gate::process()` for a genuine algorithmic infinite-loop condition
   (e.g. NaN/Inf propagation into an envelope follower that never settles).

## 4. Audio-reactive ShaderEditor bridge — deployed earlier, not yet re-verified after tonight's engine rebuild

This was **already fully built and deployed in an earlier session**
(2026-08-15), not new work: `reactive.lua` auto-loads at engine init
(`jdspController.c`, `JamesDSPInit()`) from a fixed path
(`/data/data/com.termux.jdsp/files/scripts/reactive.lua`, confirmed present
on-device), writes `audio_level`/`audio_mood`/`audio_peak` to
`/data/data/com.termux.jdsp/files/audio_bridge.txt` at 30Hz, crossfading
from full-spectrum RMS (quiet/low-fullness) to the bass-band tap
(loud/dense/high-fullness) so it reacts to hits instead of getting
overwhelmed on compressed material. ShaderEditor's real repo already has
the commit that reads this file instead of mpv's old
`audio_reactive.lua` (`bcc2c94`, confirmed on current `HEAD`).

**Status as of tonight: script-load code confirmed present in the deployed
engine (`grep -a -c "reactive.lua" /vendor/lib64/soundfx/libjamesdspaidl.so`
→ `1`, byte-identical hash to a from-scratch rebuild). A long false alarm
earlier tonight — on-device `strings` gave a false NEGATIVE for this same
string, wasting real time chasing a phantom "missing feature" theory. Use
`grep -a -c` on-device, never `strings`, for presence checks — see
`HEALTH_CHECK.md`.

**Real blocker found and partially fixed:** `reactive.lua` and its
containing `scripts/` directory were mode `600`/`700`, owned
`system:system` (UID 1000). The vendor audio HAL process that actually
calls `fopen()` on it runs as UID `audioserver` — a **different** UID,
despite `com.termux.jdsp` otherwise being part of the "zero sandbox, same
UID 1000" family used elsewhere in this project. `audioserver` could not
even traverse into the directory. Fixed live on-device (not yet in any
built artifact — this is a `/data` permission fix, not a code change):
`chmod 755 scripts/`, `chmod 644 reactive.lua`, `chmod o+w files/` (the
bridge file's output directory needs the same fix on the write side).

**Still not end-to-end confirmed as of this writing.** Two HAL bounces
after the permission fix did not produce a populated `audio_bridge.txt`
during active playback — likely because the existing audio session
survived both HAL restarts without a genuinely fresh
`EffectDSPMainConstructor` call for that specific stream. Next step: force
a real app-level stop/restart of playback (not just pause) and re-check
`audio_bridge.txt`'s mtime per `HEALTH_CHECK.md` §4. mpv's old
`audio_reactive.lua` (`/data/data/com.termux.mpv/files/scripts/
audio_reactive.lua`) is still present on-device, unarchived — harmless now
that ShaderEditor no longer reads it, archive once the JDSP-side path is
actually confirmed working end-to-end, per the original migration plan.

**Follow-up needed:** the permission fix above was applied directly
on-device via `chmod`, not captured anywhere durable. If this OTA/build is
ever regenerated from scratch (fresh `/data`, factory reset, etc.), the
permission gap will silently reappear. Worth fixing at the source — either
have the JDSP app itself `chmod` these paths after writing them (app runs
as the owning UID, so it can), or document this as a required manual step
post-flash.

**Second real bug found and fixed in the same investigation: stale
ShaderEditor install.** The on-device ShaderEditor (`com.termux.shadereditor`,
versionCode 93 both before and after) was NOT the build containing the
JDSP-bridge commit (`bcc2c94`), despite matching versionCode and a
plausible-looking `lastUpdateTime`. Confirmed via `grep -a -c
"audio_bridge.txt"` on the installed APK's `classes.dex` → `0`. The correct
build was sitting unused in `~/builds/android/ShaderEditor/app/build/
outputs/apk/release/app-release.apk` (built 2026-08-15 21:53, contains the
string). Installed via `blazer-sysapp-update install com.termux.shadereditor
<path>`, sha256 `c8622dabcc45146eb4aa2c4f1145aaefb1b8ebc1f8cac8ffb8246ff6d7dfc33b`
— this matches `APPS_MANIFEST.md`'s already-recorded `rom_sha256` for
ShaderEditor, so the ROM-tree promotion was already correct; only the live
`/data` test-install was stale. **Lesson: versionCode + timestamp proximity
is not proof of content — check the actual installed bytes when a fix
isn't visibly working**, exactly per `HEALTH_CHECK.md`.

## 5. OPEN, NOT RESOLVED: `audio_bridge.txt` never gets created

Despite every precondition above being independently confirmed true —
engine binary genuinely contains the auto-load code (`grep -a -c` = 1 on
the deployed `.so`, hash-matches a from-scratch rebuild), `scripts/`/
`reactive.lua`/`files/` all have correct permissions for the `audioserver`
UID, ShaderEditor now has the correct build installed, a fresh HAL process
is running, and real music was confirmed actively playing through it —
`/data/data/com.termux.jdsp/files/audio_bridge.txt` still never appears.

One data point, not yet confirmed as causal: a repeatedly-firing AVC denial
from the `JamesDSP v4.01` real-time thread itself —

```
avc: denied { search } for comm="JamesDSP v4.01" name="data" dev="dm-65"
ino=325 scontext=u:r:hal_audio_default:s0
tcontext=u:object_r:system_data_file:s0:c512,c768 tclass=dir permissive=1
```

— an MCS/category-based isolation denial (`hal_audio_default:s0` lacks the
`c512,c768` category pair assigned to `com.termux.jdsp`'s data). Marked
`permissive=1`, and `getenforce` confirms global Permissive, so per
SELinux's own semantics this should NOT be blocking the underlying
syscall — but it's the only concrete anomaly found before time ran out on
this session, and is unverified as cause vs. red herring (could be an
unrelated internal DSP data-blob lookup, not the `reactive.lua` fopen at
all).

**Next session, before anything else on this:** get a definitive answer on
whether this AVC is actually related, e.g. by temporarily flipping
`setenforce 0`→confirm-already-0, or by instrumenting/logging directly
around the `fopen()` call in `jdspController.c` to prove whether it's even
being reached and what `errno` it gets, rather than continuing to infer
from the outside. Don't re-verify the items already confirmed above (engine
code, permissions, ShaderEditor build) — start from the assumption those
are correct and dig into why the write still isn't happening despite them.

**RESOLVED same night, later session (2026-08-16 ~21:47).** The AVC denial
was a red herring after all — its firing rate (every 5-20s) never matched
the tick()-driven `io.open()` call rate (~80ms), so it isn't the same
codepath. Root cause was one directory level higher than anything
previously checked: **`/data/data/com.termux.jdsp/` itself (the app's data
root, one level above `files/`) was mode `0700`, owner-only
(`system:system`, UID 1000)**. The audio HAL process is UID `audioserver`
(1041) — it has `system` (1000) as a *supplementary* group, which would
satisfy a group-permission check, but `0700`'s group bits are all zero, so
group membership didn't matter; only UID 1000 could traverse in. Since
path resolution requires search/execute on *every* intermediate directory,
this blocked access to everything underneath — `scripts/reactive.lua`
(read, for the fixed-path auto-load) and `audio_bridge.txt` (write) alike —
regardless of how permissive `files/` and `scripts/` themselves were (the
2026-08-15 permission fix was real but addressed the wrong layer).

Confirmed with a hard errno, not inference: added a two-argument-corrected
`jdsp.log("error", ...)` call to `reactive.lua`'s `io.open()` failure
branch (the original single-arg call silently produced empty log lines —
`jdsp_log()` in `lua_bridge.c` takes `(level, message)`, not `(message)`)
and read it back via logcat: `Permission denied`. Fixed live with
`chmod g+x /data/data/com.termux.jdsp` (group-execute only, not opened to
"other" — audioserver's existing supplementary group membership makes this
the minimal grant). `audio_bridge.txt` now updates continuously with real
per-tick values during playback, confirmed over multiple reads.

Earlier `su 1041 -c '...'` tests during that same investigation had
falsely suggested permissions were fine — invalid test: `su`-spawned
shells commonly retain `CAP_DAC_OVERRIDE` from root regardless of the
requested UID, silently bypassing exactly this kind of check. The real
process (confirmed via `/proc/<pid>/status`) has `CapEff`/`CapBnd` with
neither `CAP_DAC_OVERRIDE` nor `CAP_DAC_READ_SEARCH` set — it gets no such
bypass. **Lesson: `su <uid> -c` is not a valid stand-in for "what can this
UID actually do" when DAC-bypass capabilities are in play — verify via the
real process's own `/proc/<pid>/status` capability bits, or better, get a
live errno from the real process itself (as done here).**

Not yet made durable: the `chmod g+x` is a live on-device fix, same
category as the earlier `files/`/`scripts/` chmods — confirmed to survive
a plain `adb install -r` (this fix was applied *after* that reinstall and
the gap was still present, i.e. reinstall-without-wipe doesn't touch it),
but would need reapplying after a full data wipe. Worth having the app
`chmod` its own data root at startup (it runs as the owning UID 1000 and
can do this itself) so this class of bug can't recur silently, matching
the same durability gap already noted for the `files/`/`scripts/` fix
above.

**Also shipped this session: a Lua script paste-box UI card.** Added
`EditTextPreference` "Lua bridge script (paste)" to the Liveprog
preferences screen (`dsp_liveprog_preferences.xml`), wired through
`JamesDspBaseEngine.setLuaScript()` → `JamesDspRemoteEngine.
setLuaScriptInternal()` → `effect.setParameterCharBuffer(12001, 10020,
script)` — reusing the existing generic chunked-string-send mechanism
(same one the EEL Liveprog editor uses, just committing to native cmd
`10020`/"Load Lua script from accumulated string" instead of `10010`/EEL).
Needed **no vendor rebuild** — cmd 10020 was already present and working
in the currently-deployed `.so`; this was a pure app-side (`RootFdroid`
flavor) change, built and installed live. Beyond being a real feature
(paste any Lua script without a file or adb push), this is what let the
`audio_bridge.txt` root-cause get nailed down quickly: it loads scripts
over the AIDL binder (already in memory, no filesystem read), so it kept
working throughout the `/data/data/com.termux.jdsp/` traversal bug and
gave a clean way to isolate the failure to `io.open()` specifically.
`JamesDspLocalEngine.setLuaScriptInternal()` is a no-op (that JNI path has
no Lua bridge at all).

## 6. Gate hang (§3) — root cause now well-evidenced, fix NOT YET applied

Same session as the `audio_bridge.txt` fix above. Reading `w_gate_lsp()`
in `jdspController.c` (the real-time audio-chain wrapper that calls
`lsp_gate_process()`) turned up a clear, systemic gap: unlike `w_conv`,
`w_ddc`, and `w_liveprog`, which all wrap their processing call in
`jdsp_lock()`/`jdsp_unlock()` (`jdsp->m_in_processing`), `w_gate_lsp()` —
and `w_mb_dyn()` (compressor) — take **no lock at all**. Meanwhile, `cmd
10100` (Gate) in `EffectDSPMainCommand` (fired from whatever thread
handles the AIDL `setParameter` call — a UI slider change, i.e. a binder
thread) calls `lsp_gate_configure()` directly on the same `gate_handle`,
also with no lock. Same story for the Lua-bridge `gate_apply()`/
`gate_config()` path (`JDSP_DYN_PROP` macro in `jamesdsp.c`) — it also
touches `lsp_gate_configure()`/`lsp_gate_get_config()` unsynchronized
(only `g_lua_mutex` protects the Lua VM itself, not this C-side call).

So the real-time thread can be inside `gh->gate.process()` (reading/
mutating `lsp::dspu::Gate`'s internal envelope/hold-counter state) at the
exact moment a binder thread calls `gh->gate.set_hold()`/`update_settings()`
etc. via a UI change — a genuine, unsynchronized concurrent read-modify-
write race on a non-thread-safe LSP object. That's consistent with every
observed symptom: no crash/tombstone (a livelock/NaN-propagation hang, not
a fault), and the original hang coinciding with the user navigating
JDSP's settings UI while music played (exactly when a binder-thread config
call would fire). Compressor has the identical gap (`w_mb_dyn`) but hasn't
been observed to hang yet — plausibly just not yet exercised the same way.

**Not fixed yet** — this needs a vendor `.so` rebuild + OTA (unlike the
audio_bridge.txt fix, this touches compiled code, not app-side/data-side
state). The fix is straightforward: wrap `w_gate_lsp()` (and `w_mb_dyn()`)
in `jdsp_lock()`/`jdsp_unlock()` like the other three chain wrappers
already do, and wrap the `cmd 10100`/`10101` config-apply calls and the
`JDSP_DYN_PROP` gate/comp accessors in the same lock.

**Gate was found re-enabled on-device (2026-08-16 ~21:35), contradicting
the "leave it off until fixed" guidance above** — flagged to the user;
recommend disabling it again until the locking fix actually ships.

## Files touched tonight

- `app/src/main/cpp/lua_bridge.c` / `.h`
- `app/src/main/cpp/chain_modules.c` / `.h`
- `app/src/main/cpp/libjamesdsp/.../jamesdsp/jamesdsp.c`
- `app/src/main/cpp/libjamesdsp/.../jdsp/jdspController.c`
- `app/src/main/cpp/libjamesdsp/.../jdsp/jdsp_header.h`
- crDroid vendor tree: `vendor/JamesDSP/proprietary/include/jdsp/jdsp_header.h`
  (the ABI-mismatch fix, §2)
- `device/google/blazer-secur/OTA_UPDATE_HOWTO.md` — new "Preferred Flow"
  and "Avoiding `out/.lock` contention" sections
