# JDSP Post-OTA Health Check

**Read this before ever saying "JDSP is healthy" after an OTA or a live
patch.** A shallow check (process alive, no crash log) is not a health
check — it only proves the process didn't die, not that the feature you
actually shipped is running. This distinction cost a multi-hour debugging
session on 2026-08-16: the crash-fix OTA was declared "healthy" via a
shallow check while a real Gate-hang bug and a dead-on-arrival Lua script
bridge both shipped in the same build, undetected.

Run every check below. Don't stop at the first "looks fine."

## 1. Process-level (the shallow check — necessary, not sufficient)

```bash
ps -A | grep -i audio                    # android.hardware.audio.service-aidl.aoc alive?
ls -la /data/tombstones/ | tail -8       # any NEW tombstone since this OTA/patch?
```

## 2. Build identity (are you even looking at the build you think you are?)

```bash
sha256sum /vendor/lib64/soundfx/libjamesdspaidl.so
```

Compare against the sha256 you recorded when you built/verified the OTA.
If it doesn't match, you're checking a stale/different build — stop and
figure out why before checking anything else.

**Do NOT use `strings` on-device to check for embedded content.** Android's
toybox `strings` gave a false negative on a real, present string during
this exact session — wasted over an hour chasing a phantom "missing
feature" bug. Use `grep -a -c "<string>" <file>` instead; it's reliable.

## 3. Real-time thread health (the Gate-hang class of bug)

```bash
for t in $(ls /proc/<audio-hal-pid>/task/); do
  cat /proc/<audio-hal-pid>/task/$t/stat 2>/dev/null
done
```

Look for any thread in state `R` (running) with `utime` wildly higher than
every other thread in the same process — that's a thread spinning instead
of blocking/sleeping between buffers as it should. If found, get a real
backtrace before assuming anything:

```bash
debuggerd -b <audio-hal-pid> > /data/local/tmp/bt.txt
grep -A25 "<thread-name>" /data/local/tmp/bt.txt
```

## 4. Feature-specific liveness — the part that's easy to skip

Don't just confirm a feature's *code* is present. Confirm it's *doing
something*, during real playback, right now.

**reactive.lua / audio bridge:**
```bash
stat /data/data/com.termux.jdsp                                 # THE ONE THAT GOT MISSED 2026-08-16: this dir itself needs g+x too, not just its contents (see below)
ls -laZ /data/data/com.termux.jdsp/files/scripts/reactive.lua   # mode must allow audioserver to read
ls -ldZ /data/data/com.termux.jdsp/files/scripts                # dir needs o+x to be traversable
ls -ldZ /data/data/com.termux.jdsp/files                        # dir needs o+w for the bridge file to be created
cat /data/data/com.termux.jdsp/files/audio_bridge.txt           # play something first — check this EXISTS and its mtime is recent, not just present
stat -c '%y' /data/data/com.termux.jdsp/files/audio_bridge.txt
```

The vendor audio HAL process runs as UID `audioserver`, NOT the same UID as
`com.termux.jdsp` (UID 1000/`android.uid.system`) despite being part of the
same "zero sandbox" family elsewhere in this project. Any file the native
engine needs to read/write under `/data/data/com.termux.jdsp/` needs
explicit `o+r`/`o+w`/`o+x` — the usual same-UID assumption does NOT hold
for this specific cross-boundary (app data dir ↔ vendor HAL process).

**Path resolution needs search permission on *every* intermediate
directory, not just the leaf.** 2026-08-16: `files/` and `scripts/` were
both already correctly permissioned, but `/data/data/com.termux.jdsp/`
itself (the app's data root, one level up) was still the Android default
`0700` owner-only — silently blocking traversal into everything below it,
for both read (`reactive.lua`) and write (`audio_bridge.txt`) alike. This
one is easy to miss because it's not "the file's directory" or "the
adjacent script folder" — check the app's data root explicitly:
`stat /data/data/com.termux.jdsp` and confirm `audioserver`'s group
(`system`, via supplementary group membership — see `Groups:` in
`/proc/<pid>/status`) has at least execute.

**`su <uid> -c '...'` is not proof that UID can do it for real.**
`su`-spawned shells commonly retain `CAP_DAC_OVERRIDE` from root
regardless of the requested UID, which silently bypasses exactly this
class of permission check — a `su audioserver-uid -c 'cat/echo ...'` test
can report success while the real, unprivileged process gets `EACCES`.
Verify the real process's actual capabilities instead:
```bash
cat /proc/<hal-pid>/status | grep -E '^(Uid|Gid|Groups|CapEff|CapBnd)'
```
If `CapEff`/`CapBnd` don't include `CAP_DAC_OVERRIDE` (bit 1) or
`CAP_DAC_READ_SEARCH` (bit 2), DAC is fully enforced for that process and
a `su`-based test is not representative. Better: get a real errno from the
process itself — e.g. temporarily log `io.open()`'s second return value
in Lua (`local f, err = io.open(path, "w")`), or instrument the native
`fopen()` call directly.

**Lua property tree / telemetry (from the same-day 2026-08-16 work):**
Confirm the 30Hz tick is actually populating properties during real
playback, not just that `lua_bridge_register_property()` got called once
at init. If there's no live consumer script running yet, this needs a
purpose-built probe rather than an inferred "should be fine."

## 5. App-level ANR check

```bash
adb logcat -d -b all | grep -i "ANR in com.termux.jdsp\|am_proc_died.*jdsp"
```

An ANR with no native crash is a real symptom (usually the app's own
health-check binder call hanging on a wedged native thread — see §3). It
will not show up in §1's tombstone check.

## Why this file exists

Written 2026-08-16 after being told directly: a health check that only
proves "the phone is on" is not useful when the person relying on it can't
be expected to re-derive from scratch, next session, whether "healthy" was
ever actually rigorous. Follow this checklist in full, every time, rather
than reconstructing an ad-hoc version under time pressure.
