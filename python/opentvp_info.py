#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""opentvp_info — prints the structure of a TVPaint file (.tvpp) without TVPaint.

Usage:
    python opentvp_info.py file.tvpp [file2.tvpp ...]
    python opentvp_info.py file.tvpp --json        # JSON output (pipeline)

No dependencies: Python 3.8+ standard library only.
Does not decode pixels: reading is near-instant, even on large shots.

Format reverse-engineered on TVPaint Animation 11.7 (Flavio Perez).
"""
import json
import math
import os
import re
import struct
import sys
import zlib


def _read_version():
    import os
    for p in (os.path.join(os.path.dirname(os.path.abspath(__file__)), 'VERSION'),
              os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'VERSION')):
        try:
            with open(p) as f:
                return f.read().strip()
        except OSError:
            pass
    return 'dev'


__version__ = _read_version()


def utf8_console():
    """UTF-8 output, like the C binary (otherwise cp1252 crashes on Windows when output is redirected)."""
    for s in (sys.stdout, sys.stderr):
        if hasattr(s, 'reconfigure'):
            s.reconfigure(encoding='utf-8')

BLEND_MODES = {
    0: 'Color', 1: 'Behind', 2: 'Erase', 3: 'Shade', 4: 'Light', 5: 'Colorize',
    6: 'Hue', 7: 'Add', 8: 'Sub', 9: 'Multiply', 10: 'Screen', 11: 'Replace',
    12: 'Copy', 15: 'Difference', 16: 'Divide', 17: 'Overlay', 18: 'Light2',
    19: 'Shade2', 20: 'HardLight', 21: 'SoftLight', 22: 'GrainExtract',
    23: 'GrainMerge', 24: 'Sub2', 25: 'Darken', 26: 'Lighten', 27: 'Value',
    28: 'Saturation',
}

# Pre/post behaviours (LRHD: u16 @52 = pre, u16 @44 = post)
BEHAVIORS = {0: 'none', 1: 'repeat', 2: 'pingpong', 3: 'hold'}
BEHAVIORS_SHORT = {'none': '-', 'repeat': 'repeat', 'pingpong': 'ping-pong', 'hold': 'hold'}

# Frame entry types (first u32 of an SRAW chunk)
FRAME_NEW, FRAME_HOLD, FRAME_BREAK, FRAME_EMPTY = 0x40, 6, 2, 5


# ---------------------------------------------------------------- low-level reading

def _chunks(d, start):
    """IFF-style chunks: 4-byte ASCII tag + big-endian u32 size (+ padding byte)."""
    i = start
    while i + 8 <= len(d):
        tag = d[i:i + 4]
        if not re.fullmatch(rb'[A-Z0-9 ]{4}', tag):
            break
        size = struct.unpack('>I', d[i + 4:i + 8])[0]
        yield tag.decode(), d[i + 8:i + 8 + size]
        i += 8 + size + (size & 1)


def _zchk_inner(body, full=False):
    """ZCHK chunk = a compressed inner chunk (zlib, in 64 KB blocks).
    body: inner tag (4) + inner size (4) + 'czmp' + u32 total size
          + u32 number of blocks, then for each block: u32 method (100 = zlib),
          u32 raw size, u32 compressed size, data.
    The decompressed stream holds the complete inner chunk (tag + size + content).
    With full=False, only the start is decompressed (enough to read the frame type)."""
    tag = body[:4].decode()
    nb = struct.unpack('>I', body[16:20])[0]
    i, out = 20, bytearray()
    for _ in range(nb):
        method, raw, comp = struct.unpack('>3I', body[i:i + 12])
        i += 12
        chunk = body[i:i + comp]
        i += comp
        if method == 100:
            out += zlib.decompressobj().decompress(chunk, 0 if full else 16)
        else:
            out += chunk
        if not full and len(out) >= 16:
            break
    return tag, bytes(out[8:])


# Overall container: sections = GUID (16 bytes) + big-endian u64 size + content.
# They are identified by the first 4 bytes of the GUID.
SEC_ROOT, SEC_PROJECT_HDR, SEC_SHOT = '3384780e', '3385553a', '338631b2'
SEC_SHOT_HDR, SEC_CLIP, SEC_CLIP_HDR, SEC_CLIP_BODY = '3388da98', '3389b846', '3387e34a', '33871154'


def _sections(d, start, end):
    """Iterates over the child sections of [start, end): (id, content start, content end)."""
    i = start
    while i + 24 <= end:
        size = struct.unpack('>Q', d[i + 16:i + 24])[0]
        if i + 24 + size > end:
            break
        yield d[i:i + 4].hex(), i + 24, i + 24 + size
        i += 24 + size


def _find(d, start, end, sec_id):
    return [(a, b) for k, a, b in _sections(d, start, end) if k == sec_id]


def _kv(d, start):
    """Key/value list: u32 number of pairs, then (u16 length + UTF-16 BE) × 2."""
    count = struct.unpack('>I', d[start:start + 4])[0]
    i, out = start + 4, {}
    for _ in range(count):
        kv = []
        for _ in range(2):
            n = struct.unpack('>H', d[i:i + 2])[0]
            i += 2
            kv.append(d[i:i + 2 * n].decode('utf-16-be', 'replace'))
            i += 2 * n
        out[kv[0]] = kv[1]
    return out


def _header(d):
    return _kv(d, 0x30)


def _ini_section(d, name):
    """Reads a "[name]" text section (key=value) stored as plain text in the file."""
    m = re.search(rb'\[' + re.escape(name.encode()) + rb'\]\n((?:[^\[\n][^\n]*\n)*)', d)
    if not m:
        return {}
    out = {}
    for line in m.group(1).decode('utf-8', 'replace').splitlines():
        if '=' in line:
            k, v = line.split('=', 1)
            out[k.strip()] = v.strip()
    return out


def _ini(body):
    sections, cur = {}, None
    for line in body.decode('utf-8-sig', 'replace').splitlines():
        line = line.strip()
        if line.startswith('[') and line.endswith(']'):
            cur = sections.setdefault(line[1:-1], {})
        elif '=' in line and cur is not None:
            k, v = line.split('=', 1)
            cur[k] = v
    return sections


# ---------------------------------------------------------------- layers

def _layers(d, start=0, end=None):
    out = []
    for m in re.compile(rb'LNAM').finditer(d, start, len(d) if end is None else end):
        cur = {'frames': [], 'ext': {}, 'ctg': False, 'result': None}
        target = cur['frames']
        for tag, body in _chunks(d, m.start()):
            if tag == 'LNAM':
                if 'name' in cur:
                    break
                cur['name'] = body.split(b'\0')[0].decode('cp1252', 'replace')
            elif tag == 'LNAW':   # same name, in UTF-8: takes precedence
                cur['name'] = body.split(b'\0')[0].decode('utf-8', 'replace')
            elif tag in ('LRHD', 'LRSH'):   # LRSH: header of a CTG layer
                cur['lrhd'] = body
                cur['ctg'] = tag == 'LRSH'
            elif tag == 'LRSR':             # CTG: start of the computed fill images
                cur['result'] = []
                target = cur['result']
            elif tag == 'DBOD':
                target.append(FRAME_NEW)
            elif tag == 'SRAW':
                target.append(struct.unpack('>I', body[:4])[0])
            elif tag == 'ZCHK':
                inner, data = _zchk_inner(body)
                if inner == 'DBOD':
                    target.append(FRAME_NEW)
                elif inner == 'SRAW':
                    target.append(struct.unpack('>I', data[:4])[0])
            elif tag == 'LEXT':
                cur['ext'].update(_ini(body))
        if 'lrhd' in cur:
            out.append(cur)
    return out


def _ctg_info(raw):
    """Settings of a CTG layer (LEXT section) and whether the fill is stored."""
    ext = raw['ext']
    src = ext.get('scribbles source pos', {})
    sources = [int(v) + 1 for k, v in sorted(src.items()) if k.isdigit()]   # layer index (1-based)
    cols = ext.get('scribblescolors', {})
    colors = [{'name': cols.get(f'color_{i}_name'), 'rgba': cols.get(f'color_{i}_color')}
              for i in range(int(cols.get('count', 0) or 0))]
    n = len(raw['frames'])
    stored = len(raw['result'] or [])
    return {
        'sources': sources,
        'colors': colors,
        'textures': int(ext.get('scribblestextures', {}).get('count', 0) or 0),
        'result_stored': stored >= n and n > 0,
        'result_frames': stored,
    }


def _layer_info(index, raw):
    h = raw['lrhd']
    start, end, count = struct.unpack('>3I', h[4:16])
    flags = struct.unpack('>I', h[28:32])[0]
    blend = struct.unpack('>I', h[60:64])[0]
    pre = BEHAVIORS.get(struct.unpack('>H', h[52:54])[0], 'unknown')
    post = BEHAVIORS.get(struct.unpack('>H', h[44:46])[0], 'unknown')
    names = raw['ext'].get('images name', {})

    exposures = []
    if count == 0:  # image layer: one still image from start to end
        exposures.append({'start': start, 'duration': end - start + 1})
    else:
        for k, kind in enumerate(raw['frames']):
            if kind == FRAME_HOLD and exposures:
                exposures[-1]['duration'] += 1
            else:
                e = {'start': start + k, 'duration': 1}
                if kind == FRAME_EMPTY:
                    e['empty'] = True
                if str(k) in names:
                    e['instance'] = names[str(k)]
                exposures.append(e)

    return {
        'index': index,
        'name': raw['name'],
        'type': 'ctg' if raw['ctg'] else ('anim' if count else 'image'),
        'start': start,
        'end': end,
        'drawings': sum(1 for e in exposures if not e.get('empty')),
        'visible': not (flags & 0x01),
        'locked': bool(flags & 0x10),
        'density': round(h[19] / 255 * 100),
        'blending': BLEND_MODES.get(blend, f'unknown ({blend})'),
        'pre_behavior': pre,
        'post_behavior': post,
        'exposures': exposures,
        **({'ctg': _ctg_info(raw)} if raw['ctg'] else {}),
    }


# ---------------------------------------------------------------- API

def _camera(d, start, end):
    """Clip camera (TVPaint 11): the clip's [cameradata] text section.
    Each point: centre (x, y) in project pixels, rotation in degrees,
    zoom (1 = camera frame at its nominal size)."""
    m = re.compile(rb'\[cameradata\]\n((?:[^\[\n][^\n]*\n)*)').search(d, start, end)
    if not m:
        return None
    kv = {}
    for line in m.group(1).decode('utf-8', 'replace').splitlines():
        if '=' in line:
            k, v = line.split('=', 1)
            kv[k] = v
    points = []
    for i in range(int(kv.get('pathcount', 0) or 0)):
        g = lambda f, dflt='0': float(kv.get(f'mpoints-{i}-{f}', dflt))
        points.append({'x': g('x'), 'y': g('y'), 'rotation': g('rotation'), 'zoom': g('zoomfactor', '1')})
    npp = int(kv.get('mprofile_prof_numpoint', 0) or 0)
    prof = [tuple(float(x) for x in kv[f'mprofile_prof_point{i}'].split())
            for i in range(npp) if f'mprofile_prof_point{i}' in kv]
    ptype = {0: 'curve', 1: 'linear', 2: 'polynomial'}.get(
        int(kv.get('mprofile_prof_linear', 1) or 1), 'curve')
    return {
        'points': points,
        'mode': int(kv.get('mmode', 0) or 0),
        # blur: fraction of the interval between two frames (0 = none);
        # TVPaint averages the framing between t and t + blur (forward window)
        'motion_blur': float(kv.get('mmotionblur', 0) or 0),
        'speed_profile': {'type': ptype, 'points': prof},
    }


def profile_eval(profile, t):
    """Time -> Position speed profile ("Profile: Camera Time/Position" editor).
    linear     : straight segments between the points
    curve      : cubic Hermite, Catmull-Rom tangents (slope between neighbours),
                 zero tangent at both ends; a straight line if there are only two points
    polynomial : Lagrange polynomial through every point
    The result is clamped to [0, 1]."""
    pts = sorted(profile.get('points') or [(0.0, 0.0), (1.0, 1.0)])
    if len(pts) < 2:
        return t
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    kind = profile.get('type', 'linear')
    t = min(max(t, xs[0]), xs[-1])
    if kind == 'polynomial':
        v = 0.0
        for i in range(len(xs)):
            li = 1.0
            for j in range(len(xs)):
                if j != i:
                    li *= (t - xs[j]) / (xs[i] - xs[j])
            v += ys[i] * li
        return min(max(v, 0.0), 1.0)
    k = max(i for i in range(len(xs) - 1) if xs[i] <= t) if t < xs[-1] else len(xs) - 2
    h = xs[k + 1] - xs[k]
    s = (t - xs[k]) / h if h else 0.0
    if kind == 'linear' or len(xs) == 2:
        # a "curve" with only two points stays a straight line (measured on B1)
        return ys[k] + (ys[k + 1] - ys[k]) * s
    def slope(i):
        if i == 0 or i == len(xs) - 1:
            return 0.0
        return (ys[i + 1] - ys[i - 1]) / (xs[i + 1] - xs[i - 1])
    m0, m1 = slope(k) * h, slope(k + 1) * h
    s2, s3 = s * s, s * s * s
    v = ((2 * s3 - 3 * s2 + 1) * ys[k] + (s3 - 2 * s2 + s) * m0
         + (-2 * s3 + 3 * s2) * ys[k + 1] + (s3 - s2) * m1)
    return min(max(v, 0.0), 1.0)


def _camera_weights(points):
    """Relative durations of the camera path segments (TVPaint 11, inferred from the corpus):
    L_i = x/y distance of segment i (zoom and rotation do not count), n segments,
    w_0 = L_0 and w_i = (n-1)·L_(i-1) + L_i."""
    n = len(points) - 1
    L = [math.hypot(points[i + 1]['x'] - points[i]['x'], points[i + 1]['y'] - points[i]['y'])
         for i in range(n)]
    return [L[0]] + [(n - 1) * L[i - 1] + L[i] for i in range(1, n)] if n > 0 else []


def camera_at(points, u):
    """Framing interpolated at position u ∈ [0, 1] of the path (x, y, rotation, zoom).
    Within a segment, the four values vary linearly."""
    if not points:
        return None
    if len(points) == 1 or u <= 0:
        return dict(points[0])
    w = _camera_weights(points)
    total = sum(w)
    if total == 0:
        # no x/y movement (zoom or rotation only): segments of equal duration
        w = [1.0] * (len(points) - 1)
        total = float(len(w))
    t, acc = u * total, 0.0
    for i, wi in enumerate(w):
        if wi > 0 and t <= acc + wi + 1e-12:
            f = (t - acc) / wi
            a, b = points[i], points[i + 1]
            return {k: a[k] + (b[k] - a[k]) * f for k in ('x', 'y', 'rotation', 'zoom')}
        acc += wi
    return dict(points[-1])


def camera_track(points, nframes, profile=None):
    """Framing of each frame. The path is spread over nframes (extent of the
    VISIBLE layers): time t = f / (nframes - 1), then u = profile(t)
    (speed profile), then framing = camera_at(points, u)."""
    if not nframes:
        return []
    prof = profile or {'type': 'linear', 'points': [(0.0, 0.0), (1.0, 1.0)]}
    return [camera_at(points, profile_eval(prof, f / (nframes - 1) if nframes > 1 else 0.0))
            for f in range(nframes)]


def _sound(d, start, end, level):
    """Sound tracks of the project (level 'sound') or of a clip (level 'audio'): INI sections
    [level] then [level+N]. The audio itself is not in the file: each track gives a path."""
    def section(name):
        m = re.compile(rb'\[' + re.escape(name.encode()) + rb'\]\n((?:[^\[\n][^\n]*\n)*)').search(d, start, end)
        if not m:
            return None
        return dict(l.split('=', 1) for l in m.group(1).decode('utf-8', 'replace').splitlines() if '=' in l)
    head = section(level) or {}
    tracks = []
    for n in range(int(float(head.get('trackcount', 0) or 0))):
        t = section(f'{level}+{n}')
        if t is None:
            continue
        num = lambda k, dflt=0.0: float(t.get(k, dflt) or dflt)
        path = t.get('filepath', '')
        tracks.append({
            'index': n + 1,
            'file': path,
            'found': bool(path) and os.path.isfile(path),
            'offset': num('offset'),
            'volume': num('volume', 1.0),
            'mute': t.get('mute') == '1',
            'fade_in': [num('envelopein'), num('envelopein2')],
            # fade-out: full at envelopeout2, silent at envelopeout (B05_fades_1s_keyframes)
            'fade_out': [num('envelopeout2'), num('envelopeout')],
        })
    return {'volume': float(head.get('volume', 1) or 1), 'mute': head.get('mute') == '1', 'tracks': tracks}


def _clips(d):
    """Shots, then clips of the project. Fallback: the whole file = one clip."""
    clips = []
    for ra, rb in _find(d, 0, len(d), SEC_ROOT):
        for sa, sb in _find(d, ra, rb, SEC_SHOT):
            for ca, cb in _find(d, sa, sb, SEC_CLIP):
                hdr = _find(d, ca, cb, SEC_CLIP_HDR)
                body = _find(d, ca, cb, SEC_CLIP_BODY)
                if hdr and body:
                    clips.append((_kv(d, hdr[0][0]), body[0][0], body[0][1]))
    return clips or [({}, 0, len(d))]


def read_tvpp(path):
    """Returns a dict describing the project, its clips and their layers."""
    with open(path, 'rb') as f:
        d = f.read()
    hdr = _header(d)
    info = _ini_section(d, 'info')
    fps = float(hdr.get('FrameRate', 0) or 0)
    clips = []
    for k, (ch, a, b) in enumerate(_clips(d)):
        layers = [_layer_info(i + 1, l) for i, l in enumerate(_layers(d, a, b))]
        frames = max((l['end'] for l in layers), default=-1) + 1
        # the camera (and the render) cover the extent of the visible layers only
        visible_frames = max((l['end'] for l in layers if l['visible']), default=-1) + 1
        cam = _camera(d, a, b)
        if cam is not None:
            cam['frames'] = visible_frames
            cam['track'] = (camera_track(cam['points'], visible_frames, cam['speed_profile'])
                            if len(cam['points']) > 1 else None)
            w = _camera_weights(cam['points']) if len(cam['points']) > 1 else []
            if w and sum(w) == 0:
                w = [1.0] * len(w)
            tot, acc, arr = sum(w), 0.0, [0]
            for wi in w:
                acc += wi
                target = acc / tot if tot else 0
                # first frame where u = profile(f/(N-1)) reaches the point's position
                arr.append(next((f for f in range(visible_frames)
                                 if profile_eval(cam['speed_profile'], f / max(1, visible_frames - 1))
                                 >= target - 1e-9), visible_frames - 1))
            cam['arrival_frames'] = arr if w else None
        clips.append({
            'index': k + 1,
            'name': ch.get('Name', f'clip {k + 1}'),
            'hidden': ch.get('Hidden') == '1',
            'mark_in': int(ch['MarkInPosition']) if ch.get('MarkIn') == '1' else None,
            'mark_out': int(ch['MarkOutPosition']) if ch.get('MarkOut') == '1' else None,
            'frames': frames,
            'duration_s': round(frames / fps, 3) if fps else None,
            'visible_frames': visible_frames,
            'camera': cam,
            'sound': _sound(d, a, b, 'audio'),
            'layers': layers,
        })
    return {
        'file': path,
        'width': int(hdr.get('Width', 0)),
        'height': int(hdr.get('Height', 0)),
        'pixel_aspect': float(hdr.get('PixelAspectRatio', 1) or 1),
        'fps': fps,
        'start_frame': int(hdr.get('StartFrame', 0) or 0),
        'field_order': hdr.get('FieldOrder'),
        'author': hdr.get('Author'),
        'created': hdr.get('CreationDate'),
        'modified': hdr.get('LastChangeDate'),
        'tvpaint': info.get('lastchangeversion') or hdr.get('Host'),
        'output': {'width': int(hdr.get('Camera.Width', 0) or 0),
                   'height': int(hdr.get('Camera.Height', 0) or 0),
                   'fps': float(hdr.get('Camera.FrameRate', 0) or 0) or fps},
        'sound': _sound(d, 0, len(d), 'sound'),
        'clips': clips,
    }


# ---------------------------------------------------------------- display

def _timecode(frames, fps):
    if not fps:
        return '?'
    s = frames / fps
    return f"{int(s // 60):02d}:{s % 60:05.2f}"


def _print_sound(s, title, indent):
    """Sound tracks of the project or of a clip (nothing printed without tracks)."""
    if not s['tracks']:
        return
    extra = (', all muted' if s['mute'] else '') + (f", volume {s['volume']:g}" if s['volume'] != 1 else '')
    print(f"{indent}{title}{len(s['tracks'])} track(s){extra}")
    for t in s['tracks']:
        line = f"offset {t['offset']:g} s, volume {t['volume']:g}"
        if t['mute']:
            line += ', muted'
        if t['fade_in'][1] > t['fade_in'][0]:
            line += f", fade in {t['fade_in'][0]:g}-{t['fade_in'][1]:g} s"
        if t['fade_out'][1] > t['fade_out'][0]:
            line += f", fade out {t['fade_out'][0]:g}-{t['fade_out'][1]:g} s"
        print(f"{indent}  track {t['index']}: {t['file']}{'' if t['found'] else '  (file not found)'}")
        print(f"{indent}    {line}")


def print_report(p):
    print(f"\n{p['file']}")
    print(f"  Resolution   {p['width']} × {p['height']}  (pixel aspect {p['pixel_aspect']:g})")
    print(f"  Output       {p['output']['width']} × {p['output']['height']}  (camera size)")
    print(f"  Frame rate   {p['fps']:g} fps")
    if abs(p['output']['fps'] - p['fps']) > 1e-3:
        print(f"  ⚠ Camera frame rate {p['output']['fps']:g} fps ≠ project: TVPaint resamples at render"
              f" (duplicated or skipped frames)")
    print(f"  TVPaint      {p['tvpaint']}")
    print(f"  Modified     {p['modified']}  by {p['author']}")
    _print_sound(p['sound'], 'Sound        project, ', '  ')
    print(f"  Clips        {len(p['clips'])}")
    for c in p['clips']:
        marks = ''
        if c['mark_in'] is not None or c['mark_out'] is not None:
            marks = f"   marks {c['mark_in'] if c['mark_in'] is not None else '-'}" \
                    f" → {c['mark_out'] if c['mark_out'] is not None else '-'}"
        hidden = '  (hidden)' if c['hidden'] else ''
        print(f"\n  ▸ Clip {c['index']} \"{c['name']}\"{hidden} — {c['frames']} "
              f"frame{'s' if c['frames'] > 1 else ''} ({_timecode(c['frames'], p['fps'])}){marks}")
        cols = ['#', 'Name', 'Type', 'Frames', 'Pre', 'Post', 'Drawings', 'Visible', 'Locked', 'Opacity', 'Mode']
        rows = [[str(l['index']), l['name'], l['type'], f"{l['start']}-{l['end']}",
                 BEHAVIORS_SHORT.get(l['pre_behavior'], l['pre_behavior']),
                 BEHAVIORS_SHORT.get(l['post_behavior'], l['post_behavior']), str(l['drawings']),
                 'yes' if l['visible'] else 'no', 'yes' if l['locked'] else '-',
                 f"{l['density']} %", l['blending']] for l in c['layers']]
        widths = [max([len(col)] + [len(r[i]) for r in rows]) for i, col in enumerate(cols)]
        print('    ' + '  '.join(col.ljust(w) for col, w in zip(cols, widths)))
        print('    ' + '  '.join('-' * w for w in widths))
        for r in rows:
            print('    ' + '  '.join(v.ljust(w) for v, w in zip(r, widths)))
        cam = c.get('camera')
        if cam and cam['points']:
            pts = cam['points']
            moving = any((q['x'], q['y'], q['rotation'], q['zoom']) != (pts[0]['x'], pts[0]['y'], pts[0]['rotation'], pts[0]['zoom']) for q in pts)
            print(f"    Camera: {len(pts)} point(s), {'moving' if moving else 'still'}"
                  f"{', motion blur ' + format(cam['motion_blur'], 'g') if cam['motion_blur'] else ''}"
                  f"{', speed profile ' + cam['speed_profile']['type'] if len(cam['speed_profile']['points']) > 2 or cam['speed_profile']['type'] != 'linear' else ''}")
            if moving:
                print(f"      spread over {cam['frames']} frames (extent of the visible layers)")
            for k, q in enumerate(pts):
                arr = cam.get('arrival_frames')
                when = f"  → reached at frame {arr[k]}" if moving and arr else ''
                print(f"      point {k}: centre ({q['x']:g}, {q['y']:g})  zoom {q['zoom']:g}  rotation {q['rotation']:g}°{when}")
        _print_sound(c['sound'], 'Sound: ', '    ')
        names ={l['index']: l['name'] for l in c['layers']}
        for l in c['layers']:
            if 'ctg' not in l:
                continue
            g = l['ctg']
            src = ', '.join(f"{i} \"{names.get(i, '?')}\"" for i in g['sources']) or 'none'
            res = 'stored' if g['result_stored'] else \
                  ('partial' if g['result_frames'] else 'missing (needs recomputing)')
            print(f"    CTG {l['index']} \"{l['name']}\": source(s) {src} — {len(g['colors'])} colour(s)"
                  f"{', ' + str(g['textures']) + ' texture(s)' if g['textures'] else ''} — fill {res}")
    print()


def main(argv):
    utf8_console()
    if '--version' in argv:
        print('opentvp_info', __version__)
        return 0
    as_json = '--json' in argv
    paths = [a for a in argv if not a.startswith('--')]
    if not paths:
        print(__doc__)
        return 1
    results, code = [], 0
    for path in paths:
        try:
            results.append(read_tvpp(path))
        except Exception as e:  # unreadable file: carry on with the others
            print(f"{path}: cannot read ({e})", file=sys.stderr)
            code = 2
    if as_json:
        print(json.dumps(results[0] if len(results) == 1 else results, indent=2, ensure_ascii=False))
    else:
        for r in results:
            print_report(r)
    return code


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
