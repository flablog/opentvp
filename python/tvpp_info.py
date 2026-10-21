#!/usr/bin/env python3
"""tvpp_info — affiche la structure d'un fichier TVPaint (.tvpp) sans TVPaint.

Usage :
    python tvpp_info.py fichier.tvpp [fichier2.tvpp ...]
    python tvpp_info.py fichier.tvpp --json        # sortie JSON (pipeline)

Aucune dépendance : bibliothèque standard Python 3.8+ uniquement.
Ne décode pas les pixels : la lecture est quasi instantanée, même sur de gros plans.

Format établi par rétro-ingénierie sur TVPaint Animation 11.7 (Les Fées Spéciales).
"""
import json
import math
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

BLEND_MODES = {
    0: 'Color', 1: 'Behind', 2: 'Erase', 3: 'Shade', 4: 'Light', 5: 'Colorize',
    6: 'Hue', 7: 'Add', 8: 'Sub', 9: 'Multiply', 10: 'Screen', 11: 'Replace',
    12: 'Copy', 15: 'Difference', 16: 'Divide', 17: 'Overlay', 18: 'Light2',
    19: 'Shade2', 20: 'HardLight', 21: 'SoftLight', 22: 'GrainExtract',
    23: 'GrainMerge', 24: 'Sub2', 25: 'Darken', 26: 'Lighten', 27: 'Value',
    28: 'Saturation',
}

# Comportements avant/après (LRHD : u16 @52 = avant, u16 @44 = après)
BEHAVIORS = {0: 'none', 1: 'repeat', 2: 'pingpong', 3: 'hold'}
BEHAVIORS_FR = {'none': '-', 'repeat': 'répéter', 'pingpong': 'ping-pong', 'hold': 'tenir'}

# Types d'entrée de frame (premier u32 d'un bloc SRAW)
FRAME_NEW, FRAME_HOLD, FRAME_BREAK, FRAME_EMPTY = 0x40, 6, 2, 5


# ---------------------------------------------------------------- lecture bas niveau

def _chunks(d, start):
    """Blocs façon IFF : tag ASCII 4 octets + taille u32 big-endian (+ octet de bourrage)."""
    i = start
    while i + 8 <= len(d):
        tag = d[i:i + 4]
        if not re.fullmatch(rb'[A-Z0-9 ]{4}', tag):
            break
        size = struct.unpack('>I', d[i + 4:i + 8])[0]
        yield tag.decode(), d[i + 8:i + 8 + size]
        i += 8 + size + (size & 1)


def _zchk_inner(body, full=False):
    """Bloc ZCHK = un bloc interne compressé (zlib par blocs de 64 Ko).
    body : tag interne (4) + taille interne (4) + 'czmp' + u32 taille totale
           + u32 nombre de blocs, puis par bloc : u32 méthode (100 = zlib),
           u32 taille brute, u32 taille compressée, données.
    Le flux décompressé contient le bloc interne complet (tag + taille + contenu).
    Avec full=False, ne décompresse que le début (assez pour lire le type de frame)."""
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


# Conteneur général : sections = GUID (16 octets) + taille u64 big-endian + contenu.
# On les reconnaît par les 4 premiers octets du GUID.
SEC_ROOT, SEC_PROJECT_HDR, SEC_SHOT = '3384780e', '3385553a', '338631b2'
SEC_SHOT_HDR, SEC_CLIP, SEC_CLIP_HDR, SEC_CLIP_BODY = '3388da98', '3389b846', '3387e34a', '33871154'


def _sections(d, start, end):
    """Itère les sections filles de [start, end) : (id, début contenu, fin contenu)."""
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
    """Liste clé/valeur : u32 nombre de paires, puis (u16 longueur + UTF-16 BE) × 2."""
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
    """Lit une section texte « [name] » (clé=valeur) stockée en clair dans le fichier."""
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


# ---------------------------------------------------------------- calques

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
            elif tag == 'LNAW':   # même nom, en UTF-8 : prioritaire
                cur['name'] = body.split(b'\0')[0].decode('utf-8', 'replace')
            elif tag in ('LRHD', 'LRSH'):   # LRSH : en-tête d'un calque CTG
                cur['lrhd'] = body
                cur['ctg'] = tag == 'LRSH'
            elif tag == 'LRSR':             # CTG : début des images de remplissage calculé
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
    """Réglages d'un calque CTG (section LEXT) et présence du remplissage enregistré."""
    ext = raw['ext']
    src = ext.get('scribbles source pos', {})
    sources = [int(v) + 1 for k, v in sorted(src.items()) if k.isdigit()]   # index de calque (base 1)
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
    pre = BEHAVIORS.get(struct.unpack('>H', h[52:54])[0], 'inconnu')
    post = BEHAVIORS.get(struct.unpack('>H', h[44:46])[0], 'inconnu')
    names = raw['ext'].get('images name', {})

    exposures = []
    if count == 0:  # calque image : une image fixe de start à end
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
        'blending': BLEND_MODES.get(blend, f'inconnu ({blend})'),
        'pre_behavior': pre,
        'post_behavior': post,
        'exposures': exposures,
        **({'ctg': _ctg_info(raw)} if raw['ctg'] else {}),
    }


# ---------------------------------------------------------------- API

def _camera(d, start, end):
    """Caméra du clip (TVPaint 11) : section texte [cameradata] du clip.
    Chaque point : centre (x, y) en pixels du projet, rotation en degrés,
    zoom (1 = cadre caméra à sa taille nominale)."""
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
    ptype = {0: 'courbe', 1: 'lineaire', 2: 'polynomial'}.get(
        int(kv.get('mprofile_prof_linear', 1) or 1), 'courbe')
    return {
        'points': points,
        'mode': int(kv.get('mmode', 0) or 0),
        # flou : fraction de l'intervalle entre deux frames (0 = aucun) ;
        # TVPaint moyenne le cadrage entre t et t + flou (fenêtre vers l'avant)
        'motion_blur': float(kv.get('mmotionblur', 0) or 0),
        'speed_profile': {'type': ptype, 'points': prof},
    }


def profile_eval(profile, t):
    """Profil de vitesse Temps -> Position (éditeur « Profil : Camera Temps/Position »).
    lineaire   : segments droits entre les points
    courbe     : Hermite cubique, tangentes de Catmull-Rom (pente entre les voisins),
                 tangente nulle aux deux extrémités ; droite s'il n'y a que deux points
    polynomial : polynôme de Lagrange passant par tous les points
    Le résultat est borné à [0, 1]."""
    pts = sorted(profile.get('points') or [(0.0, 0.0), (1.0, 1.0)])
    if len(pts) < 2:
        return t
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    kind = profile.get('type', 'lineaire')
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
    if kind == 'lineaire' or len(xs) == 2:
        # une « courbe » à deux points seulement reste une droite (mesuré sur B1)
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
    """Durées relatives des segments du chemin caméra (TVPaint 11, déduit du corpus) :
    L_i = distance x/y du segment i (zoom et rotation ne comptent pas), n segments,
    w_0 = L_0 et w_i = (n-1)·L_(i-1) + L_i."""
    n = len(points) - 1
    L = [math.hypot(points[i + 1]['x'] - points[i]['x'], points[i + 1]['y'] - points[i]['y'])
         for i in range(n)]
    return [L[0]] + [(n - 1) * L[i - 1] + L[i] for i in range(1, n)] if n > 0 else []


def camera_at(points, u):
    """Cadrage interpolé à la position u ∈ [0, 1] du chemin (x, y, rotation, zoom).
    Dans un segment, les quatre valeurs varient linéairement."""
    if not points:
        return None
    if len(points) == 1 or u <= 0:
        return dict(points[0])
    w = _camera_weights(points)
    total = sum(w)
    if total == 0:
        # aucun déplacement x/y (zoom ou rotation seuls) : segments de durées égales
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
    """Cadrage de chaque frame. Le chemin est réparti sur nframes (étendue des
    calques VISIBLES) : temps t = f / (nframes - 1), puis u = profil(t)
    (profil de vitesse), puis cadrage = camera_at(points, u)."""
    if not nframes:
        return []
    prof = profile or {'type': 'lineaire', 'points': [(0.0, 0.0), (1.0, 1.0)]}
    return [camera_at(points, profile_eval(prof, f / (nframes - 1) if nframes > 1 else 0.0))
            for f in range(nframes)]


def _clips(d):
    """Plans (shots) puis clips du projet. Repli : tout le fichier = un clip."""
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
    """Retourne un dict décrivant le projet, ses clips et leurs calques."""
    with open(path, 'rb') as f:
        d = f.read()
    hdr = _header(d)
    info = _ini_section(d, 'info')
    fps = float(hdr.get('FrameRate', 0) or 0)
    clips = []
    for k, (ch, a, b) in enumerate(_clips(d)):
        layers = [_layer_info(i + 1, l) for i, l in enumerate(_layers(d, a, b))]
        frames = max((l['end'] for l in layers), default=-1) + 1
        # la caméra (et le rendu) couvrent l'étendue des calques visibles seulement
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
                # première frame où u = profil(f/(N-1)) atteint la position du point
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
        'clips': clips,
    }


# ---------------------------------------------------------------- affichage

def _timecode(frames, fps):
    if not fps:
        return '?'
    s = frames / fps
    return f"{int(s // 60):02d}:{s % 60:05.2f}"


def print_report(p):
    print(f"\n{p['file']}")
    print(f"  Résolution   {p['width']} × {p['height']}  (pixel aspect {p['pixel_aspect']:g})")
    print(f"  Sortie       {p['output']['width']} × {p['output']['height']}  (taille caméra)")
    print(f"  Cadence      {p['fps']:g} i/s")
    if abs(p['output']['fps'] - p['fps']) > 1e-3:
        print(f"  ⚠ Cadence caméra {p['output']['fps']:g} i/s ≠ projet : TVPaint rééchantillonne au rendu"
              f" (frames dupliquées ou sautées)")
    print(f"  TVPaint      {p['tvpaint']}")
    print(f"  Modifié le   {p['modified']}  par {p['author']}")
    print(f"  Clips        {len(p['clips'])}")
    for c in p['clips']:
        marks = ''
        if c['mark_in'] is not None or c['mark_out'] is not None:
            marks = f"   marques {c['mark_in'] if c['mark_in'] is not None else '-'}" \
                    f" → {c['mark_out'] if c['mark_out'] is not None else '-'}"
        hidden = '  (masqué)' if c['hidden'] else ''
        print(f"\n  ▸ Clip {c['index']} « {c['name']} »{hidden} — {c['frames']} "
              f"frame{'s' if c['frames'] > 1 else ''} ({_timecode(c['frames'], p['fps'])}){marks}")
        cols = ['#', 'Nom', 'Type', 'Frames', 'Avant', 'Après', 'Dessins', 'Visible', 'Verrou', 'Opacité', 'Mode']
        rows = [[str(l['index']), l['name'], l['type'], f"{l['start']}-{l['end']}",
                 BEHAVIORS_FR.get(l['pre_behavior'], l['pre_behavior']),
                 BEHAVIORS_FR.get(l['post_behavior'], l['post_behavior']), str(l['drawings']),
                 'oui' if l['visible'] else 'non', 'oui' if l['locked'] else '-',
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
            print(f"    Caméra : {len(pts)} point(s), {'en mouvement' if moving else 'fixe'}"
                  f"{', flou de mouvement ' + format(cam['motion_blur'], 'g') if cam['motion_blur'] else ''}"
                  f"{', profil de vitesse ' + cam['speed_profile']['type'] if len(cam['speed_profile']['points']) > 2 or cam['speed_profile']['type'] != 'lineaire' else ''}")
            if moving:
                print(f"      réparti sur {cam['frames']} frames (étendue des calques visibles)")
            for k, q in enumerate(pts):
                arr = cam.get('arrival_frames')
                when = f"  → atteint frame {arr[k]}" if moving and arr else ''
                print(f"      point {k} : centre ({q['x']:g}, {q['y']:g})  zoom {q['zoom']:g}  rotation {q['rotation']:g}°{when}")
        names = {l['index']: l['name'] for l in c['layers']}
        for l in c['layers']:
            if 'ctg' not in l:
                continue
            g = l['ctg']
            src = ', '.join(f"{i} « {names.get(i, '?')} »" for i in g['sources']) or 'aucune'
            res = 'enregistré' if g['result_stored'] else \
                  ('partiel' if g['result_frames'] else 'absent (à recalculer)')
            print(f"    CTG {l['index']} « {l['name']} » : source(s) {src} — {len(g['colors'])} couleur(s)"
                  f"{', ' + str(g['textures']) + ' texture(s)' if g['textures'] else ''} — remplissage {res}")
    print()


def main(argv):
    if '--version' in argv:
        print('tvpp_info', __version__)
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
        except Exception as e:  # fichier illisible : on continue avec les autres
            print(f"{path} : lecture impossible ({e})", file=sys.stderr)
            code = 2
    if as_json:
        print(json.dumps(results[0] if len(results) == 1 else results, indent=2, ensure_ascii=False))
    else:
        for r in results:
            print_report(r)
    return code


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
