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

-- Same calibration mpv's script landed on after live tuning against a real
-- loud rock track (-5.7 to -22dB measured RMS range). Bass-band energy
-- sits at a different absolute level than full-spectrum RMS in principle —
-- not yet separately calibrated, watch live and split into
-- DB_FLOOR_BASS/DB_CEIL_BASS if the loud/bass-driven side feels off.
local DB_FLOOR = -24.0
local DB_CEIL = -4.0

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

local function lerp(a, b, t)
  return a + (b - a) * t
end

local function to_db(linear)
  if not linear or linear <= 0.0 then return -120.0 end
  return 20.0 * math.log(linear, 10)
end

local function db_to_level(db)
  if not db or db ~= db then return 0.0 end
  local t = (db - DB_FLOOR) / (DB_CEIL - DB_FLOOR)
  if t < 0 then return 0.0 end
  if t > 1 then return 1.0 end
  return t
end

function tick()
  local full_raw = db_to_level(to_db(jdsp.get_audio_level() or 0))
  local bass_raw = db_to_level(to_db(jdsp.get_band_energy(1) or 0))

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
      "audio_level=%.4f\naudio_mood=%.4f\naudio_peak=%.4f\n",
      smoothed, fullness, peak_hold))
    f:close()
  else
    -- Diagnostic-only: io.open() failures here were previously silent,
    -- making the missing audio_bridge.txt bug unfalsifiable from outside.
    jdsp.log("error", "reactive.lua: io.open(" .. OUTPUT_PATH .. ") failed: " .. tostring(open_err))
  end
end
