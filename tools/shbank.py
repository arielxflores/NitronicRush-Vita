#!/usr/bin/env python3
"""Packs the game's compiled shader programs into vita/shbank.bin, the built-in programs (see vita/gfx.c: bank_open).

    tools/shbank.py [--all] CACHE_DIR LABEL [OUT]

CACHE_DIR: a copy of a Vita's ux0:data/nitronic/cache (its *.prg files and used.txt). Every program that runs of the
build LABEL built (used.txt's lines "LABEL hash name kind") goes in; programs of other builds are left out, and so
are the ones only switch files build (kind "cg": the half-precision ones; "opts": other compiler settings) unless
--all is given. The programs
come from the Vita's own shader compiler (what glGetProgramBinary gave); nothing here compiles anything.
OUT defaults to vita/shbank.bin, which CMakeLists.txt puts in the VPK when it is there. Make it again after any
change to a shader or to vitaGL: a program missing from it is compiled at start-up as before, and logged.
"""
import os, struct, sys

def main():
    args = [a for a in sys.argv[1:] if a != '--all']; every = len(args) < len(sys.argv) - 1
    if len(args) < 2:
        print(__doc__); return 1
    cache, label = args[0], args[1]
    out = args[2] if len(args) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'vita', 'shbank.bin')
    want = {}
    with open(os.path.join(cache, 'used.txt'), encoding='utf-8', errors='replace') as f:
        for line in f:
            p = line.split()
            if len(p) >= 3 and p[0] == label and (every or len(p) < 4 or p[3] == 'plain'):
                want[int(p[1], 16)] = p[2]
    if not want:
        print(f'no programs of the build {label} in used.txt'); return 1
    ents, blobs, missing, off = [], [], [], 0
    for h in sorted(want):
        path = os.path.join(cache, f'{h:08x}.prg')
        if not os.path.exists(path):
            missing.append(f'{want[h]} ({h:08x})'); continue
        b = open(path, 'rb').read()
        fmt, n = struct.unpack_from('<II', b, 0)
        if n <= 0 or 8 + n > len(b):
            missing.append(f'{want[h]} ({h:08x}: a short file)'); continue
        ents.append((h, fmt, n)); blobs.append(b[8:8 + n])
    head = 12 + 16 * len(ents)
    data = bytearray(b'NRSB' + struct.pack('<II', 1, len(ents)))
    for (h, fmt, n), b in zip(ents, blobs):
        data += struct.pack('<IIII', h, fmt, head + off, n); off += (n + 15) & ~15
    for b in blobs:
        data += b + bytes(((len(b) + 15) & ~15) - len(b))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    open(out, 'wb').write(data)
    print(f'{out}: {len(ents)} programs, {len(data) / 1024:.0f} KB (build {label})')
    for m in missing: print('  missing:', m)
    return 0 if not missing else 2

if __name__ == '__main__':
    sys.exit(main())
