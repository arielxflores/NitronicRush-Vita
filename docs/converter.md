# The data converter

The Vita version reads its levels, models, textures, menus, ghosts, music and sounds from data packs made from the
PC version's files. `converter/make_vita_data.py` makes them; the README's [Game data](../README.md#game-data)
section has the short version.

```
python3 converter/make_vita_data.py "/path/to/Nitronic Rush" [output folder]
```

(`convert.sh` and `CONVERT (Windows).bat` run the same thing, after checking for Python's packages.) The default
output folder is `converter/output`. The finished data is `output/nitronic` (about 400 MB): `nitronic.idx`, which
lists the packs, and the `.pak` files. The `build` folder next to it holds the converted files before they're
packed and can be deleted once the data is on the Vita; keeping it makes a later run much faster.

## What it does

1. **Sounds and music** (`nr_audio.py`): the game's Wwise sound bank (`Sounds/Nitronic/GeneratedSoundBanks/
   Windows/Main.bnk`) is read with [wwiser](https://github.com/bnnm/wwiser), downloaded the first time from its
   GitHub releases, and flattened into the events, music and mixer settings the Vita's sound player needs. Every
   sound is converted from Wwise Vorbis to Ogg Vorbis with [ww2ogg](https://github.com/hcs64/ww2ogg).
2. **Everything else** (`nr_pack.py` and the modules it uses): the levels and their objects (`nr_objs.py`), level
   geometry merged per material, collision meshes, the lighting (`nr_light.py`), textures compressed to DXT with
   mipmaps, the cars and their moving parts, particles and shader settings (`nr_fx.py`), fonts, menus and HUD art
   (`nr_ui.py`), the level sets, ghost recordings and default high scores.

The result is the same on every run and every system, so a pack that hasn't changed comes out byte for byte the
same.

## The game files

The converter needs an installed (or unpacked) copy of Nitronic Rush for PC: the folder with `NitronicRush.exe`,
`Levels`, `Objects`, `Textures`, `Meshes`, `Materials`, `LevelSets` and `Sounds`. The game is free from
[DigiPen's game gallery](https://games.digipen.edu/games/nitronic-rush); the
[Internet Archive](https://archive.org/details/nitronic-rush) has the installer (`NitronicRush_setup_20121221.2.exe`)
and the installed game as `Nitronic Rush.rar`. On Linux and macOS, unpack the .rar, or the installer with
innoextract. File names are matched
without regard to case, as Windows does.

## Troubleshooting

- **"does not look like a Nitronic Rush folder"**: give the folder that contains `NitronicRush.exe`, not the one
  above it.
- **"could not download wwiser"**: the first run needs internet access to fetch it. Download `wwiser.pyz` from
  <https://github.com/bnnm/wwiser/releases> and put it in `converter/third_party/`, then run the converter again.
- **"ww2ogg could not run here"** (Linux, macOS): build it with `make -C converter/third_party/ww2ogg` (it needs
  a C++ compiler: `build-essential` on Debian/Ubuntu, the Xcode command line tools on macOS). `convert.sh` does
  this by itself.
- **numpy or Pillow missing**: `convert.sh` installs them into `converter/.venv`. If Python's `venv` module is
  missing (Debian/Ubuntu), install `python3-venv`, or the packages themselves: `sudo apt install python3-numpy
  python3-pil`.
- If it stops for any other reason, run it again: finished steps are kept and it continues where it was.
