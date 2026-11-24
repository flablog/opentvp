# SPDX-License-Identifier: CC0-1.0
"""Sound export (render --sound) on corpus/data/13_son.

1. C and Python write the same WAV, byte for byte, and print the same sound messages.
2. With ffmpeg available: the WAV matches TVPaint's movie export (<file>.mp4) within the AAC loss.
3. With ffmpeg available: the sound files re-encoded in other formats (AIFF, FLAC, MOV, MP3), under
   the same names, are decoded through ffmpeg: lossless formats give the same WAV as the sources.
   TVPaint's AAC export starts with a 1023-sample encoder delay; the first 50 ms are skipped
   (encoder transient).

The .tvpp files point to C:/projets/nebuleuse/TECH/test_tvpp/sorties18/sources_son: the test
maps that prefix to corpus/data/13_son/sources_son with --sound-map.
"""
import glob
import os
import shutil
import subprocess
import sys
import tempfile
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _common import CORPUS, opentvp_cmd, run                       # noqa: E402
import opentvp_info as ti                             # noqa: E402
import opentvp_sound as ts                            # noqa: E402

DATA = os.path.join(CORPUS, 'data', '13_son')
MAP = 'C:/projets/nebuleuse/TECH/test_tvpp/sorties18/sources_son=' + os.path.join(DATA, 'sources_son')
AAC_DELAY = 1023
MAX_RMS = 0.006          # rms of (mix - TVPaint export), measured ≤ 0.0055 (B04: first tick after a cut)


def sound_lines(out):
    return [l for l in out.splitlines() if 'sound' in l]


def export_audio(mp4):
    r = subprocess.run(['ffmpeg', '-v', 'error', '-i', mp4, '-vn', '-f', 'f32le', '-ac', '2', '-ar', '48000', '-'],
                       capture_output=True)
    if r.returncode or not r.stdout:
        return None
    return np.frombuffer(r.stdout, np.float32).reshape(-1, 2)


# sound files rewritten in other formats, under the same names: ffmpeg decodes them. Lossless ones
# must give the WAV of the original files, byte for byte; lossy ones, the same WAV in C and Python.
FORMATS = [  # (label, ffmpeg options; {bits} = 16 or 24, as the source; lossless)
    ('AIFF', ['-c:a', 'pcm_s{bits}be', '-f', 'aiff'], True),
    ('FLAC', ['-c:a', 'flac', '-sample_fmt', '{flacfmt}', '-f', 'flac'], True),
    ('MOV PCM', ['-c:a', 'pcm_s{bits}le', '-f', 'mov'], True),
    ('MOV AAC', ['-c:a', 'aac', '-b:a', '192k', '-f', 'mov'], False),
    ('MP3', ['-c:a', 'libmp3lame', '-b:a', '192k', '-f', 'mp3'], False),
]


def convert_sources(dst, opts):
    """Copies sources_son to dst with each WAV re-encoded by ffmpeg (same file name). False on failure."""
    os.makedirs(dst)
    for f in glob.glob(os.path.join(DATA, 'sources_son', '*.wav')):
        with wave.open(f) as w:
            bits = 24 if w.getsampwidth() == 3 else 16
        o = [x.format(bits=bits, flacfmt='s32' if bits == 24 else 's16') for x in opts]
        r = subprocess.run(['ffmpeg', '-v', 'error', '-i', f] + o + ['-y', os.path.join(dst, os.path.basename(f))],
                           capture_output=True)
        if r.returncode:
            return False
    return True


def check_formats(tmp, refs):
    fails = 0
    for label, opts, lossless in FORMATS:
        src = os.path.join(tmp, 'src_' + label.replace(' ', '_'))
        if not convert_sources(src, opts):
            print(f'{label:28s} (this ffmpeg cannot encode it: skipped)')
            continue
        m = MAP.split('=')[0] + '=' + src
        bad = []
        for (tvpp, k), (ref, plays) in refs.items():
            outs, wavs = {}, {}
            for impl in ('c', 'python'):
                d = os.path.join(tmp, f'fmt_{os.path.basename(src)}_{os.path.basename(tvpp)}_{k}_{impl}')
                outs[impl] = run(impl, tvpp, d, '--clip', str(k), '--scale', '0.25', '--sound', '--sound-map', m)
                w = glob.glob(os.path.join(d, '*.wav'))
                wavs[impl] = open(w[0], 'rb').read() if w else None
            name = f'{os.path.basename(tvpp)[:-5]} clip {k}'
            if wavs['c'] is None or wavs['c'] != wavs['python']:
                bad.append(f'{name}: C and Python WAVs differ')
            elif lossless and wavs['c'] != ref:
                bad.append(f'{name}: differs from the WAV sources')
            if sound_lines(outs['c']) != sound_lines(outs['python']):
                bad.append(f'{name}: C and Python sound messages differ')
            if plays and 'decoded with ffmpeg' not in outs['c']:
                bad.append(f'{name}: not decoded with ffmpeg')
        fails += bool(bad)
        what = 'same WAV as the sources' if lossless else 'C = Python'
        print(f'{label:28s} {len(refs)} clip(s), {what:24s} {"OK" if not bad else "FAIL: " + "; ".join(bad[:3])}')
    return fails


def check_no_ffmpeg(tmp, tvpp, k):
    """Without ffmpeg (PATH emptied): the track is reported and skipped, the same way in C and Python."""
    src = os.path.join(tmp, 'src_MOV_PCM')
    env = dict(os.environ, PATH=os.path.dirname(sys.executable))
    outs = {}
    for impl in ('c', 'python'):
        d = os.path.join(tmp, f'noff_{impl}')
        r = subprocess.run(opentvp_cmd(impl) + [tvpp, d, '--clip', str(k), '--scale', '0.25', '--sound',
                                                '--sound-map', MAP.split('=')[0] + '=' + src],
                           capture_output=True, text=True, env=env)
        outs[impl] = r.stdout
    ok = '(ffmpeg not found)' in outs['c'] and sound_lines(outs['c']) == sound_lines(outs['python'])
    print(f'{"no ffmpeg":28s} {os.path.basename(tvpp)[:-5]} clip {k}{"":12s} {"OK" if ok else "FAIL: not reported the same way"}')
    return 0 if ok else 1


def main():
    ffmpeg = shutil.which('ffmpeg') is not None
    fails = n = 0
    tmp = tempfile.mkdtemp(prefix='opentvp_sound_')
    refs = {}
    try:
        for tvpp in sorted(glob.glob(os.path.join(DATA, '*.tvpp'))):
            name = os.path.basename(tvpp)[:-5]
            for c in ti.read_tvpp(tvpp)['clips']:
                if not c['sound']['tracks']:
                    continue
                n += 1
                k = c['index']
                outs, wavs = {}, {}
                for impl in ('c', 'python'):
                    d = os.path.join(tmp, f'{name}_{k}_{impl}')
                    outs[impl] = run(impl, tvpp, d, '--clip', str(k), '--scale', '0.25', '--sound', '--sound-map', MAP)
                    w = glob.glob(os.path.join(d, '*.wav'))
                    wavs[impl] = open(w[0], 'rb').read() if w else None
                msg = []
                if wavs['c'] is None or wavs['c'] != wavs['python']:
                    msg.append('C and Python WAVs differ')
                if sound_lines(outs['c']) != sound_lines(outs['python']):
                    msg.append('C and Python sound messages differ')
                mp4 = os.path.join(DATA, name + '.mp4')
                truth = ''
                if ffmpeg and k == 1 and os.path.exists(mp4) and wavs['python']:
                    e = export_audio(mp4)
                    if e is not None:
                        path = glob.glob(os.path.join(tmp, f'{name}_{k}_python', '*.wav'))[0]
                        m = ts.read_wav(path)[1]
                        e = e[AAC_DELAY:]
                        L = min(len(m), len(e))
                        rms = float(np.sqrt(((m[2400:L] - e[2400:L]) ** 2).mean()))
                        truth = f'  vs TVPaint: rms {rms:.5f}'
                        if rms > MAX_RMS:
                            msg.append(f'differs from the TVPaint export (rms {rms:.5f} > {MAX_RMS})')
                if not msg and wavs['c']:
                    plays = not c['sound']['mute'] and any(not t['mute'] for t in c['sound']['tracks'])
                    refs[(tvpp, k)] = (wavs['c'], plays)
                status = 'OK' if not msg else 'FAIL: ' + '; '.join(msg)
                fails += bool(msg)
                print(f'{name:28s} clip {k}{truth:24s} {status}')
        if ffmpeg and refs:
            print()
            fails += check_formats(tmp, refs)
            if os.path.isdir(os.path.join(tmp, 'src_MOV_PCM')):
                fails += check_no_ffmpeg(tmp, *next(k for k, v in refs.items() if v[1]))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"\n[sound] {n} clip(s) with sound, {fails} failed"
          f"{'' if ffmpeg else ' (ffmpeg not found: TVPaint exports not compared)'}")
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
