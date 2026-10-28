#!/usr/bin/env bash
# Nitronic Rush Vita: makes the game data for the Vita from your PC copy of Nitronic Rush (Linux, macOS).
#
#   bash convert.sh "/path/to/Nitronic Rush"
#
# Needs Python 3.8 or newer. numpy and Pillow are installed into a private folder (converter/.venv) the
# first time if Python doesn't already have them; ww2ogg is built from third_party/ww2ogg if needed
# (that needs a C++ compiler: build-essential on Debian/Ubuntu, the Xcode command line tools on macOS).
set -uo pipefail
cd "$(dirname "$0")" || exit 1
if [[ -z "${1:-}" ]]; then echo 'usage: bash convert.sh "/path/to/Nitronic Rush"'; exit 1; fi
PY=python3
command -v "${PY}" >/dev/null || { echo "Python 3 is not installed (Debian/Ubuntu: sudo apt install python3)"; exit 1; }
if ! "${PY}" -c "import numpy, PIL" 2>/dev/null; then
  if [[ ! -x .venv/bin/python ]]; then
    echo "== setting up numpy and Pillow in converter/.venv"
    "${PY}" -m venv .venv || { echo "STOPPED: could not make a Python venv. On Debian/Ubuntu: sudo apt install python3-venv"
                               echo "  (or install the packages instead: sudo apt install python3-numpy python3-pil)"; exit 1; }
  fi
  PY=.venv/bin/python
  "${PY}" -c "import numpy, PIL" 2>/dev/null || "${PY}" -m pip install --quiet --disable-pip-version-check numpy pillow \
    || { echo "STOPPED: could not install numpy and Pillow (see above)"; exit 1; }
fi
W=third_party/ww2ogg
if ! { [[ -x "${W}/ww2ogg" ]] && "./${W}/ww2ogg" 2>&1 | grep -q "converter 0"; }; then
  echo "== building ww2ogg"
  make -C "${W}" >/dev/null || { echo "STOPPED: building ww2ogg failed (a C++ compiler is needed: sudo apt install build-essential)"; exit 1; }
fi
exec "${PY}" make_vita_data.py "$1" "$(pwd)/output"
