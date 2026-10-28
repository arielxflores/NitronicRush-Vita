#!/usr/bin/env python3
"""Makes the converter zip for the releases page: NitronicRush-Vita-Converter.zip, the converter/ folder
plus, for Windows, an embedded Python with numpy and Pillow and ww2ogg.exe, so that nothing has to be
installed there. Runs on Linux (or WSL) with Python 3 and pip, and downloads the Windows parts.

usage: python3 tools/make_converter_zip.py [--out DIR]
(the Python running it needs pip, for example a venv: python3 -m venv .venv && .venv/bin/python tools/make_converter_zip.py)
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

PYTHON = 'https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip'
WW2OGG = 'https://github.com/hcs64/ww2ogg/releases/download/0.24/ww2ogg024.zip'
WHEELS = ['numpy', 'pillow']
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NAME = 'NitronicRush-Vita-Converter'


def get(url, dest):
    if not os.path.exists(dest):
        print('downloading', url, flush=True)
        with urllib.request.urlopen(url, timeout=600) as r, open(dest + '.part', 'wb') as f:
            shutil.copyfileobj(r, f)
        os.replace(dest + '.part', dest)
    return dest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(ROOT, 'build', 'converter'))
    a = ap.parse_args()
    dl = os.path.join(a.out, 'downloads')
    stage = os.path.join(a.out, NAME)
    os.makedirs(dl, exist_ok=True)
    if os.path.isdir(stage):
        shutil.rmtree(stage)

    # the converter itself (without build leftovers)
    def skip(folder, names):   # build leftovers (not the ww2ogg folder itself: only a built ww2ogg program in it)
        return [n for n in names if n in ('__pycache__', '.venv', 'output', 'wwiser.pyz') or n.endswith(('.pyc', '.o'))
                or (n == 'ww2ogg' and os.path.isfile(os.path.join(folder, n)))]
    shutil.copytree(os.path.join(ROOT, 'converter'), stage, ignore=skip)
    with zipfile.ZipFile(get(WW2OGG, os.path.join(dl, os.path.basename(WW2OGG)))) as z:
        with z.open('ww2ogg.exe') as s, open(os.path.join(stage, 'third_party', 'ww2ogg', 'ww2ogg.exe'), 'wb') as d:
            shutil.copyfileobj(s, d)

    # Python for Windows, with the converter's folder on its path
    py = os.path.join(stage, 'python')
    with zipfile.ZipFile(get(PYTHON, os.path.join(dl, os.path.basename(PYTHON)))) as z:
        z.extractall(py)
    pth = glob.glob(os.path.join(py, 'python3*._pth'))[0]
    with open(pth, 'w', newline='\r\n') as f:
        f.write('%s\n.\nLib\\site-packages\n..\nimport site\n' % os.path.basename(pth).replace('._pth', '.zip'))
    if subprocess.run([sys.executable, '-m', 'pip', '--version'], capture_output=True).returncode != 0:
        sys.exit('pip is not available in this Python; run this with a venv: python3 -m venv .venv && .venv/bin/python %s'
                 % os.path.relpath(__file__))
    whl = os.path.join(dl, 'wheels')
    subprocess.run([sys.executable, '-m', 'pip', 'download', '-q', '--only-binary=:all:', '--platform', 'win_amd64',
                    '--python-version', '3.12', '--implementation', 'cp', '-d', whl] + WHEELS, check=True)
    site = os.path.join(py, 'Lib', 'site-packages')
    for w in sorted(glob.glob(os.path.join(whl, '*.whl'))):
        with zipfile.ZipFile(w) as z:
            z.extractall(site)

    zp = os.path.join(a.out, NAME + '.zip')
    with zipfile.ZipFile(zp, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for dp, dn, fn in os.walk(stage):
            dn.sort()
            for f in sorted(fn):
                p = os.path.join(dp, f)
                zi = zipfile.ZipInfo.from_file(p, os.path.relpath(p, a.out))
                if f.endswith('.sh'):
                    zi.external_attr = 0o100755 << 16
                with open(p, 'rb') as s:
                    z.writestr(zi, s.read(), zipfile.ZIP_DEFLATED, 9)
    print('%s: %.0f MB' % (zp, os.path.getsize(zp) / 1e6))


if __name__ == '__main__':
    main()
