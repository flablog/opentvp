#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""opentvp_sound — mixes the sound tracks of a TVPaint clip (.tvpp) into one WAV, without TVPaint.

Usage:
    python opentvp_sound.py shot.tvpp out.wav [--clip N] [--frames A-B] [--sound-map OLD=NEW ...]

Also used by opentvp_render.py --sound. Python only for now (the C tools do not mix sound yet).

The .tvpp holds no audio: each track gives the path of a sound file (docs/FORMAT.md section 8).
The mix follows TVPaint's movie export, measured on corpus/data/13_son:
  * the clip's unmuted tracks, summed; project tracks are left out, as in TVPaint's clip export;
  * each track × its volume (linear) × its fades (linear ramps, in seconds of the sound file);
  * the sound starts at offset seconds on the clip timeline (offset not confirmed yet);
  * output: 48 kHz, stereo, 24-bit, exactly the duration of the frames (f1 - f0 + 1) / fps.
Sound files: WAV only (PCM 8, 16, 24, 32 bits or float; mono is played on both channels,
other rates are resampled linearly). Other formats are reported and skipped.
"""
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import opentvp_info as ti                                     # noqa: E402

OUT_RATE = 48000
PEAK = 8388607            # 24-bit


def read_wav(path):
    """Returns (rate, float64 array frames × 2 in [-1, 1]). Raises ValueError if not a WAV we read."""
    with open(path, 'rb') as f:
        d = f.read()
    if len(d) < 12 or d[:4] != b'RIFF' or d[8:12] != b'WAVE':
        raise ValueError('not a WAV file')
    fmt = data = None
    i = 12
    while i + 8 <= len(d):
        tag, size = d[i:i + 4], struct.unpack('<I', d[i + 4:i + 8])[0]
        body = d[i + 8:i + 8 + size]
        if tag == b'fmt ':
            fmt = body
        elif tag == b'data':
            data = body
        i += 8 + size + (size & 1)
    if fmt is None or data is None or len(fmt) < 16:
        raise ValueError('incomplete WAV file')
    kind, channels, rate = struct.unpack('<HHI', fmt[:8])
    bits = struct.unpack('<H', fmt[14:16])[0]
    if kind == 0xFFFE and len(fmt) >= 26:          # WAVE_FORMAT_EXTENSIBLE: sub-format
        kind = struct.unpack('<H', fmt[24:26])[0]
    width = bits // 8
    if channels < 1 or rate < 1 or width < 1:
        raise ValueError('invalid WAV header')
    n = len(data) // (width * channels)
    raw = np.frombuffer(data[:n * width * channels], np.uint8)
    if kind == 1 and bits == 8:
        x = (raw.astype(np.float64) - 128) / 128
    elif kind == 1 and bits == 16:
        x = raw.view('<i2').astype(np.float64) / 32768
    elif kind == 1 and bits == 24:
        b = raw.reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        x = (np.where(v >= 1 << 23, v - (1 << 24), v)).astype(np.float64) / 8388608
    elif kind == 1 and bits == 32:
        x = raw.view('<i4').astype(np.float64) / 2147483648
    elif kind == 3 and bits == 32:
        x = raw.view('<f4').astype(np.float64)
    elif kind == 3 and bits == 64:
        x = raw.view('<f8').astype(np.float64)
    else:
        raise ValueError(f'unsupported WAV encoding (format {kind}, {bits} bits)')
    x = x.reshape(n, channels)
    if channels == 1:
        x = np.repeat(x, 2, axis=1)
    return rate, x[:, :2]


def map_path(path, mapping):
    """Applies the first OLD=NEW prefix that matches (slashes normalised, case-insensitive)."""
    p = path.replace('\\', '/')
    for old, new in mapping:
        o = old.replace('\\', '/')
        if p.lower().startswith(o.lower()):
            return new.replace('\\', '/') + p[len(o):]
    return p


def envelope(s, fade_in, fade_out):
    """Fade gain at file time s (seconds): linear ramps, 1 outside them."""
    g = np.ones_like(s)
    a, b = fade_in
    if b > a:
        g = np.where(s < a, 0.0, np.where(s < b, (s - a) / (b - a), g))
    a, b = fade_out
    if b > a:
        g = g * np.where(s <= a, 1.0, np.where(s < b, (b - s) / (b - a), 0.0))
    return g


def mix_clip(clip, fps, f0, f1, mapping=(), log=print):
    """Mix of the clip's tracks over frames f0..f1 (from 0): float64 array samples × 2."""
    n = int(round((f1 - f0 + 1) * OUT_RATE / fps))
    t = f0 / fps + np.arange(n, dtype=np.float64) / OUT_RATE
    out = np.zeros((n, 2))
    snd = clip['sound']
    if snd['mute']:
        log("  sound: the clip's tracks are all muted")
        return out
    for tr in snd['tracks']:
        if tr['mute']:
            continue
        path = map_path(tr['file'], mapping)
        if not os.path.isfile(path):
            log(f"  ⚠ sound track {tr['index']}: file not found, skipped: {path}")
            continue
        try:
            rate, x = read_wav(path)
        except (ValueError, OSError) as e:
            log(f"  ⚠ sound track {tr['index']}: {e}, skipped: {path}")
            continue
        s = t - tr['offset']                       # time in the sound file
        pos = s * rate
        j = np.floor(pos)
        a = (pos - j)[:, None]
        j = j.astype(np.int64)
        pad = np.vstack([np.zeros((1, 2)), x, np.zeros((1, 2))])   # silence before and after
        lo = pad[np.clip(j + 1, 0, len(pad) - 1)]
        hi = pad[np.clip(j + 2, 0, len(pad) - 1)]
        v = lo * (1 - a) + hi * a
        gain = tr['volume'] * snd['volume'] * envelope(s, tr['fade_in'], tr['fade_out'])
        out += v * gain[:, None]
    return out


def write_wav(path, x):
    """48 kHz stereo 24-bit PCM; values beyond [-1, 1] are clipped."""
    q = np.floor(np.clip(x, -1.0, 1.0) * PEAK + 0.5).astype(np.int32).reshape(-1)
    b = np.empty((len(q), 3), np.uint8)
    b[:, 0], b[:, 1], b[:, 2] = q & 0xFF, (q >> 8) & 0xFF, (q >> 16) & 0xFF
    data = b.tobytes()
    hdr = struct.pack('<4sI4s4sIHHIIHH4sI', b'RIFF', 36 + len(data), b'WAVE', b'fmt ', 16, 1, 2,
                      OUT_RATE, OUT_RATE * 6, 6, 24, b'data', len(data))
    with open(path, 'wb') as f:
        f.write(hdr)
        f.write(data)


def export_sound(info, clip, f0, f1, path, mapping=(), log=print):
    """Writes the clip's mix to path. Returns False (and writes nothing) if the clip has no track."""
    if info['sound']['tracks']:
        log(f"  sound: {len(info['sound']['tracks'])} project track(s) left out (TVPaint does not "
            f"export them with a clip)")
    if not clip['sound']['tracks']:
        log("  sound: the clip has no sound track, no WAV written")
        return False
    write_wav(path, mix_clip(clip, info['fps'], f0, f1, mapping, log))
    log(f"  sound: {os.path.basename(path)} ({(f1 - f0 + 1) / info['fps']:.3f} s, "
        f"{sum(1 for t in clip['sound']['tracks'] if not t['mute'])} track(s) mixed)")
    return True


def parse_map(items):
    out = []
    for it in items or ():
        if '=' not in it:
            raise SystemExit(f"--sound-map expects OLD=NEW, got: {it}")
        out.append(tuple(it.split('=', 1)))
    return out


def main(argv):
    import argparse
    ti.utf8_console()
    ap = argparse.ArgumentParser(description='Mixes the sound tracks of a TVPaint clip into a WAV.')
    ap.add_argument('tvpp')
    ap.add_argument('wav')
    ap.add_argument('--clip', type=int, default=1)
    ap.add_argument('--frames', help='A-B, from 0 (default: in/out marks, otherwise the whole clip)')
    ap.add_argument('--sound-map', action='append', metavar='OLD=NEW',
                    help='replace the OLD path prefix of the sound files by NEW (repeatable)')
    a = ap.parse_args(argv)
    info = ti.read_tvpp(a.tvpp)
    if not 1 <= a.clip <= len(info['clips']):
        raise SystemExit(f"clip {a.clip} does not exist (1 to {len(info['clips'])})")
    c = info['clips'][a.clip - 1]
    f0, f1 = clip_range(c, a.frames)
    print(f"{a.tvpp} — clip \"{c['name']}\" — frames {f0} to {f1}")
    return 0 if export_sound(info, c, f0, f1, a.wav, parse_map(a.sound_map)) else 1


def clip_range(c, frames):
    """--frames A-B, otherwise the marks, otherwise the whole clip (frames from 0)."""
    total = max(c['frames'], 1)
    if frames:
        f0, f1 = (int(x) for x in (frames.split('-') + frames.split('-'))[:2])
    elif c['mark_in'] is not None or c['mark_out'] is not None:
        f0 = c['mark_in'] or 0
        f1 = c['mark_out'] if c['mark_out'] is not None else total - 1
    else:
        f0, f1 = 0, total - 1
    return f0, f1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
