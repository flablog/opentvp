# SPDX-License-Identifier: GPL-3.0-or-later
"""Layer-by-layer export (--layers --keyframes) compared with TVPaint's structure export
(tv_ClipSaveStructure: CSV + one PNG per instance) over the whole corpus.
Checks pixel decoding, holds, instances, pre/post behaviours.

    python tests/check_layers.py [--impl c|python] [folder ...]
"""
import argparse
import glob
import json
import os
import shutil
import sys
import tempfile

import numpy as np
from PIL import Image

from _common import CORPUS, clip_index, read_gt, run


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--impl', default='c', choices=('c', 'python'))
    ap.add_argument('dirs', nargs='*')
    a = ap.parse_args()
    dirs = a.dirs or sorted(glob.glob(os.path.join(CORPUS, 'data', '*')))
    tmp = tempfile.mkdtemp(prefix='opentvp_test_')
    nfiles = nframes = nbad = 0
    for d in dirs:
        for f in sorted(glob.glob(os.path.join(d, '*.tvpp'))):
            base = f[:-5]
            if not os.path.exists(base + '_gt.csv'):
                continue
            clip, meta, gframes = read_gt(base)
            out = os.path.join(tmp, os.path.basename(base))
            try:
                run(a.impl, f, out, '--clip', str(clip_index(f, clip)), '--layers', 'all',
                    '--include-hidden', '--keyframes', '--frames', f'0-{len(gframes) - 1}')
            except RuntimeError as e:
                print(f"{os.path.basename(base):34} ERROR {e}")
                nbad += 1
                continue
            doc = json.load(open(glob.glob(out + '/*.json')[0], encoding='utf-8'))
            errs, missing = [], set()
            # At each frame, the CSV only lists the layers that change instance;
            # the "[NNN]" file name prefix gives the layer (#Layers order).
            names = meta['#Layers']
            for li, gname in enumerate(names):
                same = [L for L in doc['layers'] if L['name'] == gname]
                L = same[0] if len(same) == 1 else doc['layers'][li]
                sub = os.path.join(base + '_gt.layers', os.path.basename(meta['#Folder'][li].replace('\\', '/')))
                tag = f'[{li + 1:03d}]'
                cur = None
                for fr, row in enumerate(gframes):
                    name = next((x for x in row if x.startswith(tag)), '')
                    if name:
                        gp = os.path.join(sub, name)
                        if not os.path.exists(gp):          # incomplete ground truth
                            missing.add(li)
                            cur = None
                            continue
                        cur = np.asarray(Image.open(gp).convert('RGBA'))
                    if cur is None:
                        continue
                    fn = L['timeline'][fr]
                    mine = np.asarray(Image.open(os.path.join(out, fn)).convert('RGBA')) if fn else np.zeros_like(cur)
                    visible = (mine[..., 3] > 0) | (cur[..., 3] > 0)
                    dd = int((np.any(mine != cur, axis=2) & visible).sum())
                    nframes += 1
                    if dd:
                        errs.append(f'layer {li + 1} frame {fr}: {dd} px')
            nfiles += 1
            nbad += bool(errs)
            note = f"  (reference PNGs missing for layer {', '.join(str(i + 1) for i in sorted(missing))})" if missing else ''
            print(f"{os.path.basename(base):34} {'OK' if not errs else ', '.join(errs[:3])}{note}")
            shutil.rmtree(out, True)
    shutil.rmtree(tmp, True)
    print(f"\n[{a.impl}] {nfiles} files, {nframes} frames compared, {nbad} failed")
    return 1 if nbad else 0


if __name__ == '__main__':
    sys.exit(main())
