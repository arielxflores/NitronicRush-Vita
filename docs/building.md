# Building the VPK

## Requirements

- [VitaSDK](https://vitasdk.org)
- [vitaGL](https://github.com/Rinnegatamante/vitaGL), [vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK),
  [SceShaccCgExt](https://github.com/bythos14/SceShaccCgExt) and
  [math-neon](https://github.com/Rinnegatamante/math-neon), installed into VitaSDK
- CMake

Release builds use vitaGL at commit `1a3b247`, built with `SINGLE_THREADED_GC=1` (GPU memory is freed on the main
thread; with vitaGL's garbage collector thread the game crashed in the allocator):

```sh
git clone https://github.com/Rinnegatamante/vitaGL.git
cd vitaGL
git checkout 1a3b247
make SINGLE_THREADED_GC=1 install
```

With GCC 15 and newer (C23 by default), vitaGL, vitaShaRK, math-neon and SceShaccCgExt need to be built as C17:
add `CC="arm-vita-eabi-gcc -std=gnu17"` to their `make` lines (`-DCMAKE_C_FLAGS=-std=gnu17` for SceShaccCgExt's
CMake build). vitaShaRK and SceShaccCgExt include `taihen.h`, which is in VitaSDK's `taihen` package.

## Building

```sh
cmake -S . -B build
cmake --build build
```

The VPK is written to `build/nitronic.vpk`. The game's version label (written to its log) can be set with
`-DNR_BUILD_LABEL=...`.

## Layout

| Folder | Contents |
| --- | --- |
| `src/` | Platform-independent code: the data packs, level objects, collision, the car's physics, tricks |
| `vita/` | The game on the Vita: rendering, sound, menus, HUD, camera, particles, ghosts, scores and the main loop |
| `vita/sce_sys/` | The LiveArea files |
| `converter/` | The data converter (Python): reads the PC version's files and writes the Vita's data packs |
| `tools/pc/` | The PC test build (see [testing.md](testing.md)) |
| `tools/make_converter_zip.py` | Makes the converter zip of the releases page |
| `docs/re/` | Notes on the PC version's code |
