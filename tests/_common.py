# SPDX-License-Identifier: GPL-3.0-or-later
"""Helpers shared by the tests: run the C or Python implementation, read TVPaint ground truth."""
import glob
import os
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORPUS = os.path.join(ROOT, 'corpus')
sys.path.insert(0, os.path.join(ROOT, 'python'))


def opentvp_cmd(impl):
    """Base command for the chosen implementation ('c' or 'python')."""
    if impl == 'python':
        return [sys.executable, os.path.join(ROOT, 'python', 'opentvp_render.py')]
    exe = os.environ.get('OPENTVP_BIN') or os.path.join(ROOT, 'c', 'build', 'opentvp' + ('.exe' if os.name == 'nt' else ''))
    if not os.path.exists(exe):
        sys.exit(f"binary not found: {exe} (run c/build.sh, or set OPENTVP_BIN)")
    return [exe, 'render']


def run(impl, tvpp, out, *args):
    r = subprocess.run(opentvp_cmd(impl) + [tvpp, out] + list(args), capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr[-500:] or r.stdout[-500:])
    return r.stdout


def read_gt(base):
    """tv_ClipSaveStructure CSV: (clip name, per-layer metadata, per-frame rows)."""
    lines = open(base + '_gt.csv', encoding='utf-8').read().splitlines()
    clip = lines[2].split(',')[0].strip().strip('"')
    meta = {x.split(',')[0]: [v.strip().strip('"') for v in x.split(',')[1:]]
            for x in lines if x.startswith('#') and not x[1].isdigit()}
    frames = [[v.strip().strip('"') for v in x.split(',')[1:]]
              for x in lines if x.startswith('#') and x[1].isdigit()]
    return clip, meta, frames


def clip_index(tvpp, name):
    import opentvp_info as ti
    clips = ti.read_tvpp(tvpp)['clips']
    for c in clips:
        if c['name'] == name:
            return c['index']
    return 1


def load_rgb(path):
    return np.asarray(Image.open(path).convert('RGB'), np.int16)


def compare_dirs(mine_glob, ref_fmt):
    """Compares each produced image with the reference; returns (worst % of pixels > 16, max difference, n)."""
    worst = maxd = n = 0
    for f in sorted(glob.glob(mine_glob)):
        num = int(f[-8:-4])
        ref = ref_fmt.format(num)
        if not os.path.exists(ref):
            continue
        d = np.abs(load_rgb(f) - load_rgb(ref)).max(axis=2)
        worst = max(worst, float((d > 16).mean() * 100))
        maxd = max(maxd, int(d.max()))
        n += 1
    return worst, maxd, n
