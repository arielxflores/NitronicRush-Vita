"""Front-end and HUD data for the Vita build: UI textures, HUD meshes, level sets, credits,
achievements, menu camera, and per-level extras (sound emitters, boost rings, coolers).
Used by nr_pack.py; everything comes from the user's own install."""
import os, re, struct, glob
import xml.etree.ElementTree as ET
import numpy as np
from PIL import Image

UI_TEXTURES = [
    'DPLogo.tga', 'kysebad.png', 'InfoScreen.dds', 'Rush4.tga',
    'menu_button2.tga', 'Menu_button.tga', 'menu_button_new.png', 'Menu_slider.tga', 'Menu_sliderbar.tga', 'Menu_toggle.tga',
    'Loading2.tga', 'levelimage_border.tga', 'levelimage_locked.tga', 'levelimage_none.tga', 'new_level_unlocked.png',
    'trophy_gold.png', 'trophy_silver.png', 'trophy_bronze.png', 'achievement_unlocked.png', 'vehicle_unlocked.png', 'credits_nitronic_rush.tga',
    'hud.png', 'hud_heat.tga', 'hud_flame.png', 'Hud_buttons.tga',
    'countdown_1.tga', 'countdown_2.tga', 'countdown_3.tga', 'countdown_rush.tga',
    'Countdown_1_line.tga', 'Countdown_2_line.tga', 'Countdown_3_line.tga', 'Countdown_rush_line.tga',
    # countdown.fx's height maps (Diffuse2: the sweep in and out, and the grid the digits are drawn in; v44, in nr_base)
    'countdown_1Height.tga', 'countdown_2Height.tga', 'countdown_3Height.tga', 'countdown_rushHeight.tga',
]
HUD_MESHES = ['hud_main', 'hud_heat', 'hud_heatlines', 'hud_flames2']

def ui_name(src):
    return re.sub(r'[^a-z0-9_]', '_', os.path.splitext(os.path.basename(src).lower())[0])

def write_ui_texture(P, src, outdir, maxdim=1024, compress=False):
    """RGBA8 for small images (crisp HUD/menu art), DXT for big ones; power-of-two sizes."""
    p = P.TEXINDEX.get(os.path.basename(src).lower())
    if not p: print('  ui texture %s not found' % src); return None
    name = ui_name(src)
    dst = os.path.join(outdir, 'ui', name + '.nrt')
    if os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(p) and os.path.getmtime(dst) >= os.path.getmtime(__file__): return name
    im = Image.open(p); im.load(); im = im.convert('RGBA')
    w, h = P.pot(im.width, maxdim), P.pot(im.height, maxdim)
    if (w, h) != im.size: im = im.resize((w, h), Image.LANCZOS)
    a = np.asarray(im)
    alpha = (a[:, :, 3] < 250).mean() > 0.001
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst + '.tmp', 'wb') as f:
        if not compress and (w * h <= 512 * 256 or (w * h <= 512 * 512 and not name.startswith(('countdown', 'loading')))):
            f.write(b'NRT1' + struct.pack('<HHBBH', w, h, 8, 1, 1 if alpha else 0)); f.write(a.tobytes())
        else:
            f.write(b'NRT1' + struct.pack('<HHBBH', w, h, 5 if alpha else 1, 1, 1 if alpha else 0)); f.write(P.encode_dxt(im, alpha))
    os.replace(dst + '.tmp', dst)
    return name

def write_hud_meshes(P, game, outdir):
    out = bytearray(b'NRH1' + struct.pack('<I', len(HUD_MESHES)))
    for m in HUD_MESHES:
        v, idx, subs = P.load_msh(os.path.join(game, 'Meshes', m + '.msh'))
        out += m.encode()[:15].ljust(16, b'\0') + struct.pack('<II', len(v), len(idx))
        for k in range(len(v)):
            x, y, z = v['p'][k]; u, t = v['uv'][k]; c = v['c'][k]
            out += struct.pack('<4f4B', x, y, u, t, *[int(q) for q in c])
        out += idx.astype('<u2').tobytes()
        if len(idx) & 1: out += b'\0\0'
    open(os.path.join(outdir, 'ui', 'hud.bin'), 'wb').write(out)

def level_sets(game):
    sets = []
    for ls in sorted(glob.glob(os.path.join(game, 'LevelSets', '*.xml'))):
        try: r = ET.parse(ls).getroot()
        except Exception: continue
        s = dict(file=os.path.basename(ls), name=r.get('name') or os.path.basename(ls), desc='', levels=[])
        for e in r.findall('String'):
            if e.get('name') == 'description': s['desc'] = (e.text or '').strip()
        for l in r.iter('Level'):
            d = {e.get('name'): (e.get('value') if e.get('value') is not None else (e.text or '').strip()) for e in l}
            x = d.get('xml_file') or ''
            s['levels'].append(dict(name=l.get('name') or x, xml=os.path.splitext(x)[0], music=int(d.get('music') or 0),
                                    unlocked=int(d.get('unlocked') or 0), sd=int(d.get('sd_level') or 0),
                                    wings=int(d.get('wings_enabled') or 1), score=int(d.get('score_style') or 0),
                                    img=d.get('img_file') or ''))
        sets.append(s)
    return sets

ACH_NAMES = ['LAN Party', 'Outta Time', 'Great Scott!', 'Homage', 'Smash TV', 'Rampage', 'Rainbow Dash', 'Down, Not Out',
             'Top of the CORE', 'Boost Machine', 'Wheelie Master', 'Wheelie of Fortune', 'Ninja', 'Impermanence', 'Photo Finish',
             'Hardcore II', 'Hardcore I', 'Initiate', 'Made From Scratch', '360 Gamepad, Please', 'Better Handling', 'Victory Roll',
             'Aileron Roll', 'Still Alive', 'I Know Car Fu', 'Hat Trick', 'Conquistador', 'Terrible, Terrible Damage',
             'Do A Barrel Roll!', 'Bleep Bloop', 'Track Maniac', 'Make It Fast', '1337', 'Toasty', 'Tricks Make You Cool',
             'First!', 'High Speed', '88 MPH', 'Gravity? No Care.', 'Wall Ride', 'Wings Deployed', 'Rush Hour', 'Spinsanity',
             'No Hands', 'Wreckless', 'Self-Termination', 'Over 9000', '100 Grand', 'Million Point Mark']

def achievements(game):
    """Names and descriptions are kept in the game's executable (two string tables)."""
    exe = open(os.path.join(game, 'NitronicRush.exe'), 'rb').read()
    out = []
    i = exe.find(b'Played with at least one friend')
    descs = []
    if i > 0:
        seg = exe[i:exe.find(b'LAN Party\0', i)]
        descs = [s.decode('latin1') for s in seg.split(b'\0') if len(s) > 3]
    # the description table runs in reverse order with gaps for the ones built at run time;
    # match by the known pairs instead of position
    known = {
        'LAN Party': 'Played with at least one friend', 'Outta Time': 'Complete the "Clockwork"', 'Great Scott!': 'Complete the "Choochoo',
        'Homage': 'Complete the Gladiator', 'Smash TV': 'Smash 5 billboard', 'Rampage': 'Break 20 environmental',
        'Down, Not Out': 'Nearly fall out', 'Top of the CORE': 'Get to the top', 'Wheelie of Fortune': 'Do a Wheelie',
        'Impermanence': 'Reset all achievements', 'Initiate': 'Complete a tutorial', 'Made From Scratch': 'View the credits',
        '360 Gamepad, Please': 'Play using the keyboard', 'Better Handling': 'Play using a (360)', 'Still Alive': 'Complete a level,',
        'I Know Car Fu': 'Take out a CORE drone', 'Hat Trick': 'Land three tricks', 'Do A Barrel Roll!': 'Do A Barrel Roll.',
        'Bleep Bloop': 'Get any 10 other', 'Toasty': 'Overheat 3 times', 'Tricks Make You Cool': 'Land 100 tricks',
        'First!': 'Earn the top score', 'Gravity? No Care.': 'Drive upside-down', 'Wall Ride': 'Drive on a wall',
        'Wings Deployed': 'Open your wings', 'Rush Hour': 'Play for one hour', 'Wreckless': 'Die from a high-speed',
        'Self-Termination': 'Hard-reset the car', 'Over 9000': 'Land a trick worth', '100 Grand': 'Earn at least 100,000',
        'Million Point Mark': 'Earn 1,000,000'}
    for n in ACH_NAMES:
        d = next((x for x in descs if n in known and x.startswith(known[n])), '')
        if n == 'Self-Termination': d = 'Hard-reset the car.|(Press Select)'
        if n == 'Better Handling': d = 'Play using a gamepad.'
        if n == '360 Gamepad, Please': continue          # keyboard only
        if n == 'LAN Party': continue                    # no LAN play here
        out.append((n, d))
    return out

def menu_camera(game):
    r = ET.parse(os.path.join(game, 'Objects', 'menu_camera.xml')).getroot()
    pans = []
    for c in r.iter('CameraPanLogic'):
        vals = {}
        for e in c:
            nm = e.get('name') or ''
            if e.tag == 'Vector3': vals[nm] = [float(f.get('value')) for f in e.iter('Float')]
            elif e.tag in ('Float', 'Integer'): vals[nm] = float(e.get('value'))
        for k in 'ABCDE':
            if 'PosStart' + k in vals: pans.append((vals['PosStart' + k], vals['PosEnd' + k], vals.get('Speed' + k, 1.0)))
        pans_fade = vals.get('FadeSpeed', 2.0)
    return pans, pans_fade

def pack_ui(P, game, outdir):
    os.makedirs(os.path.join(outdir, 'ui'), exist_ok=True)
    n = 0
    for t in UI_TEXTURES:
        if write_ui_texture(P, t, outdir): n += 1
    sets = level_sets(game)
    for s in sets:
        for l in s['levels']:
            if l['img']:
                l['imgname'] = write_ui_texture(P, l['img'], outdir, 512, compress=True) or ''
                n += 1 if l['imgname'] else 0
    write_hud_meshes(P, game, outdir)
    with open(os.path.join(outdir, 'ui', 'levelsets.txt'), 'w') as f:
        for s in sets:
            f.write('set %s|%s\n' % (s['file'], s['name']))
            f.write('desc %s\n' % s['desc'].replace('\n', ' '))
            for l in s['levels']:
                f.write('level %s|%s|%d|%d|%d|%d|%d|%s\n' % (l['xml'], l['name'], l['music'], l['unlocked'], l['sd'], l['wings'], l['score'], l.get('imgname', '')))
    with open(os.path.join(outdir, 'ui', 'achievements.txt'), 'w') as f:
        for nme, d in achievements(game): f.write('%s|%s\n' % (nme, d))
    cr = os.path.join(game, 'data', 'credits.txt')
    if os.path.exists(cr): open(os.path.join(outdir, 'ui', 'credits.txt'), 'wb').write(open(cr, 'rb').read())
    pans, fade = menu_camera(game)
    with open(os.path.join(outdir, 'ui', 'menu.txt'), 'w') as f:
        f.write('fade %g\n' % fade)
        for a, b, sp in pans: f.write('pan %g %g %g %g %g %g %g\n' % (*a, *b, sp))
    print('ui: %d textures, %d level sets, HUD meshes, credits, achievements, menu camera (%d pans)' % (n, len(sets), len(pans)))

# ------------------------------------------------------------------ per-level extras
def _floats(e): return [float(f.get('value')) for f in e.iter('Float')]
def _named(comp, tag, name):
    for e in comp.findall(tag):
        if e.get('name') == name: return e
    return None

BP_COMPS = {}
def bp_components(game, name):
    if name in BP_COMPS: return BP_COMPS[name]
    res = {}
    p = os.path.join(game, 'Objects', name + '.xml')
    if os.path.exists(p):
        try:
            for c in ET.parse(p).getroot().find('GameObjectBlueprint').find('Components'): res[c.tag] = c
        except Exception: pass
    BP_COMPS[name] = res
    return res

def comp_values(c):
    d = {}
    if c is None: return d
    for e in c:
        nm = e.get('name') or e.tag
        if e.tag in ('Float', 'Integer', 'Boolean'): d[nm] = e.get('value')
        elif e.tag == 'String': d[nm] = (e.text or '').strip()
        elif e.tag.startswith('Vector') or e.tag.startswith('Matrix'): d[nm] = _floats(e)
        else: d[nm] = e.get('value') if e.get('value') is not None else (e.text or '').strip()
    return d

def level_extras(game, xml, skip=()):
    """sound emitters, boost rings, coolers and car dummies of a level, as .nrg lines. Objects in
    skip (GameObject ordinals) are handled by nr_objs, their sounds with them."""
    lines = []
    root = ET.parse(xml).getroot()
    for ordinal, g in enumerate(root.iter('GameObject')):
        bpn = g.get('blueprint') or ''
        comps = g.find('Components')
        if comps is None: continue
        bpc = bp_components(game, bpn)
        t = comps.find('Transform')
        pos, scl, ori = [0, 0, 0], [1, 1, 1], [1, 0, 0, 0, 1, 0, 0, 0, 1]
        if t is not None:
            e = _named(t, 'Vector3', 'position'); pos = _floats(e) if e is not None else pos
            e = _named(t, 'Vector3', 'scale'); scl = _floats(e) if e is not None else scl
            e = _named(t, 'Matrix3x3', 'orientation'); ori = _floats(e) if e is not None else ori
        R = np.array(ori, np.float32).reshape(3, 3)
        def comp(tag):
            c = comps.find(tag)
            base = comp_values(bpc.get(tag))
            if c is not None: base.update({k: v for k, v in comp_values(c).items() if v not in (None, '', 'None')})
            return base if (c is not None or tag in bpc) else None
        osr = comp('OneShotRadiusAudioLogic')      # plays when the car comes within the radius
        if osr and osr.get('Audio Name To Play') and osr.get('Audio Name To Play') != 'None':
            try: rad = float(osr.get('Radius', 25) or 25)
            except ValueError: rad = 25.0
            lines.append('sound %s 2 %.3f %.3f %.3f %.2f' % (osr['Audio Name To Play'], *pos, rad))
        ph = comp('Phantom')
        if ordinal in skip: ph = None
        if ph and ph.get('Event Name') and ph.get('Event Name') != 'None':
            lines.append('sound %s %d %.3f %.3f %.3f' % (ph['Event Name'], int(ph.get('Begin Playing?', '0') or 0), *pos))
        ring = comp('BoostRingLogic')
        if ring:
            ax = R[:, 1]     # ring meshes face along their local Y
            st = float(str(ring.get('boost_strength_', '120')).rstrip('f') or 120)
            heal = float(str(ring.get('heat_healed_', '10')).rstrip('f') or 10)
            lines.append('ring %.3f %.3f %.3f %.4f %.4f %.4f %.3f %.3f %g %g' % (*pos, *ax, max(scl), max(scl), st, heal))
        if bpn in ('CarDummy', 'CarDummyFly', 'CarDummy_Gold', 'CarDummyFly_Gold'):
            lines.append('dummy %s %.3f %.3f %.3f %.4f %.4f %.4f' % (bpn, *pos, *R[:, 1]))
    return lines
