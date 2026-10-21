"""Rendu d'un plan de production comparé à un dossier de rendus TVPaint (fichiers hors dépôt).

    python tests/compare_render.py plan.tvpp dossier_refs [--impl c|python] [--pattern ref_{:04d}.png] [--frames A-B]
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
    out = tempfile.mkdtemp(prefix='tvpp_cmp_')
    args = ['--prefix', 'x'] + (['--frames', a.frames] if a.frames else [])
    run(a.impl, a.tvpp, out, *args)
    worst, maxd, n = compare_dirs(os.path.join(out, 'x_*.png'), os.path.join(a.refs, a.pattern))
    print(f"{n} frames comparées — pire frame : {worst:.3f}% de pixels > 16/255, écart max {maxd}")
    shutil.rmtree(out, True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
