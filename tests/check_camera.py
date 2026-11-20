# SPDX-License-Identifier: GPL-3.0-or-later
"""Final render (through the camera) compared with the TVPaint renders of the camera corpus.

The camera model is not pixel-exact (see docs/FORMAT.md): each case has a baseline
value in tests/baseline_camera.json (worst frame, % of pixels off by more than
16/255). The test fails if a case gets worse by more than 0.05 points.

    python tests/check_camera.py [--impl c|python] [--update-baseline]
"""
import argparse
import json
import os
import shutil
import sys
import tempfile

from _common import CORPUS, compare_dirs, run

BASELINE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'baseline_camera.json')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--impl', default='c', choices=('c', 'python'))
    ap.add_argument('--update-baseline', action='store_true')
    a = ap.parse_args()
    refs = json.load(open(os.path.join(CORPUS, 'camera_refs.json')))
    base = json.load(open(BASELINE)) if os.path.exists(BASELINE) else {}
    tmp = tempfile.mkdtemp(prefix='opentvp_cam_')
    new, bad = {}, 0
    for r in refs:
        tv = os.path.join(CORPUS, r['tvpp'])
        out = os.path.join(tmp, 'o')
        shutil.rmtree(out, True)
        run(a.impl, tv, out, '--prefix', 'x', '--blur-samples', '16')
        worst, maxd, n = compare_dirs(os.path.join(out, 'x_*.png'),
                                      os.path.join(os.path.dirname(tv), r['prefix'] + '_{:04d}.png'))
        key = r['tvpp']
        new[key] = round(worst, 3)
        ref = base.get(key)
        status = '' if ref is None else ('OK' if worst <= ref + 0.05 else f'REGRESSED (baseline {ref}%)')
        bad += status.startswith('R')
        print(f"{key:58} {n:3} frames  >16: {worst:6.3f}%  max {maxd:3}  {status}")
    shutil.rmtree(tmp, True)
    if a.update_baseline:
        json.dump(new, open(BASELINE, 'w'), indent=1)
        print('baseline updated:', BASELINE)
    print(f"\n[{a.impl}] {len(refs)} cases, {bad} regressed")
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
