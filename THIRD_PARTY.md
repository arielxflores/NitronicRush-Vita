# Third-party code and notices

The port as a whole is distributed under the GNU GPL v3 (see `LICENSE`). It contains or uses the work below.

## Included

| Library | Where | Author | License |
| --- | --- | --- | --- |
| [stb_vorbis](https://github.com/nothings/stb) | `vita/third_party/stb_vorbis.c` | Sean Barrett | MIT or public domain |
| [tinf](https://github.com/jibsen/tinf) (inflate) | `src/tinf/` | Joergen Ibsen | zlib |
| [ww2ogg](https://github.com/hcs64/ww2ogg) | `converter/third_party/ww2ogg/` | Adam Gashlin (hcs), Xiph.org Foundation | BSD 3-Clause |

The license text of each is kept with its files (`src/tinf/LICENSE`, `converter/third_party/ww2ogg/COPYING`, the
end of `stb_vorbis.c`).

## Linked (Vita build)

| Library | Author | License |
| --- | --- | --- |
| [vitaGL](https://github.com/Rinnegatamante/vitaGL) | Rinnegatamante | GPL v3 |
| [vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK) | Rinnegatamante | LGPL v3 |
| [SceShaccCgExt](https://github.com/bythos14/SceShaccCgExt) | bythos | GPL v3 |
| [math-neon](https://github.com/Rinnegatamante/math-neon) | Lachlan Tychsen-Smith | LGPL v3 |
| [VitaSDK](https://vitasdk.org) newlib, libstdc++ and stubs | VitaSDK contributors | various open source |

## Used by the converter

| Tool | Author | Notes |
| --- | --- | --- |
| [wwiser](https://github.com/bnnm/wwiser) | bnnm | Reads the game's Wwise sound bank. Not included: the converter downloads it from its GitHub releases. |
| [numpy](https://numpy.org), [Pillow](https://python-pillow.org) | their contributors | BSD-3-Clause, MIT-CMU. Installed by the converter, and included in the Windows converter zip. |
| [Python](https://www.python.org) | Python Software Foundation | PSF License. The embeddable Windows build is included in the converter zip. |

## Game content

None is included in this repository or in the VPK. The converter makes the Vita's data files from the user's own
copy of Nitronic Rush for PC: the levels, models, textures, fonts, menus, ghosts, music and sounds, all © 2011
DigiPen (USA) Corporation. The game's sound bank was made with Audiokinetic Wwise.

The LiveArea icon, background art and logo (`vita/sce_sys/`) are Nitronic Rush's own artwork.
