#!/bin/bash
# cgcheck.sh DATA OUT ["LEVEL TIME[,TIME]" ...]: checks the game's fragment shaders as the switch file half_all has
# the Vita build them - in Cg with their colour work in half precision (vita/gfx.c: cg_half) - on the PC:
#   1. the Linux build saves every program's sources and the Cg made of its fragment shader (SHDUMP);
#   2. NVIDIA's Cg compiler (cgc 3.1, from the Cg Toolkit; CGC=/path/to/cgc if not in PATH) compiles each one - the
#      Vita's compiler grew out of it, so what it turns down the Vita would - into GLSL ES;
#   3. the game is drawn with that GLSL (CGFS) and without, at the places given (default: Campaign_Level1 at 14 s), and
#      the pictures compared: the Cg says the same as the GLSL it was made from when they match (a step of a colour
#      here and there is the order of the sums).
# NRPC_BIN: the game's Linux build (default build/nrpc).
set -u
D=${1:?the converted data folder}; O=${2:?a folder for the check}; shift 2
H=$(cd "$(dirname "$0")" && pwd); CGC=${CGC:-cgc}
[ $# -gt 0 ] || set -- "Campaign_Level1 14"
rm -rf "$O"; mkdir -p "$O/sh" "$O/dump/data"
echo Campaign_Level1 > "$O/dump/data/autoplay_level"
SHDUMP="$O/sh" AUTOPLAY_NOMENU=1 NRPC_DET=1 NRPC_MAXT=2 NOBENCH=1 "$H/run.sh" "$D" "$O/dump" autoplay > /dev/null 2>&1
n=0; bad=0
for f in "$O"/sh/*.cg; do
  b=${f%.cg}; n=$((n+1))
  if ! "$CGC" -q -profile glesf -entry main "$f" > "$b.gles0" 2> "$b.err" || grep -qi "error" "$b.err"; then
    echo "turned down: $(basename "$b")"; head -3 "$b.err"; bad=$((bad+1)); rm -f "$b.gles0"; continue
  fi
  sed -E 's/\b_(u[A-Za-z0-9]+)\b/\1/g' "$b.gles0" > "$b.gles"       # (cgc puts a _ before the uniforms' names)
done
echo "$n fragment shaders in Cg, $bad turned down by cgc; $(ls "$O"/sh/*.nocg 2>/dev/null | wc -l) stay GLSL"
for place in "$@"; do
  L=${place%% *}; T=${place#* }; MAXT=$(python3 -c "print(int(max(float(x) for x in '$T'.split(','))+20))")
  for who in glsl cg; do
    R="$O/$L/$who"; mkdir -p "$R/data"; echo "$L" > "$R/data/autoplay_level"; touch "$R/data/autoplay_all"
    ( [ $who = cg ] && export CGFS="$O/sh"
      NODRAW=1 NOBENCH=1 AUTOPLAY_NOMENU=1 NRPC_DET=1 NRPC_MAXT=$MAXT SHOTRT=$T "$H/run.sh" "$D" "$R" autoplay > /dev/null 2>&1 ) &
  done; wait
  grep -a "did not build" "$O/$L/cg/data/boot.log" | head -5
  python3 - "$O/$L" <<'PY'
import sys, glob, os
def ppm(p):                                    # P6, width, height, 255, then one byte of white space and the pixels
    b = open(p, 'rb').read(); at = 0; tok = []
    while len(tok) < 4:
        while b[at:at + 1].isspace(): at += 1
        e = at
        while not b[e:e + 1].isspace(): e += 1
        tok.append(b[at:e]); at = e
    return int(tok[1]), int(tok[2]), b[at + 1:]
d = sys.argv[1]; fs = sorted(glob.glob(d + '/glsl/*_rt*.ppm'))
if not fs: print(os.path.basename(d) + ': no pictures')
for f in fs:
    g = f.replace('/glsl/', '/cg/')
    if not os.path.exists(g): print(os.path.basename(f), 'missing with the Cg shaders'); continue
    w, h, a = ppm(f); _, _, b = ppm(g)
    n1 = n2 = mx = 0
    for i in range(0, w * h * 3, 3):
        e = max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]))
        if e: n1 += 1; mx = max(mx, e); n2 += e > 2
    print(f"{os.path.basename(f)}: {n1} px differ (by at most {mx}; {n2} by more than 2)")
PY
done
