#!/usr/bin/env python3
"""tvpp_render — rendu composé d'un clip TVPaint (.tvpp) sans TVPaint.

Usage :
    python tvpp_render.py plan.tvpp dossier_sortie [options]

Options :
    --clip N            clip à rendre (1 par défaut)
    --frames A-B        plage de frames (par défaut : marques in/out, sinon tout
                        le contenu visible)
    --bg MODE           fond : projet (défaut) | transparent | #RRGGBB
    --scale S           facteur d'échelle de la sortie (ex. 0.5 pour une preview)
    --blur-samples N    nombre d'échantillons pour le flou de mouvement (8)
    --prefix NOM        préfixe des images (défaut : nom du clip)

Export calque par calque (voir EXPORT_CALQUES.md) :
    --layers [LISTE]    exporte chaque calque dans son propre dossier : all (défaut,
                        calques visibles) ou « nom,nom,3 » (numéros de tvpp_info)
    --keyframes         une image par dessin + JSON d'exposition (sinon : toutes
                        les frames, tenues comprises)
    --camera            (toutes les frames) calques cadrés par la caméra
    --include-hidden    inclut les calques masqués avec --layers all
    --skip-empty        n'écrit pas d'image transparente quand le calque est vide
    --jobs N            calques en parallèle (Linux/Mac)
    --png-level N       compression PNG 0-9 (3)
    --json NOM          nom du fichier JSON

Dépendances : Python 3.8+, numpy, Pillow.
Modules du même dossier : tvpp_info.py (structure, caméra), proto.py (pixels).

Ce que fait le rendu (TVPaint 11.7, établi sur corpus) :
  * calques visibles, du bas vers le haut, avec leur opacité ;
  * tenues, comportements avant/après (répéter, ping-pong, tenir) ;
  * calques CTG : remplissage enregistré par TVPaint ;
  * caméra : taille de sortie, position, zoom, rotation, profil de vitesse,
    flou de mouvement ;
  * modes de fusion : Color (normal) validé ; Multiply, Screen, Add, Overlay,
    Darken, Lighten, Difference, Behind, Erase implémentés mais pas encore
    validés contre TVPaint ; les autres modes sont rendus en normal (avertissement).
"""
import argparse
import json
import multiprocessing
import os
import re
import struct
import sys
import time

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tvpp_info as ti                                     # noqa: E402
from proto import (chunks, zchk_unwrap, unpackbits_bgra,   # noqa: E402
                   _decode_chain, frame_sources)


# ---------------------------------------------------------------- lecture des pixels

def raw_layers(d, start, end):
    """Calques bruts (blocs de pixels) entre start et end, dans l'ordre du fichier."""
    out = []
    for m in re.compile(rb'LNAM').finditer(d, start, end):
        cur = {}
        dk, sk = 'dbod', 'sraw'
        for tag, off, body in chunks(d, m.start(), len(d)):
            if tag == 'ZCHK':
                tag, body = zchk_unwrap(body)
            if tag == 'LNAM':
                if 'lnam' in cur:
                    break
                cur['lnam'] = True
            elif tag in ('LRHD', 'LRSH'):
                cur['lrhd'] = body
                cur['ctg'] = tag == 'LRSH'
            elif tag == 'LRSR':
                dk, sk = 'rdbod', 'rsraw'
                cur['rsraw'] = []
            elif tag == 'DBOD':
                cur.setdefault(dk, []).append((off, body))
            elif tag == 'SRAW':
                cur.setdefault(sk, []).append((off, body))
        if 'lrhd' in cur:
            out.append(cur)
    return out


class LayerImages:
    """Accès aux images d'un calque, décodées à la demande.
    Les images suivantes dépendent des précédentes (tuiles de référence) :
    on avance dans la chaîne en ne gardant que l'image courante en mémoire,
    plus les images réutilisées plus loin par une entrée de type 2.
    Un retour en arrière (ping-pong, répéter…) relance la chaîne depuis le début."""

    def __init__(self, raw, w, h):
        self.w, self.h = w, h
        use_result = raw.get('ctg') and bool(raw.get('rsraw') or raw.get('rdbod'))
        if use_result:
            self.first = raw['rdbod'][0][1] if raw.get('rdbod') else None
            self.seq = raw.get('rsraw', [])
        else:
            self.first = raw['dbod'][0][1]
            self.seq = raw.get('sraw', [])
        # entrée de la frame k : seq[k - 1] si DBOD, seq[k] sinon
        self.off = -1 if self.first is not None else 0
        self.keep = set()
        for _, body in self.seq:
            if struct.unpack('>I', body[:4])[0] == 2:
                self.keep.add(struct.unpack('>I', body[4:8])[0])
        self.cache = {}
        self.idx, self.img = -1, None

    def _step(self, k):
        """Image de la frame k à partir de l'image de la frame k-1 (self.img)."""
        j = k + self.off
        if j < 0:
            return
        if j >= len(self.seq):
            return
        self.img = _decode_chain([self.img], [self.seq[j]], self.w, self.h,
                                 lookup=self._lookup)[-1]

    def _lookup(self, src):
        if src in self.cache:
            return self.cache[src]
        saved = (self.idx, self.img)
        img = LayerImages.__new__(LayerImages)
        img.__dict__.update({k: v for k, v in self.__dict__.items() if k not in ('cache', 'img', 'idx')})
        img.cache, img.idx, img.img = {}, -1, None
        res = img.get(src)
        self.idx, self.img = saved
        return res

    def _restart(self):
        if self.first is not None:
            self.img, _ = unpackbits_bgra(self.first, self.w, self.h)
        else:
            self.img = bytes(self.w * self.h * 4)
            self._step(0)
        self.idx = 0
        if 0 in self.keep:
            self.cache[0] = self.img

    def get(self, k):
        if self.img is None or k < self.idx:
            self._restart()
        while self.idx < k:
            self._step(self.idx + 1)
            self.idx += 1
            if self.idx in self.keep:
                self.cache[self.idx] = self.img
        return self.img


# ---------------------------------------------------------------- caméra

def camera_affine(cam_pt, proj_w, proj_h, out_w, out_h, scale):
    """Coefficients PIL (sortie -> projet) pour un cadrage (x, y, rotation, zoom).
    Relation (coordonnées continues, y vers le bas, angle en degrés) :
        sortie - centre_sortie = R(+rotation) · (projet - centre_caméra) / zoom
    """
    cx, cy = cam_pt['x'], cam_pt['y']
    z = cam_pt['zoom'] / scale
    th = np.radians(cam_pt['rotation'])
    c, s = np.cos(th), np.sin(th)
    ox, oy = out_w / 2.0, out_h / 2.0
    # projet = z · R(-θ) · (sortie - O) + C
    a, b = z * c, z * s
    d_, e = -z * s, z * c
    return (a, b, cx - a * ox - b * oy,
            d_, e, cy - d_ * ox - e * oy)


def default_camera(pw, ph):
    return {'x': pw / 2.0, 'y': ph / 2.0, 'rotation': 0.0, 'zoom': 1.0}


# ---------------------------------------------------------------- fusion

def blend(dst, src, mode):
    """dst, src : float32 prémultipliés (H, W, 4) RGBA. Renvoie le résultat."""
    sa = src[..., 3:4]
    da = dst[..., 3:4]
    if mode in ('Color', 'Normal', None):
        return src + dst * (1 - sa)
    if mode == 'Behind':
        return dst + src * (1 - da)
    if mode == 'Erase':
        return dst * (1 - sa)
    # modes séparables (formules W3C), sur couleurs non prémultipliées
    eps = 1e-6
    Cs = np.where(sa > eps, src[..., :3] / np.maximum(sa, eps), 0)
    Cb = np.where(da > eps, dst[..., :3] / np.maximum(da, eps), 0)
    if mode == 'Multiply':
        B = Cs * Cb
    elif mode == 'Screen':
        B = Cs + Cb - Cs * Cb
    elif mode == 'Add':
        B = np.minimum(Cs + Cb, 1)
    elif mode == 'Darken':
        B = np.minimum(Cs, Cb)
    elif mode == 'Lighten':
        B = np.maximum(Cs, Cb)
    elif mode == 'Difference':
        B = np.abs(Cs - Cb)
    elif mode == 'Overlay':
        B = np.where(Cb <= 0.5, 2 * Cs * Cb, 1 - 2 * (1 - Cs) * (1 - Cb))
    else:
        return src + dst * (1 - sa)
    out_a = sa + da * (1 - sa)
    out_c = (1 - da) * src[..., :3] + (1 - sa) * dst[..., :3] + sa * da * B
    return np.concatenate([out_c, out_a], axis=-1)


SUPPORTED = {'Color', 'Behind', 'Erase', 'Multiply', 'Screen', 'Add',
             'Darken', 'Lighten', 'Difference', 'Overlay'}


# ---------------------------------------------------------------- rendu

class ClipRenderer:
    def __init__(self, path, clip_index=1, scale=1.0, blur_samples=8):
        with open(path, 'rb') as f:
            self.d = f.read()
        self.info = ti.read_tvpp(path)
        self.clip = self.info['clips'][clip_index - 1]
        _, a, b = ti._clips(self.d)[clip_index - 1]
        self.pw, self.ph = self.info['width'], self.info['height']
        ow = self.info['output']['width'] or self.pw
        oh = self.info['output']['height'] or self.ph
        self.scale = scale
        self.ow, self.oh = max(1, round(ow * scale)), max(1, round(oh * scale))
        self.blur_samples = blur_samples
        self.N = self.clip['visible_frames']
        raws = raw_layers(self.d, a, b)
        self.layers = []
        for meta, raw in zip(self.clip['layers'], raws):
            if not meta['visible']:
                continue
            if meta['blending'] not in SUPPORTED:
                print(f"  ⚠ calque « {meta['name']} » : mode {meta['blending']} non géré, rendu en normal",
                      file=sys.stderr)
            self.layers.append({
                'meta': meta,
                'images': LayerImages(raw, self.pw, self.ph),
                'src': frame_sources(raw, max(self.N, 1)),
                'opacity': meta['density'] / 100.0,
            })
        cam = self.clip.get('camera')
        self.cam_points = cam['points'] if cam and cam['points'] else None
        self.cam_profile = cam['speed_profile'] if cam else None
        self.blur = cam['motion_blur'] if cam else 0.0

    # cadrage à l'instant t (0..1)
    def camera_at_t(self, t):
        if not self.cam_points:
            return default_camera(self.pw, self.ph)
        if len(self.cam_points) == 1:
            return self.cam_points[0]
        return ti.camera_at(self.cam_points, ti.profile_eval(self.cam_profile, min(max(t, 0.0), 1.0)))

    def _layer_frame(self, layer, f):
        if f >= len(layer['src']):
            return None
        k = layer['src'][f]
        if k is None:
            return None
        return layer['images'].get(k)

    def render(self, f, u_override=None):
        """Image composée (float32 prémultipliée RGBA, taille de sortie) de la frame f."""
        # instants échantillonnés pour la caméra (flou de mouvement vers l'avant)
        n = max(1, self.N - 1)
        t0 = f / n
        if u_override is not None:
            cams = [ti.camera_at(self.cam_points, u_override)]
        elif self.blur > 0 and self.blur_samples > 1:
            ts = [t0 + self.blur / n * (i / (self.blur_samples - 1)) for i in range(self.blur_samples)]
            cams = [self.camera_at_t(t) for t in ts]
        else:
            cams = [self.camera_at_t(t0)]
        # images des calques à cette frame (bas -> haut)
        stack = []
        for layer in reversed(self.layers):
            raw = self._layer_frame(layer, f)
            if raw is not None:
                stack.append((layer, Image.frombuffer('RGBa', (self.pw, self.ph), raw, 'raw', 'RGBa', 0, 1)))
        acc = np.zeros((self.oh, self.ow, 4), np.float32)
        cover = np.zeros((self.oh, self.ow, 1), np.float32)
        if not hasattr(self, '_canvas'):
            self._canvas = Image.new('L', (self.pw, self.ph), 255)
        for cam in cams:
            coeffs = camera_affine(cam, self.pw, self.ph, self.ow, self.oh, self.scale)
            # part de la sortie couverte par la feuille du projet (hors feuille : noir)
            cover += np.asarray(self._canvas.transform((self.ow, self.oh), Image.AFFINE, coeffs,
                                                       resample=Image.BILINEAR), np.float32)[..., None] / 255.0
            out = np.zeros((self.oh, self.ow, 4), np.float32)
            for layer, im in stack:
                t = im.transform((self.ow, self.oh), Image.AFFINE, coeffs, resample=Image.BILINEAR)
                src = np.asarray(t, np.float32)[..., [2, 1, 0, 3]] / 255.0   # BGRA -> RGBA
                src *= layer['opacity']
                out = blend(out, src, layer['meta']['blending'])
            acc += out
        self.last_cover = cover / len(cams)
        return acc / len(cams)

    def background(self, mode):
        if mode == 'transparent':
            return None
        if mode and mode.startswith('#'):
            h = mode.lstrip('#')
            return tuple(int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4))
        i = self.d.find(b'BGP1')
        j = self.d.find(b'BGP2')
        c1 = self.d[i + 8:i + 11]
        c2 = self.d[j + 8:j + 11]
        if c1 != c2:
            print("  ⚠ fond du projet en deux couleurs (damier probable) : rendu sur la première couleur",
                  file=sys.stderr)
        return tuple(v / 255.0 for v in c1)


def to_image(img, bg, cover=None):
    """cover : part de chaque pixel de sortie couverte par la feuille du projet ;
    le fond du projet n'est posé que sur la feuille, le reste est noir (comme TVPaint)."""
    if bg is None:
        a = img[..., 3:4]
        rgb = np.where(a > 1e-6, img[..., :3] / np.maximum(a, 1e-6), 0)
        out = np.concatenate([rgb, a], axis=-1)
        return Image.fromarray((np.clip(out, 0, 1) * 255 + 0.5).astype(np.uint8), 'RGBA')
    under = np.array(bg, np.float32) * (cover if cover is not None else 1.0)
    rgb = img[..., :3] + under * (1 - img[..., 3:4])
    return Image.fromarray((np.clip(rgb, 0, 1) * 255 + 0.5).astype(np.uint8), 'RGB')


# ---------------------------------------------------------------- export par calque

def safe_name(s):
    return re.sub(r'[^\w.-]+', '_', s).strip('_') or 'calque'


def drawing_ids(li, count):
    """Identité du dessin affiché à chaque frame du calque (0..count-1).
    Une nouvelle image (0x40, ou DBOD) crée un dessin, numéroté par la frame du
    calque où il apparaît ; une tenue (6) prolonge le précédent ; une
    référence (2) reprend le dessin d'une autre frame ; 5 = instance vide (None)."""
    ids = []
    for k in range(max(count, 1)):
        if k == 0 and li.first is not None:
            ids.append(0)
            continue
        j = k + li.off
        if j < 0 or j >= len(li.seq):
            ids.append(ids[-1] if ids else None)
            continue
        body = li.seq[j][1]
        kind = struct.unpack('>I', body[:4])[0]
        if kind == 0x40:
            ids.append(k)
        elif kind == 6:
            ids.append(ids[-1] if ids else None)
        elif kind == 5:
            ids.append(None)
        elif kind == 2:
            ref = struct.unpack('>I', body[4:8])[0]
            ids.append(ids[ref] if ref < len(ids) else ref)
        else:
            ids.append(k)
    return ids


def unpremultiply(px):
    """(H, W, 4) uint8 RGBA prémultiplié -> alpha droit, arrondi comme l'export TVPaint."""
    out = np.array(px, copy=True, order="C")
    flat = out.reshape(-1, 4)
    al = flat[:, 3]
    idx = np.flatnonzero((al > 0) & (al < 255))
    if idx.size:
        a = al[idx].astype(np.uint32)[:, None]
        rgb = flat[idx, :3].astype(np.uint32)
        flat[idx, :3] = np.minimum(255, (rgb * 510 + a) // (2 * a)).astype(np.uint8)
    return out


def raw_to_image(raw, w, h):
    """BGRA prémultiplié -> image PIL RGBA (alpha droit), prête pour un PNG."""
    px = np.frombuffer(raw, np.uint8).reshape(h, w, 4)[..., [2, 1, 0, 3]]
    return Image.fromarray(unpremultiply(px), 'RGBA')


def parse_layer_selection(spec, layers):
    """'all' ou liste « nom,nom,3 » (numéros = ordre de tvpp_info, 1 = en haut)."""
    if spec in (None, '', 'all', 'tous'):
        return list(range(len(layers)))
    out = []
    for tok in [t.strip() for t in spec.split(',') if t.strip()]:
        if tok.isdigit() and 1 <= int(tok) <= len(layers):
            out.append(int(tok) - 1)
            continue
        hits = [i for i, l in enumerate(layers) if l['name'] == tok] or \
               [i for i, l in enumerate(layers) if l['name'].lower() == tok.lower()]
        if not hits:
            raise SystemExit(f"calque introuvable : {tok!r} (voir tvpp_info.py pour la liste)")
        out.extend(hits)
    return sorted(set(out))


_LAYER_JOB = None


def _run_layer_job(i):
    return _LAYER_JOB(i)


def export_layers(a):
    """Export calque par calque : dessins seuls (+ JSON d'exposition) ou toutes les frames."""
    t_start = time.time()
    with open(a.tvpp, 'rb') as f:
        d = f.read()
    info = ti.read_tvpp(a.tvpp)
    clip = info['clips'][a.clip - 1]
    _, cs, ce = ti._clips(d)[a.clip - 1]
    pw, ph = info['width'], info['height']
    raws = raw_layers(d, cs, ce)
    metas = clip['layers']
    sel = parse_layer_selection(a.layers, metas)
    if not a.include_hidden and a.layers in (None, '', 'all', 'tous'):
        sel = [i for i in sel if metas[i]['visible']]
    total = max(clip['frames'], 1)

    # plage : --frames, sinon marques, sinon tout le clip
    if a.frames:
        f0, f1 = (int(x) for x in (a.frames.split('-') + a.frames.split('-'))[:2])
    elif clip['mark_in'] is not None or clip['mark_out'] is not None:
        f0 = clip['mark_in'] or 0
        f1 = clip['mark_out'] if clip['mark_out'] is not None else total - 1
    else:
        f0, f1 = 0, total - 1
    frames = range(f0, f1 + 1)

    # caméra (toujours décrite dans le JSON ; appliquée seulement avec --camera)
    renderer = None
    if a.camera:
        if a.keyframes:
            raise SystemExit("--camera ne s'utilise pas avec --keyframes : un dessin tenu change "
                             "de cadrage à chaque frame. La trajectoire caméra est dans le JSON.")
        renderer = ClipRenderer.__new__(ClipRenderer)
        renderer.pw, renderer.ph = pw, ph
        cam = clip.get('camera')
        renderer.cam_points = cam['points'] if cam and cam['points'] else None
        renderer.cam_profile = cam['speed_profile'] if cam else None
        renderer.blur = cam['motion_blur'] if cam else 0.0
        renderer.N = clip['visible_frames']
        ow = info['output']['width'] or pw
        oh = info['output']['height'] or ph
        renderer.ow, renderer.oh = max(1, round(ow * a.scale)), max(1, round(oh * a.scale))
    N = clip['visible_frames']
    cam = clip.get('camera')
    cam_points = cam['points'] if cam and cam['points'] else None
    track = []
    for f in frames:
        t = f / max(1, N - 1)
        if not cam_points:
            c = default_camera(pw, ph)
        elif len(cam_points) == 1:
            c = cam_points[0]
        else:
            c = ti.camera_at(cam_points, ti.profile_eval(cam['speed_profile'], min(max(t, 0.0), 1.0)))
        track.append({'frame': f, 'x': round(c['x'], 4), 'y': round(c['y'], 4),
                      'rotation': round(c['rotation'], 5), 'zoom': round(c['zoom'], 6)})

    os.makedirs(a.out, exist_ok=True)
    cw, ch = max(1, round(pw * a.scale)), max(1, round(ph * a.scale))
    mode = 'keyframes' if a.keyframes else 'frames'
    print(f"{a.tvpp} — clip « {clip['name']} » — {len(sel)} calque(s) — frames {f0} à {f1} — "
          f"{'dessins seuls' if a.keyframes else 'toutes les frames'}"
          f"{' — à travers la caméra' if a.camera else ''}")

    doc = {
        'source': os.path.basename(a.tvpp),
        'clip': clip['name'],
        'mode': mode,
        'space': 'camera' if a.camera else 'canvas',
        'fps': info['fps'],
        'canvas': {'width': pw, 'height': ph},
        'image_size': ({'width': renderer.ow, 'height': renderer.oh} if a.camera
                       else {'width': cw, 'height': ch}),
        'scale': a.scale,
        'frame_range': [f0, f1],
        'frame_numbering': "0 = première frame du projet (la timeline TVPaint affiche +1)",
        'alpha': 'straight',
        'camera': {
            'output': {'width': info['output']['width'], 'height': info['output']['height'],
                       'fps': info['output']['fps']},
            'motion_blur': cam['motion_blur'] if cam else 0.0,
            'track': track,
        },
        'layer_order': "du haut vers le bas, comme dans TVPaint ; composer en partant du dernier",
        'layers': [],
    }

    png_opts = {'compress_level': a.png_level}
    jobs = max(1, min(a.jobs or 1, len(sel)))
    if jobs > 1 and 'fork' not in multiprocessing.get_all_start_methods():
        print("  (--jobs ignoré sur ce système : export calque après calque)")
        jobs = 1

    def progress(msg):
        if jobs == 1:
            print(msg, end='\r', flush=True)

    def do_layer(i):
        meta, raw = metas[i], raws[i]
        name = safe_name(meta['name'])
        folder = name
        if any(safe_name(metas[j]['name']) == name for j in sel if j != i):
            folder = f"{i + 1:02d}_{name}"
        ldir = os.path.join(a.out, folder)
        os.makedirs(ldir, exist_ok=True)
        li = LayerImages(raw, pw, ph)
        src = frame_sources(raw, max(total, f1 + 1))
        count = struct.unpack('>I', raw['lrhd'][12:16])[0]
        ids = drawing_ids(li, count)

        def drawing_at(f):
            k = src[f] if f < len(src) else None
            if k is None or k >= len(ids):
                return None, None
            return ids[k], k

        # dessins utilisés dans la plage, nommés par leur première frame d'apparition
        first_seen, empty = {}, set()
        for f in frames:
            did, _ = drawing_at(f)
            if did is not None and did not in first_seen:
                first_seen[did] = f

        def drawing_pixels(did):
            img = li.get(did)
            return img

        def make_image(raw_px, f):
            if a.camera:
                im = Image.frombuffer('RGBa', (pw, ph), raw_px, 'raw', 'BGRa', 0, 1)
                n = max(1, renderer.N - 1)
                t0 = f / n
                if renderer.blur > 0 and a.blur_samples > 1:
                    ts = [t0 + renderer.blur / n * (s / (a.blur_samples - 1)) for s in range(a.blur_samples)]
                else:
                    ts = [t0]
                acc = None
                for t in ts:
                    coeffs = camera_affine(renderer.camera_at_t(t), pw, ph, renderer.ow, renderer.oh, a.scale)
                    arr = np.asarray(im.transform((renderer.ow, renderer.oh), Image.AFFINE, coeffs,
                                                  resample=Image.BILINEAR), np.float32)
                    acc = arr if acc is None else acc + arr
                acc = (acc / len(ts) + 0.5).astype(np.uint8)
                return Image.fromarray(unpremultiply(acc), 'RGBA')
            if a.scale != 1.0:
                im = Image.frombuffer('RGBa', (pw, ph), raw_px, 'raw', 'BGRa', 0, 1) \
                    .resize((cw, ch), Image.BILINEAR, reducing_gap=None)
                return Image.fromarray(unpremultiply(np.asarray(im)), 'RGBA')
            return raw_to_image(raw_px, pw, ph)

        drawings, timeline = [], []
        layer_doc = {
            'index': i + 1,
            'name': meta['name'],
            'folder': folder,
            'type': meta.get('type'),
            'visible': meta['visible'],
            'opacity': round(meta['density'] / 100.0, 4),
            'blending': meta['blending'],
            'pre_behavior': meta.get('pre_behavior'),
            'post_behavior': meta.get('post_behavior'),
            'start': meta.get('start'),
            'end': meta.get('end'),
        }

        if a.keyframes:
            for did, f in sorted(first_seen.items(), key=lambda kv: kv[1]):
                px = drawing_pixels(did)
                if not np.frombuffer(px, np.uint8)[3::4].any():      # dessin entièrement vide
                    empty.add(did)
                    continue
                fn = f"{name}_{f:04d}.png"
                make_image(px, f).save(os.path.join(ldir, fn), **png_opts)
                drawings.append({'file': f"{folder}/{fn}", 'first_frame': f, 'layer_frame': did})
                progress(f"  {meta['name']} : dessin {len(drawings)}")
            files = {dd['layer_frame']: dd['file'] for dd in drawings}
            for f in frames:
                did, _ = drawing_at(f)
                timeline.append(files.get(did) if did is not None and did not in empty else None)
        else:
            last_key, last_png = None, None
            blank_png = None
            written = 0
            for f in frames:
                did, _ = drawing_at(f)
                fn = f"{name}_{f:04d}.png"
                path = os.path.join(ldir, fn)
                # une tenue ne change pas l'image (sauf à travers une caméra qui bouge)
                key = (did, f) if a.camera else did
                if did is None:
                    if a.skip_empty:
                        timeline.append(None)
                        continue
                    if blank_png is None:
                        bw, bh = doc['image_size']['width'], doc['image_size']['height']
                        tmp = path + '.tmp'
                        Image.new('RGBA', (bw, bh), (0, 0, 0, 0)).save(tmp, format='PNG', **png_opts)
                        with open(tmp, 'rb') as fh:
                            blank_png = fh.read()
                        os.remove(tmp)
                    with open(path, 'wb') as fh:
                        fh.write(blank_png)
                elif key == last_key and last_png is not None:
                    with open(path, 'wb') as fh:
                        fh.write(last_png)
                else:
                    make_image(drawing_pixels(did), f).save(path, format='PNG', **png_opts)
                    with open(path, 'rb') as fh:
                        last_png = fh.read()
                    last_key = key
                    written += 1
                timeline.append(f"{folder}/{fn}")
                progress(f"  {meta['name']} : frame {f}")

        # expositions : suites de frames montrant le même fichier
        exposures = []
        for f, fn in zip(frames, timeline):
            if exposures and exposures[-1]['file'] == fn and \
                    exposures[-1]['start'] + exposures[-1]['duration'] == f and a.keyframes:
                exposures[-1]['duration'] += 1
            elif a.keyframes:
                exposures.append({'start': f, 'duration': 1, 'file': fn})
        if a.keyframes:
            layer_doc['drawings'] = drawings
            layer_doc['exposures'] = [e for e in exposures if e['file'] is not None]
        layer_doc['key_frames'] = sorted(f for did, f in first_seen.items() if did not in empty)
        layer_doc['timeline'] = timeline
        n_files = len(drawings) if a.keyframes else sum(1 for x in timeline if x)
        print(f"  {meta['name']} : {n_files} image(s)" + ' ' * 20, flush=True)
        return layer_doc

    if jobs == 1:
        doc['layers'] = [do_layer(i) for i in sel]
    else:
        global _LAYER_JOB
        _LAYER_JOB = do_layer
        with multiprocessing.get_context('fork').Pool(jobs) as pool:
            doc['layers'] = pool.map(_run_layer_job, sel, chunksize=1)

    jname = a.json or f"{safe_name(clip['name'])}_{'exposition' if a.keyframes else 'calques'}.json"
    with open(os.path.join(a.out, jname), 'w', encoding='utf-8') as fh:
        json.dump(doc, fh, ensure_ascii=False, indent=1)
    print(f"{jname} écrit — {time.time() - t_start:.1f} s")
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description='Rendu composé d\'un clip TVPaint (.tvpp), '
                                             'ou export calque par calque (--layers).')
    ap.add_argument('tvpp')
    ap.add_argument('out')
    ap.add_argument('--clip', type=int, default=1)
    ap.add_argument('--frames')
    ap.add_argument('--bg', default='projet')
    ap.add_argument('--scale', type=float, default=1.0)
    ap.add_argument('--blur-samples', type=int, default=8)
    ap.add_argument('--prefix')
    g = ap.add_argument_group('export par calque')
    g.add_argument('--layers', nargs='?', const='all',
                   help="exporter les calques séparément : all (défaut) ou liste « nom,nom,3 »")
    g.add_argument('--keyframes', action='store_true',
                   help="seulement les dessins (une image par dessin) + JSON d'exposition")
    g.add_argument('--camera', action='store_true',
                   help="calques cadrés par la caméra (toutes les frames) au lieu de la feuille entière")
    g.add_argument('--include-hidden', action='store_true',
                   help="avec --layers all : inclure les calques masqués")
    g.add_argument('--skip-empty', action='store_true',
                   help="ne pas écrire d'image transparente quand le calque est vide")
    g.add_argument('--png-level', type=int, default=3,
                   help="compression PNG 0-9 (3 par défaut : bon compromis vitesse/poids)")
    g.add_argument('--jobs', type=int, default=1,
                   help="calques exportés en parallèle (Linux/Mac ; ~1 Go de mémoire par job sur un gros plan)")
    g.add_argument('--json', help="nom du fichier JSON (défaut : <clip>_exposition.json ou _calques.json)")
    ap.add_argument('--version', action='version', version='tvpp_render ' + ti.__version__)
    a = ap.parse_args(argv)
    if a.layers is not None or a.keyframes:
        return export_layers(a)

    t_start = time.time()
    r = ClipRenderer(a.tvpp, a.clip, a.scale, a.blur_samples)
    c = r.clip
    if a.frames:
        f0, f1 = (int(x) for x in a.frames.split('-'))
    elif c['mark_in'] is not None or c['mark_out'] is not None:
        f0 = c['mark_in'] or 0
        f1 = c['mark_out'] if c['mark_out'] is not None else r.N - 1
    else:
        f0, f1 = 0, r.N - 1
    f1 = min(f1, r.N - 1)
    bg = r.background(a.bg)
    os.makedirs(a.out, exist_ok=True)
    prefix = a.prefix or re.sub(r'[^\w.-]+', '_', c['name'])
    print(f"{a.tvpp} — clip « {c['name']} » — frames {f0} à {f1} — sortie {r.ow}×{r.oh}"
          f"{' — flou ' + format(r.blur, 'g') if r.blur else ''}")
    for f in range(f0, f1 + 1):
        img = r.render(f)
        to_image(img, bg, r.last_cover).save(os.path.join(a.out, f'{prefix}_{f:04d}.png'))
        print(f"  frame {f}", end='\r', flush=True)
    print(f"\n{f1 - f0 + 1} images en {time.time() - t_start:.1f} s")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
