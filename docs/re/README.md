# Notes on NitronicRush.exe

These are the working notes made while reverse-engineering the PC version (`NitronicRush.exe` of the
2012-12-21 Windows release) for the parts of the game its data files don't describe: the level objects' logic,
the camera, the race flow, the scoring, the ghost recordings, the shaders and the sound events. The
comments in `src/` and `vita/` refer to them by file (`re_K.md`) and to the PC's functions by their addresses.

| File | Contents |
| --- | --- |
| `re_A.md` | Collisions, hazards, breakable objects |
| `re_B.md` | Things that move |
| `re_C.md` | Lights and glowing objects |
| `re_D.md` | Hazards that attack the player (air mines, shard buildings, barrels) |
| `re_E.md` | The story finale (Campaign levels 5 and 6, the ending) |
| `re_F1.md` | Logic components of the bonus and custom levels |
| `re_F2.md` | Triggers, particles, other components |
| `re_G.md` | The material shaders |
| `re_H.md` | Achievements and their exact conditions |
| `re_I.md` | Stunt mode, the results screen and the local scores |
| `re_J.md` | The Humvee enemy |
| `re_K.md` | The chase camera |
| `re_L.md` | The credits text |
| `re_M.md` | A note on one object in Campaign level 6 |
| `re_N_ghosts.md` | Ghost cars (ReplayManager) and their file format |
| `compids.txt` | Component type ids |
| `wwise_hash.txt` | Wwise event names and their hashes |

Paths such as `~/nrw/` in the notes refer to disassembly dumps and helper scripts that were kept locally and are
not part of this repository. Addresses are virtual addresses in that executable.
