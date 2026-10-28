# Testing

## Linux build

`Makefile.linux` builds the same game code (`vita/`, `src/`) for Linux, with the Vita calls it makes stood in for
by `tools/pc/nrpc.c`. It draws off-screen with EGL and OpenGL ES 2 (Mesa's llvmpipe is enough) and opens no window:
input comes from a script, and it writes the game's log, pictures and a mix of the sound. The game's clock advances
1/60 s per frame however fast the PC draws, so a run gives the same results every time.

```sh
sudo apt install gcc make libegl-dev libgles-dev
make -f Makefile.linux                 # build/nrpc;  ASAN=1 builds build/nrpc_asan with AddressSanitizer
tools/pc/run.sh converter/output/nitronic /tmp/run1 autoplay
```

`run.sh` takes the converted data folder, a new folder for the run, and the game's switch files. The game's
`ux0:data/nitronic/` is `<run>/data/`, so its log is `<run>/data/boot.log`. Mesa has no DXT texture support, so the
Linux build always uses uncompressed textures (the `rgba_tex` switch).

Environment variables (all optional):

| Variable | Meaning |
| --- | --- |
| `NRPC_SCRIPT` | An input script (the syntax is at the top of `tools/pc/nrpc.c`) |
| `NRPC_MAXT` | Quit after this many seconds of game time |
| `NRPC_SHOTS` | Save a small picture every N seconds (`<run>/thumbs/`) |
| `NRPC_WAV` | Write the mixed sound to this WAV file |
| `NRPC_CHECK` | Report black frames |
| `AUTOPLAY_NOMENU`, `AUTOPLAY_FROM=N`, `AUTOPLAY_ALL` | With `autoplay`: skip the menu tour, start at level N of the story set, or drive every level set |

## Switch files

Empty files with these names in `ux0:data/nitronic/` (or `<run>/data/`) change how the game starts:

| File | Effect |
| --- | --- |
| `autoplay` | Tours the menus, then the computer drives each story level along the developers' recorded run, the ending and the credits, logging frame times and a GPU benchmark on Campaign level 5; then the game quits |
| `autoplay_shots` | With `autoplay`: saves pictures of the start and finish of each race, and the first sparks and wing trail, in `test_shots/` |
| `no_post`, `no_bloom` | Draw without the screen effects, or without the glow |
| `no_sound` | Don't start the sound engine |
| `no_mips`, `rgba_tex`, `no_fog`, `no_cull`, `no_cutout`, `no_tex_cache`, `discard_cutout`, `no_transfer_wait`, `cg_shaders`, `msaa2x`, `msaa4x` | Graphics fallbacks used while porting |

The log of every session is `boot.log`, which ends with a line per race (`RESULT ...`) and the effects it made.
