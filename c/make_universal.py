"""Assemble un binaire macOS universel (x86_64 + arm64) sans lipo : python3 make_universal.py sortie x86_64 arm64"""
import struct, sys
out, *parts = sys.argv[1:]
blobs = []
for p in parts:
    d = open(p, 'rb').read()
    cpu, sub = struct.unpack('<2I', d[4:12])
    blobs.append((d, cpu, sub, 14 if cpu == 0x0100000C else 12))
entries, place, off = b'', [], 0x4000
for d, cpu, sub, al in blobs:
    a = 1 << al
    off = (off + a - 1) // a * a
    entries += struct.pack('>5I', cpu, sub, off, len(d), al)
    place.append((off, d))
    off += len(d)
buf = bytearray(off)
buf[:8 + len(entries)] = struct.pack('>2I', 0xCAFEBABE, len(blobs)) + entries
for o, d in place:
    buf[o:o + len(d)] = d
open(out, 'wb').write(buf)
import os as _o; _o.chmod(out, 0o755)
