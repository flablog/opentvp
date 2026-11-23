#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""Writes the source sounds of the sound corpus (corpus/george/make_tvpp_son.grg).

  python corpus/make_sound_sources.py OUT_FOLDER

Two signals, 4 seconds each, easy to measure in TVPaint's output:
  tick  a 50 ms 1 kHz beep at the start of every second, silence otherwise:
        sync and offsets (a beep must land on frames 0, 24, 48, 72 at 24 fps).
        The beep of second n has n + 1 short pulses, so a shifted second is identified.
  tone  a steady 440 Hz sine at half amplitude: volume and fades.

Files (no digits in the names: TVPaint may read them as frame numbers):
  tick_ref.wav, tone_ref.wav   48 kHz stereo 16-bit (the reference format)
  tick_mono.wav                44.1 kHz mono 16-bit
  tick_hd.wav                  48 kHz stereo 24-bit
In the stereo files the right channel is the left one at half amplitude, to see whether
channels are kept or mixed down.
"""
import math, os, struct, sys, wave

SECONDS = 4

def tick(t):
    s, f = int(t), t - int(t)
    if f >= 0.050:
        return 0.0
    pulses = s + 1                      # n + 1 pulses inside the 50 ms beep
    slot = int(f / 0.050 * (2 * pulses))
    if slot % 2:
        return 0.0
    return 0.8 * math.sin(2 * math.pi * 1000 * t)

def tone(t):
    return 0.5 * math.sin(2 * math.pi * 440 * t)

def write(path, signal, rate, channels, width):
    peak = (1 << (8 * width - 1)) - 1
    frames = bytearray()
    for i in range(rate * SECONDS):
        v = signal(i / rate)
        for c in range(channels):
            x = int(round(v * (1.0 if c == 0 else 0.5) * peak))
            frames += x.to_bytes(width, "little", signed=True)
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(width)
        w.setframerate(rate)
        w.writeframes(bytes(frames))
    print(os.path.basename(path))

def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    # no digits in the names: TVPaint may read them as frame numbers
    write(os.path.join(out, "tick_ref.wav"), tick, 48000, 2, 2)
    write(os.path.join(out, "tone_ref.wav"), tone, 48000, 2, 2)
    write(os.path.join(out, "tick_mono.wav"), tick, 44100, 1, 2)
    write(os.path.join(out, "tick_hd.wav"), tick, 48000, 2, 3)

if __name__ == "__main__":
    main()
