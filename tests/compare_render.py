# SPDX-License-Identifier: GPL-3.0-or-later
"""Render of a production shot compared with a folder of TVPaint renders (files kept out of the repository).

    python tests/compare_render.py shot.tvpp refs_folder [--impl c|python] [--pattern ref_{:04d}.png] [--frames A-B]
"""
import argparse
import os
import shutil
import sys
import tempfile

from _common import compare_dirs, run


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('tvpp')
    ap.add_argument('refs')
    ap.add_argument('--impl', default='c', choices=('c', 'python'))
    ap.add_argument('--pattern', default='ref_{:04d}.png')
    ap.add_argument('--frames')
    a = ap.parse_args()
    out = tempfile.mkdtemp(prefix='opentvp_cmp_')
    args = ['--prefix', 'x'] + (['--frames', a.frames] if a.frames else [])
    run(a.impl, a.tvpp, out, *args)
    worst, maxd, n = compare_dirs(os.path.join(out, 'x_*.png'), os.path.join(a.refs, a.pattern))
    print(f"{n} frames compared — worst frame: {worst:.3f}% of pixels > 16/255, max difference {maxd}")
    shutil.rmtree(out, True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
