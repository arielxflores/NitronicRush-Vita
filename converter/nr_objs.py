"""Level objects that move, react or animate (the PC game's logic components), for the Vita.

The static level is baked into world-space chunks (nr_pack.pack_level). The objects handled here
are not: each becomes an "obj" line in the level's .nrg with its transform, colour, collision and
the values of its logic components, and its mesh becomes a model (models/o_<name>.nrl) drawn
with a model matrix. Collision meshes of things that move become models/c_<file>.ncm (triangles
in the object's own space). The Vita runs the logic (src/nr_objs.c), ported from the exe
(docs/re/re_A..E.md). Blueprints the logic spawns at run time (debris, shards, lasers, ...) are
listed as "spawn" lines with their own models.
"""
import os, re, struct, hashlib
import numpy as np
import xml.etree.ElementTree as ET
import nr_ui
import nr_fx

# (component, kind), first match wins
KINDS = [('CheckpointLogic', 'checkpoint'), ('SpinningLaserLogic', 'spinlaser'), ('SweepLaserLogic', 'sweep'), ('BackAndForthSawLogic', 'bafsaw'),
         ('StaticSawLogic', 'saw'), ('DroneLogic', 'drone'), ('BlimpLogic', 'blimp'), ('TrainLogic', 'train'),
         ('FakeSpotLightLogic', 'spot'), ('LaserLogic', 'laserarch'), ('BlockerLogic', 'blocker'),
         ('StreetlightBreaker', 'streetlight'), ('ScreenBreaker', 'screen'), ('AirMineParentLogic', 'mineparent'),
         ('ShardBuildingLogic', 'shardbld'), ('BarrelSpawnerLogic', 'barrelspawner'), ('ScalePulseLogic', 'antenna'),
         ('DistanceColorChange', 'distcolor'), ('CoreBallLogic', 'coreball'), ('TV_RadialForce', 'forcefield'),
         ('TriggerSphereZone', 'trigger'), ('TriggerCylinderZone', 'trigger'), ('TriggerZoneAABox', 'trigger'),
         ('TriggerZoneOrientedBox', 'trigger'), ('TriggerAchievement', 'recv'), ('TriggerEndStory', 'recv'),
         ('TriggerRespawnLocation', 'recv'), ('TriggerControlScheme', 'recv'), ('TriggerTutorialText', 'recv'),
         ('TriggerAtFinish', 'recv'), ('TriggerBranchOnVehicle', 'recv'), ('TriggerDelay', 'recv'), ('TriggerAnnouncer', 'recv'),
         ('TriggerChainer', 'recv'), ('TriggerBase', 'recv'),
         # the bonus levels (re_F1), cooling and the particles (re_F2)
         ('SliderLogic', 'slider'), ('SwingerLogic', 'swinger'), ('StomperLogic', 'stomper'), ('MortarGunLogic', 'mortar'),
         ('DropShipLogic', 'dropship'), ('CooldownPadLogic', 'coolpad'), ('CarCooler', 'carcooler'),
         ('FishZone', 'fishzone'), ('Flocking', 'fish'), ('ParticleEmitter', 'emitter'),
         # the CORE drones and their garages (re_J), the story's end (re_E)
         ('GarageLogic', 'garage'), ('HumveeLogic', 'humvee'), ('CoreExploder', 'coreexploder'), ('FlickerOnHack', 'eye')]
# kinds that never move: their collision stays in the level's grid (tagged with the object's id)
STATIC_COLL = {'checkpoint', 'streetlight', 'screen', 'laserarch', 'shardbld', 'barrelspawner', 'coreball', 'blocker', 'antenna',
               'spot', 'distcolor', 'mineparent', 'forcefield', 'trigger', 'recv', 'stomper', 'finish', 'coolpad', 'carcooler',
               'emitter', 'fishzone', 'mortar', 'dropship', 'garage', 'coreexploder', 'eye'}
# kinds whose mesh never changes: baked into the level like everything else
BAKE_MESH = {'barrelspawner', 'forcefield', 'trigger', 'recv', 'stomper', 'finish', 'carcooler', 'emitter', 'fishzone', 'garage'}
SPAWNS = {'bafsaw': ['saw_path'], 'blimp': ['searchlight1', 'searchlight2', 'searchlight3', 'searchlight4'],
          'train': ['choochoo', 'choocart', 'choocaboose', 'Railroad'], 'laserarch': ['laser'], 'sweep': ['laser'],
          'spinlaser': ['SingleLaser', 'LaserDot'],
          'mineparent': ['AirMine', 'MineExplosion'] + ['DeadGunHumvee' + s for s in ('Back', 'Front', 'Top', 'SideBack', 'SideFront', 'SideMid', 'Window')],
          'shardbld': ['Shard', 'ShardCore', 'ShardTarget', 'ShardShockwave'], 'barrelspawner': ['spike_barrel'],
          'streetlight': ['streetlight_dead%d' % i for i in range(1, 5)], 'screen': ['poster_dead'],
          'coreball': ['sparkdead%d' % i for i in range(1, 5)],
          'stomper': ['stomper_foot'], 'mortar': ['MortarMine', 'MineExplosion2'] + ['mineGib%02d' % i for i in range(1, 21)],
          'dropship': ['mblock2', 'drone_hawk2', 'ShardShockwave']}
HUMVEE_SPAWNS = ['GunHumvee', 'GunHumveeBase', 'GunHumveeGun', 'Phaser', 'PhaserExplosion', 'ShardShockwave'] + \
                ['DeadGunHumvee' + s for s in ('Back', 'Front', 'Top', 'SideBack', 'SideFront', 'SideMid', 'Window', 'Base', 'Gun', 'TireL')]
SPAWNS['garage'] = HUMVEE_SPAWNS; SPAWNS['humvee'] = HUMVEE_SPAWNS
SPAWNS['coreexploder'] = (['FireRing', 'CoreDeathSparker', 'CoreEyeSparker', 'DeadCoreEye', 'CoreExplosion', 'DeadCoreBase'] +
                          ['DeadCore_Claw%d' % i for i in range(4)] + ['DeadCore_A_%d' % i for i in range(9)] + ['DeadCore_B_%d' % i for i in range(7)] +
                          ['DeadCore_C_%02d' % i for i in range(15)] + ['DeadCore_D_%d' % i for i in range(8)] + ['DeadCore_E_%02d' % i for i in range(19)] +
                          ['DeadCore_F_%d' % i for i in range(5)])
# collision materials (CollisionMaterials.xml names) -> code in the low 4 bits of a triangle's material
COLL = {'Default': 0, 'Road': 1, 'Building': 2, 'Streetlight': 3, 'Obsticle': 4, 'Metal': 5, 'Lava': 6, 'spike': 7, 'Car': 8, 'CarGib': 9}
SKIP_VALS = {'Transform', 'RenderMesh', 'StaticMesh', 'CulledStatic', 'Phantom', 'BodyRigid', 'ColliderTriMesh',
             'ColliderOrientedBox', 'ColliderCylinder', 'CollisionInfoCollector', 'Material', 'SingleName', 'ColorPaletteComponent',
             'OneShotRadiusAudioLogic', 'RoadData'}

def coll_code(name): return COLL.get(name or 'Default', 0)
def tri_mat(cmat, matid, objid): return (cmat & 15) | ((matid & 15) << 4) | ((objid & 0xffffff) << 8)

def merged(game, bpn, comps, tag):
    base = nr_ui.comp_values(nr_ui.bp_components(game, bpn).get(tag))
    if comps is not None:
        for c in comps.findall(tag):
            base.update({k: v for k, v in nr_ui.comp_values(c).items() if v not in (None, '')})
    return base

def comp_names(game, bpn, comps):
    s = set(nr_ui.bp_components(game, bpn).keys())
    if comps is not None: s |= {c.tag for c in comps}
    return s

def kind_of(names):
    for c, k in KINDS:
        if c in names: return k
    return None

def classify(game, root):
    """ordinal of each GameObject (document order) -> kind, for the objects handled at run time"""
    out = {}
    for i, g in enumerate(root.iter('GameObject')):
        bpn, comps = g.get('blueprint') or '', g.find('Components')
        names = comp_names(game, bpn, comps)
        k = kind_of(names)
        if not k and 'EnumLogic' in names:       # type 6: a finish line (EndTunnel); type 2: spins (fansaw)
            t = int(_f(merged(game, bpn, comps, 'EnumLogic').get('Type', 0)))
            if t == 6: k = 'finish'
            elif t == 2 and 'BodyRigid' in names: k = 'saw'
        if k: out[i] = k
    return out

def _f(v, d=0.0):
    try: return float(str(v).rstrip('f'))
    except (TypeError, ValueError): return d

def _tok(v):
    if isinstance(v, list): return ','.join('%g' % x for x in v)
    s = str(v).strip()
    try: return '%g' % float(s.rstrip('f'))
    except ValueError: return re.sub(r'\s+', '_', s) or '-'

def transform(game, bpn, comps, use_bp=True):
    T = merged(game, bpn, comps if comps is not None else None, 'Transform') if use_bp else nr_ui.comp_values(comps.find('Transform') if comps is not None else None)
    pos = (list(T.get('position', [0, 0, 0])) + [0, 0, 0])[:3]
    scl = (list(T.get('scale', [1, 1, 1])) + [1, 1, 1])[:3]
    ori = (list(T.get('orientation', [1, 0, 0, 0, 1, 0, 0, 0, 1])) + [0] * 9)[:9]
    return pos, ori, scl

def collider(game, bpn, comps):
    """-> (kind 'mesh'|'box'|'cyl', file or size, material name) or None"""
    for tag in ('ColliderTriMesh', 'ColliderOrientedBox', 'ColliderCylinder'):
        if tag in comp_names(game, bpn, comps):
            v = merged(game, bpn, comps, tag)
            if tag == 'ColliderTriMesh': return ('mesh', v.get('File'), v.get('Material'))
            if tag == 'ColliderOrientedBox': return ('box', (list(v.get('Size', [1, 1, 1])) + [1, 1, 1])[:3], v.get('Material'))
            return ('cyl', [_f(v.get('Radius', 1)), _f(v.get('Half Height', 1))], v.get('Material'))
    return None

def box_tris(size):
    # ColliderOrientedBox: Size x scale is the HALF extent (0x636470 h = scale*Size; support 0x634fd0 = +-h)
    h = np.array(size, np.float32)
    c = np.array([[sx, sy, sz] for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)], np.float32) * h
    q = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    t = []
    for a, b, cc, d in q: t += [(c[a], c[b], c[cc]), (c[a], c[cc], c[d])]
    return np.array(t, np.float32)

class Exporter:
    def __init__(self, P, game, outdir, cap):
        self.P, self.game, self.outdir, self.cap = P, game, outdir, cap
        self.models = {}; self.names = set(); self.cmodels = {}
        os.makedirs(os.path.join(outdir, 'models'), exist_ok=True)

    def model(self, mesh, mats, cols, hint, fl=None):
        """a mesh with its materials and colours as a model in its own space -> model name"""
        if not mesh: return '-'
        fl = fl or {}
        key = (mesh, tuple(sorted(mats.items())), tuple((k, tuple(np.round(v, 3))) for k, v in sorted(cols.items())), tuple(sorted(fl.items())))
        if key in self.models: return self.models[key]
        P = self.P
        try: v, idx, subs = P.load_msh(os.path.join(self.game, 'Meshes', mesh))
        except Exception as e:
            print('  object mesh %s: %s' % (mesh, e)); self.models[key] = '-'; return '-'
        base = 'o_' + re.sub(r'[^A-Za-z0-9_]', '_', hint)[:40]
        name = base
        if name in self.names: name = base + '_' + hashlib.md5(repr(key).encode()).hexdigest()[:6]
        self.names.add(name)
        texlist, texidx, matlist, matidx, flat = [], {}, [], {}, []
        def tex(src, want_alpha=False):
            if not src: return -1
            r = P.convert_texture(src, self.outdir, self.cap, want_alpha)
            if not r: return -1
            if r[0] not in texidx: texidx[r[0]] = len(texlist); texlist.append(r)
            else: texlist[texidx[r[0]]] = r
            return texidx[r[0]]
        c1 = cols.get(1, [1, 1, 1, 1]); c2 = cols.get(2); F = [fl.get(k, 0) for k in (1, 2, 3)]
        def mat(mname):
            if mname in matidx: return matidx[mname]
            m = P.material(self.game, mname)
            kind, gloss, flags = P.classify(m)
            if not m['passes']: kind = P.KIND_ADD
            fxd = nr_fx.info(P, m, c1, c2, F)
            if fxd['kind'] is not None: kind = fxd['kind']
            flags |= fxd['flags']
            if fxd['fx'] in (nr_fx.FX_TRONICS, nr_fx.FX_CITY): fxd['fx'] = 0     # (level geometry only)
            d = tex(m['tex'].get('Diffuse1'), kind in (P.KIND_LIT, P.KIND_REF, P.KIND_TV)); e = tex(m['tex'].get('Emit1'), True)
            r = tex(fxd['reflect'] or m['tex'].get('Reflect1')) if kind == P.KIND_REF else -1
            an = tex(fxd['anim'], True) if fxd['anim'] else -1
            if kind == P.KIND_SKY: kind = P.KIND_LIT
            blend = 2 if kind == P.KIND_ADD else 0
            if e >= 0: flags |= P.MF_EMIT
            if d < 0: flags |= P.MF_NODIFF
            matidx[mname] = len(matlist)
            matlist.append((d, e, -1, r, blend, kind, flags, P.add_variant(m) if kind == P.KIND_ADD else 0, gloss, an, fxd['fx'], fxd['c2'], fxd['p']))
            FXD[mname] = fxd
            return matidx[mname]
        FXD = {}
        nrm = np.nan_to_num(v['n'][:, :3])
        nrm = nrm / np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-6)
        for si, (a, b) in enumerate(subs):
            if b <= a: continue
            slot = si + 1 if (si + 1) in mats else (max(mats) if mats else None)
            mname = mats.get(slot, '') if slot else ''
            mi = mat(mname)
            used = np.unique(idx[a:b])
            remap = np.zeros(len(v), np.int64); remap[used] = np.arange(len(used))
            out = np.zeros(len(used), P.VDT)
            out['p'] = v['p'][used]; out['uv'] = v['uv'][used]
            out['n'][:, :3] = np.clip(np.rint(nrm[used] * 127), -127, 127)
            out['c'] = P.vcolour(nr_fx.vertex_colours(FXD[mname], c1, v['c'][used], len(used)))
            flat.append((1, mi, out, remap[idx[a:b]].astype('<u2')))
        if not flat: self.models[key] = '-'; return '-'
        P.write_nrl(os.path.join(self.outdir, 'models', name + '.nrl'), texlist, matlist, flat)
        self.models[key] = name
        return name

    def cmodel(self, col):
        """collision of a moving object, in its own space -> name ('-' for none)"""
        if not col: return '-'
        kind, what, matname = col
        if kind == 'box': return 'box:%s' % ','.join('%g' % x for x in what)
        if kind == 'cyl': return 'cyl:%g,%g' % tuple(what)
        if what in self.cmodels: return self.cmodels[what]
        t = self.P.load_mdc(self.game, what)
        if t is None or not len(t): self.cmodels[what] = '-'; return '-'
        name = 'c_' + re.sub(r'[^A-Za-z0-9_]', '_', os.path.splitext(what)[0])
        with open(os.path.join(self.outdir, 'models', name + '.ncm'), 'wb') as f:
            f.write(b'NCM1' + struct.pack('<I', len(t))); f.write(np.ascontiguousarray(t, '<f4').tobytes())
        self.cmodels[what] = name
        return name

    def describe(self, bpn, comps, use_bp=True):
        """the common fields of an obj / spawn line"""
        game = self.game
        names = comp_names(game, bpn, comps)
        pos, ori, scl = transform(game, bpn, comps, use_bp)
        rm = merged(game, bpn, comps, 'RenderMesh')
        mesh = rm.get('Mesh') or None
        mats = {int(k[8:]): v for k, v in rm.items() if k.startswith('Material') and k[8:].isdigit()}
        cols = {int(k[5:]): v for k, v in rm.items() if k.startswith('Color') and k[5:].isdigit() and isinstance(v, list)}
        fl = {int(k[5:]): _f(v) for k, v in rm.items() if k.startswith('Float') and k[5:].isdigit()}
        model = self.model(mesh, mats, cols, bpn, fl) if rm.get('_play', '1') != '0' else '-'
        if names & {'TriggerSphereZone', 'TriggerCylinderZone', 'TriggerZoneAABox', 'TriggerZoneOrientedBox'} or kind_of(names) == 'recv': model = '-'   # editor-only blocks
        col = collider(game, bpn, comps)
        cm = self.cmodel(col)
        mid = int(_f(merged(game, bpn, comps, 'Material').get('Material ID', 0))) if 'Material' in names else 0
        ds = 0.0
        for k in ('CustomDeathSpeed',):
            if k in names: ds = _f(merged(game, bpn, comps, k).get('Death Speed', 0))
        c1 = (list(cols.get(1, [1, 1, 1, 1])) + [1, 1, 1, 1])[:4]
        f = ['bp=%s' % bpn, 'm=%s' % model, 'cm=%s' % cm, 'cmat=%d' % coll_code(col[2] if col else None), 'mid=%d' % mid,
             'ds=%g' % ds, 'p=%s' % _tok(pos), 'r=%s' % _tok(ori), 's=%s' % _tok(scl), 'c=%s' % _tok(c1)]
        if 'Phantom' in names:
            ph = merged(game, bpn, comps, 'Phantom')
            ev = ph.get('Event Name')
            if ev and ev != 'None': f.append('snd=%s:%d' % (ev, int(_f(ph.get('Begin Playing?', 0)))))
        if 'LightPulse' in names: f.append('pulse=%g' % _f(merged(game, bpn, comps, 'LightPulse').get('Pulse Intensity', 1)))
        if 'Light' in names:
            L = merged(game, bpn, comps, 'Light')
            if isinstance(L.get('Color'), list): f.append('light=%s' % _tok((L['Color'] + [1, 1, 1, 1])[:4] + [_f(L.get('Attenuation Near', 7)), _f(L.get('Attenuation Far', 25))]))
        if 'SingleName' in names:
            sn = merged(game, bpn, comps, 'SingleName')
            nm = next((v for v in sn.values() if isinstance(v, str) and v), '')
            if nm: f.append('name=%s' % _tok(nm))
        for tag in sorted(names - SKIP_VALS):
            vals = merged(game, bpn, comps, tag)
            for k, v in vals.items():
                if v is None or v == '': continue
                f.append('%s.%s=%s' % (tag, re.sub(r'[^A-Za-z0-9_]', '_', k), _tok(v)))
        return f

    def bezier(self, bpn, comps):
        for c in ([comps.find('TrainLogic')] if comps is not None else []) + [nr_ui.bp_components(self.game, bpn).get('TrainLogic')]:
            if c is None: continue
            b = c.find('BezierPath')
            if b is None: continue
            pts = [[float(x.get('value')) for x in v.iter('Float')] for v in b if v.tag == 'Vector3']
            if pts: return 'path=' + ';'.join('%g,%g,%g' % tuple(p[:3]) for p in pts)
        return None

    def level(self, root, dyn):
        """obj / spawn lines for a level; dyn = classify(...)"""
        lines, spawns = [], set()
        objid = 0
        for i, g in enumerate(root.iter('GameObject')):
            if i not in dyn: continue
            objid += 1
            k = dyn[i]; bpn = g.get('blueprint') or ''; comps = g.find('Components')
            f = ['obj', 'k=%s' % k, 'id=%d' % objid] + self.describe(bpn, comps)
            if k == 'train':
                b = self.bezier(bpn, comps)
                if b: f.append(b)
            lines.append(' '.join(f))
            for s in SPAWNS.get(k, []): spawns.add(s)
            if k == 'blocker' and len(bpn) > 12: spawns.add('blocker' + bpn[12:])
            if k == 'saw' and 'CoreLightningLogic' in comp_names(self.game, bpn, comps):
                for s in SPAWNS['coreball']: spawns.add(s)
            if k == 'dropship':
                dv = merged(self.game, bpn, comps, 'DropShipLogic')
                for fld in ('Block Name', 'Ship Name'):
                    if dv.get(fld): spawns.add(dv[fld])
            if k == 'mortar':
                mn = merged(self.game, bpn, comps, 'MortarGunLogic').get('Mortar Name')
                if mn: spawns.add(mn)
        for g in root.iter('GameObject'):             # CheckpointLogic: its ghosts rise as the car passes
            if 'CheckpointLogic' in comp_names(self.game, g.get('blueprint') or '', g.find('Components')): spawns.add('checkpoint_ghost'); break
        for s in sorted(spawns):
            if not os.path.exists(os.path.join(self.game, 'Objects', s + '.xml')): continue
            lines.append(' '.join(['spawn'] + self.describe(s, None)))
        return lines
