# SPDX-License-Identifier: CC0-1.0
"""Sound export (render --sound) on corpus/data/13_son.

1. C and Python write the same WAV, byte for byte, and print the same sound messages.
2. With ffmpeg available: the WAV matches TVPaint's movie export (<file>.mp4) within the AAC loss.
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

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _common import CORPUS, run                       # noqa: E402
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


def main():
    ffmpeg = shutil.which('ffmpeg') is not None
    fails = n = 0
    tmp = tempfile.mkdtemp(prefix='opentvp_sound_')
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
                status = 'OK' if not msg else 'FAIL: ' + '; '.join(msg)
                fails += bool(msg)
                print(f'{name:28s} clip {k}{truth:24s} {status}')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"\n[sound] {n} clip(s) with sound, {fails} failed"
          f"{'' if ffmpeg else ' (ffmpeg not found: TVPaint exports not compared)'}")
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
