#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""opentvp_render — composited render of a TVPaint clip (.tvpp) without TVPaint.

Usage:
    python opentvp_render.py shot.tvpp out_folder [options]

Options:
    --clip N            clip to render (1 by default)
    --frames A-B        frame range, from 0 (default: in/out marks, otherwise all
                        visible content)
    --bg MODE           background: project (default) | transparent | #RRGGBB
    --scale S           output scale factor (e.g. 0.5 for a preview)
    --blur-samples N    number of motion blur samples (8)
    --prefix NAME       image prefix (default: clip name)
    --sound             also writes the mix of the clip's sound tracks: <prefix>.wav
                        (48 kHz stereo 24-bit, the duration of the frames; see opentvp_sound.py)
    --sound-map OLD=NEW replace the OLD path prefix of the sound files by NEW (repeatable)
    --project-sound     with --sound: also mix the project's sound tracks (TVPaint leaves
                        them out of a clip's export; the visible clips follow one another)
    --ffmpeg PATH       ffmpeg used for sound files other than WAV (MP3, AAC, AIFF, MOV…)
                        (default: the PATH, then next to opentvp_sound.py)

Layer-by-layer export (see LAYER_EXPORT.md):
    --layers [LIST]     exports each layer to its own folder: all (default,
                        visible layers) or "name,name,3" (numbers from opentvp_info)
    --keyframes         one image per drawing + exposure JSON (otherwise: every
                        frame, holds included)
    --camera            (every frame) layers framed by the camera
    --include-hidden    includes hidden layers with --layers all
    --skip-empty        writes no transparent image when the layer is empty
    --jobs N            layers in parallel (Linux/Mac)
    --png-level N       PNG compression 0-9 (3)
    --json NAME         JSON file name

Dependencies: Python 3.8+, numpy, Pillow.
Modules from the same folder: opentvp_info.py (structure, camera), proto.py (pixels).

What the render does (TVPaint 11.7, established on the corpus):
  * visible layers, bottom to top, with their opacity;
  * holds, pre/post behaviours (repeat, ping-pong, hold);
  * CTG layers: fill stored by TVPaint;
  * camera: output size, position, zoom, rotation, speed profile,
    motion blur;
  * blending modes: Color (normal) validated; Multiply, Screen, Add, Overlay,
    Darken, Lighten, Difference, Behind, Erase implemented but not yet
    validated against TVPaint; other modes are rendered as normal (warning).
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
import opentvp_info as ti                                     # noqa: E402
import opentvp_sound as ts                                    # noqa: E402
from proto import (chunks, zchk_unwrap, unpackbits_bgra,   # noqa: E402
                   _decode_chain, frame_sources)


# ---------------------------------------------------------------- reading pixels

def raw_layers(d, start, end):
    """Raw layers (pixel chunks) between start and end, in file order."""
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
    """Access to a layer's images, decoded on demand.
    Each image depends on the previous ones (reference tiles): we walk the
    chain keeping only the current image in memory, plus the images reused
    later by a type 2 entry.
    Going backwards (ping-pong, repeat…) replays the chain from the start."""

    def __init__(self, raw, w, h):
        self.w, self.h = w, h
        use_result = raw.get('ctg') and bool(raw.get('rsraw') or raw.get('rdbod'))
        if use_result:
            self.first = raw['rdbod'][0][1] if raw.get('rdbod') else None
            self.seq = raw.get('rsraw', [])
        else:
            self.first = raw['dbod'][0][1]
            self.seq = raw.get('sraw', [])
        # entry for frame k: seq[k - 1] if there is a DBOD, seq[k] otherwise
        self.off = -1 if self.first is not None else 0
        self.keep = set()
        for _, body in self.seq:
            if struct.unpack('>I', body[:4])[0] == 2:
                self.keep.add(struct.unpack('>I', body[4:8])[0])
        self.cache = {}
        self.idx, self.img = -1, None

    def _step(self, k):
        """Image of frame k from the image of frame k-1 (self.img)."""
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


# ---------------------------------------------------------------- camera

def camera_affine(cam_pt, proj_w, proj_h, out_w, out_h, scale):
    """PIL coefficients (output -> project) for a framing (x, y, rotation, zoom).
    Relation (continuous coordinates, y down, angle in degrees):
        output - output_centre = R(+rotation) · (project - camera_centre) / zoom
    """
    cx, cy = cam_pt['x'], cam_pt['y']
    z = cam_pt['zoom'] / scale
    th = np.radians(cam_pt['rotation'])
    c, s = np.cos(th), np.sin(th)
    ox, oy = out_w / 2.0, out_h / 2.0
    # project = z · R(-θ) · (output - O) + C
    a, b = z * c, z * s
    d_, e = -z * s, z * c
    return (a, b, cx - a * ox - b * oy,
            d_, e, cy - d_ * ox - e * oy)


def default_camera(pw, ph):
    return {'x': pw / 2.0, 'y': ph / 2.0, 'rotation': 0.0, 'zoom': 1.0}


# ---------------------------------------------------------------- blending

def blend(dst, src, mode):
    """dst, src: premultiplied float32 (H, W, 4) RGBA. Returns the result."""
    sa = src[..., 3:4]
    da = dst[..., 3:4]
    if mode in ('Color', 'Normal', None):
        return src + dst * (1 - sa)
    if mode == 'Behind':
        return dst + src * (1 - da)
    if mode == 'Erase':
        return dst * (1 - sa)
    # separable modes (W3C formulas), on non-premultiplied colours
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


# ---------------------------------------------------------------- rendering

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
                print(f"  ⚠ layer \"{meta['name']}\": unsupported mode {meta['blending']}, rendered as normal",
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

    # framing at time t (0..1)
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
        """Composited image (premultiplied float32 RGBA, output size) of frame f."""
        # times sampled for the camera (forward motion blur)
        n = max(1, self.N - 1)
        t0 = f / n
        if u_override is not None:
            cams = [ti.camera_at(self.cam_points, u_override)]
        elif self.blur > 0 and self.blur_samples > 1:
            ts = [t0 + self.blur / n * (i / (self.blur_samples - 1)) for i in range(self.blur_samples)]
            cams = [self.camera_at_t(t) for t in ts]
        else:
            cams = [self.camera_at_t(t0)]
        # layer images at this frame (bottom -> top)
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
            # share of the output covered by the project canvas (outside the canvas: black)
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
            print("  ⚠ two-colour project background (probably a checkerboard): rendered on the first one",
                  file=sys.stderr)
        return tuple(v / 255.0 for v in c1)


def to_image(img, bg, cover=None):
    """cover: share of each output pixel covered by the project canvas;
    the project background is only laid on the canvas, the rest is black (like TVPaint)."""
    if bg is None:
        a = img[..., 3:4]
        rgb = np.where(a > 1e-6, img[..., :3] / np.maximum(a, 1e-6), 0)
        out = np.concatenate([rgb, a], axis=-1)
        return Image.fromarray((np.clip(out, 0, 1) * 255 + 0.5).astype(np.uint8), 'RGBA')
    under = np.array(bg, np.float32) * (cover if cover is not None else 1.0)
    rgb = img[..., :3] + under * (1 - img[..., 3:4])
    return Image.fromarray((np.clip(rgb, 0, 1) * 255 + 0.5).astype(np.uint8), 'RGB')


# ---------------------------------------------------------------- per-layer export

def safe_name(s):
    return re.sub(r'[^\w.-]+', '_', s).strip('_') or 'layer'


def drawing_ids(li, count):
    """Identity of the drawing shown at each layer frame (0..count-1).
    A new image (0x40, or DBOD) creates a drawing, numbered after the layer
    frame where it appears; a hold (6) extends the previous one; a
    reference (2) reuses the drawing of another frame; 5 = empty instance (None)."""
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
    """(H, W, 4) uint8 premultiplied RGBA -> straight alpha, rounded like TVPaint's export."""
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
    """Premultiplied BGRA -> PIL RGBA image (straight alpha), ready for a PNG."""
    px = np.frombuffer(raw, np.uint8).reshape(h, w, 4)[..., [2, 1, 0, 3]]
    return Image.fromarray(unpremultiply(px), 'RGBA')


def parse_layer_selection(spec, layers):
    """'all' or a "name,name,3" list (numbers = opentvp_info order, 1 = top)."""
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
            raise SystemExit(f"layer not found: {tok!r} (see opentvp_info.py for the list)")
        out.extend(hits)
    return sorted(set(out))


_LAYER_JOB = None


def _run_layer_job(i):
    return _LAYER_JOB(i)


def export_layers(a):
    """Layer-by-layer export: drawings only (+ exposure JSON) or every frame."""
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

    # range: --frames, otherwise marks, otherwise the whole clip
    if a.frames:
        f0, f1 = (int(x) for x in (a.frames.split('-') + a.frames.split('-'))[:2])
    elif clip['mark_in'] is not None or clip['mark_out'] is not None:
        f0 = clip['mark_in'] or 0
        f1 = clip['mark_out'] if clip['mark_out'] is not None else total - 1
    else:
        f0, f1 = 0, total - 1
    frames = range(f0, f1 + 1)

    # camera (always described in the JSON; applied only with --camera)
    renderer = None
    if a.camera:
        if a.keyframes:
            raise SystemExit("--camera cannot be used with --keyframes: a held drawing is framed "
                             "differently on every frame. The camera track is in the JSON.")
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
    print(f"{a.tvpp} — clip \"{clip['name']}\" — {len(sel)} layer(s) — frames {f0} to {f1} — "
          f"{'drawings only' if a.keyframes else 'every frame'}"
          f"{' — through the camera' if a.camera else ''}")

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
        'frame_numbering': "0 = first frame of the project (the TVPaint timeline shows +1)",
        'alpha': 'straight',
        'camera': {
            'output': {'width': info['output']['width'], 'height': info['output']['height'],
                       'fps': info['output']['fps']},
            'motion_blur': cam['motion_blur'] if cam else 0.0,
            'track': track,
        },
        'layer_order': "top to bottom, as in TVPaint; composite starting from the last one",
        'layers': [],
    }

    png_opts = {'compress_level': a.png_level}
    jobs = max(1, min(a.jobs or 1, len(sel)))
    if jobs > 1 and 'fork' not in multiprocessing.get_all_start_methods():
        print("  (--jobs ignored on this system: exporting one layer after another)")
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

        # drawings used in the range, named after the first frame they appear on
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
                if not np.frombuffer(px, np.uint8)[3::4].any():      # completely empty drawing
                    empty.add(did)
                    continue
                fn = f"{name}_{f:04d}.png"
                make_image(px, f).save(os.path.join(ldir, fn), **png_opts)
                drawings.append({'file': f"{folder}/{fn}", 'first_frame': f, 'layer_frame': did})
                progress(f"  {meta['name']}: drawing {len(drawings)}")
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
                # a hold does not change the image (except through a moving camera)
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
                progress(f"  {meta['name']}: frame {f}")

        # exposures: runs of frames showing the same file
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
        print(f"  {meta['name']}: {n_files} image(s)" + ' ' * 20, flush=True)
        return layer_doc

    if jobs == 1:
        doc['layers'] = [do_layer(i) for i in sel]
    else:
        global _LAYER_JOB
        _LAYER_JOB = do_layer
        with multiprocessing.get_context('fork').Pool(jobs) as pool:
            doc['layers'] = pool.map(_run_layer_job, sel, chunksize=1)

    if a.sound:
        wname = safe_name(clip['name']) + '.wav'
        if ts.export_sound(info, clip, f0, f1, os.path.join(a.out, wname), ts.parse_map(a.sound_map),
                           project=a.project_sound):
            doc['sound'] = {'file': wname, 'start_frame': f0, 'rate': ts.OUT_RATE}

    jname = a.json or f"{safe_name(clip['name'])}_{'exposure' if a.keyframes else 'layers'}.json"
    with open(os.path.join(a.out, jname), 'w', encoding='utf-8') as fh:
        json.dump(doc, fh, ensure_ascii=False, indent=1)
    print(f"{jname} written — {time.time() - t_start:.1f} s")
    return 0


def main(argv):
    ti.utf8_console()
    ap = argparse.ArgumentParser(description='Composited render of a TVPaint clip (.tvpp), '
                                             'or layer-by-layer export (--layers).')
    ap.add_argument('tvpp')
    ap.add_argument('out')
    ap.add_argument('--clip', type=int, default=1)
    ap.add_argument('--frames')
    ap.add_argument('--bg', default='project')
    ap.add_argument('--scale', type=float, default=1.0)
    ap.add_argument('--blur-samples', type=int, default=8)
    ap.add_argument('--prefix')
    ap.add_argument('--sound', action='store_true',
                    help="also write the mix of the clip's sound tracks as a WAV")
    ap.add_argument('--sound-map', action='append', metavar='OLD=NEW',
                    help="replace the OLD path prefix of the sound files by NEW (repeatable)")
    ap.add_argument('--project-sound', action='store_true',
                    help="with --sound: also mix the project's sound tracks (TVPaint leaves them out of a "
                         "clip's export); the visible clips are placed one after the other")
    ap.add_argument('--ffmpeg', metavar='PATH',
                    help="ffmpeg used for sound files other than WAV (default: the PATH, then next to "
                         "opentvp_sound.py)")
    g = ap.add_argument_group('per-layer export')
    g.add_argument('--layers', nargs='?', const='all',
                   help="export layers separately: all (default) or a \"name,name,3\" list")
    g.add_argument('--keyframes', action='store_true',
                   help="drawings only (one image per drawing) + exposure JSON")
    g.add_argument('--camera', action='store_true',
                   help="layers framed by the camera (every frame) instead of the whole canvas")
    g.add_argument('--include-hidden', action='store_true',
                   help="with --layers all: include hidden layers")
    g.add_argument('--skip-empty', action='store_true',
                   help="write no transparent image when the layer is empty")
    g.add_argument('--png-level', type=int, default=3,
                   help="PNG compression 0-9 (3 by default: good speed/size trade-off)")
    g.add_argument('--jobs', type=int, default=1,
                   help="layers exported in parallel (Linux/Mac; ~1 GB of memory per job on a large shot)")
    g.add_argument('--json', help="JSON file name (default: <clip>_exposure.json or _layers.json)")
    ap.add_argument('--version', action='version', version='opentvp_render ' + ti.__version__)
    a = ap.parse_args(argv)
    if a.ffmpeg:
        ts.set_ffmpeg(a.ffmpeg)
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
    print(f"{a.tvpp} — clip \"{c['name']}\" — frames {f0} to {f1} — output {r.ow}×{r.oh}"
          f"{' — blur ' + format(r.blur, 'g') if r.blur else ''}")
    for f in range(f0, f1 + 1):
        img = r.render(f)
        to_image(img, bg, r.last_cover).save(os.path.join(a.out, f'{prefix}_{f:04d}.png'))
        print(f"  frame {f}", end='\r', flush=True)
    print(f"\n{f1 - f0 + 1} images in {time.time() - t_start:.1f} s")
    if a.sound:
        ts.export_sound(r.info, c, f0, f1, os.path.join(a.out, prefix + '.wav'), ts.parse_map(a.sound_map),
                        project=a.project_sound)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
