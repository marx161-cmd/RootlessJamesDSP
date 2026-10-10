#!/usr/bin/env python3
"""Check the v2 bands.bin layout + publish rate (2026-10-10 continuous meter).

Run on the phone (Termux python) while music plays:
    python bands_bin_check.py [/data/data/com.termux.jdsp/files/bands.bin]
Prints header fields, the measured frame rate over 5 s, and how smooth the
low bands are frame to frame (the old 12 Hz single-block snapshots jumped
around; continuous frames should change gradually).
"""
import struct
import sys
import time

PATH = sys.argv[1] if len(sys.argv) > 1 else "/data/data/com.termux.jdsp/files/bands.bin"
BANDS = 108
HDR_OFF = BANDS * 4
HDR = struct.Struct("<5I3f")  # magic version seq ringLen bands frameHz sampleRate rms
RING_OFF = HDR_OFF + HDR.size


def read():
    with open(PATH, "rb") as f:
        return f.read()


def header(buf):
    return HDR.unpack_from(buf, HDR_OFF)


buf = read()
print("size", len(buf))
if len(buf) < RING_OFF:
    sys.exit("v1 file (no header) -- old engine still loaded")
magic, version, seq0, ring, bands, hz, fs, rms = header(buf)
print(f"magic {magic:#x} version {version} seq {seq0} ring {ring} bands {bands} "
      f"frameHz {hz} fs {fs} rms {rms:.5f}")
t0 = time.monotonic()
time.sleep(5.0)
buf = read()
seq1 = header(buf)[2]
dt = time.monotonic() - t0
print(f"frames in {dt:.2f} s: {seq1 - seq0} -> {(seq1 - seq0) / dt:.2f} Hz")

# smoothness: last 16 ring frames of band 24..35 (A2..G#3 octave)
frames = []
for k in range(max(0, seq1 - 16), seq1):
    off = RING_OFF + (k % ring) * BANDS * 4
    frames.append(struct.unpack_from(f"<{BANDS}f", buf, off))
row = [sum(fr[24:36]) / 12 for fr in frames]
print("A2 octave mean, last 16 frames:", " ".join(f"{v:.4f}" for v in row))
latest = struct.unpack_from(f"<{BANDS}f", buf, 0)
ringlast = frames[-1] if frames else None
print("latest == newest ring frame:", ringlast is not None and
      all(abs(a - b) < 1e-9 for a, b in zip(latest, ringlast)))
