#!/usr/bin/env python3
"""Live beat-clock test on the phone (2026-10-10): the desktop tracker
(shader-backdrop/beat_track.py, copied next to this file, UNCHANGED) fed from
JDSP's v2 bands.bin ring at 60 Hz.

    python phone_beat_test.py            # terminal metronome
    python phone_beat_test.py --log FILE # also log t, bpm, conf, phase, count

Phone differences vs the desktop, on purpose:
  * input = raw linear band RMS of the full mix (no drum stem), converted to
    log magnitude like the desktop's norm_db so the log-domain flux matches;
  * no look-ahead (no speaker delay to hide it in), so phase is the causal
    comb -- the tracker's documented fallback.
Every frame is fed exactly once, using the audio clock (frame index / 60),
so a slow read never drops or doubles a frame.
"""
import argparse
import math
import struct
import sys
import time

import numpy as np

from beat_track import BeatTracker, CONF_GATE

PATH = "/data/data/com.termux.jdsp/files/bands.bin"
BANDS = 108
HDR = struct.Struct("<5I3f")
HDR_OFF = BANDS * 4
RING_OFF = HDR_OFF + HDR.size
DB_FLOOR = -90.0  # dB -> 0..1, like the desktop's norm_db


def norm_db(lin):
    db = 20.0 * np.log10(np.maximum(lin, 1e-9))
    return np.clip((db - DB_FLOOR) / -DB_FLOOR, 0.0, 1.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log")
    args = ap.parse_args()
    log = open(args.log, "w") if args.log else None
    if log:
        log.write("t\tbpm\tconf\tphase\tcount\n")

    tr = BeatTracker(n_bands=BANDS)
    last_seq = None
    last_beat = None
    u = tr.uniforms()
    shown = 0.0
    while True:
        with open(PATH, "rb") as f:
            buf = f.read()
        if len(buf) < RING_OFF:
            sys.exit("bands.bin is v1 -- new JDSP engine not loaded")
        magic, _ver, seq, ring, _bands, hz, _fs, _rms = HDR.unpack_from(buf, HDR_OFF)
        if last_seq is None or seq < last_seq or seq - last_seq > ring - 2:
            last_seq = max(0, seq - 1)  # start / engine restart / we fell behind
        for k in range(last_seq, seq):
            off = RING_OFF + (k % ring) * BANDS * 4
            row = np.frombuffer(buf, dtype="<f4", count=BANDS, offset=off)
            t = k / hz
            u = tr.update(norm_db(row.astype(np.float64)), t, t)
            beat = int(math.floor(u["u_beat_count"]))
            if log:
                log.write(f"{t:.4f}\t{u['u_beat_bpm']:.2f}\t{u['u_beat_conf']:.3f}\t"
                          f"{u['u_beat_phase']:.3f}\t{u['u_beat_count']:.3f}\n")
            if last_beat is not None and beat != last_beat:
                shown = time.monotonic()
            last_beat = beat
        last_seq = seq
        flash = time.monotonic() - shown < 0.08
        locked = u["u_beat_conf"] > CONF_GATE
        bar = int(u["u_beat_phase"] * 20)
        line = (f"\r{'##' if flash else '  '} {u['u_beat_bpm']:6.1f} BPM  "
                f"conf {u['u_beat_conf']:.2f} {'LOCK' if locked else '----'}  "
                f"[{'=' * bar}{' ' * (20 - bar)}]  beat {last_beat or 0:5d}")
        sys.stdout.write(line)
        sys.stdout.flush()
        if log:
            log.flush()
        time.sleep(1.0 / 120.0)


if __name__ == "__main__":
    main()
