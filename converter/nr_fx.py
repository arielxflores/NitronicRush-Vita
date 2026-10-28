"""What the PC game's material shaders need beyond the textures (docs/re/re_G.md): the animated
ones (ramps and lava, CORE veins, turbines, the ground circuits, the backdrop city, screens),
Color2 and Float1..3, and how the vertices carry the colours. Used by nr_pack.py and nr_objs.py."""
import os
import numpy as np
from PIL import Image

FX_NONE, FX_PULSE, FX_VEINS, FX_ANIM, FX_TRONICS, FX_CITY, FX_TV, FX_TRAFFIC = range(8)
MF_SPHERE_N, MF_REFSPEC = 64, 128

def pc_col(c, default=(1, 1, 1, 1)):
    """a RenderMesh colour as the game stores it: rgb bytes (int)(x*255) & 255, glow (int)(w*10) & 255 / 10
    (so the ramps' Color2 w 30 is 4.4, the Car's Color2 (1,-1,-1) is pure red)"""
    c = (list(c) + list(default))[:4] if c is not None else list(default)
    out = [((int(x * 255)) & 255) / 255.0 for x in c[:3]]
    out.append(((int(c[3] * 10)) & 255) / 10.0)
    return out

def shaded(m):
    return next((sh.lower() for p, sh in m['passes'] if p == 'Shaded'), '')
def additive(m):
    return next((sh.lower() for p, sh in m['passes'] if p == 'Additive'), '')

STRENGTH = {}
def mean_strength(P, src):
    """mean brightness of a ReflectStrength1 texture (phong_ref_spec)"""
    if not src: return 1.0
    if src in STRENGTH: return STRENGTH[src]
    p = P.TEXINDEX.get(os.path.basename(src).lower())
    v = 1.0
    if p:
        try:
            a = np.asarray(Image.open(p).convert('RGB'), np.float32) / 255.0
            v = float((a @ np.array([0.299, 0.587, 0.114], np.float32)).mean())
        except Exception: pass
    STRENGTH[src] = v
    return v

def info(P, m, c1, c2, F):
    """-> dict: fx, anim (texture source), c2, p (4 floats), flags to add, kind (None = keep),
    uv ('block', 'tronics', 'city' or None), vc ('road', 'city' or None), glow_k (x Color1 glow), reflect (source or None)"""
    sh, ad = shaded(m), additive(m)
    c1p, c2p = pc_col(c1), pc_col(c2, (0, 0, 0, 0))
    F = (list(F) + [0, 0, 0])[:3]
    d = dict(fx=FX_NONE, anim=None, c2=[0, 0, 0, 0], p=[0, 0, 0, 0], flags=0, kind=None, uv=None, vc=None, glow_k=1.0, reflect=None)
    if ad.startswith('pulse_veins'): d.update(fx=FX_VEINS, c2=c2p)
    elif sh.startswith('phong_newpulse'): d.update(fx=FX_PULSE, c2=c2p, p=F + [0])
    elif sh.startswith('phong_emit_anim'): d.update(fx=FX_ANIM, anim=m['tex'].get('Anim1'))
    elif sh.startswith('tronics'): d.update(fx=FX_TRONICS, anim=m['tex'].get('Tron1'), c2=c2p, uv='tronics')
    elif sh.startswith('city'): d.update(fx=FX_CITY, anim=m['tex'].get('Tron1'), c2=c2p, p=c1p, uv='city', vc='city')
    elif sh.startswith('tvscreen_red'): d.update(fx=FX_TV, p=[0.5, 0.25, 0.3, 0])
    elif sh.startswith('tvscreen'): d.update(fx=FX_TV, p=[0.35, 0.05, 0.1, 0])
    elif sh.startswith('phong_ref_spec'):
        d.update(flags=MF_REFSPEC, reflect=m['tex'].get('ReflectMap1') or m['tex'].get('Reflect1'),
                 p=[0, 0, 0, mean_strength(P, m['tex'].get('ReflectStrength1'))])
    elif sh.startswith('phong_ref_pulse'): d.update(flags=MF_SPHERE_N)
    elif sh.startswith('phong_ref_cube'): d.update(kind=P.KIND_LIT)                     # its cube map: left out
    elif sh.startswith('corecorecore'): d.update(kind=P.KIND_LIT, flags=P.MF_EMIT_COLOUR | P.MF_NODIFF, glow_k=0.5)   # (1 - cube) x Color
    elif sh.startswith('road'): d.update(vc='road', c2=c2p)
    elif sh.startswith('block'): d.update(uv='block')
    return d

def key(d):
    """what has to differ between two materials of the same name"""
    return (d['fx'], tuple(np.round(d['c2'], 4)), tuple(np.round(d['p'], 4)), d['vc'] == 'road' and tuple(np.round(d['c2'], 4)))

def vertex_colours(d, c1, vc_mesh, n):
    """(n,4) float colours (rgb, glow) for the vertices"""
    c1p = np.array(pc_col(c1), np.float32)
    if d['vc'] == 'road':          # mix(C1 x C1.w, C2 x C2.w, vertex red)
        a = c1p[:3] * c1p[3]; b = np.array(d['c2'][:3], np.float32) * d['c2'][3]
        t = vc_mesh[:, 0:1].astype(np.float32) / 255.0
        m = a[None, :] * (1 - t) + b[None, :] * t
        mx = np.maximum(m.max(1, keepdims=True), 1e-6)
        return np.concatenate([m / mx, mx], 1)
    c = np.tile(c1p, (n, 1))
    c[:, 3] *= d['glow_k']
    return c

def block_uv(uv, n_obj, scl):
    """block.fx: the texture repeats every 4 units of the object's size, by face direction"""
    sx, sy, sz = [abs(float(s)) for s in scl]
    k = lambda x: max(1.0, float(np.ceil(x / 4)))
    out = uv.copy()
    ax = np.abs(n_obj[:, 0]) > 0.1; ay = ~ax & (np.abs(n_obj[:, 1]) > 0.1); az = ~ax & ~ay
    out[ax] *= np.array([k(sz), k(sy)], np.float32)
    out[ay] *= np.array([k(sz), k(sx)], np.float32)
    out[az] *= np.array([k(sy), k(sx)], np.float32)
    return out
