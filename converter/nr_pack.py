#!/usr/bin/env python3
"""nr_pack: turn an installed copy of Nitronic Rush into the data pack the Vita engine loads.

    python3 nr_pack.py "<game dir>" <out dir> [--cap 512] [--levels a.xml,b.xml]

Output: <out dir>/nitronic.idx plus the packs it lists (all go to ux0:data/nitronic/), holding
    levels/<Level>.nrl    level geometry pre-merged into world-space chunks per material
    textures/<name>.nrt   textures as DXT1/DXT5 with mipmaps, power-of-two, capped size
    levels.txt            level list (campaign order first)
The loose files are kept in <out dir>/work/.
Nothing here is shipped with the project; it is all generated from your own install.
"""
import nr_ci  # noqa: F401  (case-insensitive file names off Windows: first)
import os, sys, struct, math, glob, argparse, re
import xml.etree.ElementTree as ET
import numpy as np
from PIL import Image
import nr_ui
import nr_objs

import nr_light
import nr_fx
# vertex: pos, uv, normal (snorm8 x3 + pad), colour (rgb 0..1, a = glow strength 0..32 as a/32),
# light (baked level lights, rgb 0..4 as v/4)
VDT = np.dtype([('p', '<f4', 3), ('uv', '<f4', 2), ('n', 'i1', 4), ('c', 'u1', 4), ('l', 'u1', 4)])
assert VDT.itemsize == 32
# material shading kinds (which of the PC game's shaders the material is drawn with)
KIND_LIT, KIND_TV, KIND_SKY, KIND_ADD, KIND_REF, KIND_UNLIT = range(6)
MF_SKY, MF_EMIT, MF_NODIFF, MF_GLOSS_EMITMASK, MF_PULSE, MF_EMIT_COLOUR = 1, 2, 4, 8, 16, 32

# additive variants (the pad byte of a material): plain, glass.fx, traffic.fx, jet_flame.fx
ADD_PLAIN, ADD_GLASS, ADD_TRAFFIC, ADD_FLAME, ADD_GROUND = range(5)
def add_variant(m):
    a = next((sh.lower() for p, sh in m['passes'] if p == 'Additive'), '')
    return (ADD_GLASS if a.startswith('glass') else ADD_TRAFFIC if a.startswith('traffic') else ADD_FLAME if a.startswith('jet_flame')
            else ADD_GROUND if a.startswith('groundlaser') else ADD_PLAIN)

def classify(m):
    """-> (kind, gloss, flags) from a material's passes"""
    passes = [p[0] for p in m['passes']]; shaders = [p[1].lower() for p in m['passes']]
    shaded = next((sh.lower() for p, sh in m['passes'] if p == 'Shaded'), '')
    if any('skybox' in x for x in shaders): return KIND_SKY, 0.0, MF_SKY
    if shaded.startswith('simple_emit'): return KIND_LIT, 0.0, MF_EMIT_COLOUR | MF_NODIFF   # Color1 x intensity, nothing else
    if 'Additive' in passes: return KIND_ADD, 0.0, 0
    if shaded.startswith('tvscreen') or shaded.startswith('splashscreen'): return KIND_TV, 1.0, 0
    if 'ref' in shaded or 'corecorecore' in shaded: return KIND_REF, 1.0, (MF_PULSE if 'pulse' in shaded else 0)
    if shaded.startswith('building'): return KIND_LIT, 0.6, MF_GLOSS_EMITMASK
    if shaded: return KIND_LIT, 1.0, 0
    return KIND_UNLIT, 0.0, 0

def vcolour(rgba):
    """(n,4) float colours (rgb tint, a = glow strength) -> u8 vertex colour"""
    c = np.zeros((len(rgba), 4), np.uint8)
    c[:, :3] = np.clip(np.rint(np.clip(rgba[:, :3], 0, 1) * 255), 0, 255)
    c[:, 3] = np.clip(np.rint(np.clip(rgba[:, 3], 0, 32) / 32 * 255), 0, 255)
    return c

def write_nrl(path, texlist, matlist, flat, route=(), start=(0, 0, 0), light=None):
    """NRL3: flat = [(chunk kind, material, vertices (VDT), indices u16)],
    matlist = [(diffuse, emit, detail, reflect, blend, kind, flags, variant, gloss[, anim, fx, c2 (4), p (4)])]"""
    L = np.zeros(24, np.float32)
    if light is not None: L[:len(light)] = light
    with open(path, 'wb') as f:
        f.write(b'NRL3' + struct.pack('<5I', 3, len(texlist), len(matlist), len(flat), len(route)))
        f.write(struct.pack('<3f', *[float(x) for x in start])); f.write(L.tobytes())
        for tn, alpha in texlist: f.write(tn.encode()[:63].ljust(64, b'\0'))
        for m in matlist:
            m = tuple(m) + (-1, 0, (0, 0, 0, 0), (0, 0, 0, 0))[len(m) - 9:] if len(m) < 13 else tuple(m)
            f.write(struct.pack('<hhhhBBBBf', *m[:9]))
            f.write(struct.pack('<hBB4f4f', m[9], m[10], 0, *[float(x) for x in m[11]], *[float(x) for x in m[12]]))
        for kind, mi, vv, ii in flat:
            p = vv['p']
            f.write(struct.pack('<HHII6f', mi, kind, len(vv), len(ii), *p.min(0), *p.max(0)))
        for kind, mi, vv, ii in flat:
            f.write(vv.tobytes()); f.write(ii.tobytes())
            if len(ii) & 1: f.write(b'\0\0')
        for q in route: f.write(struct.pack('<3f', *[float(x) for x in q]))

# ------------------------------------------------------------------ meshes
def load_msh(path):
    d = open(path, 'rb').read()
    n = struct.unpack_from('<I', d, 0)[0]
    assert d[4:4 + 2 * n] == b'p3n4ccu2t3b3', path
    o = 4 + 2 * n + 1
    nsub = struct.unpack_from('<I', d, o)[0]; o += 4
    bounds = struct.unpack_from('<%dI' % (nsub + 1), d, o); o += 4 * (nsub + 1)
    nv = struct.unpack_from('<I', d, o)[0]; o += 4
    ni = bounds[-1]
    idx = np.frombuffer(d, '<u2', ni, o); o += ni * 2
    vt = np.dtype([('p', '<f4', 3), ('n', '<f4', 4), ('c', 'u1', 4), ('uv', '<f4', 2), ('t', '<f4', 3), ('b', '<f4', 3)])
    v = np.frombuffer(d, vt, nv, o)
    subs = [(bounds[i], bounds[i + 1]) for i in range(nsub)]
    return v, idx, subs

# ------------------------------------------------------------------ xml helpers
def floats(el):
    return [float(f.get('value')) for f in el.iter('Float')]

def named(comp, tag, name):
    for e in comp.findall(tag):
        if e.get('name') == name: return e
    return None

def read_rendermesh(rm):
    """-> mesh, {slot: material}, {n: ColorN}, {n: FloatN}, shown in play (Play attribute)"""
    mesh = None; mats = {}; cols = {}; fl = {}
    if rm is None: return mesh, mats, cols, fl, True
    for e in rm:
        nm = e.get('name') or ''
        if e.tag == 'String' and nm == 'Mesh': mesh = (e.text or '').strip()
        elif e.tag == 'String' and nm.startswith('Material'):
            try: mats[int(nm[8:])] = (e.text or '').strip()
            except ValueError: pass
        elif e.tag == 'Vector4' and nm.startswith('Color'):
            try: cols[int(nm[5:])] = floats(e)
            except ValueError: pass
        elif e.tag == 'Float' and nm.startswith('Float'):
            try: fl[int(nm[5:])] = float(e.get('value'))
            except (ValueError, TypeError): pass
    return mesh, mats, cols, fl, rm.get('Play') != '0'     # Play="0": an editor helper, never drawn in the game

ROADDATA = {}
def roaddata(game, name):
    """RoadData end points of a road piece blueprint: [(local pos, local outward direction)]"""
    if name in ROADDATA: return ROADDATA[name]
    eps = []
    try:
        r = ET.parse(os.path.join(game, 'Objects', name + '.xml')).getroot()
        rd = r.find('GameObjectBlueprint/Components/RoadData')
        if rd is not None:
            n = int([e.get('value') for e in rd if e.get('name') == 'EndPointCount'][0])
            for i in range(1, n + 1):
                ori = pos = None
                for e in rd:
                    if e.get('name') == 'Ori%d' % i: ori = np.array(floats(e), np.float32).reshape(3, 3)
                    if e.get('name') == 'Pos%d' % i: pos = np.array(floats(e), np.float32)
                if ori is not None and pos is not None: eps.append((pos, ori @ np.array([0, 1, 0], np.float32)))
    except Exception: pass
    ROADDATA[name] = eps
    return eps

def chain_route(pieces, start, gap=1500.0):
    """Follow the track: pieces = [(endpoints [(world pos, outward dir)], top-surface triangles)].
    Walk from the piece nearest the start, leaving each piece by its other end point and entering
    the piece whose end point touches it; across jumps, take the nearest piece ahead.
    Returns a list of points on the driving surface."""
    if not pieces: return []
    eps = [(pi, ei, e[0], e[1]) for pi, pc in enumerate(pieces) for ei, e in enumerate(pc[0])]
    E = np.array([e[2] for e in eps], np.float32)
    start = np.array(start, np.float32)
    used = np.zeros(len(pieces), bool)
    # the start piece: the end point closest to the start position
    k = int(np.linalg.norm(E - start, axis=1).argmin())
    cur, entry = eps[k][0], eps[k][1]
    pts = []
    for _ in range(len(pieces)):
        used[cur] = True
        ends = pieces[cur][0]
        a = ends[entry]
        if len(ends) == 1: b = a; exit_ = entry
        else:
            exit_ = max(range(len(ends)), key=lambda j: np.linalg.norm(ends[j][0] - a[0]) if j != entry else -1)
            b = ends[exit_]
        # a smooth curve from the entry to the exit point, following the end directions
        L = float(np.linalg.norm(b[0] - a[0]))
        n = max(2, int(L / 8))
        c1 = a[0] - a[1] * L / 3; c2 = b[0] - b[1] * L / 3
        for t in np.linspace(0, 1, n, endpoint=False):
            q = (1 - t) ** 3 * a[0] + 3 * (1 - t) ** 2 * t * c1 + 3 * (1 - t) * t ** 2 * c2 + t ** 3 * b[0]
            pts.append(surface(q, pieces[cur][1]))
        if len(ends) == 1 and len(pts) > 1: pass
        # next piece: an unused piece with an end point touching our exit point
        d = np.linalg.norm(E - b[0], axis=1)
        cand = [i for i in np.argsort(d)[:8] if d[i] < 2.5 and not used[eps[i][0]]]
        if not cand:
            # a jump or gap: nearest unused end point ahead of us
            dv = E - b[0]; dist = np.linalg.norm(dv, axis=1)
            ahead = (dv @ b[1]) / np.maximum(dist, 1e-3)
            score = dist * (1 + 1.5 * (1 - ahead))
            score[[used[e[0]] for e in eps]] = 1e12
            i = int(score.argmin())
            if score[i] >= 1e12 or dist[i] > gap: pts.append(surface(b[0], pieces[cur][1])); break
            cand = [i]
        cur, entry = eps[cand[0]][0], eps[cand[0]][1]
    return pts

def surface(q, tris):
    """put q on the top surface of the given triangles (closest surface above or below it)"""
    q = np.array(q, np.float32)
    if tris is None or not len(tris): return q
    lo = tris.min(1); hi = tris.max(1)
    m = (lo[:, 0] <= q[0]) & (hi[:, 0] >= q[0]) & (lo[:, 1] <= q[1]) & (hi[:, 1] >= q[1])
    best = None
    for a_, b_, c_ in tris[m]:
        v0 = b_[:2] - a_[:2]; v1 = c_[:2] - a_[:2]; v2 = q[:2] - a_[:2]
        den = v0[0] * v1[1] - v1[0] * v0[1]
        if abs(den) < 1e-9: continue
        u = (v2[0] * v1[1] - v1[0] * v2[1]) / den; w = (v0[0] * v2[1] - v2[0] * v0[1]) / den
        if u < -1e-4 or w < -1e-4 or u + w > 1 + 1e-4: continue
        z = a_[2] + u * (b_[2] - a_[2]) + w * (c_[2] - a_[2])
        if abs(z - q[2]) < 12 and (best is None or abs(z - q[2]) < abs(best - q[2])): best = z
    if best is not None: q = q.copy(); q[2] = best
    return q

def read_collider(comps):
    """(mdc file, material) of a ColliderTriMesh component, or None"""
    c = comps.find('ColliderTriMesh')
    if c is None: return None
    f = m = None
    for e in c:
        if e.get('name') == 'File': f = (e.text or '').strip()
        if e.get('name') == 'Material': m = (e.text or '').strip()
    return (f, m) if f else None

MDC = {}
def load_mdc(game, name):
    """collision mesh: u32 version (2004), u32 count, count x {3 corners, normal, u32 flags}, then a tree"""
    if name in MDC: return MDC[name]
    t = None
    try:
        d = open(os.path.join(game, 'CollisionMesh', name), 'rb').read()
        ver, n = struct.unpack_from('<II', d, 0)
        T = np.frombuffer(d, np.dtype([('v', '<f4', (3, 3)), ('n', '<f4', 3), ('k', '<u4')]), n, 8)
        if np.allclose(np.linalg.norm(T['n'], axis=1), 1, atol=0.02): t = T['v'].copy()
    except Exception as e:
        print('  collision mesh %s: %s' % (name, e))
    MDC[name] = t
    return t

COLL_MATS = nr_objs.COLL
CELL_C = 32.0

def write_collision(path, tris, mats):
    """world collision for the car: triangles + a 2D grid (x, y) of triangle lists"""
    if len(tris) == 0:
        tris = np.zeros((0, 3, 3), np.float32); mats = np.zeros(0, np.uint32)
    lo = tris.min(1) if len(tris) else np.zeros((0, 3)); hi = tris.max(1) if len(tris) else np.zeros((0, 3))
    mn = lo.min(0)[:2] - 1 if len(tris) else np.zeros(2); mx = hi.max(0)[:2] + 1 if len(tris) else np.ones(2)
    nx = int(np.ceil((mx[0] - mn[0]) / CELL_C)) + 1; ny = int(np.ceil((mx[1] - mn[1]) / CELL_C)) + 1
    cells = [[] for _ in range(nx * ny)]
    c0 = np.floor((lo[:, :2] - mn) / CELL_C).astype(int); c1 = np.floor((hi[:, :2] - mn) / CELL_C).astype(int)
    for i in range(len(tris)):
        for cy in range(c0[i, 1], c1[i, 1] + 1):
            row = cy * nx
            for cx in range(c0[i, 0], c1[i, 0] + 1): cells[row + cx].append(i)
    n = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0]) if len(tris) else np.zeros((0, 3))
    n = n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
    rec = np.zeros(len(tris), np.dtype([('v', '<f4', (3, 3)), ('n', '<f4', 3), ('m', '<u4')]))
    rec['v'] = tris; rec['n'] = n; rec['m'] = mats
    starts = np.zeros(nx * ny + 1, np.uint32); flat = []
    for i, c in enumerate(cells): starts[i + 1] = starts[i] + len(c); flat.extend(c)
    with open(path, 'wb') as f:
        f.write(b'NRC1' + struct.pack('<III3f', len(tris), nx, ny, float(mn[0]), float(mn[1]), CELL_C))
        f.write(rec.tobytes()); f.write(starts.astype('<u4').tobytes()); f.write(np.array(flat, '<u4').tobytes())
    return len(tris), nx * ny, len(flat)

BLUEPRINTS = {}
def blueprint(game, name):
    if name in BLUEPRINTS: return BLUEPRINTS[name]
    bp = None
    p = os.path.join(game, 'Objects', name + '.xml')
    if os.path.exists(p):
        try:
            r = ET.parse(p).getroot()
            g = r.find('GameObjectBlueprint')
            comps = g.find('Components')
            mesh, mats, cols, fl, play = read_rendermesh(comps.find('RenderMesh'))
            bp = dict(mesh=mesh, mats=mats, cols=cols, fl=fl, play=play, comps=[c.tag for c in comps], collider=read_collider(comps))
        except Exception as e:
            print('  blueprint %s unreadable: %s' % (name, e))
    BLUEPRINTS[name] = bp
    return bp

# ------------------------------------------------------------------ materials
MATS = {}
def material(game, name, depth=0):
    if name in MATS: return MATS[name]
    m = dict(passes=[], tex={})
    p = os.path.join(game, 'Materials', name + '.txt')
    if os.path.exists(p) and depth < 8:
        for line in open(p, errors='replace').read().splitlines():
            t = line.split()
            if len(t) >= 3 and t[0] == 'LOAD' and t[1] == 'MATERIAL':
                base = material(game, t[2], depth + 1)
                m['passes'] = list(base['passes']); m['tex'].update(base['tex'])
            elif len(t) >= 3 and t[0] == 'PASS':
                m['passes'].append((t[1], t[2]))
            elif len(t) >= 4 and t[0] == 'PARAM' and t[1] == 'TEXTURE':
                m['tex'][t[2]] = t[3]
    MATS[name] = m
    return m

# ------------------------------------------------------------------ textures
TEXINDEX = {}
def index_textures(game):
    for p in glob.glob(os.path.join(game, 'Textures', '**', '*'), recursive=True):
        if os.path.isfile(p): TEXINDEX.setdefault(os.path.basename(p).lower(), p)

def tex_out_name(src):
    return re.sub(r'[^a-z0-9_]', '_', src.lower())

def pot(x, cap):
    p = 1 << max(2, int(round(math.log2(max(x, 4)))))
    return max(4, min(p, cap))

def dxt_color(blk):
    """blk: (N,16,3) float -> (N,) c0, c1, indices for 4-colour blocks."""
    mean = blk.mean(1)
    c = blk - mean[:, None]
    cov = np.einsum('npi,npj->nij', c, c)
    v = np.ones((len(blk), 3), np.float32) / math.sqrt(3)
    for _ in range(6):
        v2 = np.einsum('nij,nj->ni', cov, v)
        l = np.linalg.norm(v2, axis=1, keepdims=True)
        v = np.where(l > 1e-6, v2 / np.maximum(l, 1e-12), v)
    proj = np.einsum('npi,ni->np', c, v)
    e0 = np.clip(mean + v * proj.max(1)[:, None], 0, 255)
    e1 = np.clip(mean + v * proj.min(1)[:, None], 0, 255)
    def q(e):
        r = np.rint(e[:, 0] * 31 / 255).astype(np.uint32)
        g = np.rint(e[:, 1] * 63 / 255).astype(np.uint32)
        b = np.rint(e[:, 2] * 31 / 255).astype(np.uint32)
        return (r << 11) | (g << 5) | b
    c0, c1 = q(e0), q(e1)
    swap = c0 < c1
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    def deq(c):
        r = (c >> 11) & 31; g = (c >> 5) & 63; b = c & 31
        return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], 1).astype(np.float32)
    p0, p1 = deq(c0), deq(c1)
    pal = np.stack([p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3], 1)          # (N,4,3)
    d = ((blk[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(-1)               # (N,16,4)
    idx = d.argmin(2).astype(np.uint32)
    idx[c0 == c1] = 0
    bits = np.zeros(len(blk), np.uint32)
    for i in range(16): bits |= idx[:, i] << (2 * i)
    return c0.astype('<u2'), c1.astype('<u2'), bits.astype('<u4')

def dxt_alpha(a):
    a0 = a.max(1); a1 = a.min(1)
    f0 = a0.astype(np.float32); f1 = a1.astype(np.float32)
    pal = np.stack([f0, f1] + [((7 - k) * f0 + k * f1) / 7 for k in range(1, 7)], 1)  # (N,8)
    idx = np.abs(a[:, :, None].astype(np.float32) - pal[:, None, :]).argmin(2).astype(np.uint64)
    idx[a0 == a1] = 0
    bits = np.zeros(len(a), np.uint64)
    for i in range(16): bits |= idx[:, i] << np.uint64(3 * i)
    out = np.zeros((len(a), 8), np.uint8)
    out[:, 0] = a0; out[:, 1] = a1
    for k in range(6): out[:, 2 + k] = ((bits >> np.uint64(8 * k)) & np.uint64(255)).astype(np.uint8)
    return out

def encode_dxt(img, alpha):
    a = np.asarray(img.convert('RGBA'), np.uint8)
    h, w = a.shape[:2]
    blk = a.reshape(h // 4, 4, w // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4)
    c0, c1, bits = dxt_color(blk[:, :, :3].astype(np.float32))
    col = np.zeros((len(blk), 8), np.uint8)
    col[:, 0:2] = c0.view(np.uint8).reshape(-1, 2); col[:, 2:4] = c1.view(np.uint8).reshape(-1, 2)
    col[:, 4:8] = bits.view(np.uint8).reshape(-1, 4)
    if not alpha: return col.tobytes()
    return np.concatenate([dxt_alpha(blk[:, :, 3]), col], 1).tobytes()

TEXDONE = {}
SRCALPHA = {}
def source_has_alpha(p):
    if p not in SRCALPHA:
        try:
            im = Image.open(p); im.load()
            SRCALPHA[p] = im.mode in ('RGBA', 'LA', 'P') and (np.asarray(im.convert('RGBA'))[:, :, 3] < 250).mean() > 0.002
        except Exception:
            SRCALPHA[p] = False
    return SRCALPHA[p]

def convert_texture(src, outdir, cap, want_alpha=False):
    """returns (out name, has_alpha) or None. The alpha channel is kept (DXT5) when the caller
    needs it (a diffuse map's alpha is its gloss / reflection mask) and the image has one."""
    key = src.lower()
    if key in TEXDONE and (TEXDONE[key] is None or TEXDONE[key][1] or not want_alpha): return TEXDONE[key]
    res = None
    p = TEXINDEX.get(os.path.basename(key))
    if p:
        done = os.path.join(outdir, 'textures', tex_out_name(os.path.basename(p)) + '.nrt')
        alpha = bool(want_alpha and source_has_alpha(p))
        if os.path.exists(done) and os.path.getsize(done) > 12:      # resume: already converted
            hdr = open(done, 'rb').read(12)
            has = bool(struct.unpack_from('<H', hdr, 10)[0] & 1)
            if has or not alpha:
                res = (tex_out_name(os.path.basename(p)), has)
                TEXDONE[key] = res
                return res
        try:
            im = Image.open(p); im.load()
            im = im.convert('RGBA')
            w, h = pot(im.width, cap), pot(im.height, cap)
            im = im.resize((w, h), Image.LANCZOS) if (w, h) != im.size else im
            name = tex_out_name(os.path.basename(p))
            mips = []
            while True:
                mips.append(encode_dxt(im, alpha))
                if im.width <= 4 or im.height <= 4: break
                im = im.resize((max(4, im.width // 2), max(4, im.height // 2)), Image.BOX)
            with open(done + '.tmp', 'wb') as f:
                f.write(b'NRT1' + struct.pack('<HHBBH', w, h, 5 if alpha else 1, len(mips), 1 if alpha else 0))
                for m in mips: f.write(m)
            os.replace(done + '.tmp', done)
            res = (name, bool(alpha))
        except Exception as e:
            print('  texture %s failed: %s' % (p, e))
    else:
        print('  texture %s not found' % src)
    TEXDONE[key] = res
    return res

# ------------------------------------------------------------------ levels
CELL = 192.0
MAXV = 60000

def sky_average(game, skymats, tint):
    """the sky's average colour near the horizon (what far geometry fades into)"""
    cols = []
    for m_, texlist in skymats:
        ti = m_[1] if m_[1] >= 0 else m_[0]
        if ti < 0: continue
        p = None
        for k, v in TEXINDEX.items():
            if tex_out_name(k) == texlist[ti][0]: p = v; break
        if not p: continue
        try:
            a = np.asarray(Image.open(p).convert('RGB'), np.float32) / 255.0
            h = a.shape[0]
            cols.append(a[h // 3: 2 * h // 3].reshape(-1, 3).mean(0))
        except Exception:
            pass
    c = np.mean(cols, 0) if cols else np.array([0.05, 0.05, 0.1])
    t = (list(tint) + [1, 1, 1, 1])[:4]
    return (c * np.array(t[:3]) * t[3] * 2).tolist()

def pack_level(game, xml, outdir, cap, stats):
    root = ET.parse(xml).getroot()
    objs = []; start = None; road = []; colltris = []; collmats = []; finish = []; checkpoints = []; finishbox = []
    # objects the Vita animates (nr_objs): not baked. The menu's backdrop keeps everything baked
    dyn = nr_objs.classify(game, root) if 'an_menu_level' not in os.path.basename(xml) else {}
    dyn_id = {o: k + 1 for k, o in enumerate(sorted(dyn))}
    layer_of = {}
    for layer in root.iter('LevelLayer'):
        for g in layer.iter('GameObject'): layer_of.setdefault(g, layer.get('name') or '')
    if True:
        for ordinal, g in enumerate(root.iter('GameObject')):
            lname = layer_of.get(g, '')
            bpn = g.get('blueprint') or ''
            comps = g.find('Components')
            if comps is None: continue
            bp = blueprint(game, bpn)
            tags = [c.tag for c in comps]
            t = comps.find('Transform')
            pos, scl, ori = [0, 0, 0], [1, 1, 1], [1, 0, 0, 0, 1, 0, 0, 0, 1]
            if t is not None:
                e = named(t, 'Vector3', 'position'); pos = floats(e) if e is not None else pos
                e = named(t, 'Vector3', 'scale'); scl = floats(e) if e is not None else scl
                e = named(t, 'Matrix3x3', 'orientation'); ori = floats(e) if e is not None else ori
            if 'StartTunnelLogic' in tags or (bp and 'StartTunnelLogic' in bp['comps']): start = pos
            if 'LevelSwitch' in tags or (bp and 'LevelSwitch' in bp['comps']):
                finish.append(pos)
                finishbox.append(list(pos) + list(ori) + list(scl))   # LevelSwitch 0x6a8040: its trigger, in the tunnel's frame
                Rw = np.array(ori, np.float32).reshape(3, 3)
                for p_, d_ in roaddata(game, bpn):     # where the road enters the finish tunnel
                    finish.append((Rw @ (p_ * np.array(scl, np.float32)) + np.array(pos, np.float32)).tolist())
            if 'CheckpointLogic' in tags or (bp and 'CheckpointLogic' in bp['comps']): checkpoints.append(list(pos) + list(ori) + list(scl))
            if 'RoadData' in tags or (bp and 'RoadData' in bp['comps']): road.append(pos)
            kind = dyn.get(ordinal)
            col = nr_objs.collider(game, bpn, comps)
            if col and (kind is None or kind in nr_objs.STATIC_COLL):
                ct = load_mdc(game, col[1]) if col[0] == 'mesh' else (nr_objs.box_tris(col[1]) if col[0] == 'box' else None)
                if ct is not None and len(ct):
                    Rw = np.array(ori, np.float32).reshape(3, 3) * np.array(scl, np.float32)[None, :]
                    w = ct @ Rw.T + np.array(pos, np.float32)
                    if np.linalg.det(Rw) < 0: w = w[:, ::-1]
                    names_ = nr_objs.comp_names(game, bpn, comps)
                    mid = int(nr_objs._f(nr_objs.merged(game, bpn, comps, 'Material').get('Material ID', 0))) if 'Material' in names_ else 0
                    colltris.append(w); collmats.append(np.full(len(w), nr_objs.tri_mat(COLL_MATS.get(col[2], 0), mid, dyn_id.get(ordinal, 0)), np.uint32))
            if kind is not None and kind not in nr_objs.BAKE_MESH: continue
            rme = comps.find('RenderMesh')
            mesh, mats, cols, fl, play = read_rendermesh(rme)
            if bp:
                mesh = mesh or bp['mesh']
                mats = {**bp['mats'], **mats}; cols = {**bp['cols'], **cols}; fl = {**bp['fl'], **fl}
                # an instance's RenderMesh without a Play attribute keeps the blueprint's: the trigger
                # zones' boxes (Play="0") are editor helpers, never drawn in the game
                if rme is None or rme.get('Play') is None: play = bp['play']
            if not mesh or not play: continue
            isroad = 'RoadData' in tags or bool(bp and 'RoadData' in bp['comps'])
            pulse = 0.0
            if 'LightPulse' in nr_objs.comp_names(game, bpn, comps):
                pulse = nr_objs._f(nr_objs.merged(game, bpn, comps, 'LightPulse').get('Pulse Intensity', 1), 1)
            objs.append(dict(bp=bpn, layer=lname, pos=pos, scl=scl, ori=ori, mesh=mesh, mats=mats, cols=cols, fl=fl, road=isroad, pulse=pulse))
            if isroad:
                Rw = np.array(ori, np.float32).reshape(3, 3)
                ends = [(Rw @ (p_ * np.array(scl, np.float32)) + np.array(pos, np.float32), Rw @ d_ / max(np.linalg.norm(Rw @ d_), 1e-6))
                        for p_, d_ in roaddata(game, bpn)]
                objs[-1]['ends'] = ends

    # materials -> textures
    matlist = []; matidx = {}; texlist = []; texidx = {}
    def tex(src, want_alpha=False):
        if not src: return -1
        r = convert_texture(src, outdir, cap, want_alpha)
        if not r: return -1
        if r[0] not in texidx: texidx[r[0]] = len(texlist); texlist.append(r)
        else: texlist[texidx[r[0]]] = r
        return texidx[r[0]]
    def mat(name, sky, face=None, fxd=None):
        key = (name, sky, face, nr_fx.key(fxd) if fxd else None)
        if key in matidx: return matidx[key]
        m = material(game, name)
        kind, gloss, flags = classify(m)
        if fxd and fxd['kind'] is not None: kind = fxd['kind']
        if fxd: flags |= fxd['flags']
        if sky: kind, flags = KIND_SKY, flags | MF_SKY
        emit = m['tex'].get('Emit1')
        if face and emit:   # sky boxes name one face (..._north / ..._n); the shader picks the others
            words = dict(n='north', s='south', e='east', w='west', t='top', b='bottom')
            low = emit.lower()
            if 'north' in low: emit = emit[:low.index('north')] + words[face] + emit[low.index('north') + 5:]
            elif re.search(r'_n\.', low): emit = re.sub(r'_[nN]\.', '_%s.' % face, emit)
        d = tex(m['tex'].get('Diffuse1'), kind in (KIND_LIT, KIND_REF, KIND_TV)); e = tex(emit, True)
        r = tex((fxd and fxd['reflect']) or m['tex'].get('Reflect1')) if kind == KIND_REF else -1
        an = tex(fxd['anim'], True) if fxd and fxd['anim'] else -1
        blend = 2 if kind == KIND_ADD else 0
        if e >= 0: flags |= MF_EMIT
        if d < 0: flags |= MF_NODIFF
        if not m['passes'] and d < 0 and e < 0: stats['unknown_mats'].add(name)
        matidx[key] = len(matlist)
        matlist.append((d, e, -1, r, blend, kind, flags, add_variant(m) if kind == KIND_ADD else 0, gloss,
                        an, fxd['fx'] if fxd else 0, fxd['c2'] if fxd else (0, 0, 0, 0), fxd['p'] if fxd else (0, 0, 0, 0)))
        return matidx[key]
    sun, lights = nr_light.level_lighting(game, root)
    lights_s = [L for L in lights if not L.get('pulse')]; lights_p = [L for L in lights if L.get('pulse')]

    chunks = {}   # (kind, cx, cy, mat) -> [verts list, idx list, nv]
    roadtris = []; roadpieces = []
    meshcache = {}
    for o in objs:
        if o['mesh'] not in meshcache:
            p = os.path.join(game, 'Meshes', o['mesh'])
            try: meshcache[o['mesh']] = load_msh(p)
            except Exception as ex:
                print('  mesh %s: %s' % (o['mesh'], ex)); meshcache[o['mesh']] = None
        m = meshcache[o['mesh']]
        if m is None: continue
        v, idx, subs = m
        sky = o['bp'].lower().startswith('skybox')
        R = np.array(o['ori'], np.float32).reshape(3, 3)
        S = np.array(o['scl'], np.float32)
        M = R * S[None, :]
        P = np.zeros(3, np.float32) if sky else np.array(o['pos'], np.float32)
        wp = v['p'] @ M.T + P
        flip = np.linalg.det(M) < 0
        if o['road'] and not sky:
            tw = wp[idx.astype(np.int64)].reshape(-1, 3, 3)
            nz = np.cross(tw[:, 1] - tw[:, 0], tw[:, 2] - tw[:, 0])
            nz = nz[:, 2] / np.maximum(np.linalg.norm(nz, axis=1), 1e-9)
            roadtris.append(tw[nz > 0.6])          # upward-facing faces of road pieces (the top surface)
            if o.get('ends'): roadpieces.append((o['ends'], tw[nz > 0.6]))
        nm = v['n'][:, :3] @ (R / np.where(S == 0, 1, S)[None, :]).T
        nm /= np.maximum(np.linalg.norm(nm, axis=1, keepdims=True), 1e-6)
        nm = np.nan_to_num(nm)
        center = wp.mean(0)
        kind = 0 if sky else (2 if o['layer'] == 'Skybox' else 1)    # 0 sky, 1 world, 2 backdrop
        cx, cy = (0, 0) if kind != 1 else (int(math.floor(center[0] / CELL)), int(math.floor(center[1] / CELL)))
        for si, (a, b) in enumerate(subs):
            if b <= a: continue
            slot = si + 1 if (si + 1) in o['mats'] else (max(o['mats']) if o['mats'] else None)
            mname = o['mats'].get(slot, '') if slot else ''
            face = None
            if sky:
                c = v['p'][np.unique(idx[a:b])].mean(0); ax = int(np.abs(c).argmax())
                face = ['e', 'n', 't'][ax] if c[ax] > 0 else ['w', 's', 'b'][ax]
            # Color1 for every submesh, Color2 and Float1..3 for the shaders that use them (re_G)
            c1 = o['cols'].get(1, [1, 1, 1, 1]); c2 = o['cols'].get(2)
            fxd = nr_fx.info(sys.modules[__name__], material(game, mname), c1, c2, [o['fl'].get(k, 0) for k in (1, 2, 3)])
            mi = mat(mname, sky, face, fxd)
            used = np.unique(idx[a:b])
            remap = np.zeros(len(v), np.int64); remap[used] = np.arange(len(used))
            out = np.zeros(len(used), VDT)
            out['p'] = wp[used]; out['uv'] = v['uv'][used]
            out['n'][:, :3] = np.clip(np.rint(nm[used] * 127), -127, 127)
            out['c'] = vcolour(nr_fx.vertex_colours(fxd, c1, v['c'][used], len(used)))
            if fxd['uv'] == 'block':
                no = np.nan_to_num(v['n'][used, :3])
                out['uv'] = nr_fx.block_uv(v['uv'][used], no, o['scl'])
            elif fxd['uv'] == 'tronics':           # world space, a 667 unit tile
                out['uv'] = (wp[used, :2] + 3000.0) * np.array([0.0015, -0.0015], np.float32)
            elif fxd['uv'] == 'city':              # the object's x, y from above; its height and own uv in the colour
                po = v['p'][used]
                out['uv'] = po[:, :2] * np.array([0.5, -0.5], np.float32) + 0.5
                out['c'][:, 0] = np.clip(np.rint(po[:, 2] * 2 * 255), 0, 255)
                out['c'][:, 1:3] = np.clip(np.rint(v['uv'][used] * 255), 0, 255)
                out['c'][:, 3] = 0
            if kind != 0 and matlist[mi][5] in (KIND_LIT, KIND_REF, KIND_TV):
                # the static lights, plus the ones that pulse with the music (LightPulse): their
                # share of the light goes in the normal's 4th byte, the Vita scales it on the beat
                lb = nr_light.bake(wp[used], nm[used], lights_s)
                lp = nr_light.bake(wp[used], nm[used], lights_p) if lights_p else np.zeros_like(lb)
                tot = lb + lp
                out['l'][:, :3] = np.clip(np.rint(tot / 4 * 255), 0, 255)
                frac = lp.sum(1) / np.maximum(tot.sum(1), 1e-6)
                out['n'][:, 3] = np.clip(np.rint(frac * 127), 0, 127)
            if o.get('pulse'):        # its own glow pulses with the music too (Pulse Intensity x 100)
                out['l'][:, 3] = int(min(255, round(o['pulse'] * 100)))
            k = (kind, cx, cy, mi)
            lst = chunks.setdefault(k, [])
            if not lst or lst[-1][2] + len(used) > MAXV: lst.append([[], [], 0])
            c = lst[-1]
            tri = remap[idx[a:b]]
            if flip: tri = tri.reshape(-1, 3)[:, ::-1].reshape(-1)   # mirrored object: keep front faces front
            c[0].append(out); c[1].append(tri + c[2]); c[2] += len(used)

    # route through the level for the automatic fly-through: greedy walk over road pieces
    path = []
    if road:
        pts = np.array(road, np.float32)
        cur = np.array(start, np.float32) if start else pts[0]
        left = np.ones(len(pts), bool)
        path.append(cur)
        heading = None
        while left.any() and len(path) < 4000:
            dv = pts - cur
            d = np.linalg.norm(dv, axis=1)
            score = d.copy()
            if heading is not None:   # prefer carrying on in the same direction (jumps leave gaps)
                cosang = (dv @ heading) / np.maximum(d, 1e-3)
                score = d * (1 + 1.5 * (1 - cosang))
            score[~left] = 1e12
            i = int(score.argmin())
            if d[i] > 1500 or score[i] >= 1e12: break
            left[np.linalg.norm(pts - pts[i], axis=1) < 25] = False
            if d[i] > 1: heading = dv[i] / d[i]
            cur = pts[i]; path.append(cur)

    chained = chain_route(roadpieces, start if start else (path[0] if path else [0, 0, 0]))
    if len(chained) > 20:
        path = chained; roadtris = []          # already on the surface
    # put the route on the driving surface: road piece origins are often below or above it
    if path and roadtris:
        T = np.concatenate(roadtris)
        lo = T.min(1); hi = T.max(1)
        fixed = []
        for q in path:
            q = np.array(q, np.float32)
            m = (lo[:, 0] <= q[0]) & (hi[:, 0] >= q[0]) & (lo[:, 1] <= q[1]) & (hi[:, 1] >= q[1])
            best = None
            for a_, b_, c_ in T[m]:
                v0 = b_[:2] - a_[:2]; v1 = c_[:2] - a_[:2]; v2 = q[:2] - a_[:2]
                den = v0[0] * v1[1] - v1[0] * v0[1]
                if abs(den) < 1e-9: continue
                u = (v2[0] * v1[1] - v1[0] * v2[1]) / den; w = (v0[0] * v2[1] - v2[0] * v0[1]) / den
                if u < 0 or w < 0 or u + w > 1: continue
                z = a_[2] + u * (b_[2] - a_[2]) + w * (c_[2] - a_[2])
                if abs(z - q[2]) < 60 and (best is None or abs(z - q[2]) < abs(best - q[2])): best = z
            if best is not None: q[2] = best
            fixed.append(q)
        path = fixed

    name = os.path.splitext(os.path.basename(xml))[0]
    flat = []
    for (kind, cx, cy, mi), lst in sorted(chunks.items()):
        for verts, idxs, nv in lst:
            vv = np.concatenate(verts); ii = np.concatenate(idxs).astype('<u2')
            flat.append((kind, mi, vv, ii))
    st = start if start else (path[0].tolist() if path else [0, 0, 0])
    skyc = sky_average(game, [(m_, texlist) for m_ in matlist if m_[5] == KIND_SKY], sun['sky_tint'])
    light = (list(sun['col']) + [sun['amb'], sun['spec'], 0, 0, 1] + list(sun['fog'][:4]) +
             [sun['fog_near'], sun['fog_far'], sun['sky_far']] + list(skyc) + list(sun['sky_tint'][:4]))
    write_nrl(os.path.join(outdir, 'levels', name + '.nrl'), texlist, matlist, flat, path, st, light)
    with open(os.path.join(outdir, 'levels', name + '.nrg'), 'w') as f:
        f.write('name %s\n' % DISPLAY_NAMES.get(name, name))
        if start: f.write('start %.3f %.3f %.3f\n' % tuple(start))
        for q in finish: f.write('finish %.3f %.3f %.3f\n' % tuple(q))
        for q in finishbox: f.write('finishbox %.3f %.3f %.3f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f\n' % tuple(q[:15]))
        for q in checkpoints: f.write('checkpoint %.3f %.3f %.3f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f\n' % tuple(q[:15]))   # + scale (the gate's size: 0x728070)
        for l in nr_ui.level_extras(game, xml, skip=set(dyn)): f.write(l + '\n')
        for l in nr_objs.Exporter(sys.modules[__name__], game, outdir, cap).level(root, dyn): f.write(l + '\n')
        for l in nr_light.light_lines(game, root): f.write(l + '\n')
    ct = np.concatenate(colltris).astype(np.float32) if colltris else np.zeros((0, 3, 3), np.float32)
    cm = np.concatenate(collmats) if collmats else np.zeros(0, np.uint32)
    cn, ncell, nref = write_collision(os.path.join(outdir, 'levels', name + '.nrc'), ct, cm)
    tris = sum(len(c[3]) for c in flat) // 3
    verts = sum(len(c[2]) for c in flat)
    print(('%-40s objects %5d  chunks %4d  tris %7d  verts %7d  textures %3d  lights %3d  path %4d  collision %6d tris  %.1f MB' % (
        name, len(objs), len(flat), tris, verts, len(texlist), len(lights), len(path), cn,
        os.path.getsize(os.path.join(outdir, 'levels', name + '.nrl')) / 1e6)), flush=True)
    return name

# ------------------------------------------------------------------ the player's car
def blueprint_floats(game, name, comp, field):
    r = ET.parse(os.path.join(game, 'Objects', name + '.xml')).getroot()
    c = r.find('GameObjectBlueprint/Components/' + comp)
    for e in c:
        if e.get('name') == field: return floats(e)
    return None

def pack_car(game, outdir, cap, blueprint_name='Car', out_name='car'):
    """A vehicle blueprint's body mesh plus its four wheels, in car space (x right, y forward,
    z up), as one chunk in the level format. Wheel positions come from ControllerCar's Wheels
    table (10 numbers per wheel, the 2nd-4th are x, y, z); the wheel models, their scale and
    which side is mirrored from CarWheelLogic's Wheels list."""
    bp = blueprint(game, blueprint_name)
    wheels = blueprint_floats(game, blueprint_name, 'ControllerCar', 'Wheels')
    root = ET.parse(os.path.join(game, 'Objects', blueprint_name + '.xml')).getroot()
    models = []
    wl = root.find('GameObjectBlueprint/Components/CarWheelLogic')
    if wl is not None:
        for w in wl.iter('Wheel'):
            d = {e.get('name'): (e.get('value') if e.get('value') is not None else (e.text or '').strip()) for e in w}
            if d.get('Model Name'): models.append((d['Model Name'], d.get('Flip Wheel') == '1'))
    parts = [(bp['mesh'], np.zeros(3, np.float32), 1.0, False)]
    for k in range(len(wheels) // 10):
        w = wheels[k * 10:(k + 1) * 10]
        if k < len(models):
            wbp = blueprint(game, models[k][0])
            t = ET.parse(os.path.join(game, 'Objects', models[k][0] + '.xml')).getroot().find('GameObjectBlueprint/Components/Transform')
            sc = floats(named(t, 'Vector3', 'scale'))[0] if t is not None and named(t, 'Vector3', 'scale') is not None else 1.0
            parts.append((wbp['mesh'], np.array(w[1:4], np.float32), sc, models[k][1]))
        else:
            parts.append(('NewCarWheelFront.msh' if w[2] > 0 else 'NewCarWheelBack.msh', np.array(w[1:4], np.float32), 1.0, False))
    m = material(game, bp['mats'].get(1, 'car'))
    kind, gloss, mflags = classify(m)
    fxd = nr_fx.info(sys.modules[__name__], m, bp['cols'].get(1, [1, 1, 1, 1]), bp['cols'].get(2), [bp['fl'].get(k, 0) for k in (1, 2, 3)])
    mflags |= fxd['flags']
    texlist = []
    def tex(src, want_alpha=False):
        if not src: return -1
        r_ = convert_texture(src, outdir, cap, want_alpha)
        if not r_: return -1
        texlist.append(r_); return len(texlist) - 1
    d = tex(m['tex'].get('Diffuse1'), True); e = tex(m['tex'].get('Emit1'), True)
    rf = tex(m['tex'].get('Reflect1')) if kind == KIND_REF else -1
    col = np.array((bp['cols'].get(1, [1, 1, 1, 1]) + [1, 1, 1, 1])[:4], np.float32)   # emissive tint, a = strength (3)
    verts, idxs, base = [], [], 0
    for mesh, off, sc, flip in parts:
        v, idx, subs = load_msh(os.path.join(game, 'Meshes', mesh))
        out = np.zeros(len(v), VDT)
        p_ = v['p'] * sc
        nrm = np.nan_to_num(v['n'][:, :3]).copy()
        ii_ = idx.astype(np.int64)
        if flip:
            p_ = p_ * np.array([-1, 1, 1], np.float32); nrm[:, 0] = -nrm[:, 0]
            ii_ = ii_.reshape(-1, 3)[:, ::-1].reshape(-1)
        out['p'] = p_ + off; out['uv'] = v['uv']
        out['n'][:, :3] = np.clip(np.rint(nrm * 127), -127, 127)
        out['c'] = vcolour(nr_fx.vertex_colours(fxd, bp['cols'].get(1, [1, 1, 1, 1]), v['c'], len(v)))
        verts.append(out); idxs.append(ii_ + base); base += len(v)
    vv = np.concatenate(verts); ii = np.concatenate(idxs).astype('<u2')
    os.makedirs(os.path.join(outdir, 'models'), exist_ok=True)
    write_nrl(os.path.join(outdir, 'models', out_name + '.nrl'), texlist,
              [(d, e, -1, rf, 0, kind, mflags | (MF_EMIT if e >= 0 else 0), 0, gloss, -1, fxd['fx'], fxd['c2'], fxd['p'])], [(1, 0, vv, ii)])
    print('%s (%s): %d parts, %d triangles, colour %s, textures %s' % (out_name, blueprint_name, len(parts), len(ii) // 3, col.tolist(), [t[0] for t in texlist]))
    # the car's handling numbers, straight from the blueprint, for the Vita's physics
    r = ET.parse(os.path.join(game, 'Objects', blueprint_name + '.xml')).getroot()
    lines = []
    for comp in ('BodyRigid', 'ControllerCar', 'ControllerWings', 'CarLogic'):
        c = r.find('GameObjectBlueprint/Components/' + comp)
        if c is None: continue
        for e in c:
            nm = (e.get('name') or '').replace(' ', '_')
            if e.tag in ('Float', 'Integer', 'Boolean'): lines.append('%s.%s %s' % (comp, nm, e.get('value')))
            elif e.tag in ('Vector3', 'Wheels'): lines.append('%s.%s %s' % (comp, nm, ' '.join('%g' % x for x in floats(e))))
    with open(os.path.join(outdir, 'models', out_name + '.cfg'), 'w') as f: f.write('\n'.join(lines) + '\n')
    sh = r.find('GameObjectBlueprint/Components/ShockNearbyLogic')
    if sh is not None: pack_shock(game, outdir, bp['mesh'], sh, out_name)
    pack_intro(game, outdir, cap, blueprint_name, out_name, m, kind, gloss, mflags, fxd)

FX_INTRO = 8          # nr_data.h NR_FX_INTRO: phong_ref_intro.fx
def keep_top_mip(path):
    """a texture cut to its first level: the triangles' edge lines stay lines far off (mips average them away
    below the shader's 0.3 threshold, and the stand-in, small at the portal, would not glow at all)"""
    d = open(path, 'rb').read()
    w, h, fmt, nm, fl = struct.unpack_from('<HHBBH', d, 4)
    if nm <= 1: return
    size = (max(4, w) // 4) * (max(4, h) // 4) * (8 if fmt == 1 else 16) if fmt in (1, 5) else w * h * 4
    open(path, 'wb').write(b'NRT1' + struct.pack('<HHBBH', w, h, fmt, 1, fl) + d[12:12 + size])
def pack_intro(game, outdir, cap, blueprint_name, out_name, m, kind, gloss, mflags, fxd):
    """CarLogic's Intro Car Object Name (IntroCar, IntroCar_Gold, DeloreanIntro, IntroVan; the Finish Car Object is
    the same mesh and material): the car in loose triangles that fly in from the start tunnel's portal at the
    countdown and out into the end tunnel's at the finish (IntroCarLogic 0x68d0e0, phong_ref_intro.fx). Drawn
    with the vehicle's own material (the intro materials use its textures), the TriangleEdges map as the detail
    texture and the stand-in's Color1 as the edges' glow (c2). The shader's per-vertex hash frac((3.1 + n.x) * 243)
    rides in the light bytes' x (the snorm8 normal would scramble it). -> models/<out>_intro.nrl"""
    c = _comps(game, blueprint_name).get('CarLogic')
    name = _val(c, 'Intro Car Object Name') if c is not None else None
    if not name: return
    ib = blueprint(game, name)
    if not ib or not ib['mesh']: print('intro %s: no mesh' % name); return
    im = material(game, ib['mats'].get(1, ''))
    texlist = []
    def tex(src, want_alpha=False):
        if not src: return -1
        r_ = convert_texture(src, outdir, cap, want_alpha)
        if not r_: return -1
        texlist.append(r_); return len(texlist) - 1
    d = tex(m['tex'].get('Diffuse1'), True); e = tex(m['tex'].get('Emit1'), True)
    rf = tex(m['tex'].get('Reflect1')) if kind == KIND_REF else -1
    edges = tex(im['tex'].get('TriangleEdges'))
    if edges >= 0: keep_top_mip(os.path.join(outdir, 'textures', texlist[edges][0] + '.nrt'))
    col1 = [float(x) for x in (ib['cols'].get(1, [1, 1, 1, 1]) + [1, 1, 1, 1])[:4]]
    v, idx, subs = load_msh(os.path.join(game, 'Meshes', ib['mesh']))
    out = np.zeros(len(v), VDT)
    nrm = np.nan_to_num(v['n'][:, :3]).astype(np.float64)
    out['p'] = v['p']; out['uv'] = v['uv']
    out['n'][:, :3] = np.clip(np.rint(nrm * 127), -127, 127)
    out['c'] = vcolour(nr_fx.vertex_colours(fxd, col1, v['c'], len(v)))
    out['l'][:, 0] = np.clip(np.rint(np.mod((3.1 + nrm[:, 0]) * 243.0, 1.0) * 255), 0, 255)
    ii = idx.astype('<u2')
    write_nrl(os.path.join(outdir, 'models', out_name + '_intro.nrl'), texlist,
              [(d, e, edges, rf, 0, kind, mflags | (MF_EMIT if e >= 0 else 0), 0, gloss, -1, FX_INTRO, col1, fxd['p'])], [(1, 0, out, ii)])
    print('  %s: the warp-in stand-in %s (%s), %d triangles, glow %s, edges %s' % (out_name, name, ib['mesh'], len(ii) // 3, col1,
          texlist[edges][0] if edges >= 0 else '-'))

def pack_shock(game, outdir, mesh, sh, out_name):
    """ShockNearbyLogic (the Commander's sparks, 0x6b5b70): its numbers and the body mesh's points a bolt can
    start from. A bolt starts at the body vertex furthest along its direction (x, 2y, z) with y >= 0 (0x630230,
    0x6b5620), so only the vertices that are furthest along some such direction are kept (models/<car>.shock)"""
    val = {e.get('name'): float(e.get('value')) for e in sh if e.get('value') is not None}
    v, _, _ = load_msh(os.path.join(game, 'Meshes', mesh))
    P = np.asarray(v['p'], np.float64)
    n = 8192; k = np.arange(n) + 0.5
    phi = np.arccos(1 - 2 * k / n); th = np.pi * (1 + 5 ** 0.5) * k
    d = np.stack([np.cos(th) * np.sin(phi), np.abs(np.cos(phi)), np.sin(th) * np.sin(phi)], 1) * np.array([1, 2, 1])
    keep = sorted(set(np.argmax(d @ P.T, axis=1).tolist()))
    lines = ['shock %g %g %g %g' % (val.get('Min Vel', 100), val.get('Max Vel', 150), val.get('Max Spark Frequency', 250), val.get('Max Spark Distance', 25))]
    lines += ['p %.4f %.4f %.4f' % tuple(P[i]) for i in keep]
    with open(os.path.join(outdir, 'models', out_name + '.shock'), 'w') as f: f.write('\n'.join(lines) + '\n')
    print('  %s: sparks from %d of the body\'s %d points' % (out_name, len(keep), len(P)))

def pack_model(game, outdir, cap, blueprint_name, out_name):
    """one blueprint's mesh as a model (e.g. the jet flame drawn behind the boosters)"""
    bp = blueprint(game, blueprint_name)
    if not bp or not bp['mesh']: print('model %s: no mesh' % blueprint_name); return
    col = np.array((bp['cols'].get(1, [1, 1, 1, 1]) + [1, 1, 1, 1])[:4], np.float32)
    m = material(game, bp['mats'].get(1, ''))
    kind, gloss, mflags = classify(m)
    if not m['passes']: kind = KIND_ADD                  # no material: draw it as glow
    fxd = nr_fx.info(sys.modules[__name__], m, bp['cols'].get(1, [1, 1, 1, 1]), bp['cols'].get(2), [bp['fl'].get(k, 0) for k in (1, 2, 3)])
    if fxd['kind'] is not None: kind = fxd['kind']
    mflags |= fxd['flags']
    texlist = []
    def tex(src, want_alpha=False):
        if not src: return -1
        r_ = convert_texture(src, outdir, cap, want_alpha)
        if not r_: return -1
        texlist.append(r_); return len(texlist) - 1
    d = tex(m['tex'].get('Diffuse1'), kind in (KIND_LIT, KIND_REF, KIND_TV)); e = tex(m['tex'].get('Emit1'), True)
    rf = tex(fxd['reflect'] or m['tex'].get('Reflect1')) if kind == KIND_REF else -1
    an = tex(fxd['anim'], True) if fxd['anim'] else -1
    blend = 2 if kind == KIND_ADD else 0
    v, idx, subs = load_msh(os.path.join(game, 'Meshes', bp['mesh']))
    out = np.zeros(len(v), VDT)
    out['p'] = v['p']; out['uv'] = v['uv']
    out['n'][:, :3] = np.clip(np.rint(np.nan_to_num(v['n'][:, :3]) * 127), -127, 127)
    out['c'] = vcolour(nr_fx.vertex_colours(fxd, bp['cols'].get(1, [1, 1, 1, 1]), v['c'], len(v)))
    ii = idx.astype('<u2')
    write_nrl(os.path.join(outdir, 'models', out_name + '.nrl'), texlist,
              [(d, e, -1, rf, blend, kind, mflags | (MF_EMIT if e >= 0 else 0) | (MF_NODIFF if d < 0 else 0), add_variant(m) if kind == KIND_ADD else 0, gloss,
                an, fxd['fx'], fxd['c2'], fxd['p'])], [(1, 0, out, ii)])
    print('model %s: %d triangles, blend %d, textures %s' % (out_name, len(ii) // 3, blend, [t[0] for t in texlist]))

def _comps(game, name):
    r = ET.parse(os.path.join(game, 'Objects', name + '.xml')).getroot()
    return {c.tag: c for c in r.find('GameObjectBlueprint/Components')}
def _val(c, name, d=None):
    if c is None: return d
    for e in c:
        if e.get('name') == name:
            if e.tag in ('Vector3', 'Matrix3x3'): return floats(e)
            return float(e.get('value')) if e.get('value') is not None else (e.text or '').strip()
    return d

def pack_car_parts(game, outdir, cap, car, out_name):
    """The moving parts of a vehicle (CarLogic: wing doors, wings, rear wings, boosters, back
    doors, jets), each a model, with the hierarchy, rest pose, AnimBasic movement and the
    FlameLogic flames, in models/<out_name>.parts"""
    c = _comps(game, car).get('CarLogic')
    if c is None: return 0
    parts = []
    def add(name, parent, group):
        cc = _comps(game, name)
        t = cc.get('Transform'); a = cc.get('AnimBasic'); rm = cc.get('RenderMesh'); fl = cc.get('FlameLogic')
        parts.append(dict(name=name, parent=parent, group=group,
                          pos=_val(t, 'position', [0, 0, 0]), ori=_val(t, 'orientation', [1, 0, 0, 0, 1, 0, 0, 0, 1]), scl=_val(t, 'scale', [1, 1, 1]),
                          anim=[_val(a, 'Start Time', 0), _val(a, 'End Time', 0)] + list(_val(a, 'Delta Pos', [0, 0, 0])) + [_val(a, 'Delta Rotation', 0)] + list(_val(a, 'Rotation Vector', [0, 0, 1])) if a is not None else [0, 0, 0, 0, 0, 0, 0, 0, 1],
                          mesh=_val(rm, 'Mesh') if rm is not None else None, flames=[]))
        if fl is not None:
            n = int(_val(fl, 'FlameCount', 0) or 0)
            for k in range(1, n + 1):
                fp = _val(fl, 'Pos%02d' % k); fs = _val(fl, 'Scl%02d' % k); fo = _val(fl, 'Ori%02d' % k)
                if fp and fs and fo: parts[-1]['flames'].append(fp + fo + fs)
        pe = cc.get('ParticleEmitter')               # the wing tips' sparkle trail (CarWingLeftOuter / RightOuter)
        if pe is not None:
            def g(k, d): x = _val(pe, k, d); return [float(q) for q in x] if isinstance(x, list) else float(x)
            parts[-1]['emit'] = ([g('Color Index', 1), g('Color Index Randomness', 0), g('Color Intensity', 1), g('Texture Index', 1),
                                  g('Texture Index Randomness', 0), g('Texture Delta', 0), g('Size Intensity', 1), g('Size Randomness', 0)]
                                 + g('Position', [0, 0, 0]) + g('Position Randomness', [0, 0, 0])
                                 + [g('Rotation', 0), g('Rotation Randomness', 0), g('Delta Rotation', 0)] + g('Direction', [0, 0, 1])
                                 + [g('Angle', 0), g('Speed', 0), g('Speed Randomness', 0), g('Gravity', 0), g('Particle Lifetime', 1), g('Spawn Frequency', 0)])
        return len(parts) - 1
    wi = add(_val(c, 'Wings Empty Name'), -1, 0) if _val(c, 'Wings Empty Name') else -1
    bi = add(_val(c, 'Boosters Empty Name'), -1, 1) if _val(c, 'Boosters Empty Name') else -1
    ji = add(_val(c, 'Jets Empty'), -1, 2) if _val(c, 'Jets Empty') else -1
    ch_root = None
    for e in c:
        if e.tag == 'Children': ch_root = e
    for ch in (ch_root if ch_root is not None else []):
        typ = int(_val(ch, 'Child Of Type', 1)); nm = _val(ch, 'Name')
        if not nm: continue
        me = add(nm, wi if typ == 1 else bi, 0 if typ == 1 else 1)
        cn = ch.find('ChildNames')
        if cn is not None:
            for st in cn.findall('String'): add(st.text.strip(), me, 0 if typ == 1 else 1)
    bn = _val(c, 'Booster Name')
    if bn: add(bn, bi, 1)
    for j in c.iter('Jet'):
        nm = _val(j, 'Name')
        if nm: add(nm, ji, 2)
    lines = []
    for i, p_ in enumerate(parts):
        model = '-'
        if p_['mesh']:
            model = '%s_p%d' % (out_name, i)
            pack_model(game, outdir, cap, p_['name'], model)
        lines.append('part %d %d %d %s %s' % (i, p_['parent'], p_['group'], model, ' '.join('%g' % x for x in p_['pos'] + p_['ori'] + p_['scl'] + p_['anim'])))
        for f_ in p_['flames']: lines.append('flame %d %s' % (i, ' '.join('%g' % x for x in f_)))
        if p_.get('emit'): lines.append('emit %d %s' % (i, ' '.join('%g' % x for x in p_['emit'])))   # nr_pemit's order
    open(os.path.join(outdir, 'models', out_name + '.parts'), 'w').write('\n'.join(lines) + '\n')
    return len(parts)

def pack_vehicles(game, outdir, cap):
    """the cars you can pick (VehicleData.xml): model, handling and pictures"""
    r = ET.parse(os.path.join(game, 'VehicleData', 'VehicleData.xml')).getroot()
    lines = []
    for v in r.findall('Vehicle'):
        d = {e.get('name'): (e.get('value') if e.get('value') is not None else (e.text or '').strip()) for e in v}
        bp = d.get('object_file', 'Car'); key = re.sub(r'[^a-z0-9]', '', v.get('name', bp).lower())
        pack_car(game, outdir, cap, bp, 'veh_' + key)
        print('  %s: %d moving parts' % (key, pack_car_parts(game, outdir, cap, bp, 'veh_' + key)))
        imgs = [nr_ui.write_ui_texture(sys.modules[__name__], d.get(k, ''), outdir, 512) or '' for k in ('unlocked_img_file', 'locked_img_file')]
        lines.append('%s|%s|%s|%s|%s|%s|%s' % (key, v.get('name'), d.get('unlocked', '0'), d.get('unlocked_description', ''),
                                               d.get('locked_description', ''), imgs[0], imgs[1]))
    os.makedirs(os.path.join(outdir, 'ui'), exist_ok=True)
    open(os.path.join(outdir, 'ui', 'vehicles.txt'), 'w').write('\n'.join(lines) + '\n')

def pack_gibs(game, outdir, cap, car='Car'):
    """the pieces the car breaks into when it explodes (CarLogic's Dead Car Body / Gib names),
    each a model, with where it sits on the car (models/gibs.txt)"""
    r = ET.parse(os.path.join(game, 'Objects', car + '.xml')).getroot()
    names = []
    for e in r.iter('String'):
        if e.get('name') in ('Dead Car Body Name', 'Dead Car Gib Name') and e.text: names.append(e.text.strip())
    lines = []
    for i, n in enumerate(names):
        g = ET.parse(os.path.join(game, 'Objects', n + '.xml')).getroot()
        t = g.find('GameObjectBlueprint/Components/Transform')
        pos = floats(named(t, 'Vector3', 'position')) if t is not None and named(t, 'Vector3', 'position') is not None else [0, 0, 0]
        pack_model(game, outdir, cap, n, 'gib%d' % i)
        lines.append('gib%d %g %g %g %s' % (i, pos[0], pos[1], pos[2], n))
    open(os.path.join(outdir, 'models', 'gibs.txt'), 'w').write('\n'.join(lines) + '\n')
    print('gibs: %d pieces' % len(lines))

# ------------------------------------------------------------------ start transforms
SNAMES = {}
def single_name(game, bpn, comps):
    """an object's SingleName: the level's own, else its blueprint's"""
    sn = comps.find('SingleName') if comps is not None else None
    if sn is not None and _val(sn, 'Name'): return _val(sn, 'Name')
    if bpn not in SNAMES:
        try: SNAMES[bpn] = _val(_comps(game, bpn).get('SingleName'), 'Name')
        except Exception: SNAMES[bpn] = None
    return SNAMES[bpn]

CHILDREN = {}
def child_names(game, bpn, comps):
    """the names a ChildObjectLogic (the level's own, else the blueprint's) makes"""
    def names(c):
        return [(e.text or '').strip() for e in c.iter('String')] if c is not None else []
    own = comps.find('ChildObjectLogic') if comps is not None else None
    if own is not None and names(own): return names(own)
    if bpn not in CHILDREN:
        try: CHILDREN[bpn] = names(_comps(game, bpn).get('ChildObjectLogic'))
        except Exception: CHILDREN[bpn] = []
    return CHILDREN[bpn]

def pack_starts(game, outdir, levels):
    """Where the car starts in each level: the StartTunnelLogic object's position and orientation
    (the PC puts the car there, turned as the tunnel is; the shipped ghosts' first records match).
    models/starts.txt: 'start <level> x y z' + the 3x3 (rows; columns = the local axes) + its scale (the warp-in's
    portal is at (0, -36 sy, sz) in the tunnel, 0x7062d0); 'end <level> ...' the same for the first object named
    EndTunnel (the warp-out's portal at (0, -37 sy, sz), 0x705da0)"""
    lines = []
    def tf(comps):
        t = comps.find('Transform')
        e = named(t, 'Vector3', 'position') if t is not None else None; pos = floats(e) if e is not None else [0, 0, 0]
        e = named(t, 'Matrix3x3', 'orientation') if t is not None else None; ori = floats(e) if e is not None else [1, 0, 0, 0, 1, 0, 0, 0, 1]
        e = named(t, 'Vector3', 'scale') if t is not None else None; scl = floats(e) if e is not None else [1, 1, 1]
        return ' '.join('%.5f' % x for x in list(pos) + list(ori) + list(scl))
    nend = nportal = 0
    for lv in levels:
        p = os.path.join(game, 'Levels', lv + '.xml')
        try: root = ET.parse(p).getroot()
        except Exception: continue
        start = end = None
        for g in root.iter('GameObject'):
            bpn = g.get('blueprint') or ''; comps = g.find('Components')
            if comps is None: continue
            if start is None:
                bp = blueprint(game, bpn); tags = [c.tag for c in comps]
                if 'StartTunnelLogic' in tags or (bp and 'StartTunnelLogic' in bp['comps']): start = tf(comps)
            if end is None and single_name(game, bpn, comps) == 'EndTunnel': end = tf(comps)
            if start is not None and end is not None: break
        if start is not None: lines.append('start %s %s' % (lv, start))
        if end is not None: lines.append('end %s %s' % (lv, end)); nend += 1
        # the tunnels' Portal (tunnel2 / EndTunnel2's ChildObjectLogic child: the blue swirl at their far end;
        # models/portal.nrl, drawn at the parent's transform)
        for g in root.iter('GameObject'):
            bpn = g.get('blueprint') or ''; comps = g.find('Components')
            if comps is None: continue
            if 'Portal' in child_names(game, bpn, comps): lines.append('portal %s %s' % (lv, tf(comps))); nportal += 1
    open(os.path.join(outdir, 'models', 'starts.txt'), 'w').write('\n'.join(lines) + '\n')
    print('starts: %d levels, %d with an end tunnel; %d tunnel portals' % (len(lines) - nend - nportal, nend, nportal))

# ------------------------------------------------------------------ ghost cars (re_N)
def ghost_names(game):
    """every blueprint a ReplayRecorder records as ('Replay Object Name'), test objects aside"""
    out = set()
    for f_ in glob.glob(os.path.join(game, 'Objects', '*.xml')):
        try: r = ET.parse(f_).getroot()
        except Exception: continue
        for c in r.iter('ReplayRecorder'):
            for e in c:
                if e.get('name') == 'Replay Object Name' and (e.text or '').strip(): out.add(e.text.strip())
    return sorted(n for n in out if not n.startswith('Test') and os.path.exists(os.path.join(game, 'Objects', n + '.xml')))

def pack_ghosts(game, outdir, cap):
    """The ghost cars and their wreck pieces (ReplayPlayer blueprints, material shockwave): each a
    tree of parts - the blueprint's mesh, then its ChildObjectLogic children (the wings) with their
    rest pose and AnimBasic - in models/ghosts.txt, meshes as models/gh_<name>.nrl. And the shipped
    replays and score tables (data/default_scores/Default) in ghosts/: <level>.<N>.ngb (the
    NrGhostBin file as it is) and default_scores.txt ('score <level> <initials> <time> <score> <trick>')."""
    lines = []; nmod = 0
    names = ghost_names(game)
    # the "S" copies: your own run replayed behind the results (CopyRecordingAsSelf adds 'S' to every name)
    names += [n + 'S' for n in names if os.path.exists(os.path.join(game, 'Objects', n + 'S.xml'))]
    for gn in names:
        parts = []
        def add(name, parent):
            cc = _comps(game, name)
            t = cc.get('Transform'); a = cc.get('AnimBasic'); rm = cc.get('RenderMesh'); co = cc.get('ChildObjectLogic')
            anim = [0, 0, 0, 0, 0, 0, 0, 0, 1, -1]
            if a is not None:
                anim = [_val(a, 'Start Time', 0), _val(a, 'End Time', 0)] + list(_val(a, 'Delta Pos', [0, 0, 0])) + \
                       [_val(a, 'Delta Rotation', 0)] + list(_val(a, 'Rotation Vector', [0, 0, 1])) + [_val(a, 'State', 0)]
            parts.append(dict(name=name, parent=parent, mesh=_val(rm, 'Mesh') if rm is not None else None,
                              pos=_val(t, 'position', [0, 0, 0]) if parent >= 0 else [0, 0, 0],
                              ori=_val(t, 'orientation', [1, 0, 0, 0, 1, 0, 0, 0, 1]) if parent >= 0 else [1, 0, 0, 0, 1, 0, 0, 0, 1],
                              scl=_val(t, 'scale', [1, 1, 1]), anim=anim))
            me = len(parts) - 1
            if co is not None:
                nm_ = co.find("Names[@name='Names']")
                for st in (nm_.findall('String') if nm_ is not None else []):
                    if st.text and os.path.exists(os.path.join(game, 'Objects', st.text.strip() + '.xml')): add(st.text.strip(), me)
        add(gn, -1)
        cc_ = _comps(game, gn)
        # label 1: a car with its runner's initials (PlayerName); failed 1: a car the results camera follows (FailedReplayCar)
        lines.append('ghost %s %d %d %d' % (gn, len(parts), 1 if 'PlayerName' in cc_ else 0, 1 if 'FailedReplayCar' in cc_ else 0))
        for i, p_ in enumerate(parts):
            model = '-'
            if p_['mesh']:
                model = 'gh_' + p_['name']
                if not os.path.exists(os.path.join(outdir, 'models', model + '.nrl')): pack_model(game, outdir, cap, p_['name'], model)
                nmod += 1
            lines.append('part %d %d %s %s' % (i, p_['parent'], model, ' '.join('%g' % float(x) for x in p_['pos'] + p_['ori'] + p_['scl'] + p_['anim'])))
    open(os.path.join(outdir, 'models', 'ghosts.txt'), 'w').write('\n'.join(lines) + '\n')
    # the shipped ghosts and score tables
    gd = os.path.join(outdir, 'ghosts'); os.makedirs(gd, exist_ok=True)
    sc = []
    root = os.path.join(game, 'data', 'default_scores', 'Default')
    for d_ in sorted(os.listdir(root)) if os.path.isdir(root) else []:
        lv = d_[:-4] if d_.endswith('.xml') else d_
        ss = os.path.join(root, d_, 'scores.NrScoreSet')
        if os.path.exists(ss):
            r = ET.parse(ss).getroot()
            for e in r.findall('Score'):
                v = {x.get('name'): (x.get('value') if x.get('value') is not None else (x.text or '').strip()) for x in e}
                sc.append('score %s %s %s %s %s' % (lv, v.get('initials', '???').replace(' ', '_') or '???', v.get('time', '0'), v.get('score', '0'), v.get('biggest_trick', '0')))
        for f_ in sorted(os.listdir(os.path.join(root, d_))):
            m_ = re.match(r'(.+)\.(\d)\.NrGhostBin$', f_)
            if m_:
                b = open(os.path.join(root, d_, f_), 'rb').read()
                dst = os.path.join(gd, '%s.%s.ngb' % (m_.group(1), m_.group(2)))
                if not os.path.exists(dst) or open(dst, 'rb').read() != b: open(dst, 'wb').write(b)
    # the developers' replays (Developer/), for the test drives: per level the best clean run in the standard car
    # (one spawn, CarGhost: no crash, not the van), else the best in the car, else rank 0 -> dev_<level>.0.ngb
    def ghost_info(b):
        import struct
        try:
            o = 4; nA = struct.unpack_from('<I', b, o)[0]; o += 4 + 4 * nA
            nB = struct.unpack_from('<I', b, o)[0]; o += 4 + 4 * nB
            nC = struct.unpack_from('<I', b, o)[0]; o += 4 + 2 * nC
            nD = struct.unpack_from('<I', b, o)[0]; o += 4 + 28 * nD
            nN = struct.unpack_from('<I', b, o)[0]; o += 4; names = []
            for _ in range(nN):
                ln = struct.unpack_from('<I', b, o)[0]; o += 4; names.append(b[o:o + ln].rstrip(b'\0').decode('latin1')); o += ln
            return nC, names
        except Exception: return 99, []
    droot = os.path.join(game, 'data', 'default_scores', 'Developer')
    for d_ in sorted(os.listdir(droot)) if os.path.isdir(droot) else []:
        lv = d_[:-4] if d_.endswith('.xml') else d_
        cands = []
        for r_ in range(10):
            src = os.path.join(droot, d_, '%s.%d.NrGhostBin' % (lv, r_))
            if os.path.exists(src): cands.append((r_, open(src, 'rb').read()))
        if not cands: continue
        def rank_key(c):
            nC, names = ghost_info(c[1])
            clean = nC == 1 and names == ['CarGhost']
            car = 'CarGhost' in names and 'VanGhost' not in names
            return (0 if clean else 1 if car else 2, c[0])
        r_, b = sorted(cands, key=rank_key)[0]
        dst = os.path.join(gd, 'dev_%s.0.ngb' % lv)
        if not os.path.exists(dst) or open(dst, 'rb').read() != b: open(dst, 'wb').write(b)
    open(os.path.join(gd, 'default_scores.txt'), 'w').write('\n'.join(sc) + '\n')
    # the Developer category's tables (data/default_scores/Developer/<level>.xml/scores.NrScoreSet: Calcite, Ruby)
    dsc = []
    for d_ in sorted(os.listdir(droot)) if os.path.isdir(droot) else []:
        ss = os.path.join(droot, d_, 'scores.NrScoreSet')
        if not os.path.exists(ss): continue
        lv = d_[:-4] if d_.endswith('.xml') else d_
        for e in ET.parse(ss).getroot().findall('Score'):
            v = {x.get('name'): (x.get('value') if x.get('value') is not None else (x.text or '').strip()) for x in e}
            dsc.append('score %s %s %s %s %s' % (lv, v.get('initials', '???').replace(' ', '_') or '???', v.get('time', '0'), v.get('score', '0'), v.get('biggest_trick', '0')))
    open(os.path.join(gd, 'dev_scores.txt'), 'w').write('\n'.join(dsc) + '\n')
    # the story's cinematics' texts (config/cinema_config.ini: the intro's and the ending's lines and timings)
    ci = os.path.join(game, 'config', 'cinema_config.ini')
    if os.path.exists(ci):
        os.makedirs(os.path.join(outdir, 'ui'), exist_ok=True)
        open(os.path.join(outdir, 'ui', 'cinema_config.txt'), 'wb').write(open(ci, 'rb').read())
    print('ghosts: %d blueprints, %d part meshes; %d shipped replays, %d default scores' % (
        sum(1 for l in lines if l.startswith('ghost ')), nmod, len([f_ for f_ in os.listdir(gd) if f_.endswith('.ngb')]), len(sc)))

# ------------------------------------------------------------------ particles
def pack_particles(game, outdir):
    """particles.ddsv (64 slices of 64x64, A8R8G8B8) -> an 8 x 8 atlas with mips that keep the cells
    apart (textures/particles_atlas.nrt); particle_color.tga (row = colour index, column = age)
    -> textures/particle_palette.nrt, RGBA8 for the CPU"""
    d = open(os.path.join(game, 'Textures', 'particles.ddsv'), 'rb').read()
    vol = np.frombuffer(d, np.uint8, 64 * 64 * 64 * 4, 128).reshape(64, 64, 64, 4)[..., [2, 1, 0, 3]]   # BGRA -> RGBA
    atlas = np.zeros((512, 512, 4), np.uint8)
    for k in range(64):
        y, x = (k // 8) * 64, (k % 8) * 64
        atlas[y:y + 64, x:x + 64] = vol[k]
    im = Image.fromarray(atlas, 'RGBA')
    mips = []
    while im.width >= 64:
        mips.append(encode_dxt(im, False))
        im = im.resize((im.width // 2, im.height // 2), Image.BOX)
    os.makedirs(os.path.join(outdir, 'textures'), exist_ok=True)
    with open(os.path.join(outdir, 'textures', 'particles_atlas.nrt'), 'wb') as f:
        f.write(b'NRT1' + struct.pack('<HHBBH', 512, 512, 1, len(mips), 0))
        for m in mips: f.write(m)
    pal = np.asarray(Image.open(os.path.join(game, 'Textures', 'particle_color.tga')).convert('RGBA').resize((64, 64), Image.NEAREST), np.uint8)
    with open(os.path.join(outdir, 'textures', 'particle_palette.nrt'), 'wb') as f:
        f.write(b'NRT1' + struct.pack('<HHBBH', 64, 64, 8, 1, 1)); f.write(pal.tobytes())
    print('particles: atlas 512x512 (%d mips), palette 64x64' % len(mips))

def write_rgba_mips(path, im):
    """RGBA8 with its whole mip chain (box-filtered, down to 1 x 1): textures that DXT would spoil"""
    w, h = im.size; mips = []
    while True:
        mips.append(np.asarray(im.convert('RGBA'), np.uint8).tobytes())
        if im.width <= 1 and im.height <= 1: break
        im = im.resize((max(1, im.width // 2), max(1, im.height // 2)), Image.BOX)
    with open(path, 'wb') as f:
        f.write(b'NRT1' + struct.pack('<HHBBH', w, h, 8, len(mips), 0))
        for m in mips: f.write(m)
    return len(mips)

TRAIL_TEX = ('tire_trail', 'trail_rainbow')
def pack_trails(game, outdir):
    """the tyre tracks' textures (tire_track.fx, TireTrailLogic 0x6bbc20): tire_trail.tga (r tread, g the ends'
    fade, b the glowing sparkle; three unrelated channels, so RGBA8) and the middle row of craterjar.tga, the
    rainbow the fastest tracks glow in across their width (Diffuse2 at (1.5 u, 0.5))"""
    os.makedirs(os.path.join(outdir, 'textures'), exist_ok=True)
    n1 = write_rgba_mips(os.path.join(outdir, 'textures', 'tire_trail.nrt'), Image.open(os.path.join(game, 'Textures', 'tire_trail.tga')).convert('RGBA'))
    cj = np.asarray(Image.open(os.path.join(game, 'Textures', 'craterjar.tga')).convert('RGBA'), np.uint8)
    row = np.repeat(cj[cj.shape[0] // 2:cj.shape[0] // 2 + 1], 8, 0)
    n2 = write_rgba_mips(os.path.join(outdir, 'textures', 'trail_rainbow.nrt'), Image.fromarray(row, 'RGBA').resize((256, 8), Image.BOX))
    print('tyre tracks: tire_trail 64x512 (%d mips), trail_rainbow 256x8 (%d mips)' % (n1, n2))

# ------------------------------------------------------------------ fonts (BMFont text + page)
HUD_FONTS = ['hemihead', 'ethon']
def pack_fonts(game, outdir):
    os.makedirs(os.path.join(outdir, 'fonts'), exist_ok=True)
    for name in HUD_FONTS:
        t = open(os.path.join(game, 'Fonts', name + '.fnt')).read()
        page = re.search(r'file="([^"]+)"', t).group(1)
        src = next((os.path.join(game, d, page) for d in ('Fonts', 'Textures') if os.path.exists(os.path.join(game, d, page))), None)
        if not src: print('  font %s: page %s missing' % (name, page)); continue
        im = Image.open(src).convert('RGBA')
        a = np.asarray(im).copy()
        if a[:, :, 3].min() == 255:            # glyphs stored as brightness, not alpha
            a[:, :, 3] = a[:, :, :3].max(2)
        a[:, :, :3] = 255                       # white glyphs; the HUD tints them
        with open(os.path.join(outdir, 'textures', 'font_%s.nrt' % name), 'wb') as f:
            f.write(b'NRT1' + struct.pack('<HHBBH', im.width, im.height, 8, 1, 1))   # format 8 = RGBA8
            f.write(a.tobytes())
        open(os.path.join(outdir, 'fonts', name + '.fnt'), 'w').write(t)
        print('  font %s: %dx%d page, %d characters' % (name, im.width, im.height, t.count('char id=')))

# ------------------------------------------------------------------ level game info
def level_display_names(game):
    names = {}
    for ls in sorted(glob.glob(os.path.join(game, 'LevelSets', '*.xml'))):
        try:
            for lv in ET.parse(ls).getroot().iter('Level'):
                for st in lv.findall('String'):
                    if st.get('name') == 'xml_file' and st.text:
                        names.setdefault(os.path.splitext(st.text.strip())[0], lv.get('name'))
        except Exception: pass
    return names

def level_textures(path):
    d = open(path, 'rb').read()
    ntex = struct.unpack_from('<I', d, 8)[0]
    o = 36 + (96 if d[:4] in (b'NRL2', b'NRL3') else 0)
    return [d[o + 64 * i:o + 64 + 64 * i].split(b'\0')[0].decode() for i in range(ntex)]

DISPLAY_NAMES = {}

def refresh_nrg(game, xml, outdir):
    """rewrite the extras (sounds, rings, coolers, dummies) of an already converted level"""
    p = os.path.join(outdir, 'levels', os.path.splitext(os.path.basename(xml))[0] + '.nrg')
    if not os.path.exists(p): return
    old = open(p).read().splitlines()
    head = [l for l in old if l.split(' ', 1)[0] in ('name', 'start', 'finish', 'finishbox', 'checkpoint')]
    objs = [l for l in old if l.split(' ', 1)[0] in ('obj', 'spawn')]
    try:   # (the same lines in the same order as pack_level writes them; the moving objects' sounds are theirs)
        root = ET.parse(xml).getroot()
        dyn = nr_objs.classify(game, root) if 'an_menu_level' not in os.path.basename(xml) else {}
        extra, lights = nr_ui.level_extras(game, xml, skip=set(dyn)), nr_light.light_lines(game, root)
    except Exception as e: print('  extras for %s failed: %s' % (xml, e)); return
    new = '\n'.join(head + extra + objs + lights) + '\n'
    if open(p).read() != new: open(p, 'w').write(new)

def write_one_pak(src, names, dst):
    """NRP2 pack: name[56] off size raw per entry; level data and textures are deflated
    (raw = unpacked size), sound streams are stored as they are (the Vita streams them)."""
    import zlib
    datas = []
    for n in names:
        d = open(os.path.join(src, n), 'rb').read()
        raw = 0
        if not n.startswith('snd/') and len(d) > 512:
            c = zlib.compressobj(6, zlib.DEFLATED, -15)
            z = c.compress(d) + c.flush()
            if len(z) < len(d) * 0.9: raw = len(d); d = z
        datas.append((n, d, raw))
    v2 = any(raw for _, _, raw in datas)      # nothing deflated (sound packs): the original NRPK layout
    hdr = 8 + (68 if v2 else 64) * len(names)
    off = (hdr + 15) & ~15
    ents = []
    for n, d, raw in datas:
        ents.append((n, off, len(d), raw)); off = (off + len(d) + 15) & ~15
    tmp = dst + '.tmp'
    with open(tmp, 'wb') as f:
        f.write((b'NRP2' if v2 else b'NRPK') + struct.pack('<I', len(names)))
        for n, o, sz, raw in ents: f.write(n.encode()[:55].ljust(56, b'\0') + (struct.pack('<III', o, sz, raw) if v2 else struct.pack('<II', o, sz)))
        for (n, o, sz, raw), (_, d, _r) in zip(ents, datas):
            f.seek(o); f.write(d)
        f.truncate((f.tell() + 15) & ~15)
    if os.path.exists(dst) and open(dst, 'rb').read() == open(tmp, 'rb').read(): os.remove(tmp)
    else: os.replace(tmp, dst)
    return os.path.getsize(dst)

PAK_LIMIT = 28 * 1000 * 1000     # each pack checks within the upload tool's 20 s limit on the Vita

def write_pak(src, outdir, levels):
    """several packs + nitronic.idx listing them: nr_base.pak (level list, car), nr_tex<N>.pak
    (textures, up to ~28 MB each) and nr_lvl_<level>.pak (geometry + collision per level).
    Unchanged packs come out byte-identical, so only changed ones need uploading."""
    tex = set()
    for n in levels: tex.update(level_textures(os.path.join(src, 'levels', n + '.nrl')))
    models = sorted(os.listdir(os.path.join(src, 'models'))) if os.path.isdir(os.path.join(src, 'models')) else []
    for m in models:
        if m.endswith('.nrl'): tex.update(level_textures(os.path.join(src, 'models', m)))
    for x in ('particles_atlas', 'particle_palette'):           # read by name (particles.c)
        if os.path.exists(os.path.join(src, 'textures', x + '.nrt')): tex.add(x)
    # the tyre tracks' and the warp-in's edge textures go in nr_base (added in v44: the texture packs stay as they were)
    basetex = sorted(t for t in set(TRAIL_TEX) | {t for t in tex if 'triangleedges' in t} if os.path.exists(os.path.join(src, 'textures', t + '.nrt')))
    tex -= set(basetex)
    fonts = sorted(os.listdir(os.path.join(src, 'fonts'))) if os.path.isdir(os.path.join(src, 'fonts')) else []
    fonttex = ['textures/font_%s.nrt' % os.path.splitext(f_)[0] for f_ in fonts if os.path.exists(os.path.join(src, 'textures', 'font_%s.nrt' % os.path.splitext(f_)[0]))]
    snd = sorted(os.listdir(os.path.join(src, 'snd'))) if os.path.isdir(os.path.join(src, 'snd')) else []
    sndbase = ['snd/sound.nsb'] if 'sound.nsb' in snd else []
    ui = sorted('ui/' + f_ for f_ in os.listdir(os.path.join(src, 'ui'))) if os.path.isdir(os.path.join(src, 'ui')) else []
    uitex = [f_ for f_ in ui if f_.endswith('.nrt') and not f_.endswith('height.nrt')]   # (the countdown's height maps: nr_base, v44)
    packs = [('nr_base.pak', ['levels.txt'] + ['models/' + m for m in models] + ['fonts/' + f_ for f_ in fonts] + fonttex + sndbase + [f_ for f_ in ui if f_ not in uitex]
              + ['textures/%s.nrt' % t for t in basetex])]
    if uitex: packs.append(('nr_ui.pak', uitex))
    gh = sorted('ghosts/' + f_ for f_ in os.listdir(os.path.join(src, 'ghosts'))) if os.path.isdir(os.path.join(src, 'ghosts')) else []
    if gh: packs.append(('nr_ghosts.pak', gh))
    group, size, k = [], 0, 0
    for t in sorted(tex):
        n = 'textures/%s.nrt' % t; sz = os.path.getsize(os.path.join(src, n))
        if group and size + sz > PAK_LIMIT: packs.append(('nr_tex%d.pak' % k, group)); group, size, k = [], 0, k + 1
        group.append(n); size += sz
    if group: packs.append(('nr_tex%d.pak' % k, group))
    group, size, k = [], 0, 0
    for f_ in snd:
        if not f_.endswith('.ogg'): continue
        n = 'snd/' + f_; sz = os.path.getsize(os.path.join(src, n))
        if group and size + sz > PAK_LIMIT: packs.append(('nr_snd%d.pak' % k, group)); group, size, k = [], 0, k + 1
        group.append(n); size += sz
    if group: packs.append(('nr_snd%d.pak' % k, group))
    for lv in levels:
        files = ['levels/%s.nrl' % lv] + [('levels/%s.%s' % (lv, ext)) for ext in ('nrc', 'nrg') if os.path.exists(os.path.join(src, 'levels', '%s.%s' % (lv, ext)))]
        packs.append(('nr_lvl_%s.pak' % re.sub(r'[^A-Za-z0-9_]', '_', lv), files))
    total = 0
    for name, files in packs:
        sz = write_one_pak(src, files, os.path.join(outdir, name)); total += sz
        print('  %-36s %3d files %6.1f MB' % (name, len(files), sz / 1e6))
    with open(os.path.join(outdir, 'nitronic.idx'), 'w') as f:
        for name, _ in packs: f.write(name + '\n')
    print('packs: %d, %.1f MB in total, listed in %s' % (len(packs), total / 1e6, os.path.join(outdir, 'nitronic.idx')))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('game'); ap.add_argument('out')
    ap.add_argument('--cap', type=int, default=512)
    ap.add_argument('--levels', default='')
    ap.add_argument('--sets', default='1_Campaign', help="level sets to include (LevelSets/ name prefixes, comma separated) or 'all'")
    ap.add_argument('--resume', action='store_true', help='skip levels already converted')
    ap.add_argument('--budget', type=float, default=0, help='stop after this many seconds (exit code 3)')
    a = ap.parse_args()
    final = a.out; a.out = os.path.join(final, 'work')
    os.makedirs(os.path.join(a.out, 'levels'), exist_ok=True)
    os.makedirs(os.path.join(a.out, 'textures'), exist_ok=True)
    index_textures(a.game)
    DISPLAY_NAMES.update(level_display_names(a.game))
    # campaign order first, then everything else
    order = []
    for ls in sorted(glob.glob(os.path.join(a.game, 'LevelSets', '*.xml'))):
        try:
            for lv in ET.parse(ls).getroot().iter('Level'):
                for s in lv.findall('String'):
                    if s.get('name') == 'xml_file' and s.text and s.text.strip() not in order: order.append(s.text.strip())
        except Exception: pass
    allx = sorted(os.path.basename(p) for p in glob.glob(os.path.join(a.game, 'Levels', '*.xml')))
    order = [x for x in order if x in allx] + [x for x in allx if x not in order]
    if a.levels: order = [x for x in order if x in a.levels.split(',')]
    if True:     # only levels that are in level sets ('all' = every set; test levels stay out)
        keep = []
        for ls in sorted(glob.glob(os.path.join(a.game, 'LevelSets', '*.xml'))):
            if a.sets != 'all' and not any(os.path.basename(ls).startswith(w) for w in a.sets.split(',')): continue
            for lv in ET.parse(ls).getroot().iter('Level'):
                for st in lv.findall('String'):
                    if st.get('name') == 'xml_file' and st.text: keep.append(st.text.strip())
        order = [x for x in order if x in keep]
    if os.path.exists(os.path.join(a.game, 'Levels', 'an_menu_level.xml')) and 'an_menu_level.xml' not in order:
        order.insert(0, 'an_menu_level.xml')     # the 3D scene behind the main menu
    if os.path.exists(os.path.join(a.game, 'Levels', 'credits_level.xml')) and 'credits_level.xml' not in order:
        order.append('credits_level.xml')        # the drive-through credits
    if os.path.exists(os.path.join(a.game, 'Levels', 'core_demo.xml')) and 'core_demo.xml' not in order:
        order.append('core_demo.xml')            # the story's intro and ending (CampaignIntroLevel, the core)
    stats = dict(unknown_mats=set())
    done = []
    import time
    t_end = time.time() + a.budget if a.budget else None
    for x in order:
        lv = os.path.join(a.out, 'levels', os.path.splitext(x)[0] + '.nrl')
        if a.resume and os.path.exists(lv):
            refresh_nrg(a.game, os.path.join(a.game, 'Levels', x), a.out)
            done.append(os.path.splitext(x)[0]); continue
        if t_end and time.time() > t_end:
            print('TIME BUDGET REACHED, run again to continue'); sys.exit(3)
        try: done.append(pack_level(a.game, os.path.join(a.game, 'Levels', x), a.out, a.cap, stats))
        except Exception as e: print('%-40s FAILED: %s' % (x, e))
    with open(os.path.join(a.out, 'levels.txt'), 'w') as f:
        for n in done: f.write(n + '\n')
    tsz = sum(os.path.getsize(p) for p in glob.glob(os.path.join(a.out, 'textures', '*.nrt')))
    lsz = sum(os.path.getsize(p) for p in glob.glob(os.path.join(a.out, 'levels', '*.nrl')))
    print('levels %d (%.1f MB), textures %d (%.1f MB)' % (len(done), lsz / 1e6, len(TEXDONE), tsz / 1e6))
    try: pack_fonts(a.game, a.out)
    except Exception as e: print('fonts FAILED:', e)
    try: pack_car(a.game, a.out, a.cap)
    except Exception as e: print('car FAILED:', e)
    try: pack_car(a.game, a.out, a.cap, 'GunHumvee', 'humvee')     # the CORE's drones (HumveeLogic: a car each)
    except Exception as e: print('drone car FAILED:', e)
    try: pack_vehicles(a.game, a.out, a.cap)
    except Exception as e:
        import traceback; traceback.print_exc(); print('vehicles FAILED:', e)
    try: pack_model(a.game, a.out, a.cap, 'jet_flame', 'flame')
    except Exception as e: print('flame FAILED:', e)
    try: pack_particles(a.game, a.out)
    except Exception as e: print('particles failed:', e)
    try: pack_trails(a.game, a.out)
    except Exception as e: print('tyre tracks failed:', e)
    try: pack_model(a.game, a.out, a.cap, 'Portal', 'portal')        # the tunnels' portals (starts.txt 'portal' lines)
    except Exception as e: print('portal FAILED:', e)
    try: pack_gibs(a.game, a.out, a.cap, 'Car')
    except Exception as e: print('gibs FAILED:', e)
    try: pack_starts(a.game, a.out, sorted(os.path.splitext(os.path.basename(x))[0] for x in glob.glob(os.path.join(a.game, 'Levels', '*.xml'))))
    except Exception as e: print('starts FAILED:', e)
    try: pack_ghosts(a.game, a.out, a.cap)
    except Exception as e: print('ghosts FAILED:', e)
    try: nr_ui.pack_ui(sys.modules[__name__], a.game, a.out)
    except Exception as e:
        import traceback; traceback.print_exc(); print('ui FAILED:', e)
    write_pak(a.out, final, done)
    if stats['unknown_mats']: print('materials with no definition:', ' '.join(sorted(stats['unknown_mats']))[:2000])

if __name__ == '__main__':
    main()
