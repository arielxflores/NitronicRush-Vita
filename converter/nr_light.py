"""Level lighting from the game's own data.

The PC game lights its levels with a light pre-pass: one directional "sun" (Light Type 2 on the
level's skybox object; it shines straight down and adds an ambient term), plus point (0), spot (1)
and tube (3) lights on the level's objects (streetlights, tunnel tubes, arrows, checkpoints...).
Per light and pixel it adds  colour * intensity * att * (max(N.L, 0) + ambient)  with
att = clamp(near / d * (far - d) / (far - near), 0, 1.5)  (light_point.fx, light_spot.fx...).
Here the static lights are added up per vertex (baked); the sun is applied on the Vita.
The skybox object also carries the fog (FogTweaks) and the sky's colour (RenderMesh Color1).
"""
import os, math
import numpy as np
import xml.etree.ElementTree as ET

_BP = {}
def _bp(game, name):
    """components of a blueprint: tag -> element (first of each)"""
    if name in _BP: return _BP[name]
    res = {}
    p = os.path.join(game, 'Objects', name + '.xml')
    if name and os.path.exists(p):
        try:
            g = ET.parse(p).getroot().find('GameObjectBlueprint')
            if g is not None and g.find('Components') is not None:
                for c in g.find('Components'):
                    res.setdefault(c.tag, c)
        except Exception:
            pass
    _BP[name] = res
    return res

def _vals(el):
    d = {}
    if el is None: return d
    for c in el:
        n = c.get('name')
        if n is None: continue
        if c.tag in ('Float', 'Integer', 'Boolean'):
            try: d[n] = float(c.get('value'))
            except (TypeError, ValueError): pass
        elif c.tag in ('Vector3', 'Vector4', 'Matrix3x3'):
            d[n] = [float(x.get('value')) for x in c.iter('Float')]
        elif c.tag == 'String':
            d[n] = (c.text or '').strip()
    return d

def _merged(game, bpname, comps, tag):
    """blueprint values of a component, overridden by the level object's own"""
    d = _vals(_bp(game, bpname).get(tag))
    if comps is not None:
        for el in comps.findall(tag): d.update(_vals(el))
    return d

# objects whose light moves or only flashes on events: not baked
DYNAMIC = {'BodyRigid', 'SearchlightLogic', 'EnumLogic', 'ExplosionLogic', 'SweepLaserLogic',
           'BackAndForthSawLogic', 'FlipperLogic', 'StaticSawLogic', 'CoreBallLogic', 'Flocking',
           'TV_RadialImpulse', 'TimedDeathLogic', 'ControllerCar', 'AnimBasic', 'ChildObjectLogic'}

def level_lighting(game, root):
    """-> (sun dict, list of light dicts) for a level's XML root"""
    sun = dict(col=[0.2, 0.2, 0.2], amb=0.7, spec=9.65, fog=[0.1, 0.1, 0.2, 0.3], fog_near=-100.0,
               fog_far=1000.0, sky_far=2000.0, sky_tint=[1, 1, 1, 0.4], sky_obj=None)
    lights = []
    for g in root.iter('GameObject'):
        bpn = g.get('blueprint') or ''
        comps = g.find('Components')
        bpc = _bp(game, bpn)
        has_light = 'Light' in bpc or (comps is not None and comps.find('Light') is not None)
        if not has_light: continue
        L = _merged(game, bpn, comps, 'Light')
        T = _merged(game, bpn, comps, 'Transform')
        typ = int(L.get('Type', 0))
        col = L.get('Color', [1, 1, 1, 1]) + [1, 1, 1, 1]
        if typ == 2:
            sun['col'] = [col[0] * col[3], col[1] * col[3], col[2] * col[3]]
            sun['amb'] = L.get('Ambient Strength', 0.7)
            sun['spec'] = L.get('Specular Strength', 9.65)
            F = _merged(game, bpn, comps, 'FogTweaks')
            if F:
                sun['fog'] = (F.get('FogColor', sun['fog']) + [0, 0, 0, 0])[:4]
                sun['fog_near'] = F.get('FogNear', sun['fog_near'])
                sun['fog_far'] = F.get('FogFar', sun['fog_far'])
                sun['sky_far'] = F.get('SkyFar', sun['sky_far'])
            R = _merged(game, bpn, comps, 'RenderMesh')
            if 'Color1' in R: sun['sky_tint'] = (R['Color1'] + [1, 1, 1, 1])[:4]
            sun['sky_obj'] = bpn
            continue
        names = set(bpc.keys())
        if comps is not None: names |= {c.tag for c in comps}
        if names & DYNAMIC or bpn.lower().startswith('car'): continue
        pulse = 'LightPulse' in names                        # pulses with the music (baked apart)
        k = _merged(game, bpn, comps, 'LightPulse').get('Pulse Intensity', 1.0) if pulse else 1.0
        pos = np.array((T.get('position', [0, 0, 0]) + [0, 0, 0])[:3], np.float64)
        R = np.array((T.get('orientation', [1, 0, 0, 0, 1, 0, 0, 0, 1]) + [0] * 9)[:9], np.float64).reshape(3, 3)
        near = max(L.get('Attenuation Near', 7.0), 1e-3); far = L.get('Attenuation Far', 25.0)
        if far <= near or col[3] * max(col[:3]) <= 0: continue
        d = dict(type=typ, pos=pos, col=np.array(col[:3], np.float64) * col[3] * k, pulse=pulse, amb=L.get('Ambient Strength', 0.1),
                 near=near, far=far, half=0.0, axis=None, dir=None, cos_in=1.0, cos_out=0.0)
        if typ == 3:                                         # tube: along the object's own Y axis
            a = R[:, 1]; d['axis'] = a / max(np.linalg.norm(a), 1e-9); d['half'] = L.get('Tube Length', 1.0) * 0.5
        if typ == 1:                                         # spot: shines down the object's -Z
            a = -R[:, 2]; d['dir'] = a / max(np.linalg.norm(a), 1e-9)
            d['cos_in'] = math.cos(math.radians(L.get('Inner Spot Angle', 10.0)))
            d['cos_out'] = math.cos(math.radians(L.get('Outer Spot Angle', 20.0)))
        lights.append(d)
    return sun, lights

def bake(P, N, lights):
    """light at each vertex (world positions P, unit normals N) from the given lights, (n,3)"""
    P = np.asarray(P, np.float64); N = np.asarray(N, np.float64)
    out = np.zeros((len(P), 3), np.float64)
    if not len(P): return out.astype(np.float32)
    lo = P.min(0); hi = P.max(0)
    for L in lights:
        r = L['far'] + L['half']
        if np.any(L['pos'] + r < lo) or np.any(L['pos'] - r > hi): continue
        m = np.all(np.abs(P - L['pos']) <= r, axis=1)
        if not m.any(): continue
        p = P[m]; n = N[m]
        if L['type'] == 3:
            t = np.clip((p - L['pos']) @ L['axis'], -L['half'], L['half'])
            q = L['pos'][None, :] + t[:, None] * L['axis'][None, :]
        else:
            q = L['pos'][None, :]
        v = q - p
        dist = np.maximum(np.linalg.norm(v, axis=1), 1e-3)
        l = v / dist[:, None]
        att = np.clip(L['near'] * (L['far'] - dist) / (dist * (L['far'] - L['near'])), 0.0, 1.5)
        ndl = np.einsum('ij,ij->i', n, l)
        dif = np.where(ndl > 0, ndl, 0.0) + L['amb']
        if L['type'] == 1:
            cs = (-l) @ L['dir']
            x = np.clip((cs - L['cos_out']) / max(L['cos_in'] - L['cos_out'], 1e-4), 0.0, 1.0)
            att = att * x * x * (3 - 2 * x)
        out[m] += (att * dif)[:, None] * L['col'][None, :]
    return out.astype(np.float32)

def light_lines(game, root):
    """the level's static lights for the Vita (it lights moving things like the car with the
    nearest of them): light type x y z r g b ambient near far ax ay az half cos_in cos_out"""
    sun, lights = level_lighting(game, root)
    out = []
    for L in lights:
        ax = L['axis'] if L['axis'] is not None else (L['dir'] if L['dir'] is not None else np.zeros(3))
        out.append('light %d %.3f %.3f %.3f %.4f %.4f %.4f %.3f %.3f %.3f %.4f %.4f %.4f %.3f %.4f %.4f' % (
            L['type'], *L['pos'], *(L['col'] * (0.75 if L.get('pulse') else 1.0)), L['amb'], L['near'], L['far'], *ax, L['half'], L['cos_in'], L['cos_out']))
    return out
