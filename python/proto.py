# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Les Fées Spéciales
""".tvpp decoder prototype — layers and pixels (TVPaint 11.7)."""
import struct, sys, re

def chunks(d, start, end):
    """Iterates over IFF chunks: 4-byte tag + big-endian u32 size."""
    i = start
    while i + 8 <= end:
        tag = d[i:i+4]
        if not re.fullmatch(rb'[A-Z0-9 ]{4}', tag):
            break
        size = struct.unpack('>I', d[i+4:i+8])[0]
        yield tag.decode(), i, d[i+8:i+8+size]
        i += 8 + size

def unpackbits_bgra(data, width, height):
    """PackBits on 4-byte pixels (BGRA), row by row."""
    out = bytearray(); i = 0
    for _ in range(height):
        row = bytearray()
        while len(row) < width * 4:
            n = data[i]; i += 1
            if n >= 128:                      # run: 257 - n pixels
                count = 257 - n
                row += data[i:i+4] * count; i += 4
            else:                             # literal: n + 1 pixels
                count = n + 1
                row += data[i:i+4*count]; i += 4 * count
        out += row
    return bytes(out), i

def unpremultiply_bgra(raw):
    """Pixels are stored with premultiplied alpha: convert back to straight alpha."""
    out = bytearray(raw)
    for i in range(0, len(out), 4):
        a = out[i+3]
        if 0 < a < 255:
            for k in range(3):
                out[i+k] = min(255, round(out[i+k] * 255 / a))
    return bytes(out)

def zchk_unwrap(body):
    """ZCHK chunk: returns (inner tag, decompressed inner content)."""
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
        dk, sk = 'dbod', 'sraw'          # after LRSR: "result" series of a CTG
        for tag, off, body in chunks(d, m.start(), len(d)):
            if tag == 'ZCHK':
                tag, body = zchk_unwrap(body)
            if tag == 'LRSH':            # header of a CTG layer
                cur['ctg'] = True
                tag = 'LRHD'
            if tag == 'LRSR':            # start of the result images (computed fill)
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
    """Splits a BGRA image into tiles (raster order), to serve as reference."""
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
    """Returns the list of a layer's images (raw premultiplied BGRA).

    CTG layers: the main series holds the squiggles; if TVPaint stored the
    computed fill (series after the LRSR chunk), it is rendered instead.
    result=True/False forces one series or the other.

    Image 0: DBOD chunk, PackBits row by row over the full width.
    Following images: SRAW chunks =
      4×u32 header: tile size (64), thumbnail size, thumbnail width and height
      PackBits thumbnail (continuous stream)
      u32 number of tiles, then for each tile (raster order):
        u32 size > 0: PackBits tile of that size
        u32 0, u32 source, u32 index:
            source 1 = tile <index> of the previous image
            source 0 = tile <index> of the current image (deduplication)
    """
    use_result = layer.get('ctg') and bool(layer.get('rsraw') or layer.get('rdbod'))
    if result is not None:
        use_result = result and use_result
    if use_result:
        seq = layer.get('rsraw', [])
        if layer.get('rdbod'):
            raw, _ = unpackbits_bgra(layer['rdbod'][0][1], width, height)
            return _decode_chain([raw], seq, width, height)
        # no DBOD: the result series starts from an empty image (not rendered)
        return _decode_chain([bytes(width * height * 4)], seq, width, height, base=1)[1:]
    raw, _ = unpackbits_bgra(layer['dbod'][0][1], width, height)
    return _decode_chain([raw], layer.get('sraw', []), width, height)

def _decode_chain(imgs, seq, width, height, base=0, lookup=None):
    """Applies the SRAW entries of seq to the imgs list.
    base: offset between the frame index and the index in imgs (1 for a CTG
    result series that starts with a dummy empty image).
    lookup: frame_index -> image function, for references (type 2)."""
    for _, body in seq:
        kind = struct.unpack('>I', body[:4])[0]
        if kind == 6:
            # 6 = the frame extends the previous exposure (hold)
            imgs.append(imgs[-1])
            continue
        if kind == 2:
            # 2 = new instance reusing the image of layer frame <src>
            #     (exposure break: src = previous frame; otherwise reuse of an
            #     older drawing)
            src = struct.unpack('>I', body[4:8])[0]
            if lookup is not None:
                imgs.append(lookup(src))
            elif 0 <= src + base < len(imgs):
                imgs.append(imgs[src + base])
            else:
                imgs.append(imgs[-1])
            continue
        if kind == 5:
            # 5 = new empty instance (transparent image)
            imgs.append(bytes(width * height * 4))
            continue
        if kind != 0x40:
            raise NotImplementedError(f'unknown SRAW frame type: {kind}')
        ts, thumb_size, thw, thh = struct.unpack('>4I', body[:16])
        # the thumbnail (PackBits, thw×thh) takes thumb_size - 8 bytes
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
    """LEXT chunk (INI text): instance ids and names, indexed by frame."""
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
    """Layer instances: list of {start, duration, raw} dicts.

    The layer stores one entry per frame (DBOD, then one SRAW per frame):
      SRAW type 0x40 -> new instance with its pixels (tiles)
      SRAW type 6    -> hold: the frame extends the current instance
      SRAW type 2    -> new instance, same pixels (exposure break)
      SRAW type 5    -> new empty instance
    The first frame of the layer is given by LRHD (bytes 4-7).
    """
    frames = layer_images(layer, width, height)
    start, end, count = struct.unpack('>3I', layer['lrhd'][4:16])
    if count == 0:
        # image layer (not animated): a single image shown from start to end
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
    """Index (0..n-1) of the layer frame shown at relative position rel,
    outside the layer's range, according to the behaviour; None if nothing is shown."""
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
    """For each frame of the shot, the index of the layer frame shown (or None).
    Applies pre/post behaviours (LRHD: u16 @52 = pre, u16 @44 = post)."""
    start, end, count = struct.unpack('>3I', layer['lrhd'][4:16])
    pre = BEHAVIORS.get(struct.unpack('>H', layer['lrhd'][52:54])[0], 'none')
    post = BEHAVIORS.get(struct.unpack('>H', layer['lrhd'][44:46])[0], 'none')
    n = count or 1
    out = []
    for f in range(total_frames):
        rel = f - start
        if count == 0:                       # image layer: visible from start to end
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
    """Project header: u32 number of pairs at 0x30, then key/value pairs
    (u16 length + UTF-16 BE)."""
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
    """Known fields of the LRHD chunk (104 bytes)."""
    pre = BEHAVIORS.get(struct.unpack('>H', lrhd[52:54])[0], 'unknown')
    post = BEHAVIORS.get(struct.unpack('>H', lrhd[44:46])[0], 'unknown')
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
        'blending': BLEND_MODES.get(blend, f'unknown_{blend}'),
        'pre_behavior': pre,
        'post_behavior': post,
    }

if __name__ == '__main__':
    # usage: python proto.py file.tvpp [out_folder]
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
        print(f"layer {li+1} '{l['name']}': {props}")
        for t in timing:
            print(f"    frames {t['start']}-{t['start'] + t['duration'] - 1}: {t['image']}")
    if out:
        with open(os.path.join(out, 'structure.json'), 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=2, ensure_ascii=False)
