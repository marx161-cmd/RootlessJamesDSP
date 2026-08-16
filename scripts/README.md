# Lua bridge scripts

`reactive.lua` — audio-reactive live wallpaper driver, ported from
`com.termux.mpv`'s `scripts/audio_reactive.lua` (the mpv script is now
archived on-device, this replaces it). Runs at 30Hz via the engine's Lua
bridge (`lua_bridge.c`, `jdsp.get_audio_level()`/`get_band_energy(n)`).

Crossfades between full-spectrum RMS (quiet passages) and bass-band-only
energy (loud/dense passages, so the envelope reacts to beat hits instead
of getting pegged/overwhelmed by compressed material), driven by the
existing `fullness` tracker.

Deployed by copying to a fixed path the engine auto-loads at init
(`jdspController.c`'s `JamesDSPInit()`, added 2026-08-15):

    adb push scripts/reactive.lua /data/data/com.termux.jdsp/files/scripts/reactive.lua

ShaderEditor (the wallpaper app, separate private fork at
`marx161-cmd/ShaderEditor`) reads the resulting audio-level/band-energy
values via the shared bridge file — see that repo's
"Switch audio-reactive uniform source from mpv to JDSP's bridge file"
commit.
