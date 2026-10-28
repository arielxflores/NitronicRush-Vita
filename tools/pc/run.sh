#!/usr/bin/env bash
# One run of the PC test build (build/nrpc, see docs/testing.md).
#   tools/pc/run.sh DATA OUT [switch ...]
# DATA: a converted data folder (the converter's output/nitronic); OUT: a new folder for this run (the game's
# ux0:data/nitronic/ is OUT/data, its log OUT/data/boot.log). Switches are the game's switch files, for
# example autoplay. Environment variables of build/nrpc (NRPC_SCRIPT, NRPC_MAXT, NRPC_SHOTS ...) pass through;
# ASAN=1 runs build/nrpc_asan.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DATA="$(cd "${1:?data folder}" && pwd)"; OUT="${2:?output folder}"; shift 2
BIN="${ROOT}/build/nrpc"; [[ -n "${ASAN:-}" ]] && BIN="${ROOT}/build/nrpc_asan" && export ASAN_OPTIONS=detect_leaks=0
[[ -x "${BIN}" ]] || { echo "build it first: make -f Makefile.linux"; exit 1; }
mkdir -p "${OUT}/data"; OUT="$(cd "${OUT}" && pwd)"
for f in "${DATA}"/*; do ln -sf "${f}" "${OUT}/data/$(basename "${f}")"; done
touch "${OUT}/data/rgba_tex"            # (Mesa has no DXT textures: the PC build uses the RGBA path)
for s in "$@"; do touch "${OUT}/data/${s}"; done
export NRPC_OUT="${OUT}" NRPC_FRAMES="${OUT}/frames.csv"
cd "${OUT}"
set +e; "${BIN}" > "${OUT}/nrpc.txt" 2> "${OUT}/stderr.txt"; code=$?; set -e
echo "exit ${code}; log: ${OUT}/data/boot.log"
exit "${code}"
