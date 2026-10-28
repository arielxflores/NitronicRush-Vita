#!/usr/bin/env python3
"""nr_audio: convert the game's Wwise sound bank into what the Vita engine plays.

    python3 nr_audio.py "<game dir>" <work dir> [--ww2ogg path] [--wwiser path]

Writes <work dir>/snd/sound.nsb (events, actions, the sound/music hierarchy, curves) and
<work dir>/snd/<media id>.ogg (every referenced Wwise Vorbis stream as standard Ogg Vorbis
with correct granule positions). nr_pack.py puts them into nr_snd*.pak.
"""
import nr_ci  # noqa: F401  (case-insensitive file names off Windows: first)
import os, sys, struct, subprocess, argparse, re, shutil, tempfile, glob
import xml.etree.ElementTree as ET
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nr_ogg

HERE = os.path.dirname(os.path.abspath(__file__))
BANKDIR = 'Sounds/Nitronic/GeneratedSoundBanks/Windows'

# ---------------------------------------------------------------- bank -> xml (wwiser)
def bank_xml(game, tmp, wwiser):
    src = os.path.join(game, BANKDIR)
    for n in ('Main.txt', 'Init.bnk', 'SoundbanksInfo.xml', 'Init.txt'):
        if os.path.exists(os.path.join(src, n)): shutil.copy(os.path.join(src, n), tmp)
    # the HIRC is all we need from Main.bnk; drop the 237 MB DATA chunk for wwiser
    media = []
    with open(os.path.join(src, 'Main.bnk'), 'rb') as f, open(os.path.join(tmp, 'Main.bnk'), 'wb') as o:
        data_off = None; didx = []
        while True:
            h = f.read(8)
            if len(h) < 8: break
            tag, sz = h[:4], struct.unpack('<I', h[4:])[0]
            if tag == b'DATA': data_off = f.tell(); f.seek(sz, 1); continue
            d = f.read(sz); o.write(h + d)
            if tag == b'DIDX': didx = [struct.unpack_from('<III', d, k) for k in range(0, sz, 12)]
    media = {i: (data_off + off, size) for i, off, size in didx}
    out = os.path.join(tmp, 'dump')
    subprocess.check_call([sys.executable, wwiser, '-d', 'xml', '-dn', out, 'Main.bnk', 'Init.bnk'], cwd=tmp,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    s = open(out + '.xml', encoding='utf-8').read()
    s = re.sub(r'<\?xml[^>]*>\s*', '', s)
    return ET.fromstring('<all>' + s + '</all>'), media

# ---------------------------------------------------------------- xml helpers
def fval(e, name, conv=None, d=None):
    for f in e.iter('field'):
        if f.get('name') == name:
            v = f.get('value'); return conv(v) if conv else v
    return d
def dfield(e, name, conv=None, d=None):
    """direct child field only"""
    for f in e.findall('field'):
        if f.get('name') == name:
            v = f.get('value'); return conv(v) if conv else v
    return d
def sub(e, name):
    for o in e.iter():
        if o.tag in ('object', 'list') and o.get('name') == name: return o
    return None
def sub_or(e, name):
    """sub(e, name) when it is there and has children, else e (an Element's truth value is its child count)"""
    x = sub(e, name)
    return x if x is not None and len(x) else e
def subs(e, name):
    return [o for o in e.iter() if o.tag in ('object', 'list') and o.get('name') == name]
def pname(f):
    v = f.get('valuefmt') or ''
    m = re.search(r'\[([^\]]+)\]', v); return m.group(1) if m else v

def props_of(e):
    """(plain props, ranged props) from the first NodeInitialParams/ActionInitialValues found"""
    p = {}; r = {}
    for b in e.iter('object'):
        n = b.get('name') or ''
        if n.startswith('AkPropBundle<AkPropValue'):
            for it in b.iter('object'):
                if it.get('name') == 'AkPropBundle':
                    pid = [f for f in it.findall('field') if f.get('name') == 'pID']
                    pv = [f for f in it.findall('field') if f.get('name') == 'pValue']
                    if pid and pv: p.setdefault(pname(pid[0]), float(pv[0].get('value')))
        elif n.startswith('AkPropBundle<RANGED'):
            for it in b.iter('object'):
                if it.get('name') == 'AkPropBundle':
                    pid = [f for f in it.findall('field') if f.get('name') == 'pID']
                    mn = [f for f in it.findall('field') if f.get('name') == 'min']
                    mx = [f for f in it.findall('field') if f.get('name') == 'max']
                    if pid and mn and mx: r.setdefault(pname(pid[0]), (float(mn[0].get('value')), float(mx[0].get('value'))))
    return p, r

def points_of(lst):
    pts = []
    for g in lst.iter('object'):
        if g.get('name') == 'AkRTPCGraphPoint':
            pts.append((float(dfield(g, 'From')), float(dfield(g, 'To')), int(dfield(g, 'Interp'))))
    return pts

# ---------------------------------------------------------------- model
T_SOUND, T_RANSEQ, T_LAYER, T_MIXER, T_BUS, T_MRANSEQ, T_SEGMENT, T_TRACK, T_SWITCH = 1, 2, 3, 4, 5, 6, 7, 8, 9
TYPEMAP = {'CAkSound': T_SOUND, 'CAkRanSeqCntr': T_RANSEQ, 'CAkLayerCntr': T_LAYER, 'CAkActorMixer': T_MIXER,
           'CAkBus': T_BUS, 'CAkMusicRanSeqCntr': T_MRANSEQ, 'CAkMusicSegment': T_SEGMENT, 'CAkMusicTrack': T_TRACK,
           'CAkSwitchCntr': T_SWITCH, 'CAkMusicSwitchCntr': T_SWITCH}
A_PLAY, A_STOP, A_PAUSE, A_RESUME, A_SETVOL, A_RESETVOL, A_SETPITCH, A_RESETPITCH, A_SETLPF, A_RESETLPF, A_SETSTATE, A_OTHER = range(1, 13)
ACTKIND = {0x04: A_PLAY, 0x01: A_STOP, 0x02: A_PAUSE, 0x03: A_RESUME, 0x0A: A_SETVOL, 0x0B: A_RESETVOL,
           0x08: A_SETPITCH, 0x09: A_RESETPITCH, 0x0E: A_SETLPF, 0x0F: A_RESETLPF, 0x12: A_SETSTATE}

class Bank:
    def __init__(s):
        s.nodes = []; s.nidx = {}
        s.children = []; s.rtpcs = []; s.curves = []; s.points = []
        s.events = []; s.actions = []; s.media = []; s.midx = {}
        s.clips = []; s.mplist = []; s.rsitems = []; s.assocs = []; s.gparams = []; s.gpidx = {}
        s.attens = []; s.aidx = {}
    defaults = {}
    def gparam(s, rid):
        if rid not in s.gpidx: s.gpidx[rid] = len(s.gparams); s.gparams.append(rid)
        return s.gpidx[rid]
    def curve(s, scaling, pts):
        c = len(s.curves); s.curves.append((scaling, len(s.points), len(pts))); s.points.extend(pts); return c
    def media_ref(s, mid, streamed):
        if mid not in s.midx: s.midx[mid] = len(s.media); s.media.append({'id': mid, 'streamed': streamed})
        return s.midx[mid]

def rtpcs_of(B, e, scope_name='InitialRTPC'):
    """RTPC list directly owned by e (not by nested layers)"""
    out = []
    for ir in e.iter('object'):
        if ir.get('name') != scope_name: continue
        for r in ir.iter('object'):
            if r.get('name') != 'RTPC': continue
            rid = int(dfield(r, 'RTPCID')); pf = [f for f in r.findall('field') if f.get('name') == 'ParamID'][0]
            par = int(pf.get('value')); sc = int(dfield(r, 'eScaling', int, 0))
            tgt = {0: 0, 2: 1, 3: 2}.get(par)
            if tgt is None: continue
            out.append((B.gparam(rid), tgt, B.curve(sc, points_of(r))))
        break
    return out

def build(root, media_loc):
    B = Bank(); objs = []
    for bank in root.findall('root'):
        for o in bank.iter('object'):
            n = o.get('name') or ''
            if not n.startswith('CAk'): continue
            idf = dfield(o, 'ulID')
            if idf is None: continue
            objs.append((n, int(idf), o))
    # game parameter defaults (Init.bnk global settings)
    for bank in root.findall('root'):
        ids = [f for f in bank.iter('field') if f.get('name') in ('RTPC_ID', 'fValue')]
        for a, b in zip(ids, ids[1:]):
            if a.get('name') == 'RTPC_ID' and b.get('name') == 'fValue': B.defaults[int(a.get('value'))] = float(b.get('value'))
    # attenuations
    for n, i, o in objs:
        if n != 'CAkAttenuation': continue
        use = [int(f.get('value')) for f in o.iter('field') if (f.get('name') or '').startswith('curveToUse[')]
        tabs = [t for t in o.iter('object') if t.get('name') == 'CAkConversionTable']
        cv = []
        for t in tabs: cv.append(B.curve(int(dfield(t, 'eScaling', int, 0)), points_of(t)))
        def pick(k): return cv[use[k]] if k < len(use) and 0 <= use[k] < len(cv) else -1
        vol, lpf = pick(0), pick(3)
        maxd = B.points[B.curves[vol][1] + B.curves[vol][2] - 1][0] if vol >= 0 else 0
        B.aidx[i] = len(B.attens); B.attens.append((vol, lpf, maxd))
    # nodes
    for n, i, o in objs:
        t = TYPEMAP.get(n)
        if t is None: continue
        if i in B.nidx: continue
        B.nidx[i] = len(B.nodes)
        B.nodes.append({'id': i, 'type': t, 'el': o, 'name': n})
    def nref(i): return B.nidx.get(i, -1)
    for nd in B.nodes:
        o = nd['el']; t = nd['type']
        p, r = props_of(sub(o, 'BusInitialParams') if t == T_BUS else sub_or(o, 'NodeInitialParams'))
        nd['vol'] = p.get('Volume', 0.0); nd['pitch'] = p.get('Pitch', 0.0); nd['lpf'] = p.get('LPF', 0.0)
        nd['rv'] = r.get('Volume', (0, 0)); nd['rp'] = r.get('Pitch', (0, 0)); nd['rl'] = r.get('LPF', (0, 0))
        nd['loop'] = int(p.get('Loop', 1))
        if t == T_BUS:   # in bank v62 a bus names its parent bus as OverrideBusId
            pb = fval(o, 'OverrideBusId', int, 0) or fval(o, 'ulParentBusID', int, 0)
            nd['parent'] = nref(pb) if pb else -1
            nd['obus'] = 0
        else:
            nbp = sub(o, 'NodeBaseParams')
            nd['parent'] = nref(int(dfield(nbp, 'DirectParentID', d='0'))) if nbp is not None else -1
            nd['obus'] = int(dfield(nbp, 'OverrideBusId', d='0')) if nbp is not None else 0
        pos = sub(o, 'PositioningParams'); att = -1; is3d = 0
        if pos is not None:
            et = fval(pos, 'eType', int, -1)
            if et in (2, 3) and fval(pos, 'cbIs3DPositioningAvailable', int, 1):
                is3d = 1; att = B.aidx.get(fval(pos, 'uAttenuationID', int, 0), -1)
        nd['is3d'] = is3d; nd['atten'] = att
        nd['rtpc'] = rtpcs_of(B, o)
        if t == T_BUS:
            # The music bus' Music_Volume curve tops out at -35.5 dB, which would leave the music
            # nearly silent at any slider setting; volume sliders on busses are normalised so
            # their top end is 0 dB (the other busses already are).
            for g, tgt, c in nd['rtpc']:
                sc, f0, n0 = B.curves[c]
                ys = [B.points[f0 + j][1] for j in range(n0)]
                xs = [B.points[f0 + j][0] for j in range(n0)]
                if tgt == 0 and n0 and max(ys) < -6 and min(xs) >= 0 and max(xs) <= 1:
                    top = max(ys)
                    for j in range(n0):
                        x, y, it = B.points[f0 + j]; B.points[f0 + j] = (x, y - top if y > -96 else y, it)
        ch = sub(o, 'Children')
        nd['kids'] = [int(f.get('value')) for f in ch.iter('field') if f.get('name') == 'ulChildID'] if ch is not None else []
        nd['xi'] = [0, 0, 0, 0]; nd['xf'] = [0.0, 0.0, 0.0, 0.0]; nd['flags'] = 0
        if t == T_SOUND:
            st = fval(o, 'StreamType', int, 0); sid = fval(o, 'sourceID', int, 0)
            nd['xi'][0] = B.media_ref(sid, st != 0)
        elif t == T_RANSEQ:
            nd['xi'][0] = len(B.rsitems)
            items = [(int(dfield(it, 'ulPlayID')), int(dfield(it, 'weight', d='50000'))) for it in o.iter('object') if it.get('name') == 'AkPlaylistItem']
            for cid, w in items: B.rsitems.append((cid, w))
            nd['xi'][1] = len(items)
            nd['xi'][2] = fval(o, 'sLoopCount', int, 1)
            nd['xi'][3] = (fval(o, 'eMode', int, 0) & 1) | (fval(o, 'wAvoidRepeatCount', int, 0) << 8) | (fval(o, 'eTransitionMode', int, 0) << 4)
            nd['xf'][0] = fval(o, 'fTransitionTime', float, 0.0)
            nd['flags'] |= (2 if fval(o, 'bIsContinuous', int, 0) else 0) | (4 if fval(o, 'bResetPlayListAtEachPlay', int, 1) else 0) | (8 if fval(o, '_bIsUsingWeight', int, 0) else 0) | (16 if fval(o, 'eRandomMode', int, 0) else 0)
        elif t == T_LAYER:
            nd['xi'][0] = len(B.assocs); n0 = len(B.assocs)
            for lay in o.iter('object'):
                if lay.get('name') != 'CAkLayer': continue
                lr = rtpcs_of(B, lay, 'LayerInitialValues')
                rid = int(fval(lay, 'rtpcID', d='0'))
                lst = [(B.rtpcs.__len__(),)]
                lr_first = len(B.rtpcs); B.rtpcs.extend(lr)
                for a in lay.iter('object'):
                    if a.get('name') != 'CAssociatedChildData': continue
                    cid = int(dfield(a, 'ulAssociatedChildID'))
                    pts = points_of(a)
                    cv = B.curve(2, pts) if pts else -1
                    B.assocs.append([cid, B.gparam(rid) if rid else -1, cv, lr_first, len(lr)])
            nd['xi'][1] = len(B.assocs) - n0
        elif t == T_SEGMENT:
            nd['xf'][0] = fval(o, 'fDuration', float, 0.0)
            mk = [(int(dfield(m, 'id')), float(dfield(m, 'fPosition'))) for m in o.iter('object') if m.get('name') == 'AkMusicMarkerWwise']
            mk.sort(key=lambda x: x[1])
            nd['xf'][1] = mk[0][1] if mk else 0.0
            nd['xf'][2] = mk[-1][1] if mk else nd['xf'][0]
        elif t == T_TRACK:
            srcs = {}
            for sd in o.iter('object'):
                if sd.get('name') == 'AkBankSourceData':
                    srcs[fval(sd, 'sourceID', int)] = fval(sd, 'StreamType', int, 0)
            nd['xi'][0] = len(B.clips); n0 = len(B.clips)
            for ti in o.iter('object'):
                if ti.get('name') != 'AkTrackSrcInfo': continue
                sid = int(dfield(ti, 'sourceID'))
                B.clips.append((B.media_ref(sid, srcs.get(sid, 0) != 0), float(dfield(ti, 'fPlayAt')), float(dfield(ti, 'fBeginTrimOffset')),
                                float(dfield(ti, 'fEndTrimOffset')), float(dfield(ti, 'fSrcDuration'))))
            nd['xi'][1] = len(B.clips) - n0
        elif t == T_MRANSEQ:
            # playlist tree, flattened breadth-first: each item lists its children contiguously
            top = [it for it in o.iter('object') if it.get('name') == 'AkMusicRanSeqPlaylistItem']
            def direct_items(e):
                pl = [c for c in e if c.tag == 'list' and c.get('name') == 'pPlayList']
                return [c for c in pl[0] if c.tag == 'object'] if pl else []
            root_items = direct_items(sub_or(o, 'MusicRanSeqCntrInitialValues'))
            if not root_items:
                lst = [c for c in o.iter('list') if c.get('name') == 'pPlayList']
                root_items = [c for c in lst[0] if c.tag == 'object'] if lst else []
            nd['xi'][0] = len(B.mplist)
            queue = [(it, len(B.mplist) + k) for k, it in enumerate(root_items)]
            B.mplist.extend([None] * len(root_items))
            while queue:
                it, slot = queue.pop(0)
                kids = direct_items(it)
                first = len(B.mplist); B.mplist.extend([None] * len(kids))
                for k, c in enumerate(kids): queue.append((c, first + k))
                B.mplist[slot] = [int(dfield(it, 'SegmentID')), len(kids), first, int(dfield(it, 'eRSType', d='0')),
                                  int(dfield(it, 'Loop', d='1')), int(dfield(it, 'Weight', d='50000')),
                                  int(dfield(it, 'wAvoidRepeatCount', d='0')), int(dfield(it, 'bIsShuffle', d='0'))]
            nd['xi'][1] = len(root_items)
    # output bus resolution: own override, else the parent's
    def outbus(k, depth=0):
        nd = B.nodes[k]
        if nd['type'] == T_BUS: return -1
        if nd['obus']: return nref(nd['obus'])
        if nd['parent'] >= 0 and depth < 32: return outbus(nd['parent'], depth + 1)
        return -1
    for k, nd in enumerate(B.nodes): nd['bus'] = outbus(k)
    # child lists (as node indices)
    for nd in B.nodes:
        nd['child_first'] = len(B.children)
        ks = [nref(c) for c in nd['kids'] if nref(c) >= 0]
        B.children.extend(ks); nd['child_n'] = len(ks)
        nd['rtpc_first'] = len(B.rtpcs); B.rtpcs.extend(nd['rtpc']); nd['rtpc_n'] = len(nd['rtpc'])
    B.rsitems = [(nref(c), w) for c, w in B.rsitems]
    for a in B.assocs: a[0] = nref(a[0])
    for m in B.mplist: m[0] = nref(m[0]) if m[0] else -1
    # events and actions
    acts = {}
    for n, i, o in objs:
        if n.startswith('CAkAction'): acts[i] = (n, o)
    for n, i, o in objs:
        if n != 'CAkEvent': continue
        ids = [int(f.get('value')) for f in o.iter('field') if f.get('name') == 'ulActionID']
        first = len(B.actions)
        for aid in ids:
            if aid not in acts: continue
            an, ao = acts[aid]
            atype = fval(ao, 'ulActionType', int, 0); kind = ACTKIND.get(atype >> 8, A_OTHER); scope = atype & 0xFF
            tgt = fval(ao, 'ulTargetID', int, 0)
            p, r = props_of(ao)
            val = fval(ao, 'base', float, 0.0); vmin = fval(ao, 'min', float, 0.0); vmax = fval(ao, 'max', float, 0.0)
            meaning = fval(ao, 'eValueMeaning', int, 0)
            curve = fval(ao, 'eFadeCurve', int, 4)
            if kind == A_SETSTATE:
                tgt = fval(ao, 'ulStateGroupID', int, 0); val = float(fval(ao, 'ulTargetStateID', int, 0))
            B.actions.append({'kind': kind, 'scope': scope, 'target': nref(tgt) if kind != A_SETSTATE else -1,
                              'delay': p.get('DelayTime', 0.0), 'fade': p.get('TransitionTime', 0.0),
                              'curve': curve, 'meaning': meaning, 'value': val, 'vmin': vmin, 'vmax': vmax,
                              'raw_target': tgt})
        B.events.append((i, first, len(B.actions) - first))
    return B

# ---------------------------------------------------------------- media
def wem_bytes(game, mid, loc):
    if mid in loc:
        off, size = loc[mid]
        with open(os.path.join(game, BANKDIR, 'Main.bnk'), 'rb') as f:
            f.seek(off); return f.read(size)
    p = os.path.join(game, BANKDIR, '%d.wem' % mid)
    if os.path.exists(p): return open(p, 'rb').read()
    return None

def wem_info(buf):
    info = {'loop': None}
    p = 12
    while p + 8 <= len(buf):
        tag = buf[p:p + 4]; sz = struct.unpack('<I', buf[p + 4:p + 8])[0]; d = buf[p + 8:p + 8 + sz]
        if tag == b'fmt ':
            fmt, ch, rate = struct.unpack('<HHI', d[:8]); info.update(fmt=fmt, ch=ch, rate=rate)
            if sz >= 0x42: info['samples'] = struct.unpack('<I', d[0x18:0x1c])[0]
        elif tag == b'vorb': info['samples'] = struct.unpack('<I', d[:4])[0]
        elif tag == b'smpl' and len(d) >= 36:
            nl = struct.unpack('<I', d[28:32])[0]
            if nl and len(d) >= 36 + 24:
                st, en = struct.unpack('<II', d[36 + 8:36 + 16]); info['loop'] = (st, en + 1)
        p += 8 + sz + (sz & 1)
    return info

PLUGINS = set()
def plugin_sources(game):
    """ids of the bank's source plug-ins (SoundSeed Woosh) from Main.txt"""
    sec = False
    for l in open(os.path.join(game, BANKDIR, 'Main.txt'), encoding='latin1'):
        p = l.rstrip('\r\n').split('\t')
        if p and p[0].startswith('Source plug-ins'): sec = True; continue
        if sec and (not l.strip() or not l.startswith('\t')): sec = False
        if sec and len(p) > 2 and p[1].strip().isdigit(): PLUGINS.add(int(p[1]))

def convert_media(game, loc, B, outdir, ww2ogg, pcb):
    os.makedirs(outdir, exist_ok=True)
    tmp = tempfile.mkdtemp()
    ok = 0; total = 0
    for k, m in enumerate(B.media):
        mid = m['id']; dst = os.path.join(outdir, '%d.ogg' % mid); meta = dst + '.meta'
        if os.path.exists(dst) and os.path.exists(meta):
            m.update(eval(open(meta).read())); ok += 1; total += os.path.getsize(dst); continue
        w = wem_bytes(game, mid, loc)
        if w is None and mid in PLUGINS:     # SoundSeed Woosh: synthesised by the Vita engine
            m.update(rate=48000, ch=1, samples=int(48000 * 0.9), loop=None, missing=2); continue
        if w is None: print('  media %d missing' % mid); m.update(rate=48000, ch=2, samples=0, loop=None, missing=1); continue
        inf = wem_info(w)
        src = os.path.join(tmp, 'a.wem'); og = os.path.join(tmp, 'a.ogg')
        open(src, 'wb').write(w)
        r = subprocess.run([ww2ogg, src, '-o', og, '--pcb', pcb], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if r.returncode != 0 or not os.path.exists(og):
            print('  media %d: ww2ogg failed: %s' % (mid, r.stdout.decode(errors='replace')[-200:])); m.update(rate=48000, ch=2, samples=0, loop=None, missing=1); continue
        pk = nr_ogg.read_packets(open(og, 'rb').read()); os.remove(og)
        data, gran = nr_ogg.write_ogg(pk, inf.get('samples'))
        open(dst, 'wb').write(data)
        d = dict(rate=inf['rate'], ch=inf['ch'], samples=inf.get('samples', gran), loop=inf['loop'])
        open(meta, 'w').write(repr(d)); m.update(d); ok += 1; total += len(data)
        if k % 50 == 0: print('  media %d/%d' % (k, len(B.media)), flush=True)
    shutil.rmtree(tmp, ignore_errors=True)
    print('media: %d of %d converted, %.1f MB' % (ok, len(B.media), total / 1e6))

# ---------------------------------------------------------------- output
def write_nsb(B, path, names):
    o = bytearray(b'NSB1')
    counts = [len(B.nodes), len(B.children), len(B.rtpcs), len(B.curves), len(B.points), len(B.events), len(B.actions),
              len(B.media), len(B.clips), len(B.mplist), len(B.rsitems), len(B.assocs), len(B.gparams), len(B.attens)]
    o += struct.pack('<I', 3) + struct.pack('<%dI' % len(counts), *counts)
    for nd in B.nodes:
        o += struct.pack('<IBBhhh', nd['id'], nd['type'], nd['flags'] | (1 if nd['is3d'] else 0), nd['parent'], nd['bus'], nd['atten'])
        o += struct.pack('<9f', nd['vol'], nd['pitch'], nd['lpf'], nd['rv'][0], nd['rv'][1], nd['rp'][0], nd['rp'][1], nd['rl'][0], nd['rl'][1])
        o += struct.pack('<hHI', nd['loop'], nd['rtpc_n'], nd['rtpc_first'])
        o += struct.pack('<II', nd['child_first'], nd['child_n'])
        o += struct.pack('<4i4f', *nd['xi'], *nd['xf'])
    for c in B.children: o += struct.pack('<i', c)
    for g, t, c in B.rtpcs: o += struct.pack('<HBBi', g, t, 0, c)
    for sc, f, n in B.curves: o += struct.pack('<BBHI', sc, 0, n, f)
    for x, y, i in B.points: o += struct.pack('<ffI', x, y, i)
    for i, f, n in B.events: o += struct.pack('<III', i, f, n)
    for a in B.actions:
        o += struct.pack('<BBBBi5f', a['kind'], a['scope'], a['curve'], a['meaning'], a['target'], a['delay'], a['fade'], a['value'], a['vmin'], a['vmax'])
        o += struct.pack('<I', a['raw_target'])
    for m in B.media:
        lp = m.get('loop') or (0, 0)
        o += struct.pack('<IIBBHIII', m['id'], m.get('rate', 48000), m.get('ch', 2), 1 if m.get('streamed') else 0, int(m.get('missing', 0)),
                         m.get('samples', 0), lp[0], lp[1])
    for c in B.clips: o += struct.pack('<i4f', *c)
    for m in B.mplist: o += struct.pack('<iHHbbhIHH', m[0], m[1], 0, m[3], m[7], m[4], m[5], m[6], 0) + struct.pack('<I', m[2])
    for n, w in B.rsitems: o += struct.pack('<iI', n, w)
    for a in B.assocs: o += struct.pack('<iiiII', *a)
    for g in B.gparams: o += struct.pack('<If', g, B.defaults.get(g, 0.0))
    for v, l, d in B.attens: o += struct.pack('<iif', v, l, d)
    # names: event id -> name (for logging/debug), game parameter names
    nm = '\n'.join('%d %s' % (k, v) for k, v in sorted(names.items())).encode()
    o += struct.pack('<I', len(nm)) + nm
    open(path, 'wb').write(o)
    print('sound.nsb: %d nodes, %d events, %d actions, %d media, %d clips, %.1f KB' % (len(B.nodes), len(B.events), len(B.actions), len(B.media), len(B.clips), len(o) / 1024))

def bank_names(game):
    names = {}
    for n in ('Main.txt', 'Init.txt'):
        p = os.path.join(game, BANKDIR, n)
        if not os.path.exists(p): continue
        for l in open(p, encoding='latin1'):
            parts = l.rstrip('\r\n').split('\t')
            if len(parts) > 2 and parts[1].strip().isdigit(): names[int(parts[1])] = parts[2].strip()
    return names

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('game'); ap.add_argument('work')
    ap.add_argument('--ww2ogg', default=os.path.join(HERE, 'third_party', 'ww2ogg', 'ww2ogg'))
    ap.add_argument('--wwiser', default=os.path.join(HERE, 'third_party', 'wwiser', 'wwiser.py'))
    ap.add_argument('--no-media', action='store_true')
    a = ap.parse_args()
    tmp = tempfile.mkdtemp()
    root, loc = bank_xml(a.game, tmp, a.wwiser)
    B = build(root, loc)
    snd = os.path.join(a.work, 'snd'); os.makedirs(snd, exist_ok=True)
    pcb = os.path.join(os.path.dirname(a.ww2ogg), 'packed_codebooks_aoTuV_603.bin')
    plugin_sources(a.game)
    if not a.no_media: convert_media(a.game, loc, B, snd, a.ww2ogg, pcb)
    names = bank_names(a.game)
    write_nsb(B, os.path.join(snd, 'sound.nsb'), names)
    shutil.rmtree(tmp, ignore_errors=True)

if __name__ == '__main__':
    main()
