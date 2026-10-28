#!/usr/bin/env python3
"""Nitronic Rush for PS Vita: the data converter.

Turns an installed copy of Nitronic Rush for PC (free from DigiPen) into the files the Vita
game reads. Nothing from the game is included with the Vita build; this makes them from yours.

    python make_vita_data.py "<your Nitronic Rush folder>" [output folder]

(On Windows you can drag the game folder onto "CONVERT (Windows).bat" instead.)

When it is done, copy the folder it names ("nitronic") into ux0:data/ on your Vita, so that the
files end up in ux0:data/nitronic/. It takes a while (about 10-40 minutes, depending on the PC).
If it is stopped, run it again: it carries on where it stopped.
"""
import os, sys, subprocess, shutil, time, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
WWISER_URL = 'https://github.com/bnnm/wwiser/releases/latest/download/wwiser.pyz'


def say(msg=''):
    print(msg, flush=True)


def stop(msg):
    say()
    say('STOPPED: ' + msg)
    sys.exit(1)


def check_python():
    if sys.version_info < (3, 8):
        stop('Python 3.8 or newer is needed (this is %d.%d). Get it from https://www.python.org/downloads/' % sys.version_info[:2])
    missing = []
    try:
        import numpy  # noqa: F401
    except ImportError:
        missing.append('numpy')
    try:
        import PIL  # noqa: F401
    except ImportError:
        missing.append('pillow')
    if missing:
        stop('these Python packages are missing: %s\n  install them with:  %s -m pip install %s'
             % (' '.join(missing), os.path.basename(sys.executable), ' '.join(missing)))


def check_game(game):
    need = ['Levels', 'Objects', 'Textures', 'Meshes', 'Materials', 'LevelSets',
            os.path.join('Sounds', 'Nitronic', 'GeneratedSoundBanks', 'Windows', 'Main.bnk')]
    miss = [n for n in need if not os.path.exists(os.path.join(game, n))]
    if miss:
        stop('"%s" does not look like a Nitronic Rush folder (missing: %s).\n'
             '  Give the folder the game was installed to (the one with NitronicRush.exe in it).' % (game, ', '.join(miss)))


def find_ww2ogg():
    d = os.path.join(HERE, 'third_party', 'ww2ogg')
    exe = os.path.join(d, 'ww2ogg.exe' if os.name == 'nt' else 'ww2ogg')
    if os.path.exists(exe):
        if os.name != 'nt':
            try:
                os.chmod(exe, 0o755)
            except OSError:
                pass
        try:
            subprocess.run([exe], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return exe
        except OSError:
            pass
    stop('ww2ogg could not run here. On Windows it is third_party/ww2ogg/ww2ogg.exe; on other systems build it:\n'
         '  cd "%s" && make' % d)


def find_wwiser(work):
    for p in (os.path.join(HERE, 'third_party', 'wwiser', 'wwiser.py'), os.path.join(HERE, 'third_party', 'wwiser.pyz'),
              os.path.join(work, 'wwiser.pyz')):
        if os.path.exists(p):
            return p
    dst = os.path.join(HERE, 'third_party', 'wwiser.pyz')
    say('  downloading wwiser (reads the game\'s Wwise sound bank) from %s' % WWISER_URL)
    try:
        with urllib.request.urlopen(WWISER_URL, timeout=60) as r, open(dst + '.part', 'wb') as f:
            shutil.copyfileobj(r, f)
        os.replace(dst + '.part', dst)
        return dst
    except Exception as e:
        stop('could not download wwiser (%s).\n  Download wwiser.pyz yourself from https://github.com/bnnm/wwiser/releases\n'
             '  and put it in "%s", then run this again.' % (e, os.path.join(HERE, 'third_party')))


def run(step, args):
    t = time.time()
    say()
    say('== %s' % step)
    env = dict(os.environ, PYTHONUTF8='1', PYTHONIOENCODING='utf-8')   # (Windows writes the same text files as Linux)
    r = subprocess.run([sys.executable] + args, cwd=HERE, env=env)
    if r.returncode != 0:
        stop('%s failed (see the lines above). Run this again to retry; finished parts are kept.' % step)
    say('   (%.0f s)' % (time.time() - t))


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ('-h', '--help'):
        say(__doc__)
        sys.exit(0 if len(sys.argv) > 1 else 1)
    game = os.path.abspath(sys.argv[1].strip().strip('"'))
    out = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.path.join(HERE, 'output')
    build = os.path.join(out, 'build')
    work = os.path.join(build, 'work')
    final = os.path.join(out, 'nitronic')
    t0 = time.time()
    say('Nitronic Rush for PS Vita - data converter')
    say('  game:   %s' % game)
    say('  output: %s' % out)
    check_python()
    check_game(game)
    os.makedirs(work, exist_ok=True)
    ww2ogg = find_ww2ogg()

    # 1. the sounds: the Wwise bank's events and music, every sound as Ogg Vorbis
    snd_done = os.path.join(work, 'snd', 'sound.nsb')
    if not os.path.exists(snd_done):
        wwiser = find_wwiser(work)
        run('1/2 sounds and music (a few minutes)', ['nr_audio.py', game, work, '--ww2ogg', ww2ogg, '--wwiser', wwiser])
    else:
        say()
        say('== 1/2 sounds and music: already done')

    # 2. the levels, textures, cars, fonts, menus and replays, written into the packs
    run('2/2 levels, textures and the rest (the long part)', ['nr_pack.py', game, build, '--sets', 'all', '--resume'])

    # the packs nitronic.idx lists -> output/nitronic
    idx = os.path.join(build, 'nitronic.idx')
    if not os.path.exists(idx):
        stop('no nitronic.idx was written (see the lines above)')
    names = [l.strip() for l in open(idx) if l.strip()]
    os.makedirs(final, exist_ok=True)
    for f in os.listdir(final):
        if f.endswith('.pak') and f not in names:
            os.remove(os.path.join(final, f))
    total = 0
    for n in names + ['nitronic.idx']:
        shutil.copyfile(os.path.join(build, n), os.path.join(final, n))
        total += os.path.getsize(os.path.join(final, n))
    say()
    say('DONE in %.0f minutes: %d files, %.0f MB in' % ((time.time() - t0) / 60, len(names) + 1, total / 1e6))
    say('  %s' % final)
    say()
    say('Copy that "nitronic" folder into ux0:data/ on your Vita (so you have ux0:data/nitronic/nitronic.idx),')
    say('then install the VPK. The "build" folder next to it is only needed to run the converter again.')


if __name__ == '__main__':
    main()
