"""Prototype décodeur .tvpp — calques et pixels (TVPaint 11.7)."""
import struct, sys, re

def chunks(d, start, end):
    """Itère les chunks IFF : tag 4 octets + taille u32 big-endian."""
    i = start
    while i + 8 <= end:
        tag = d[i:i+4]
        if not re.fullmatch(rb'[A-Z0-9 ]{4}', tag):
            break
        size = struct.unpack('>I', d[i+4:i+8])[0]
        yield tag.decode(), i, d[i+8:i+8+size]
        i += 8 + size

def unpackbits_bgra(data, width, height):
    """PackBits sur des pixels de 4 octets (BGRA), ligne par ligne."""
    out = bytearray(); i = 0
    for _ in range(height):
        row = bytearray()
        while len(row) < width * 4:
            n = data[i]; i += 1
            if n >= 128:                      # répétition : 257 - n pixels
                count = 257 - n
                row += data[i:i+4] * count; i += 4
            else:                             # littéral : n + 1 pixels
                count = n + 1
                row += data[i:i+4*count]; i += 4 * count
        out += row
    return bytes(out), i

def unpremultiply_bgra(raw):
    """Les pixels sont stockés en alpha prémultiplié : on revient en alpha droit."""
    out = bytearray(raw)
    for i in range(0, len(out), 4):
        a = out[i+3]
        if 0 < a < 255:
            for k in range(3):
                out[i+k] = min(255, round(out[i+k] * 255 / a))
    return bytes(out)

def zchk_unwrap(body):
    """Bloc ZCHK : renvoie (tag interne, contenu interne décompressé)."""
    import zlib
    nb = struct.unpack('>I', body[16:20])[0]
    i, out = 20, bytearray()
    for _ in range(nb):
        method, raw, comp = struct.unpack('>3I', body[i:i + 12]); i += 12
        out += zlib.decompress(body[i:i + comp]) if method == 100 else body[i:i + comp]
        i += comp
    return out[:4].decode(), bytes(out[8:])

def layers(path):
    d = open(path, 'rb').read()
    res = []
    for m in re.finditer(rb'LNAM', d):
        cur = {}
        dk, sk = 'dbod', 'sraw'          # après LRSR : série « résultat » d'un CTG
        for tag, off, body in chunks(d, m.start(), len(d)):
            if tag == 'ZCHK':
                tag, body = zchk_unwrap(body)
            if tag == 'LRSH':            # en-tête d'un calque CTG
                cur['ctg'] = True
                tag = 'LRHD'
            if tag == 'LRSR':            # début des images résultat (remplissage calculé)
                dk, sk = 'rdbod', 'rsraw'
                cur['rsraw'] = []
                continue
            if tag == 'LNAW':
                cur['name'] = body.split(b'\0')[0].decode('utf-8', 'replace')
                continue
            if tag == 'LNAM':
                if 'name' in cur: break
                cur['name'] = body.split(b'\0')[0].decode('cp1252', 'replace')
            elif tag == 'LRHD':
                cur['lrhd'] = body
            elif tag == 'DBOD':
                cur.setdefault(dk, []).append((off, body))
            elif tag == 'SRAW':
                cur.setdefault(sk, []).append((off, body))
            elif tag == 'LEXT':
                cur.setdefault('lext', []).append(body)
        res.append(cur)
    return d, res

def _tiles_from_image(raw, width, height, ts):
    """Découpe une image BGRA en tuiles (raster), pour servir de référence."""
    tiles = []
    for ty in range(0, height, ts):
        for tx in range(0, width, ts):
            tw, th = min(ts, width - tx), min(ts, height - ty)
            t = bytearray()
            for y in range(ty, ty + th):
                t += raw[(y * width + tx) * 4:(y * width + tx + tw) * 4]
            tiles.append((tw, th, bytes(t)))
    return tiles

def _image_from_tiles(tiles, width, height, ts):
    out = bytearray(width * height * 4); k = 0
    for ty in range(0, height, ts):
        for tx in range(0, width, ts):
            tw, th, t = tiles[k]; k += 1
            for y in range(th):
                o = ((ty + y) * width + tx) * 4
                out[o:o + tw * 4] = t[y * tw * 4:(y + 1) * tw * 4]
    return bytes(out)

def layer_images(layer, width, height, result=None):
    """Rend la liste des images d'un calque (BGRA prémultiplié brut).

    Calques CTG : la série principale contient les squiggles ; si TVPaint a
    enregistré le remplissage calculé (série après le bloc LRSR), il est rendu
    à la place. result=True/False force l'une ou l'autre série.

    Image 0 : bloc DBOD, PackBits ligne par ligne sur toute la largeur.
    Images suivantes : blocs SRAW =
      en-tête 4×u32 : taille de tuile (64), taille miniature, largeur et hauteur miniature
      miniature PackBits (flux continu)
      u32 nombre de tuiles, puis pour chaque tuile (ordre raster) :
        u32 taille > 0 : tuile PackBits de cette taille
        u32 0, u32 source, u32 index :
            source 1 = tuile <index> de l'image précédente
            source 0 = tuile <index> de l'image courante (déduplication)
    """
    use_result = layer.get('ctg') and bool(layer.get('rsraw') or layer.get('rdbod'))
    if result is not None:
        use_result = result and use_result
    if use_result:
        seq = layer.get('rsraw', [])
        if layer.get('rdbod'):
            raw, _ = unpackbits_bgra(layer['rdbod'][0][1], width, height)
            return _decode_chain([raw], seq, width, height)
        # pas de DBOD : la série résultat part d'une image vide (non rendue)
        return _decode_chain([bytes(width * height * 4)], seq, width, height, base=1)[1:]
    raw, _ = unpackbits_bgra(layer['dbod'][0][1], width, height)
    return _decode_chain([raw], layer.get('sraw', []), width, height)

def _decode_chain(imgs, seq, width, height, base=0, lookup=None):
    """Applique les entrées SRAW de seq à la liste imgs.
    base : décalage entre l'index de frame et l'index dans imgs (1 pour une série
    résultat CTG qui commence par une image vide fictive).
    lookup : fonction index_de_frame -> image, pour les références (type 2)."""
    for _, body in seq:
        kind = struct.unpack('>I', body[:4])[0]
        if kind == 6:
            # 6 = la frame prolonge l'exposition précédente (tenue)
            imgs.append(imgs[-1])
            continue
        if kind == 2:
            # 2 = nouvelle instance qui reprend l'image de la frame <src> du calque
            #     (coupure d'exposition : src = frame précédente ; réutilisation d'un
            #     dessin plus ancien sinon)
            src = struct.unpack('>I', body[4:8])[0]
            if lookup is not None:
                imgs.append(lookup(src))
            elif 0 <= src + base < len(imgs):
                imgs.append(imgs[src + base])
            else:
                imgs.append(imgs[-1])
            continue
        if kind == 5:
            # 5 = nouvelle instance vide (image transparente)
            imgs.append(bytes(width * height * 4))
            continue
        if kind != 0x40:
            raise NotImplementedError(f'type de frame SRAW inconnu : {kind}')
        ts, thumb_size, thw, thh = struct.unpack('>4I', body[:16])
        # la miniature (PackBits, thw×thh) occupe thumb_size - 8 octets
        i = 16 + thumb_size - 8
        prev = _tiles_from_image(imgs[-1], width, height, ts)
        count = struct.unpack('>I', body[i:i + 4])[0]; i += 4
        cur = []
        for k in range(count):
            tw, th = prev[k][0], prev[k][1]
            size = struct.unpack('>I', body[i:i + 4])[0]; i += 4
            if size:
                t, _ = unpackbits_bgra(body[i:i + size], tw, th); i += size
                cur.append((tw, th, t))
            else:
                src, idx = struct.unpack('>2I', body[i:i + 8]); i += 8
                cur.append(prev[idx] if src == 1 else cur[idx])
        imgs.append(_image_from_tiles(cur, width, height, ts))
    return imgs

def layer_ext(layer):
    """Bloc LEXT (texte INI) : identifiants et noms d'instance, indexés par frame."""
    sections = {}; cur = None
    for body in layer.get('lext', []):
        for line in body.decode('utf-8-sig', 'replace').splitlines():
            line = line.strip()
            if line.startswith('[') and line.endswith(']'):
                cur = sections.setdefault(line[1:-1], {})
            elif '=' in line and cur is not None:
                k, v = line.split('=', 1); cur[k] = v
    return sections

def layer_timeline(layer, width, height):
    """Instances du calque : liste de dicts {start, duration, raw}.

    Le calque stocke une entrée par frame (DBOD puis un SRAW par frame) :
      SRAW type 0x40 -> nouvelle instance avec ses pixels (tuiles)
      SRAW type 6    -> tenue : la frame prolonge l'instance en cours
      SRAW type 2    -> nouvelle instance, mêmes pixels (coupure d'exposition)
      SRAW type 5    -> nouvelle instance vide
    La première frame du calque est donnée par LRHD (octets 4-7).
    """
    frames = layer_images(layer, width, height)
    start, end, count = struct.unpack('>3I', layer['lrhd'][4:16])
    if count == 0:
        # calque image (non animé) : une seule image affichée de start à end
        return [{'start': start, 'duration': end - start + 1, 'raw': frames[0]}]
    names = layer_ext(layer).get('images name', {})
    kinds = [None] + [struct.unpack('>I', b[:4])[0] for _, b in layer.get('sraw', [])]
    inst = []
    for k, (raw, kind) in enumerate(zip(frames, kinds)):
        if kind == 6 and inst:
            inst[-1]['duration'] += 1
        else:
            inst.append({'start': start + k, 'duration': 1, 'raw': raw,
                         'name': names.get(str(k))})
    return inst

BEHAVIORS = {0: 'none', 1: 'repeat', 2: 'pingpong', 3: 'hold'}

def _behave(rel, n, mode):
    """Index (0..n-1) de la frame du calque montrée à la position relative rel,
    hors de la plage du calque, selon le comportement ; None si rien n'est affiché."""
    if mode == 'repeat':
        return rel % n
    if mode == 'pingpong':
        if n == 1:
            return 0
        k = rel % (2 * n - 2)
        return k if k < n else 2 * n - 2 - k
    if mode == 'hold':
        return 0 if rel < 0 else n - 1
    return None

def frame_sources(layer, total_frames):
    """Pour chaque frame du plan, l'index de la frame du calque affichée (ou None).
    Applique les comportements avant/après (LRHD : u16 @52 = avant, u16 @44 = après)."""
    start, end, count = struct.unpack('>3I', layer['lrhd'][4:16])
    pre = BEHAVIORS.get(struct.unpack('>H', layer['lrhd'][52:54])[0], 'none')
    post = BEHAVIORS.get(struct.unpack('>H', layer['lrhd'][44:46])[0], 'none')
    n = count or 1
    out = []
    for f in range(total_frames):
        rel = f - start
        if count == 0:                       # calque image : visible de start à end
            out.append(0 if start <= f <= end else None)
        elif 0 <= rel < n:
            out.append(rel)
        else:
            out.append(_behave(rel, n, pre if rel < 0 else post))
    return out

BLEND_MODES = {
    0: 'Color', 1: 'Behind', 2: 'Erase', 3: 'Shade', 4: 'Light', 5: 'Colorize',
    6: 'Hue', 7: 'Add', 8: 'Sub', 9: 'Multiply', 10: 'Screen', 11: 'Replace',
    12: 'Copy', 15: 'Difference', 16: 'Divide', 17: 'Overlay', 18: 'Light2',
    19: 'Shade2', 20: 'HardLight', 21: 'SoftLight', 22: 'GrainExtract',
    23: 'GrainMerge', 24: 'Sub2', 25: 'Darken', 26: 'Lighten', 27: 'Value',
    28: 'Saturation',
}

def project_info(d):
    """En-tête projet : u32 nombre de paires à 0x30, puis paires clé/valeur
    (u16 longueur + UTF-16 BE)."""
    count = struct.unpack('>I', d[0x30:0x34])[0]
    i = 0x34; info = {}
    for _ in range(count):
        vals = []
        for _ in range(2):
            n = struct.unpack('>H', d[i:i + 2])[0]; i += 2
            vals.append(d[i:i + 2 * n].decode('utf-16-be')); i += 2 * n
        info[vals[0]] = vals[1]
    return info

def layer_props(lrhd):
    """Champs connus du bloc LRHD (104 octets)."""
    pre = BEHAVIORS.get(struct.unpack('>H', lrhd[52:54])[0], 'inconnu')
    post = BEHAVIORS.get(struct.unpack('>H', lrhd[44:46])[0], 'inconnu')
    start, end, count = struct.unpack('>3I', lrhd[4:16])
    flags = struct.unpack('>I', lrhd[28:32])[0]
    blend = struct.unpack('>I', lrhd[60:64])[0]
    return {
        'type': 'anim' if count else 'image',
        'start': start,
        'end': end,
        'density': round(lrhd[19] / 255, 4),
        'visible': not (flags & 0x01),
        'locked': bool(flags & 0x10),
        'blending': BLEND_MODES.get(blend, f'inconnu_{blend}'),
        'pre_behavior': pre,
        'post_behavior': post,
    }

if __name__ == '__main__':
    # usage : python proto.py fichier.tvpp [dossier_sortie]
    import os, json
    from PIL import Image
    path = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else None
    d, ls = layers(path)
    info = project_info(d)
    w, h = int(info['Width']), int(info['Height'])
    report = {'width': w, 'height': h, 'framerate': float(info['FrameRate']), 'layers': []}
    for li, l in enumerate(ls):
        inst = layer_timeline(l, w, h)
        props = layer_props(l['lrhd'])
        timing = []
        for it in inst:
            fname = f"[{li+1:03d}][{it['start']:05d}] {l['name']}.png"
            entry = {'image': fname, 'start': it['start'], 'duration': it['duration']}
            if it.get('name'):
                entry['instance'] = it['name']
            timing.append(entry)
            if out:
                os.makedirs(out, exist_ok=True)
                Image.frombytes('RGBA', (w, h), unpremultiply_bgra(it['raw']), 'raw', 'BGRA') \
                     .save(os.path.join(out, fname))
        report['layers'].append({'name': l['name'], **props, 'exposures': timing})
        print(f"calque {li+1} '{l['name']}' : {props}")
        for t in timing:
            print(f"    frames {t['start']}-{t['start'] + t['duration'] - 1} : {t['image']}")
    if out:
        with open(os.path.join(out, 'structure.json'), 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=2, ensure_ascii=False)
