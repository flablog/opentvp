"""Outils partagés par les tests : lancer l'implémentation C ou Python, lire la vérité terrain TVPaint."""
import glob
import os
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORPUS = os.path.join(ROOT, 'corpus')
sys.path.insert(0, os.path.join(ROOT, 'python'))


def tvpp_cmd(impl):
    """Commande de base pour l'implémentation choisie ('c' ou 'python')."""
    if impl == 'python':
        return [sys.executable, os.path.join(ROOT, 'python', 'tvpp_render.py')]
    exe = os.environ.get('TVPP_BIN') or os.path.join(ROOT, 'c', 'build', 'tvpp' + ('.exe' if os.name == 'nt' else ''))
    if not os.path.exists(exe):
        sys.exit(f"binaire introuvable : {exe} (lancer c/build.sh, ou définir TVPP_BIN)")
    return [exe, 'render']


def run(impl, tvpp, out, *args):
    r = subprocess.run(tvpp_cmd(impl) + [tvpp, out] + list(args), capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr[-500:] or r.stdout[-500:])
    return r.stdout


def read_gt(base):
    """CSV de tv_ClipSaveStructure : (nom du clip, métadonnées par calque, lignes par frame)."""
    lines = open(base + '_gt.csv', encoding='utf-8').read().splitlines()
    clip = lines[2].split(',')[0].strip().strip('"')
    meta = {x.split(',')[0]: [v.strip().strip('"') for v in x.split(',')[1:]]
            for x in lines if x.startswith('#') and not x[1].isdigit()}
    frames = [[v.strip().strip('"') for v in x.split(',')[1:]]
              for x in lines if x.startswith('#') and x[1].isdigit()]
    return clip, meta, frames


def clip_index(tvpp, name):
    import tvpp_info as ti
    clips = ti.read_tvpp(tvpp)['clips']
    for c in clips:
        if c['name'] == name:
            return c['index']
    return 1


def load_rgb(path):
    return np.asarray(Image.open(path).convert('RGB'), np.int16)


def compare_dirs(mine_glob, ref_fmt):
    """Compare chaque image produite à la référence ; renvoie (pire % de pixels > 16, écart max, n)."""
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
