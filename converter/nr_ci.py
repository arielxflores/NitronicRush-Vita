"""Case-insensitive file names on Linux and macOS, and Linux line ends on Windows.

The game was made on Windows, where "Blimp.msh" and "blimp.msh" are the same file, and its data names its files
in any case. On a case-sensitive file system a name that does not exist as written is looked up again here,
one folder at a time, ignoring case. On Windows, text files are written with "\n" line ends (not "\r\n"), so the
data comes out the same on every system. Imported first by nr_pack.py and nr_audio.py."""
import os, builtins

_open, _stat = builtins.open, os.stat
_dirs = {}


def resolve(p):
    if os.name == 'nt' or not isinstance(p, str):
        return p
    try:
        _stat(p)
        return p
    except OSError:
        pass
    ap = os.path.abspath(p)
    cur = os.sep
    for comp in ap.split(os.sep)[1:]:
        if not comp:
            continue
        nxt = os.path.join(cur, comp)
        try:
            _stat(nxt)
            cur = nxt
            continue
        except OSError:
            pass
        ents = _dirs.get(cur)
        if ents is None:
            try:
                ents = {e.lower(): e for e in os.listdir(cur)}
            except OSError:
                return p
            _dirs[cur] = ents
        e = ents.get(comp.lower())
        if e is None:
            return p
        cur = os.path.join(cur, e)
    return cur


def _ci_open(file, mode='r', *a, **k):
    if isinstance(file, str) and not any(c in mode for c in 'wax+'):
        file = resolve(file)
    return _open(file, mode, *a, **k)


def _ci_stat(p, *a, **k):
    return _stat(resolve(p) if isinstance(p, str) else p, *a, **k)


def _lf_open(file, mode='r', *a, **k):
    if 'b' not in mode and any(c in mode for c in 'wax') and len(a) < 4 and 'newline' not in k:
        k['newline'] = '\n'
    return _open(file, mode, *a, **k)


if os.name != 'nt':
    builtins.open = _ci_open
    os.stat = _ci_stat
else:
    builtins.open = _lf_open
