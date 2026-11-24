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
| `SND_EVLOG` | Every sound event posted, with the sound clock's time (stderr, `EV` lines); with `SND_EVACT` too, each event's actions the first time (`EVACT`: kind - 1 play, 2 stop -, target, fade, curve) |
| `SNDFLICKER=scrape\|skid\|crash[,N]` | From 5 to 10 s of race time, the scrape or skid sound switched on and off every N frames (or a small crash posted every N frames), as a car grazing a wall would have it |
| `BENCHPIC=dir` | Each benchmark way's finished picture, as `dir/bench_NN.ppm` |
| `NRPC_CHECK` | Report black frames |
| `AUTOPLAY_NOMENU`, `AUTOPLAY_FROM=N`, `AUTOPLAY_ALL` | With `autoplay`: skip the menu tour, start at level N of the story set (`1i`: the story's intro first), or drive every level set |
| `NOBENCH` | No GPU benchmark at its built-in places (Campaign level 5 at 2 s) |
| `FRAGAT=t[,t...]` | At these race times, count the pixels each draw into the 3D scene has the GPU shade, by what drew it (stderr, `FRAGS` lines; `FRAGTRIS`: the same by triangles). With `FDPIX=x,y`, which solid draw is seen at that point of the picture; with `FDPOST=1`, the screen passes' render targets saved as pictures; `PSTAT` lines: the short-lived particles alive by distance from the camera, how many and how many screens' worth of pixels (before the near shrink and the budget) |
| `SHOTRT=t[,t...]` | Save a picture at these race times |
| `GFXDBG=n` | `vita/gfx.c`'s debug switches (`gfx_dbg`: 1 no glowing level parts, 4 no level geometry, 8 / 16 no glowing / solid objects, 64 the old screen passes) |
| `GHNOMERGE=1` | Ghost cars in the very same place drawn one by one (and black ones drawn) |
| `MODELSTAT=1` | Each level and model loaded: its vertices and triangles (stderr) |
| `FRAGEVERY=s` | The pixel counts (as `FRAGAT`) every s seconds of race time, a `FRAGLINE` each, and a `LIGHTLINE`: the moving objects' triangles drawn by how many of the level's lights reach them (0 to 4) |
| `OBJHASH=t` | With `STATEHASH`'s runs: a hash of all the moving objects' state every 30 frames (`OBJHASH` lines), and each object's own at race time t (`OBJHASH1`): where two runs part |
| `TEXTRIM=1` | Let go of every texture and object model no loaded level uses before each level loads (the Vita does it while less than 40 MB of video memory is free): the pictures must not change |
| `GHOSTCAR=name` | The ghost cars drawn with another car's ghost body (e.g. `DeloreanGhost`) |
| `SPHREC=file` | Save the level collision's sphere queries that leave the moving objects out (the barrels', the gibs') for `collbench` |
| `PCHECK=1` | Report (stderr, `PCHECK` lines, the first 40) any particle made with a number that is not finite or is past 100000 (where, when, its speed, size and slices): what the Vita's GPU could choke on that the PC's would only leave out |
| `ALLLIGHTS=1` | The moving objects drawn with the four-light shaders whatever lights reach them (the programs before those for 0, 1 or 2 lights: the pictures must match) |
| `SLOWLOGIC=ms`, `SLOWALL=1` | Log the race frames whose logic takes over ms (the Vita: 6), where it went (`slow logic`); with `SLOWALL`, every one (not at most two a second). The times are the thread's CPU time |
| `OBJPROF=1` | With each `slow logic` line, the object kinds (`nr_objs.h`'s `OK_` order) that took the most |
| `STATEHASH=1` | Every 30 race frames (`STATEHASH=2`: every frame) a hash of the car's position, speed, spin and turn, by level: two builds driven the same way (`chase`) can be compared for the very same physics |
| `DYNREF=1` | The moving objects' triangles looked at whole for each query (no grid, no box in their own space): the reference for the faster way |
| `NOSUB=1` | The level's dense collision cells looked at whole (not by their parts): the reference for the faster way |
| `NODRAW=1` | The race and the cinematics not drawn (the logic, sound and timings run as usual): fast runs of the physics |
| `RUNLOG=1` | Each big part's runs: how many, their size against the part's |
| `FSHIGHP=1` | The fragment shaders at full precision (Mesa runs `mediump` at half precision; the Vita doesn't): to tell a precision difference from a real one |
| `CAMSTICK=t` | From race time t (negative: earlier), the right stick moved in a 14 s pattern (right, half left, up, down, circles; with the stick's one-step wobble), as the free look with Advanced Controls gets it |
| `CAMVIEW=n` | The race camera starts in the Far (1) or Bumper (2) view |
| `CAMLOG=file` | The camera every frame in a race (CSV: time, race time, wings, fly and tumble blends, eye, forward, up, car position, the free look's stick; then a `B,` line: the chase camera's own forward, the look's turn and tilt, how far out the walls let it, wheels down) |
| `IOREAD_DELAY=n` | A level picture the level select reads ahead is ready only the n-th time it is asked for (as the Vita's I/O thread would be later; the PC reads it at once otherwise) |
| `WRECKAT=t` | Wreck the car at this race time, once in each level (the explosion; `FRAGAT` also counts while the car is destroyed; with `CHASE=1`, a Challenge level restarts as after a real wreck) |
| `ENGINECHECK=1` | Twice a second in a race (from a second after the start or a respawn), whether the car's engine loop is heard; a silence of 1.5 s or more is logged (`ENGINE silent`) |
| `GPUSIM=F,P[,T,P2]` | A made-up GPU clock for the dynamic resolution (the PC has none): each frame takes F + P x size² ms, known two frames later as on the Vita; with T and P2, P2 instead of P every other T seconds. The log's GPU times and the changes of size are then as the Vita's would be |
| `SHDUMP=dir` | Save every shader program's sources as the Vita gets them (`name.vs`, `name.fs`), for `tools/pc/vglsl`, and the Cg `half_all` would make of its fragment shader (`name.cg`; an empty `name.nocg` where it stays GLSL) |
| `CGFS=dir` | Draw each program that has a `name.gles` there with that as its fragment shader: NVIDIA's Cg compiler's GLSL ES of `name.cg` (`tools/pc/cgcheck.sh` makes them and compares the pictures) |
| `BOOMFULL=1` | The explosion's ten glows at their full size however close the camera is (the game draws them as one glow when they would cover more than 1.5 screens' worth) |

## Switch files

Empty files with these names in `ux0:data/nitronic/` (or `<run>/data/`) change how the game starts:

| File | Effect |
| --- | --- |
| `autoplay` | Tours the menus, then the computer drives each story level along the developers' recorded run, the ending and the credits, logging frame times and a GPU benchmark on Campaign level 5; then the game quits |
| `autoplay_shots` | With `autoplay`: saves pictures of the start and finish of each race, the first sparks and wing trail, and each GPU benchmark's view (`LEVEL_bench`), in `test_shots/` |
| `autoplay_all` | With `autoplay`: every level of every set instead of the story's, each driven to its end (along its shipped replay, else along its route), the car's physics running meanwhile. The file may hold two numbers, the first and last level set (0-based), to run the test in parts. Each level ends with a `PERF` line and the log with a `PERF SUMMARY` |
| `autoplay_level` | With `autoplay`: only the levels it names, one level file name a line (`Evil_Carwash`), without the menu tour |
| `nocap` | With `autoplay`: races with the 60 fps cap off, so each frame takes what it costs (tearing shows) |
| `bench_at` | With `autoplay`: more places for the GPU benchmark, a `level race-time` a line (`Campaign_Level6 3.0`), or `level fin time`: that long after the finish line (`warp_speed fin 0.5`: the warp-out, the finish's clock held while the bench runs) |
| `bench_ways` | The benchmark's ways by number, only these (`0 6 7 13 17 28 29 30`; the first, everything, always runs): a place where a frame takes long, measured without the rest |
| `xbar111` | Leave the GPU crossbar at its default 111 MHz (the game sets 166 MHz) |
| `chase` | With `autoplay`: the car steered along the replay with its own physics instead of following it |
| `sndbench` | The sound mixer's benchmark instead of the game: each line of the file names a recording (`sndrec/carwash.bin`, made with the Linux build's `SND_REC`); they are mixed again and timed, then the game quits |
| `no_particles`, `no_instancing` | Draw without the particles, or each object with its own draw call (to measure what they cost) |
| `particles_skip` | Its number: the particle rings made but not drawn, a bit each (1 the short-lived ones - sparks, the wings' trails, the exhaust; 2 the long-lived ones; 4 the darkening ones) |
| `part_dbg` | Its number, ways of drawing the particles to find what costs the GPU (vita/gfx.h: `gfx_part_dbg`): 1 draws of 256 quads, 4 every quad of no size, 32 only the newest quarter of each draw, 64 the rings in video memory, 128 the rings carried on from level to level (as before dev51: each level now starts them from their first slot), 8192 each ring given new memory at each level |
| `autoplay_secs` | Its number: with `autoplay_level`, each level this many seconds from its start instead of 50 |
| `bench_hitch` | With `autoplay`: the benchmark (its ways as `bench_ways` has them) at the first race frame of each level whose 3D scene the GPU took over 50 ms with, the view held as it was then |
| `part_dump` | Each time the particles are logged (the benchmark's start, the slowest frames), the short-lived ring's window as the CPU sees it saved as `test_shots/pringN.tga` (not a picture: a header of eight words - a tag, the ring, its size, head, count, slots drawn, the first, a slot's size - the particles' clock as a double, then each slot's four corners) |
| `no_lod` | Draw the moving objects whole however far off (the game draws a simpler shape of a part when it differs from the whole by under a quarter of a pixel on screen; on the PC, also `NOLOD=1`, and `LODPX=n` for another limit) |
| `no_runs` | Draw the big parts of the level and of the moving objects whole when partly in view (the game draws only the runs of 128 of their triangles, in their order, whose box is in view; on the PC, also `NORUNS=1`, and `RUNTRIS=n` for another run length) |
| `no_post`, `no_bloom` | Draw without the screen effects, or without the glow |
| `half_test` | Builds the half-precision trial's fragment shaders (`vita/gfx.c`: the scrolling glow, the plain glow, the one-slice particles and the final pass written in Cg with their colours in half precision) for the benchmark's `half:` ways; the game itself does not draw with them. The log says which of them the Vita's compiler took |
| `no_interp` | The car drawn where its last physics step left it, as before 1.0.1-dev30 (`car_view` in `vita/main.c`: the physics runs in 5 ms steps, three or four to a frame, so drawn there the car and the world round it judder by a third every third frame at 60 fps; it is drawn between its last two steps instead). `NOINTERP=1` on the PC |
| `no_lock` | The frame's time as the clock gives it, not in whole refreshes (`frame_lock`: with the cap on, the loop waits on a free screen buffer for an uneven time, so the clock's time between frames is uneven though each picture shows for a whole number of refreshes) |
| `snd_evlog` | Every sound event posted, with the sound clock's time, into the log (`EV` lines). Without it the log still counts them: a level's most posted at its end, and the last 5 seconds' (with the voices playing, once a level) when 40 or more voices play or the mixer has come near its time |
| `full_final` | The final screen pass in full (float) precision, as before 1.0.1-dev35 (the game draws it in half precision: 0.4 to 0.9 ms a frame less, the picture at most 1 in 255 off) |
| `snd_flicker` | Its text (`scrape,1`): from 5 to 10 s of race time the scrape sound switched on and off every 1 frame (`skid`; `crash`: a small crash posted), as a car grazing a wall has it |
| `half_all` | Every fragment shader of the game that can be (all but the tyre tracks', the TV screens', the ground lasers', the city buildings' and the cinematics' variants) is built from Cg made of its GLSL, with its colour work in half precision (`vita/gfx.c`: `cg_half`); one the Vita's compiler turns down is built as GLSL as before. The log says how many |
| `shader_opts` | Its four numbers: the Vita's shader compiler's settings instead of vitaGL's own `3 1 0 1` (optimization level 0 to 4, fastmath, fastprecision, fastint). Programs built with other settings are kept apart in the cache |
| `dynres` | Its numbers: the dynamic resolution's limits instead of `15.0 14.3 14.1 0.6 3.0` (the GPU time in ms past which the 3D scene shrinks, the time it shrinks to, the time it grows under, the smallest size, and - optional - the GPU time the final pass is counted as) |
| `double_buffer` | Two screen buffers instead of vitaGL's three: with the 60 fps cap the game is then one frame ahead of the screen, not two (a picture shows two refreshes after its input was read, not three), with a refresh less for the GPU to make up a late frame in |
| `param_mb` | Its number (8 to 64): the GPU's parameter buffer in MB instead of the default 16 (the scene's shaded vertices, binned into tiles; when it runs out, the GPU waits or draws the scene in parts) |
| `no_sound` | Don't start the sound engine |
| `mip_in_order` | A texture's levels given to vitaGL one by one, as before 1.1 (the game gives level 0, then the last, then the rest: vitaGL grows the texture's memory once, not once a level, moving what it holds each time it can't grow it in place) |
| `tex_ab` | Every other texture of several levels given one by one: the loading report then times the two orders apart (`textures of several levels: ...`) |
| `tex_check` | Each compressed texture's levels as the GPU holds them checked against the file's (an order-free sum of each level's blocks' hashes, so the GPU's own layout of the blocks does not matter; levels under 4 x 4, which vitaGL leaves out, are not checked): `texture check: N textures so far, M not as in the file`, and a line for each that differs |
| `tex_redo_test` | Every 5th compressed texture given to vitaGL again, as the game does when the first block of one of its levels, read back from the GPU's copy, is not the file's (vitaGL copies a level from its data pool wherever the pool has got to: from an address 8 past a 16-byte boundary a DXT5 level arrives spoiled; 1.0.1-dev54's `tex_check` on the Vita: 36 of 567 textures). The log counts the textures given again at each level load |
| `no_mips`, `rgba_tex`, `no_fog`, `no_cull`, `no_cutout`, `no_tex_cache`, `discard_cutout`, `no_transfer_wait`, `cg_shaders`, `msaa2x`, `msaa4x` | Graphics fallbacks used while porting |

The log of every session is `boot.log`, which ends with a line per race (`RESULT ...`) and the effects it made.
It is written over the last session's log in place, never cut short or made anew (`vita/io.c`: on the Vita the first
write of a run that needs new room on the memory card held up every read and write for up to 25 s), so this session's
text ends at the line `=== the end of this run's log ===`; the older text after it is blanked with spaces while the
game has nothing else to write. The log says how long its first write took (`log: the first write took ...`) and any
write or read of the card by the file thread over 0.15 s (`io: ...`).

## Performance

Every 5 seconds the log has a line with the frame rate, the worst frame, how the frame's time divides (logic,
physics, drawing, waiting for the GPU), the GPU's own time a frame (mean and worst) and the 3D scene's size, what was
drawn and the sound mixer's load. With `autoplay`, each level ends with a line such as

```
PERF Evil_Carwash: 1088 frames, dropped 0 (0.00%), over 16.7 ms 0, worst 16.8 ms at 3.1 s, game thread worst 9.4 ms | GPU 9.8 ms a frame, worst 12.1 | 3D scene under full size 0% of the time, smallest 100% | sound late 0, mixer peak 31%
```

where `dropped` counts the frames that missed the display's refresh (over 25 ms) and `sound late` the gaps in the
sound, followed by a line counting the level's frames by GPU time (under 13 ms, 13-14, ... over 17). With `nocap`,
`over 16.7 ms` counts the frames that would have missed the refresh. A race frame whose logic takes over 6 ms logs
where it went (`slow logic`); a frame over 30 ms logs how its drawing divides (`slow frame`).

The GPU's time is measured by `vita/gputime.c`: vitaGL ends every scene with a notification, a number the GPU writes
into a word of memory when the scene's pixels are done, and a small thread notes when the numbers of a frame's scenes
turn up. The 3D scene and the glow passes are timed from their hand-over to the last of them; the final pass (into
the screen's buffer) is timed apart, because with the 60 fps cap on the GPU holds it back until that buffer has left
the screen: only with the cap off is its time its work (the `BENCH` lines and the `nocap` runs show it). A frame's
GPU time in the log is the first plus a set 3.0 ms for the final pass. The dynamic resolution goes by it (the same file: past 15 ms the 3D scene shrinks by as much as
brings the time back to about 14; it grows when the last half second's frames would still be under 14.1 ms at the
bigger size; a single long frame is taken for a hitch), so it acts on the room that is left before a refresh is
missed, not on the stutter after. It is off with `autoplay` and `nocap` together and in the benchmark. Its
controller is plain arithmetic and can be tried against made-up scenes on the PC, beside the way it replaced (going
by missed refreshes), and in the game with `GPUSIM`:

```sh
cc -O2 -Ivita tools/pc/drtest.c vita/gputime.c -DNR_PC -lm -o build/drtest && build/drtest      # -v: each change; -h: with hitches
```

The PC build's clock moves on by the frame (and a microsecond for each reading of it within the frame), so a run
repeats exactly: the same race, frame for frame. The log's own times do not move it (a line logged or not, as the
timing logs are, would change the next frame's time). Two compile-time switches help with the collision code:
`-DDYNCMP` (`make -f Makefile.linux EXTRA_CFLAGS="... -DDYNCMP"`) answers every query about the moving objects the
old way too (every listed triangle moved to world space at once) and logs the queries whose answers differ
(`DYNCMP` lines on stderr); `-DCSTAT` with `CSTAT=1` logs each race frame's collision queries and how many triangles
they looked at, and for the car's near boxes how many triangles each gathering saw and kept and the wall-clock time
of the gathering (its dense cells apart), of the queries and of the focus boxes (how many were made, how many queries
fell outside them). Changes to the collision code are checked with `STATEHASH` against a build without them: every
level (`AUTOPLAY_NOMENU=1 AUTOPLAY_ALL=1`, `autoplay chase nocap`, `NODRAW=1`) must give the same hashes.

The GPU benchmark (`autoplay` on Campaign level 5 at 2 s, and the places in `bench_at`) holds the race still, turns
vsync off and draws the same view 55 frames in each of these ways, a `BENCH` line each: everything; no level
objects; objects without their glowing or their solid pass; no ghost cars; ghost cars one by one; the 3D scene at
50%; no screen effects; no speed blur; no bloom; the previous screen passes (five render targets); no glowing level
parts; no level geometry; only the sky and the screen effects; the GPU crossbar at 111 MHz; no HUD; the moving objects whole however far (no simpler shapes); no particles; the big parts of the level and the moving objects whole (not by their runs in view); the scrolling glow (traffic, fire walls) read once as plain glow; the scrolling glow's textures without blending between mip levels; the moving objects' shaders working out four lights each (as before the programs for objects lit by fewer); five ways (`half: ...`) with the half-precision trial's shaders in place of the game's (the scrolling glow; the same with its texture reads asked for in half; the plain glow and the smoke; the final pass; all of those - drawn as "everything" without the switch file `half_test`); everything again; and three ways without one of the particles' rings each (the short-lived ones - sparks, the wings' trails, the exhaust; the long-lived ones - the weather; the darkening ones - smoke); and three for the ghost cars: drawn solid with depth written (what their layers over each other cost), without their back faces, and only the first two. Each `BENCH` line gives the frame time and, beside it, the GPU's own time by its clock, as the 3D scene and glow passes plus the final pass (with vsync off the sum should agree with the frame time where the GPU is what the frame waits for). Each way's finished picture is read back once, before its timing (the race and the shaders' clock held still), and compared with the first way's: the `BENCH` line ends `picture the same` or `picture: N px differ (by D)` - the ways that only draw the same picture another way (ghost cars one by one, the crossbar, the parts whole, the four-light shaders, everything again) must come out the same on the Vita's GPU too. With `autoplay_level`, a level with places in `bench_at` is driven until it has reached them, and left 3 seconds after the last.

The sound mixer can be measured on its own, on the PC or on the Vita, from a recording of what a run sent it:

```sh
SND_REC=/tmp/rec.bin tools/pc/run.sh converter/output/nitronic /tmp/run1 autoplay   # record (the Linux build)
make -f Makefile.linux sndbench
build/sndbench converter/output/nitronic /tmp/rec.bin [out.wav]
```

The level collision's sphere queries can be timed alone the same way (the spike barrels' three a frame each were most of
moto's game-thread time at its end), and every result saved to compare two builds of `src/nr_coll.c` byte for byte:

```sh
SPHREC=/tmp/moto.sph tools/pc/run.sh converter/output/nitronic /tmp/run2 autoplay   # with autoplay_level: moto
make -f Makefile.linux collbench
build/collbench converter/output/nitronic moto /tmp/moto.sph [results.bin]
```

What vitaGL's GLSL translator makes of the game's shaders - the Cg the Vita's shader compiler is handed - can be
looked at on the PC (every `*` becomes a call of `vglMul`, which only takes floats: why the fragment shaders cannot
ask for half precision in GLSL, and why the half-precision trial's are written in Cg):

```sh
tools/pc/vglsl/build.sh /path/to/vitaGL                       # the checkout the game is built with
mkdir /tmp/sh && SHDUMP=/tmp/sh tools/pc/run.sh converter/output/nitronic /tmp/run3 autoplay
build/vglsl/vglsl /tmp/sh/world-scrolling.vs /tmp/sh/world-scrolling.fs      # "-" for the fragment shader: it is Cg already
```

The Cg that `half_all` builds can be checked without a Vita: `tools/pc/cgcheck.sh` has NVIDIA's Cg compiler (`cgc` 3.1,
from the Cg Toolkit; the Vita's shader compiler grew out of it) compile every one, then draws the game with the GLSL
ES that compiler makes of them and compares the pictures with the game's own shaders':

```sh
CGC=/path/to/cgc tools/pc/cgcheck.sh converter/output/nitronic /tmp/cgcheck "Campaign_Level1 14,30" "MountainTop 38.2"
```

What it cannot show is the half precision itself (the PC works the Cg out in float): that is the Vita's to show, in
the benchmark's picture comparison.

## The built-in shader programs

The game builds about a hundred GPU programs at start-up. Compiled on the Vita by its shader compiler, they are kept in
`ux0:data/nitronic/cache` and read from there at the next start; the first start compiles them all (some ten seconds)
and needs the compiler, `ur0:data/libshacccg.suprx`. Shipped in the VPK as `shbank.bin`, they need neither: the game
reads them from `app0:` first, and compiles only a program it does not find there (and says so in the log: "not among
the built-in programs").

The bank is made from a Vita's cache after the game has run, with the same build, everything a player can reach
(every program a run builds is listed in the cache's `used.txt` with the build's label):

```sh
# the cache's used.txt and its .prg files copied off the Vita into /tmp/cache
tools/shbank.py /tmp/cache 1.0.1-dev30          # -> vita/shbank.bin; CMakeLists.txt puts it in the VPK
```

Make it again after any change to a shader or to vitaGL. The programs are what the compiler gave; the compiler itself
is not shipped.
