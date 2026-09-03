-- reactive.lua — JDSP audio-reactive bridge for ShaderEditor's live wallpaper.
--
-- Ported from mpv's audio_reactive.lua (derivative-based, fullness-adaptive
-- envelope), adapted to JamesDSP's native tick() API instead of mpv's
-- af-metadata/astats. Auto-loaded at engine init from a fixed path, no
-- Kotlin UI needed — mirrors mpv's own scripts/-folder convenience.
--
-- New: source crossfades from full-spectrum RMS to the bass-band tap as
-- "fullness" rises. Full-spectrum alone gets overwhelmed on loud/compressed
-- tracks (low crest factor flattens RMS near ceiling even during the most
-- rhythmically active parts of a song) — bass/kick retains real transient
-- definition even when everything else is squashed, so leaning on it once
-- things get loud/dense reacts to hits instead of sitting pegged. Same
-- "fullness" tracker that already drove attack/decay blending now also
-- drives this crossfade, so it's one mechanism, not two.

local OUTPUT_PATH = "/data/data/com.termux.jdsp/files/audio_bridge.txt"

-- Separate output, written ONLY from this phone's own local analysis --
-- deliberately never touched by the comrade-driven overwrite of
-- OUTPUT_PATH above (see shader-backdrop's scope.md, 2026-08-23 mutual
-- audio-drive entry). A Termux forwarder script tails THIS file to relay
-- the phone's own level back to comrade; reading OUTPUT_PATH instead
-- would sometimes just echo comrade's own value straight back to itself.
local OWN_LEVEL_PATH = "/data/data/com.termux.jdsp/files/phone_own_level.txt"

-- Recalibrated 2026-08-27: the old fixed DB_FLOOR/DB_CEIL (-24/-4, tuned
-- for mpv's astats.Overall.RMS_level scale) permanently clamped to zero
-- once the source stopped matching that absolute range -- confirmed live
-- via diagnostic logging that jdsp.get_audio_level()/get_band_energy() ARE
-- genuinely live and updating (real, non-zero, moving numbers every tick)
-- but sitting around -93 to -61dB, way below that old -24dB floor. Cause
-- of the absolute level being low wasn't pinned down for certain (device
-- volume was reading suspiciously low at the time, but a live A/B volume
-- test was inconclusive -- Cybersyn hijacks the physical volume keys and
-- an adb-side volume set silently didn't stick either). Rather than chase
-- one more fixed constant that breaks again under different playback
-- conditions, DB_CEIL now auto-tracks the recent loudest reading (fast
-- attack, slow decay -- same shape as the peak_hold envelope below) and
-- DB_FLOOR is just DYNAMIC_RANGE_DB under that, so this self-calibrates to
-- whatever absolute level the source is actually at, regardless of cause.
-- See project_shadereditor_audio_wallpaper memory, 2026-08-27 entry.
local DYNAMIC_RANGE_DB = 35.0
local CEIL_ATTACK = 0.1
local CEIL_DECAY = 0.0008
local db_ceil = -70.0  -- seed guess only; converges within a few seconds either direction

-- Dynamic-track parameters (softer, smoother)
local ATTACK_SOFT = 0.3
local DECAY_SOFT = 0.15

-- Compressed/loud-track parameters (sharp transients, fast drops)
local ATTACK_SHARP = 0.85
local DECAY_SHARP = 0.45

-- Fullness tracking blends attack/decay AND (new) the source crossfade
local FULLNESS_RATE = 0.005

-- Peak-hold release rate. JDSP's tick only exposes RMS-per-block, not a
-- true per-sample peak (that would need a small native addition), so this
-- is a peak-hold envelope built from repeated level reads — a reasonable
-- visualisation proxy, not a literal sample-accurate peak.
local PEAK_DECAY = 0.05

local smoothed = 0.0
local prev_raw = 0.0
local fullness = 0.0
local peak_hold = 0.0

-- Light fixed-rate smoothing for the 4 newly-exposed bands (lowmid/mid/
-- highmid/treble) -- simpler than replicating the fullness-based attack/
-- decay blend above per-band; band-pass output is already a filtered
-- signal so this doesn't need to be as elaborate as the main level.
local BAND_SMOOTH = 0.3
local smoothed_bass = 0.0
local smoothed_lowmid = 0.0
local smoothed_mid = 0.0
local smoothed_highmid = 0.0
local smoothed_treble = 0.0

local function lerp(a, b, t)
  return a + (b - a) * t
end

local function to_db(linear)
  if not linear or linear <= 0.0 then return -120.0 end
  return 20.0 * math.log(linear, 10)
end

local function db_to_level(db, ceil)
  if not db or db ~= db then return 0.0 end
  local floor = ceil - DYNAMIC_RANGE_DB
  local t = (db - floor) / (ceil - floor)
  if t < 0 then return 0.0 end
  if t > 1 then return 1.0 end
  return t
end

function tick()
  local raw_level = jdsp.get_audio_level()
  local raw_bass = jdsp.get_band_energy(1)

  -- Auto-track the ceiling off the full-spectrum level (not per-band --
  -- one shared reference keeps bands comparable to each other and to the
  -- main level, same reasoning as one shared DB_FLOOR/DB_CEIL before).
  local level_db = to_db(raw_level or 0)
  if level_db > db_ceil then
    db_ceil = db_ceil + (level_db - db_ceil) * CEIL_ATTACK
  else
    db_ceil = db_ceil + (level_db - db_ceil) * CEIL_DECAY
  end

  local full_raw = db_to_level(level_db, db_ceil)
  local bass_raw = db_to_level(to_db(raw_bass or 0), db_ceil)

  -- The other 4 real band-pass taps (engine already computes these every
  -- tick, see jdspController.c's g_bandMeterCenterHz -- 250/700/2000/6000Hz)
  -- were never exposed before now, only band 1 (bass) was read, for the
  -- crossfade above. Lightly smoothed and written out below so shaders can
  -- react to a specific part of the spectrum instead of just one blended
  -- loudness value.
  local lowmid_raw  = db_to_level(to_db(jdsp.get_band_energy(2) or 0), db_ceil)
  local mid_raw     = db_to_level(to_db(jdsp.get_band_energy(3) or 0), db_ceil)
  local highmid_raw = db_to_level(to_db(jdsp.get_band_energy(4) or 0), db_ceil)
  local treble_raw  = db_to_level(to_db(jdsp.get_band_energy(5) or 0), db_ceil)
  smoothed_bass    = smoothed_bass    + (bass_raw    - smoothed_bass)    * BAND_SMOOTH
  smoothed_lowmid  = smoothed_lowmid  + (lowmid_raw  - smoothed_lowmid)  * BAND_SMOOTH
  smoothed_mid     = smoothed_mid     + (mid_raw     - smoothed_mid)     * BAND_SMOOTH
  smoothed_highmid = smoothed_highmid + (highmid_raw - smoothed_highmid) * BAND_SMOOTH
  smoothed_treble  = smoothed_treble  + (treble_raw  - smoothed_treble)  * BAND_SMOOTH

  -- Fullness: long-running level, drives mode + source blending
  fullness = fullness + (full_raw - fullness) * FULLNESS_RATE
  local blend = math.max(0.0, math.min(1.0, fullness * 2.5))

  -- Crossfade source: full-spectrum when quiet, bass-only once loud/dense
  local raw = lerp(full_raw, bass_raw, blend)

  -- Dynamic attack/decay based on the same fullness blend
  local ATTACK = lerp(ATTACK_SOFT, ATTACK_SHARP, blend)
  local DECAY = lerp(DECAY_SOFT, DECAY_SHARP, blend)

  -- Derivative: instant direction, zero lag
  local delta = raw - prev_raw
  prev_raw = raw

  if delta > 0.01 then
    smoothed = smoothed + (raw - smoothed) * ATTACK
  else
    smoothed = smoothed + (raw - smoothed) * DECAY
  end

  if raw > peak_hold then
    peak_hold = raw
  else
    peak_hold = peak_hold + (raw - peak_hold) * PEAK_DECAY
  end

  local f, open_err = io.open(OUTPUT_PATH, "w")
  if f then
    f:write(string.format(
      "audio_level=%.4f\naudio_mood=%.4f\naudio_peak=%.4f\n" ..
      "audio_rms=%.4f\naudio_bass=%.4f\naudio_lowmid=%.4f\n" ..
      "audio_mid=%.4f\naudio_highmid=%.4f\naudio_treble=%.4f\n",
      smoothed, fullness, peak_hold,
      full_raw, smoothed_bass, smoothed_lowmid,
      smoothed_mid, smoothed_highmid, smoothed_treble))
    f:close()
  else
    -- Diagnostic-only: io.open() failures here were previously silent,
    -- making the missing audio_bridge.txt bug unfalsifiable from outside.
    jdsp.log("error", "reactive.lua: io.open(" .. OUTPUT_PATH .. ") failed: " .. tostring(open_err))
  end

  local f2 = io.open(OWN_LEVEL_PATH, "w")
  if f2 then
    f2:write(string.format("audio_level=%.4f\n", smoothed))
    f2:close()
  end
end
