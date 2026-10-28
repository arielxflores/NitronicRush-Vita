Nitronic Rush Vita - data converter
===================================

The PS Vita port of Nitronic Rush (https://github.com/arielxflores/NitronicRush-Vita) needs the game's data,
which can't be distributed with it. This converter makes it on your PC from your own copy of Nitronic Rush for
PC, which DigiPen gives away for free:
    https://games.digipen.edu/games/nitronic-rush
    (or the Internet Archive copy: https://archive.org/details/nitronic-rush)

1. Install Nitronic Rush for PC (or unpack it anywhere).
2. Windows: drag your Nitronic Rush folder (the one with NitronicRush.exe in it) onto
   "CONVERT (Windows).bat". Python is included in this zip, nothing needs to be installed.
   Linux / macOS: bash convert.sh "/path/to/Nitronic Rush"
   (needs Python 3 and a C++ compiler; it installs numpy and Pillow into a private folder.)
   It takes about 5 to 30 minutes and needs about 1.5 GB of free space. The first run downloads wwiser
   (https://github.com/bnnm/wwiser) to read the game's sound bank. If it stops, run it again: it
   continues where it was.
3. When it says DONE, copy the "nitronic" folder inside "output" to ux0:data/ on your Vita with VitaShell
   (USB or FTP), so that you have ux0:data/nitronic/nitronic.idx.
4. Install NitronicRush-Vita.vpk if you haven't yet, and start the game.

Windows may warn about running a .bat file downloaded from the internet: choose "More info", then
"Run anyway". The "build" folder inside "output" can be deleted afterwards (keeping it makes a second run
faster).

Included: ww2ogg 0.24 by hcs (BSD license, see third_party/ww2ogg/COPYING) and, in the zip from the releases page, Python
3.12 for Windows (python.org, PSF license) with numpy and Pillow.
