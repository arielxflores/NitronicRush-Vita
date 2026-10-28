# re_N - Nitronic Rush ghost cars / replays (ReplayManager, NrGhostBin) - spec for the Vita port

All addresses are VAs in NitronicRush.exe (image base 0x400000). `GC(id)` = GetComponent 0x66e730, `Spawn(name,parent)` =
0x67c8e0, `Init(obj)` = 0x66ee40, `Destroy(obj)` = 0x67b220. Component type ids: 0x0c AnimBasic, 0x10 BodyRigid, 0x33 Text,
0x47 RenderMesh, 0x56 ReplayRecorder, 0x57 ReplayPlayer, 0x58 ChildObjectLogic, 0x69 PlayerName, 0x80 FailedRunCameraLogic,
0x81 FailedReplayCar. World is Z-up; car local axes X right, Y forward, Z up. "Verified" = read in the disassembly and/or
checked against the shipped files; anything else is marked **(unverified)**.
Decoder: `/tmp/re/ghost_decode.py` (CSV + summary next to it, see section 10).

---------------------------------------------------------------------------------------------------------------------
## 0. Summary
* A ghost is a **recording of the whole ReplayManager object stream**, not of "the car": every object that has a
  `ReplayRecorder` (the car, and the 15 wreck pieces spawned when it explodes) is recorded at a **fixed 0.06 s tick**
  (16.67 Hz). The file is 5 arrays: actions (spawn / destroy / transform / wings out / wings in, u32 = type | id<<16),
  per-frame cumulative action counts, blueprint-name indices for spawns (u16), 28-byte transform records
  (quaternion w,x,y,z + position, plain floats, no compression or quantisation), blueprint names.
  The "run of 2s" is the action stream (2 = transform of object 0); the "16-bit pairs" are `type|id<<16` spawn actions.
* Playback spawns the named blueprints (`CarGhost`, `DeadCarGhost*`), feeds them the records tick by tick, and
  **interpolates** position (lerp) and orientation (nlerp, no hemisphere check) between the last two records with
  `alpha = accumulator / 0.06`. Wrecks and respawns are not special cases: they are just destroy/spawn actions in the
  stream (car destroyed + 15 `DeadCarGhost*` pieces spawned; new `CarGhost` ~2.04 s later at the respawn point).
* Wings: actions 3/4 set the ghost car's `AnimBasic` direction flag; the wing children (`CarGhostWing*`, AnimBasic
  State 3) follow the car's 0..0.6 s timer. The car's own AnimBasic has zero deltas: **it does not fade or scale the
  ghost**; ghosts pop in. Visual softening = per-frame colour dimming near the camera and where ghosts overlap.
* Colour comes from a fixed **10-entry palette by load index** (0 gold, 1 silver, 2 bronze, 3..9 others), applied to
  the RenderMesh before Init; label = 3 initials from the score table, distance-faded and scaled, decluttered.
* Ghosts race only outside Story mode ("Ghosts Enabled" runtime setting, default "1"), the player picks a score
  **category** (Local / Online / Developer) - not individual ghosts - and **all** files `<level>.0..9.NrGhostBin` of
  that category are loaded (up to 10). They (and the recorder) start at **GO**, not in the countdown.
* Recording always runs (also in Story). On a successful finish whose local rank N <= 9 the run is saved as
  `<ScoreDataRoot>Local/<level>.xml/<level>.N.NrGhostBin`, files N..8 are renamed one rank down and rank 9 is deleted.
* Ghost car material `shockwave` = additive base material + `JumpShockwave.tga`; glow = palette alpha, which replaces
  the blueprint's Color1.w 0.25 (0.5 for load indices 0-2, 0.2 for 3-9 after byte truncation).

---------------------------------------------------------------------------------------------------------------------
## 1. Classes, globals, functions

| thing | address / layout |
|---|---|
| ReplayManager_ (RTTI `.?AVReplayManager_@N@@`) | global `[0x7ff6d0]`, vtable 0x799f44, ctor 0x6ff040, vslot3 Init 0x6feda0, vslot4 Shutdown 0x6fed80, vslot5 **Update(dt) 0x6ff320** |
| ReplayLoadedFile (LF, one per ghost file) | ctor 0x683350 (`ecx`=index, `dl`=isSelf, args colour*, name), size 0x100, source `.\ReplayLoadedFile.cpp` |
| ReplayRecorder (0x56) | vtable 0x7a250c, ctor 0x6a6400, field `String "Replay Object Name"` +0x34, runtime id u16 +0x50; OnAdd 0x6a64f0 -> register 0x6fdc90, OnRemove 0x6a64e0 -> unregister 0x6fdc20; WingsOut 0x6a64b0, WingsIn 0x6a6480 |
| ReplayPlayer (0x57) | vtable 0x7a254c, size 0x70, no serialized fields; Init 0x6a62c0, OnAdd 0x6a6010 (-> 0x6fd180), OnRemove 0x6a5ff0 (-> 0x6fd080), Interpolate 0x6a6030 |
| PlayerName (0x69) | vtable 0x7a29fc, size 0x90, ctor 0x6a5930, serialize 0x6a5820, Init 0x6a59b0, OnAdd 0x6a5b30 (spawns `GhostPlayerName`), OnRemove 0x6a5ab0 |
| FailedReplayCar (0x81) | vtable 0x7a2b74, Init 0x6a4bb0 (registers its object in RM+0x144[index] via 0x6fd6d0) |
| FailedRunCameraLogic (0x80) | update 0x68c9b0 (sets CarName `[0x7ff6d4]` = "ReplayCar") |
| AnimBasic (0x0c) | vtable 0x7b0bf4, Init 0x5f7180, serialize 0x5f71e0, update 0x5f7590 (driven by AnimationManager `[0x7ff794]` 0x5ff040 with frame dt) |
| ChildObjectLogic (0x58) | vtable 0x7966ac, serialize 0x6bdb20 (`Copy Color`, `Names`), Init 0x6bdd90, no update |
| file writer | SaveGhost 0x6f4260 -> RM_SaveFile 0x6fcd00 |
| file reader | RM_LoadGhostFile 0x6fea50 (version) -> LF_ReadBody 0x682b80 |
| level hooks | LevelEnter 0x7155b0, LoadLevelGhosts 0x711dd0, StartRace 0x715080, BaseLevel::Finish 0x714d60, level unload 0x712400 |
| RM internals | RecordFrame 0x6fd790, ResetRecording 0x6fda10, ClearGhosts 0x6fecb0, RewindGhosts 0x6fec60, StartGhosts 0x6fcfd0 / 0x6fcf50, palette 0x6fd470, label projection 0x6fe510, label layout 0x6fde70, ghost dimming 0x6fd260, hide labels (editor) 0x6fe740, failed-run setup 0x6fe860, StepGhosts 0x6ff2d0, StepFailed 0x6ff280 |
| LF internals | Reset 0x682f40, Rewind 0x683230, Tick 0x6837b0, Spawn 0x683450, Destroy 0x683190, Transform 0x682860, CopyRecordingAsSelf 0x682a20 |
| paths | roots 0x6d6f74 / lookup 0x6d6a60 / names 0x6d6920; GetScoreSet 0x6f38f0 (seeds Local + copies default ghosts via 0x6f2a30) |
| menu | ghost category menu 0x6e1ef0, menu action switch 0x6ea740 (table 0x6ec2bc), level-select screen 0x6e4965 |

### 1.1 ReplayManager fields
| off | type | meaning |
|---|---|---|
| +0x04 | float | tick accumulator |
| +0x08 | float | **tick = 0.06** (ctor 0x6ff040 sets 0.015, Init 0x6feda0 overwrites it with 0.06; the files confirm 0.06) |
| +0x0c | int | recording state: 1 armed (after reset), 2 recording, 3 stopped |
| +0x10 | int | recorded frame count |
| +0x14 / +0x18 | u32 | limits: max actions 0x20000 (131072), max frames 0x2000 (8192) |
| +0x1c / +0x34 / +0x4c / +0x64 | vectors | A (u32), B (u32), C (u16), D (28-byte records) of the **current recording**; MSVC vector = 0x18 bytes, data begin/end at +0xc/+0x10 |
| +0x7c | map<string,u16> | blueprint name -> index in names |
| +0x9c | vector<string> | names |
| +0xb4 | u16 | next new object id |
| +0xb8 | vector<u16> | free ids (LIFO) |
| +0xd0 | intrusive list | registered ReplayRecorders (links +0x18/+0x1c, count `[+0xd8]`) |
| +0xe0 | intrusive list | PlayerName components (labels) |
| +0xf0 | vector<LF*> | ghost files (data +0xfc) |
| +0x108 | vector<float4> | colour palette (data +0x114), 10 entries |
| +0x120 | vector<LF*> | "self" (failed-run) replays (data +0x12c), max 20 |
| +0x138 | vector<handle> | FailedReplayCar objects per self-replay (data +0x144) |
| +0x150 | float | label fade 0..1 |
| +0x154 | bool | label fade direction (1 in StartRace 0x7152f1, 0 at level enter 0x715bb2 and at the player's finish 0x71348b) |

### 1.2 ReplayLoadedFile fields
+0x00 state (0 new, 1 empty, 2 loaded/rewound, 3 playing, 4 finished), +0x04 frame cursor (**starts at 1**), +0x08 action
cursor, +0x0c u16 index (-> ReplayPlayer+0x34), +0x10 bool isSelf (-> ReplayPlayer+0x36), +0x14 float4 colour, +0x24
string name (initials), +0x40 A, +0x58 B, +0x70 C, +0x88 C cursor, +0x8c D, +0xa4 D cursor, +0xa8 vector<string> names,
+0xc0 vector<object handle> by id, +0xd8 vector<u16> free ids, +0xf0 list of ReplayPlayers (count `[+0xf8]`).

### 1.3 ReplayPlayer fields
+0x34 u16 LF index, +0x36 bool isSelf, +0x37 bool first-record pending, +0x38 quat prev (w,x,y,z), +0x48 vec3 prev pos,
+0x54 quat cur, +0x64 vec3 cur pos. Init 0x6a62c0: prev = cur = the object's current transform (mat->quat 0x661060),
first = 1.

---------------------------------------------------------------------------------------------------------------------
## 2. NrGhostBin format (writer 0x6fcd00, reader 0x6fea50 + 0x682b80) - verified on all four files

Little endian, no padding, no compression, no checksum, no header besides the version:
```c
u32 version;                      // 500001 (0x7a121); reader rejects anything else ("Invalid Version")
u32 nA;  u32 A[nA];               // action stream
u32 nB;  u32 B[nB];               // B[f] = number of actions after recording frame f (cumulative, non-decreasing)
u32 nC;  u16 C[nC];               // names-index for every spawn action, in order
u32 nD;  struct { float qw, qx, qy, qz, x, y, z; } D[nD];   // 28 bytes, one per transform action, in order
u32 nN;  struct { u32 len /* incl. NUL */; char s[len]; } names[nN];   // unique blueprint names, first-use order
// a count of 0 is followed by no data; file ends exactly after the last name
```
Action word: `type = A & 0xffff`, `id = A >> 16` (object id, recycled through a LIFO free list):
| type | meaning (playback 0x6837b0 switch, table 0x683934) |
|---|---|
| 0 | **spawn** object `id` from blueprint `names[C[c++]]` (0x683450) |
| 1 | **destroy** object `id` (0x683190), id goes to the free list |
| 2 | **transform** object `id` with record `D[d++]` (0x682860) |
| 3 | object `id`: `AnimBasic+0xa9 = 1` (wings out) |
| 4 | object `id`: `AnimBasic+0xa9 = 0` (wings in) |
| other | assert "Invalid Action Type", ignored |

* Record = BodyRigid quaternion **(w, x, y, z)** (Hamilton; R = standard quat->matrix, row-major, column k = local axis k)
  followed by world position. Verified: |q| = 1 +- 1.6e-7, R orthonormal to 1e-15, no sign flips between frames, local +Y
  matches the direction of motion (median dot 0.999).
* Time base: **one frame per 0.06 s replay tick**; frame f is sampled at the (f+1)-th tick after GO. No dt is stored.
  nB x 0.06 = 185.22 / 316.08 / 255.42 s vs. score times 185.234 / 316.099 / 255.438 s (all within one tick).
* There is **no** separate channel for boost/jets, wheels, checkpoints, heat or the finish: only the 5 action types.
  Deaths = 15 `DeadCarGhost*` spawns + destroy of the car in one frame; respawn = new car spawn ~34 ticks later.
* After the last frame there are normally 2 extra actions (`spawn <id> CarGhost`, `destroy 0`: the finish replaces the
  car object). They are inside A but past `B[nB-1]` and are **never played**.
* Names in the shipped files: `CarGhost, DeadCarGhostBody, DeadCarGhostBack, DeadCarGhostBackFenderLeft,
  DeadCarGhostBackFenderRight, DeadCarGhostBackLeftLight, DeadCarGhostBackLightRight, DeadCarGhostFront,
  DeadCarGhostFrontFenderLeft, DeadCarGhostFrontFenderRight, DeadCarGhostFrontLightLeft, DeadCarGhostFrontLightRight,
  DeadCarGhostTire` (Tire used 4x per death). The developer file has `VanGhost` (the van vehicle) + `CarGhost`.

Reader (port version; PC = 0x682b80, which trusts the counts and reads each name into a 512-byte stack buffer
without a length check - cap it in the port):
```c
bool ghost_read(FILE *f, Ghost *g) {
    if (rd_u32(f) != 500001) return false;
    g->nA = rd_u32(f); g->A = rd_array(f, g->nA, 4);
    g->nB = rd_u32(f); g->B = rd_array(f, g->nB, 4);
    g->nC = rd_u32(f); g->C = rd_array(f, g->nC, 2);
    g->nD = rd_u32(f); g->D = rd_array(f, g->nD, 28);
    g->nN = rd_u32(f); for (i < nN) { len = rd_u32(f); read(len bytes); name[i] = up to NUL; }
    // port sanity checks (not in PC): B non-decreasing and <= nA; #spawn(A) <= nC; #xform(A) <= nD; C[i] < nN
    lf->state = 2; return true;
}
```

---------------------------------------------------------------------------------------------------------------------
## 3. ReplayManager tick loop - Update(dt) 0x6ff320 (every frame)
```c
void RM_Update(RM *rm, float dt) {
    if (levelMgr->mode == 2 /*editor*/) { for each PlayerName pn: pn.label.Text+0xa4 = 0; return; }   // 0x6fe740
    if (dt == 0) return;                                        // paused: everything freezes
    rm->labelFade = rm->fadeIn ? min(labelFade + dt, 1) : max(labelFade - dt, 0);
    if (dt > 0.1f) dt = 0.1f;                                   // same clamp as physics
    rm->acc += dt;
    while (rm->acc >= rm->step /*0.06*/) {
        for (lf : rm->ghosts) LF_Tick(lf);                      // 0x6ff2d0
        for (lf : rm->self)   LF_Tick(lf);                      // 0x6ff280
        if (rm->state == 2) RecordFrame(rm);                    // playback first, then record, same tick
        rm->acc -= rm->step;
    }
    float alpha = rm->acc / rm->step;
    for (lf in ghosts and self) if (lf->state == 3) for (rp : lf->players) RP_Interpolate(rp, alpha);
    ProjectLabels(rm);  LayoutLabels(rm, dt /*clamped*/);  DimGhosts(rm);    // section 6
}
```

---------------------------------------------------------------------------------------------------------------------
## 4. Recording

### 4.1 Registration (ReplayRecorder enable/disable)
```c
void RM_Register(RM *rm, ReplayRecorder *r) {              // 0x6fdc90, from ReplayRecorder::OnAdd (component enabled)
    list_add(&rm->recorders, r);
    r->id = rm->free.empty() ? rm->nextId++ : rm->free.pop_back();
    (it, inserted) = rm->nameMap.insert(r->objectName, (u16)rm->names.size());
    if (inserted) rm->names.push_back(r->objectName);
    rm->C.push_back(it->value);
    rm->A.push_back(0 /*spawn*/ | r->id << 16);
}
void RM_Unregister(RM *rm, ReplayRecorder *r) {            // 0x6fdc20, OnRemove (component disabled/destroyed)
    list_remove(r); rm->A.push_back(1 /*destroy*/ | r->id << 16); rm->free.push_back(r->id);
}
// CarLogic update 0x706880, on a change of CarLogic+0x58 (wings out), compared with a static copy 0x801ac4:
//   0->1: car wing AnimBasics +0xa9 = 1; recorder->WingsOut(): A.push_back(3 | id<<16)      (0x706b4d)
//   1->0: +0xa9 = 0; recorder->WingsIn(): A.push_back(4 | id<<16)                            (0x706cd5)
// Car death: Die 0x707db0 disables three car components (vslot 13) and its ReplayRecorder (0x7085b3 -> destroy action; the
// same hide sequence is in 0x702820, used by the reset flow 0x70616e); the DeadCar gibs spawned by Die carry their
// own recorders (names "DeadCarGhost*" in the data; gib blueprints not provided) -> spawn actions. CarLogic state 9
// (0x70aafc, one frame before driving resumes) calls show 0x705a50, which re-enables the recorder -> a new spawn of
// the car blueprint (same id via the free list). The gibs' recorders die with the gibs (3-4 s, 50-67 ticks in the
// data). The wings-in at death is pushed after the destroy (harmless, ignored on playback).
```
Actions are pushed whenever they happen (also between ticks); they belong to the next recorded frame.

### 4.2 RecordFrame 0x6fd790 (state 2 only)
```c
for (r : rm->recorders) {
    BodyRigid *b = GC(r->owner, 0x10);                         // assert if missing
    float t = (phys->acc - rm->acc) / phys->step;              // phys = [0x7ff788]: +4 acc, +8 step = 0.005 (200 Hz)
    vec3 p = b->prevPos(+0xf8) * (1 - t) + b->pos(+0x104) * t; // t is about -12..-32: linear EXTRAPOLATION backwards
    quat q = b->prevQuat(+0xb4);                               // previous physics step's orientation, not extrapolated
    rm->D.push_back({q.w, q.x, q.y, q.z, p.x, p.y, p.z});
    rm->A.push_back(2 | r->id << 16);
}
rm->B.push_back(rm->A.size());
rm->frameCount++;
if (rm->A.size() >= 0x20000 || rm->frameCount > 0x2000) rm->state = 3;   // silently stops after 8193 frames = 491.6 s
```
(+0xf8/+0xb4 are the previous-step copies: 0x63a0b0 copies +0x104->+0xf8, +0xc4->+0xb4 at the start of every physics
step.) Net effect (derived, **unverified at runtime**): the stored position is the car extrapolated to about one tick +
one physics step (~0.065 s) before the tick boundary. Port recorder: just store the car's rendered pose at every 0.06 s
tick (optionally 0.065 s late to be PC-identical; the difference is invisible).

### 4.3 Start / stop
* LevelEnter 0x7155b0: `level+0x58 replayRecording = 1` (always, every mode); `level+0x59 ghostsEnabled` (sec. 7);
  LoadLevelGhosts 0x711dd0 -> ClearGhosts + **ResetRecording 0x6fda10** (disables every recorder, clears A..names and
  the map, nextId = 0, free list cleared, frameCount = 0, state = 1).
* StartRace 0x715080 (race clock passes 0 = GO): `if (ghostsEnabled) StartGhosts` (every ghost LF 2 -> 3);
  `if (replayRecording) { assert(rm->state == 1); rm->state = 2; }`; `rm->fadeIn = 1`. **Both begin at GO.**
* Stop: BaseLevel::Finish 0x714d60 first thing `if (replayRecording) rm->state = 3` (success or not);
  StuntLevel::Finish(true) 0x723f40 likewise (so a stunt ghost includes the coast after time-up); the size limit above.
  Deaths, respawns, pause (dt = 0 -> no ticks) do not stop it. Level unload 0x712400 clears ghosts and resets.

### 4.4 Saving - SaveGhost 0x6f4260 (BaseLevel::Finish: only `if (success && replayRecording)`; StuntLevel::Finish(true))
```c
void SaveGhost(void) {
    LevelDesc *d = levelSetMgr->current; if (!d) return;
    int rank = RankFor(1 /*Local*/);                      // 0x6f3b60 -> scoreMgr+0x110, set by ComputeRank 0x6f3d80 at
                                                          // the finish (-1 when level id empty or score_style 0)
    string base = Path(3 /*ScoreDataRoot*/) + "Local/" + d->id + "/" + d->id;   // d->id = "Campaign_Level1.xml"
    if ((unsigned)rank > 9) return;                        // only a new top-10 local entry gets a ghost
    base.erase(base.size() - 3);                           // ".../Campaign_Level1.xml/Campaign_Level1."
    DeleteFileA(base + "9.NrGhostBin");                    // the entry pushed out of the table loses its ghost
    for (i = 9; i > rank; i--) MoveFileA(base + itoa(i-1) + ".NrGhostBin", base + itoa(i) + ".NrGhostBin");
    string fn = base + itoa(rank) + ".NrGhostBin";
    bool ok = RM_SaveFile(rm, fn);                         // 0x6fcd00: fopen "wb", writes section 2 from RM+0x1c..
    Settings["LastSavedGhostFilename"] = ok ? fn : "";     // used only by the online upload
}
```
* **N = the run's 0-based rank in the Local table** of that level (time ascending for races, score for stunt).
* Paths (0x6d6f74): UserData = `SHGetSpecialFolderPath(CSIDL_LOCAL_APPDATA) + "\digipen/nitronic_rush/"`,
  ScoreDataRoot = UserData + `scores/`, TempGhostLocation = ScoreDataRoot + `temp/`, InternalScoreDataRoot =
  `data/default_scores/` (install dir). Local ghosts: `%LOCALAPPDATA%\digipen/nitronic_rush/scores/Local/<level>.xml/<level>.N.NrGhostBin`
  next to `scores.NrScoreSet`. Online: `.../scores/Web/...`; Developer: `data/default_scores/Developer/...`.
* The table itself is only updated when the 3-letter name is accepted (menu action 0x3e -> 0x6f4ae0: insert, sort by
  style, `if (count > 10) pop_back`, save). The ghost is written at the finish, so quitting before Accept leaves the
  files shifted without a table entry (PC quirk, **unverified in game**).
* Local seeding: the first time the Local table of a level is requested and does not exist (GetScoreSet 0x6f38f0, e.g.
  the Arcade launch action 0x4a calls it before loading), it is created as a copy of the shipped Default table and
  `data/default_scores/Default/<level>.xml/<level>.0..9.NrGhostBin` are copied (CopyFileA, overwrite) into the Local
  folder (0x6f2a30). So the shipped TRN/CLU ghosts are the initial Local ranks 0/1 of Campaign_Level1.

---------------------------------------------------------------------------------------------------------------------
## 5. Playback

### 5.1 Loading - LoadLevelGhosts 0x711dd0 (LevelEnter) and RM_LoadGhostFile 0x6fea50
```c
void LoadLevelGhosts(Level *L) {
    ClearGhosts(rm); ResetRecording(rm);
    if (!L->ghostsEnabled /*+0x59*/ || !levelSetMgr->current) return;
    switch (scoreMgr->category /*+0x144*/) {
      case 3:  dir = Path(4 /*data/default_scores/*/) + "Developer/"; break;
      case 5:  dir = Path(3) + "Web/"; break;
      default: dir = Path(3) + "Local/";                   // 1 Local (default), also 2 Default and 4 Imported
    }
    dir += id + "/"; MakeDirs(dir); base = dir + id minus "xml";
    for (i = 0; i < 10; i++) RM_LoadGhostFile(rm, base + ('0'+i) + ".NrGhostBin", category);   // missing files skipped
}
bool RM_LoadGhostFile(RM *rm, string path, int cat) {
    f = fopen(path, "rb"); if (!f) return false;
    if (read_u32(f) != 500001) { fclose(f); return false; }
    int idx = rm->ghosts.size();                           // load order, NOT the rank in the file name
    string name = "???";
    ScoreSet *s = GetScoreSet(scoreMgr, cat, levelId, 0);
    if (s && idx < s->count) name = s->entry[idx].initials;
    LF *lf = new LF(idx, /*isSelf*/0, rm->palette[idx], name);
    rm->ghosts.push_back(lf); LF_Reset(lf); LF_ReadBody(lf, f); fclose(f);   // state 2
    return true;
}
```
(With a gap in the rank files - e.g. no .0 but a .1 - the initials and colours shift; PC quirk.)

### 5.2 Per-tick step - LF_Tick 0x6837b0
```c
void LF_Tick(LF *lf) {
    if (lf->state != 3) return;
    if (lf->frame == lf->B.size || lf->ai == lf->A.size) { lf->state = 4; return; }   // finished
    u32 end = lf->B[lf->frame];
    for (; lf->ai < end; lf->ai++) {
        u16 type = lf->A[lf->ai] & 0xffff, id = lf->A[lf->ai] >> 16;
        switch (type) {
        case 0: LF_Spawn(lf, id); break;
        case 1: LF_Destroy(lf, id); break;
        case 2: LF_Transform(lf, id); break;
        case 3: case 4: if (obj = lf->objects[id] alive) if (ab = GC(obj, 0x0c)) ab->forward(+0xa9) = (type == 3); break;
        }
    }
    lf->frame++;
}
```
**The frame cursor starts at 1** (LF_Reset/Rewind), so the first tick after GO consumes frames 0 and 1 (spawn, T0, T1)
and the ghost is shown between T0 and T1 during the first tick interval. Result: at time `tick_k + alpha*0.06` after GO
the ghost shows `lerp(T[k-1], T[k], alpha)`, i.e. the original run at the same time since GO (+- the GO phase inside
a tick). The last record is reached at tick nB-1; tick nB sets state 4 and nothing is interpolated any more: **the
ghost freezes at its last pose (just before the finish line) and stays visible with its label** until the level is
reloaded/rewound. Nothing destroys or hides finished ghosts.

### 5.3 Spawn / destroy / transform
```c
void LF_Spawn(LF *lf, u16 id) {                                  // 0x683450
    GameObject *o = Spawn(lf->names[lf->C[lf->ci]], NULL);       // assert
    ReplayPlayer *rp = GC(o, 0x57);                              // assert
    rp->lfIndex = lf->index; rp->isSelf = lf->isSelf;
    if (!lf->isSelf && (rm = GC(o, 0x47))) {                     // ghost colour, BEFORE Init (wings copy it)
        rm->R(+0x62) = (int)(lf->col.r*255); rm->G(+0x61) = (int)(col.g*255); rm->B(+0x60) = (int)(col.b*255);
        rm->glow(+0x63) = (int)(lf->col.a*10);
    }
    Init(o);          // ReplayPlayer init (prev=cur=blueprint pose, first=1) + OnAdd (joins lf->players);
                      // ChildObjectLogic spawns the wings; PlayerName spawns its label and samples the colour
    if ((pn = GC(o, 0x69)) && (label = pn->labelObj)) {
        Text *t = GC(label, 0x33);
        t->color(+0x80..+0x8c) = (col.r+0.2, col.g+0.2, col.b+0.2, col.a);  t->text(+0x34) = lf->name;  rebuild 0x607e70
    }
    slot = lf->free.empty() ? lf->objects.push_back(handle(o)) : (lf->objects[lf->free.pop_back()] = handle(o));
    assert(slot == id, "Object ID Mismatch");                   // same LIFO rule as the recorder
    lf->ci++;
}
void LF_Destroy(LF *lf, u16 id) { if (o = lf->objects[id] valid) Destroy(o); lf->free.push_back(id); }   // 0x683190
void LF_Transform(LF *lf, u16 id) {                              // 0x682860
    Rec *r = &lf->D[lf->di];                                     // assert di < nD
    if (o = lf->objects[id] valid) {
        rp = GC(o, 0x57);
        rp->prev = rp->cur;  rp->cur = *r;
        if (rp->first) { rp->first = 0; rp->prev = *r; }
    }
    lf->di++;
}
```

### 5.4 Interpolation - RP_Interpolate(alpha) 0x6a6030 (every frame, state-3 files only)
```c
obj->pos = prev.pos*(1-alpha) + cur.pos*alpha;                       // GO+0xa8, dirty
quat q = normalize(prev.q*(1-alpha) + cur.q*alpha);                   // nlerp, NO dot<0 check
obj->R  = quat_to_matrix(q);                                         // GO+0xfc, row-major, columns = local axes
```
(The shipped files never flip hemisphere between frames, so the missing check is harmless; keep it identical.)

### 5.5 Wreck / respawn / finish as they appear in the data
* Death frame: `spawn` x15 (`DeadCarGhostBody`, `...Back`, `...BackFenderLeft/Right`, `...BackLeftLight`,
  `...BackLightRight`, `...Front`, `...FrontFenderLeft/Right`, `...FrontLightLeft/Right`, `DeadCarGhostTire` x4), their
  transforms, `destroy <car>`, `wingsIn <car>` (ignored, object gone). The pieces then move on their own recorded
  trajectories and are destroyed 50-67 ticks (3.0-4.0 s) later. **Yes, playback shows the ghost car exploding.**
* Respawn: `spawn <id> CarGhost` normally 34-35 ticks (2.04 s) after the death, 4 ticks when CarResetHard was used;
  the first record of the new object sets prev = cur, so there is no interpolation from the wreck to the respawn point
  (jumps of 46..1131 u in the data). The new car has wings closed and a new label.
* Finish: last frame = last pose before the line; the two trailing actions are not played; ghost freezes (5.2).
* Port gib meshes (by name, **unverified beyond DeadCarGhostBody = DeadCarBody.msh**): Body gib0, Back gib1,
  BackFenderLeft gib2, BackFenderRight gib3, BackLeftLight gib4, BackLightRight gib5, Front gib6, FrontFenderLeft gib7,
  FrontFenderRight gib8, FrontLightLeft gib9, FrontLightRight gib10, Tire = the car wheel mesh (no wing gibs).

### 5.6 Rewind / clear
RewindGhosts 0x6fec60 -> LF_Rewind 0x683230 (cursors 0, frame 1, destroy every object of the file, state 2);
StartGhosts 0x6fcfd0 (2 -> 3). ClearGhosts 0x6fecb0 = LF_Reset + delete for every file.

---------------------------------------------------------------------------------------------------------------------
## 6. Ghost visuals

### 6.1 Blueprints
* `CarGhost`: RenderMesh `car_full_no_wings.msh`, material `shockwave`, Color1 (0.2,0.6,0.95,0.25) (overwritten by the
  palette), ReplayPlayer, ChildObjectLogic (Copy Color 1: `CarGhostWingInnerLeft`, `CarGhostWingInnerRight`),
  AnimBasic (Start 0, End 0.6, all deltas 0, State 2), PlayerName. No body, no collider: ghosts never collide.
  Blueprint position (-100.874, -136.234, 1) is irrelevant (first record overrides it before the first draw).
  **The actual Campaign_Level1 start is (0.000, -159.84, 111.15), heading +Y** (identity orientation); the port's level
  data has start (0, -159.78, 110.55). Evil_Carwash starts at (0, -136.24, -5.02).
* `CarGhostWingInnerLeft`: `CarWingLeftInner.msh`, shockwave, local pos (0.293061, 0.307730, -0.242621), local R =
  rotation +5 deg about Z, AnimBasic Start 0.2 End 0.45, DeltaPos (-1.0079, -0.25697, 0), DeltaRot -0.087266 rad about
  Z, **State 3**; ChildObjectLogic -> `CarGhostWingOuterLeft` (`CarWingLeftOuter.msh`, local pos (-0.97288, -0.204673,
  0.020104), AnimBasic Start 0.4 End 0.6, DeltaRot -pi about Y, State 3). Right wings: blueprints not provided
  (presumably mirrored, **unverified**).
* `DeadCarGhostBody`: `DeadCarBody.msh`, shockwave, ReplayPlayer only (no label, no dimming). Other DeadCarGhost*
  blueprints not provided (same pattern assumed).
* `GhostPlayerName` (label): Text "AAA", font hemihead.fnt, colour (1,1,1,0.9), Scale 150 (overwritten), Draw Mode 2.

### 6.2 Wings - AnimBasic update 0x5f7590 (frame dt, AnimationManager)
Fields: +0x70 Start, +0x74 End, +0x78 DeltaScale, +0x84 DeltaPos, +0x90 DeltaRotation, +0x94 RotationVector, +0xa0 time,
+0xa4 State, +0xa8 applied flag, +0xa9 forward flag; Init 0x5f7180 stores base local pos/scale/orientation, +0xa8 =
+0xa9 = 0.
```c
switch (State) {
case 0: if (fwd) { t += dt; if (t > End) t = Start; dirty; } break;            // loop while on
case 1: if (fwd || t != Start) { t += dt; if (t > End) t = Start; dirty; } break; // loop, stop at wrap when off
case 2: if (fwd) { if (t != End)   { t = min(t + dt, End);   dirty; } }          // ping-pong: the ghost car
        else     { if (t != Start) { t = max(t - dt, Start); dirty; } } break;
case 3: p = first ancestor whose AnimBasic State != 3; t = p.t; dirty; break;   // the wings follow the car
}
if (dirty) { f = clamp((t - Start)/(End - Start), 0, 1);
   if (|DeltaPos| > 0)   localPos   = basePos   + DeltaPos*f;
   if (|DeltaScale| > 0) localScale = baseScale + DeltaScale*f;
   if (|DeltaRot| > 0)   localR     = baseR * AxisAngle(RotationVector, DeltaRot*f);  // order unverified, commutes here
}
```
So action 3 makes the car timer run 0 -> 0.6 s at real time: inner wings slide out/straighten during t 0.2..0.45, outer
wings flip open (-180 deg about Y) during 0.4..0.6; action 4 runs it back (outer folds first). The car's own AnimBasic
has no deltas, so **AnimBasic never fades or scales the ghost**; ghosts appear instantly when spawned.

### 6.3 Colours - palette 0x6fd470 (index = load order) and RenderMesh bytes (R +0x62, G +0x61, B +0x60, glow +0x63 = a*10)
| idx | float (r,g,b,a) | bytes R,G,B / glow byte -> glow | label text RGB (+0.2) |
|---|---|---|---|
| 0 | 0.8, 0.619, 0.01, 0.5 (gold) | 204,157,2 / 5 -> 0.5 | 1.0, 0.819, 0.21 |
| 1 | 0.5, 0.5, 0.5, 0.5 (silver) | 127,127,127 / 5 | 0.7, 0.7, 0.7 |
| 2 | 0.6, 0.2, 0.0, 0.5 (bronze) | 153,51,0 / 5 | 0.8, 0.4, 0.2 |
| 3 | 0.8, 0.1, 0.1, 0.25 | 204,25,25 / 2 -> 0.2 | 1.0, 0.3, 0.3 |
| 4 | 0.45, 0.45, 0.1, 0.25 | 114,114,25 / 2 | 0.65, 0.65, 0.3 |
| 5 | 0.3, 0.6, 0.1, 0.25 | 76,153,25 / 2 | 0.5, 0.8, 0.3 |
| 6 | 0.1, 0.8, 0.1, 0.25 | 25,204,25 / 2 | 0.3, 1.0, 0.3 |
| 7 | 0.2, 0.6, 1.0, 0.25 | 51,153,255 / 2 | 0.4, 0.8, 1.2 |
| 8 | 0.1, 0.1, 0.8, 0.25 | 25,25,204 / 2 | 0.3, 0.3, 1.0 |
| 9 | 0.1, 0.35, 0.55, 0.25 | 25,89,140 / 2 | 0.3, 0.55, 0.75 |
The ghost body, its wings (ChildObjectLogic copies the parent RenderMesh colour + glow at Init) and its wreck pieces
all get this colour. PlayerName Init 0x6a59b0 keeps a float copy (pn+0x80..+0x8c = bytes/255, glow*0.1).

### 6.4 Per-frame ghost dimming - DimGhosts 0x6fd260 (car bodies with a PlayerName only; not wings, not debris)
```c
for each label a with 1 <= a.w <= 100:  for each later label b with 1 <= b.w <= 100:
    d2 = (b.cx/b.w - a.cx/a.w)^2 + (b.cy/b.w - a.cy/a.w)^2;  R = 2/a.w + 2/b.w;
    f = clamp((R*R - d2)/(R*R), 0, 1);  a.count += f;  b.count += f;            // count starts at 1
for each label a:
    k = clamp((a.w - 1)/6, 0, 1);                                               // invisible at depth <= 1, full at >= 7
    rm = GC(a.owner, 0x47);  rm.RGB = (int)(a.col.rgb * k / a.count * 255);  a.count = 1;   // glow byte untouched
```
(cx, cy, w = clip-space x, y, w of the car origin from 0x6fe510.) With additive blending this hides a ghost that sits on
the camera/player and stops overlapping ghosts from adding up.

### 6.5 Labels - PlayerName (fields +0x40 NearOpacity 0.8, +0x44 FarOpacity 0.7, +0x48 NearScale 4000, +0x4c FarScale
5000, +0x50 MinScale 100, +0x54 MaxScale 500, +0x58 NearDistance 10, +0x5c FarDistance 150, +0x60 MaxDistance 300,
+0x64 MinDistance 1; runtime +0x34 label handle, +0x68 declutter offset, +0x6c hold timer, +0x70/74/78 clip x,y,w,
+0x7c overlap count, +0x80 colour)
```c
// 0x6fe510: clip = Proj(cam+0x74, rows 0,1,3) * View(cam+0x34) * carWorldPos   (main camera)
// 0x6fde70 pass 1, per label:
text->drawMode(+0xa4) = 2;
sx = W/2 + W/2 * cx/w;   sy = H/2 + H/2 * (cy + 1)/w;          // +1 in clip y = label above the car; screen pixels
t  = (w - Near)/(Far - Near);
alpha = t < 0 ? max(0, (w - MinD)/(Near - MinD) * NearOp)      // 0 at <=1 u, 0.8 at 10 u
      : t > 1 ? max(0, (w - MaxD)/(Far - MaxD) * FarOp)        // 0.7 at 150 u, 0 at >=300 u
      : lerp(NearOp, FarOp, t);
s = clamp(lerp(NearScale, FarScale, t) / w, MinScale, MaxScale);   // 500 up to ~8 u, 400 at 10, 100 from ~43 u
text->alpha(+0x8c) = alpha; text->scale(+0x90,+0x94) = s;  label->pos = (sx, sy + pn->off, w);
// pass 2 declutter, pairs (a, b) with a.alpha >= 0.1 and b.alpha >= 0.2, S = a.s + b.s:
if (|a.x - b.x| < 0.08*S && |a.y - b.y| < 0.06*S) {
    upper.hold = 0.1;
    if (|a.y - b.y| < 0.057*S) { d = S*dt*0.1; upper.off += d; upper.label.y += d;
                                 if (lower.off > 0) { lower.off -= d; lower.label.y -= d; } } }
// pass 3, per label with alpha >= 0.1:
if (hold > 0) hold -= dt; else if (off > 0) off -= max(MinScale*3.5, s) * dt * 0.2;
label.x -= s*0.12;  label.y += s*0.06;  label.z = 1;  text->alpha *= rm->labelFade;   // fade 0->1 in 1 s from GO,
                                                                                       // 1->0 in 1 s after the finish
```
Labels die with their car (OnRemove destroys the label) and are re-created on respawn. Editor mode hides them.

### 6.6 "S" blueprints - self / failed-run replays (CarGhostS, CarGhostWing*S, DeadCarGhost*S)
**Superseded by section 12**, which has the exact triggers, the FailedRunCamera state machine and the visuals. In
short:
* `CopyRecordingAsSelf 0x682a20` copies the current recording into a self LF and appends 'S' to every blueprint name.
* Setup 0x6fe860 needs at least 50 actions and keeps at most 20 LFs.
* The copy is made once per level load (flag level+0x178, set by Init slot 4):
  * Campaign: at the finish (state 3);
  * Challenge and Stunt: at name entry (state 4);
  * Challenge also on each death-restart.
* Slot 23 clears the self replays on every other exit.
* S cars keep their blueprint colour (car_dead, (0.3,0.6,0.95,3.0)): no palette, no label, no dimming.

### 6.7 "_Gold" variants
No code builds or selects a `_Gold` ghost name: the only "_Gold" strings are the menu dummies `CarDummy_Gold` /
`CarDummyFly_Gold` for the **Goldenceptor** vehicle (VehicleData/VehicleData.xml `object_file`). A ghost spawns exactly
the names stored in its file, which come from the ReplayRecorder "Replay Object Name" of the vehicle (and gib)
blueprints the run was driven with - so a run with the Goldenceptor stores (and replays) whatever ghost names its
blueprints carry, e.g. a `CarGhost_Gold` set (**inferred; blueprints not in the provided data**). CarLogic also has a
serialized `Ghost Dummy Object Name` (+0x470) that no code reads. Unrelated: palette index 0 is "gold" coloured.

---------------------------------------------------------------------------------------------------------------------
## 7. Which ghosts race, and when
* **Modes**: LevelEnter 0x7155b0: `ghostsEnabled(+0x59) = atoi(Settings["Ghosts Enabled"]) != 0` (BaseLevel init
  0x7118f0), then forced 0 if `Settings["In Story Mode"]` != 0. So ghosts race in **Arcade only** (incl. stunt levels,
  via the same BaseLevel path); Story has none. Recording and ghost saving happen in every mode.
* **"Ghosts Enabled"**: entry in the runtime settings store `[0x7fe7b0]` (GetOrCreate 0x732010, value int at +0x14),
  **default "1"**; **not** saved in userdata.xml (its Settings section holds only control_scheme ... friend_group);
  toggled by the Arcade level-select toggle " Ghost Replays" (menu action 0x55: "1" <-> "0").
* **"Select Ghosts to Challenge"** (0x6e1ef0) is shown, when ghosts are enabled and not in Story, on an Arcade level
  launch ("Go" on the Arcade level select, action 0x4a, after seeding the Local table), on "Yes, Restart" of the
  "Restart Current Level?" dialog (action 0x20) and on "Continue" of the results screen (action 0x33; skipped when
  0x6f8c30 says there is no next level). Otherwise those buttons go straight on. It offers **score categories, not ghosts**:
  "Local" (action 0x59 -> category 1), "Online" (0x5a -> 5, downloads first; not for Vita), "Developer" (0x5b -> 3, only
  if the level has an entry in scoreMgr+0x64), "Back" (0xb). The focus starts on the current category; scoreMgr+0x144
  defaults to 1 (Local). Choosing one sets `scoreMgr+0x144 = +0x148 = cat` and continues; **all existing files .0..9 of
  that category are then loaded (max 10 ghosts)**. No individual picking, no limit below 10.
* **"SelectingGhostsDuringRestart"**: settings string written by 0x6e1ef0 = "LevelRestart" / "LevelContinue" /
  "LevelSelected"; after the category is chosen 0x6ea390 uses it to resume the right flow (restart 0x6ea160 / next level
  0x6ea230 / start the selected level 0x6ea300).
* **Start**: ghost LFs are loaded (state 2) at level enter but spawn nothing until **GO** (StartRace sets state 3); the
  first tick after GO spawns the cars at their first record. Nothing during the countdown. Pause freezes them (dt 0).

---------------------------------------------------------------------------------------------------------------------
## 8. Rendering
* `Materials/shockwave.txt`: `LOAD MATERIAL additive` + `PARAM TEXTURE Diffuse1 JumpShockwave.tga` - the car/wing/
  gib meshes are drawn with the additive base material using the shock-ring texture over their own UVs, tinted by the
  RenderMesh colour, intensity = glow byte * 0.1 (0.5 or 0.2), drawn in the renderer's "Additive" pass (after
  Shaded/PostShaded, before PostProcess; pass list 0x72cc30). Blueprint Color1.a 0.25 is replaced by the palette alpha.
* Blend/depth: the additive base material/effect file is not in the provided data. Assumed as for the game's other
  additive effects (re_F2: ZENABLE 1, ZWRITE 0, CULL NONE, SRC ONE, DEST ONE) **(unverified for this material)**. Port:
  use the existing object-additive program (`P_OBJADD`, FS_ADD: tex.rgb x colour x glow, ONE/ONE, depth test on, depth
  write off) with JumpShockwave as the texture, colour = RenderMesh RGB after DimGhosts, glow = 0.5 / 0.2.

---------------------------------------------------------------------------------------------------------------------
## 9. Port plan (C)
```c
typedef struct { float q[4] /*w,x,y,z*/, p[3]; } GhostRec;
typedef struct { uint32_t nA, nB, nC, nD, nN; uint32_t *A, *B; uint16_t *C; GhostRec *D; char (*name)[48];
                 int state, frame /*=1*/, ai, ci, di; float col[4]; char initials[4];
                 int16_t obj[64]; uint16_t freeid[64]; int nfree, nobj; } Ghost;   // obj -> port object or -1
typedef struct { float prev[7], cur[7]; int first, kind /*car,gib n*/, wings; float wingT; } GhostObj;
// per frame:  acc += min(dt, 0.1); while (acc >= 0.06) { for g: ghost_tick(g); if (rec.on) rec_frame(); acc -= 0.06; }
//             alpha = acc / 0.06; for live objs: pos = lerp, q = nlerp (as 5.4); wings: t +-= dt in [0,0.6]
// ghost_tick = 5.2/5.3 verbatim (frame cursor from 1, LIFO ids, first record sets prev = cur).
// recorder:  rec_reset() at level enter; on GO state = 2; car "spawn" when the race car appears/respawns,
//            "destroy" at death, 15 gib spawns/destroys if the port records gibs (else the ghost just vanishes and
//            reappears; still a valid file), wings out/in on CarLogic wingsOut changes; rec_frame() appends one
//            record per live recorded object + B entry; stop at finish or 8193 frames; on finish with rank <= 9:
//            shift files and write section 2 verbatim (PC and Vita ghost files stay interchangeable).
```
Keep: names table + C indices (needed to pick car/van/gold/gib models), the 2 unplayed trailing actions (harmless),
little-endian layout. Save folder on Vita: `ux0:data/<game>/scores/Local/<level>.xml/<level>.N.NrGhostBin`; seed it from
`data/default_scores/Default/...` the first time a level's Local table is created.

---------------------------------------------------------------------------------------------------------------------
## 10. Decoder and results (`python3 /tmp/re/ghost_decode.py [files]`)
Writes `/tmp/re/ghost_<Category>_<level>.<N>.csv` (one row per frame: frame, t_tick = (f+1)*0.06, t_lo = f*0.06, car
id/blueprint, x y z, quaternion, right/forward/up axes, wings_out, car_present, new_car_object, debris_alive, events)
and `..._objects.csv` (every transform record of every object); prints a summary (also saved in
`/tmp/re/ghost_decode_summary.txt`) and runs an exact emulation of the PC playback (`Player` class).

| file | frames | duration vs score | start pos | deaths / respawns | wings out | max speed |
|---|---|---|---|---|---|---|
| Default Campaign_Level1.0 (TRN) | 3087 | 185.22 vs 185.234 s | (0, -159.84, 111.15) | 4 / 4 (34 ticks) | 5x, 11.6 s | 126 u/s |
| Default Campaign_Level1.1 (CLU) | 5268 | 316.08 vs 316.099 s | (0, -159.84, 111.15) | 10 / 10 | 35x, 88.6 s | 153 u/s |
| Default Evil_Carwash.0 (NES) | 4257 | 255.42 vs 255.438 s | (0, -136.24, -5.02) | 25 / 25 (one after 4 ticks) | 1x | 164 u/s |
| Developer Campaign_Level1.2 (VanGhost) | 1238 | 74.28 s (no table) | (0, -159.82, 111.95) | 0 | 0 | 133 u/s |
All: positions continuous (max 9.8 u per tick) except at respawns; |q| - 1 < 1.6e-7; no hemisphere flips; PC-emulated
playback ends in state 4 after nB ticks with only the car left on screen; ID free-list replay matches ("Object ID
Mismatch" never fires).

---------------------------------------------------------------------------------------------------------------------
## 11. Follow-ups: label text size, results screen, name entry

Notation: W, H = window size in pixels (`[0x7ff6c4]+0x557a2c / +0x557a30`). "y_up" is measured from the **bottom**
edge (PC HUD space), "y_top" from the top edge (what `vita/hud.c hud_text()` takes). Example numbers are given for the
Vita (960x544) and for 1280x720. "(unverified)" marks inference and missing data. Blueprints `HudTextPrimary`,
`menu_text`, `menu_button` and `HudBlackSquare` are **not** in the provided data (Objects/ holds only 7 ghost files), so
their colour, font, Letter/Word Gap and transform are unknown. Pixel sizes below assume hemihead.fnt (as in every HUD
blueprint we have). For ethon.fnt, replace 72 (lineHeight) and 47 (cap cell) with that font's numbers (61 and about 56).

### 11.1 Text component (type 0x33) in Draw Mode 2
Fields:

| offset | field | offset | field |
|---|---|---|---|
| +0x34 | text | +0x90 / +0x94 | Scale |
| +0x50 | font | +0x98 | Position (offset) |
| +0x6c / +0x70 | wrap | +0xa4 | Draw Mode |
| +0x74 | Letter Gap | +0xa8 | extent: max right edge of the quads |
| +0x78 | Line Gap | +0xac | extent: min bottom (negative) |
| +0x7c | Word Gap | +0xcc | right-align flag |
| +0x80 | Color | +0xcd | centre flag |

The constructor (0x607c60) zeroes everything, and the serializer (0x607ac0) has no defaults: all values come from the
blueprint.

* **Build 0x607e70.** Glyph geometry is in font-texture pixels / 512 (scaleW), with y up and the origin at the **top
  of the line box**. A glyph quad covers:
  * x from pen + xoffset/512 to that plus width/512;
  * y from -yoffset/512 down to -(yoffset+height)/512.

  The pen advances by xadvance/512 x Letter Gap. Lines step by lineHeight/512 x Line Gap; '\n' and '|' break the line.
  **A space adds only Word Gap** (GhostPlayerName: 0.05) and draws no quad: the space glyph's xadvance is not used. No
  kerning is applied. Pad-icon escapes map to glyphs 127..138 of the font:

  | escape | glyph | escape | glyph | escape | glyph |
  |---|---|---|---|---|---|
  | %A | 0x7f | %LT | 0x83 | %LS | 0x87 |
  | %B | 0x80 | %RT | 0x84 | %RS | 0x88 |
  | %X | 0x81 | %LB | 0x85 | %s | 0x89 |
  | %Y | 0x82 | %RB | 0x86 | %b | 0x8a |
* **Draw 0x610750.** Pixel = object translation (world) + Scale x glyph + offset. offset.x depends on the flags:
  * right-align (+0xcc): -extent·Sx;
  * centre (+0xcd): -0.5·extent·Sx;
  * neither: Position.x.

  offset.y = Position.y. The object's transform scale is ignored. Draw Mode 2 goes to the HUD list, which uses a pixel
  ortho with the origin at the **bottom-left, y up**, in window pixels.
* **Glyph size = Scale x font pixels / 512, independent of the window size.** For hemihead (lineHeight 72, capitals
  yoffset 13, height 47 including the 1 px padding, about 41 px visible):
  * line height = 0.1406·s;
  * capital quad = 0.0918·s, visible capital about 0.080·s;
  * capital top 0.0254·s below the origin, capital bottom 0.117·s below it.

  | s | capital quad (px) | visible capital (px) | line (px) |
  |---|---|---|---|
  | 100 | 9.2 | 8.0 | 14.1 |
  | 250 | 22.9 | 20.0 | 35.2 |
  | 500 | 45.9 | 40.0 | 70.3 |
* **Ghost labels (6.5).** s = 100..500 px (a function of distance only), so labels are the same pixel size at any
  resolution; only their position scales with W and H.
  * **Horizontal:** the anchor is the **left edge** (no align flag; Position = (0,0,1)). `x -= 0.12·s` centres "AAA"
    (ink width 117/512·s = 0.229·s, so the centre is at 0.114·s, about 0.006·s right of the projected point).
  * **Vertical:** `y += 0.06·s` raises the line top above the projected point, so the capital runs from y + 0.035·s
    to y - 0.057·s (centred on the point, 0.011·s low).
  * **Port:** `hud_text(HUD_FONT_MAIN, sx - 0.12f*s, Hv - (sy + off + 0.06f*s), 0.140625f*s, rgba, HUD_LEFT, txt)`, where
    sx, sy and off are computed in Vita pixels with the 6.5 formulas (Hv = 544).

  For the same look relative to the screen as on a 720-line PC, also multiply s by 544/720. That is a choice: PC does
  not scale it. The PC window defaults to the desktop size when resolution_x/y are absent (unverified).
* **hud_timer, for comparison** (derived, not tested in game): HudLogic 0x6fc500 puts hud_main at the origin with scale
  (W, W) and sets each item's Text.Scale = local scale x W. The blueprint's 15x15 is overwritten.
  * Timer s = 0.292661·W, so the line is 0.0412·W and the capital quad 0.0269·W.
    * 1280 px wide: s = 374.6, line 52.7 px, capital 34.4 px.
    * 960 px wide: s = 281, line 39.5 px, capital 25.8 px.
  * Anchor (left edge, line top): (0.425·W, 0.0539·W) y_up.

  So the timer **does** scale with the window, while ghost labels do not.

### 11.2 Menu items: MenuButton (0x36), created by 0x6df700 / 0x6e19b0
`0x6df700(ui, label, action, snapX, snapY)` takes ecx = &size (half extents, in units of H) and ebx = &offset (in
units of H). It spawns `menu_text` or `menu_button`, sets MenuButton +0x34 size, +0x3c offset, +0x44 snap and +0xa4
action, sets the text, and appends the object to the menu's item list.

MenuButton update 0x6a7ae0 places the item each frame. Per axis k, with D = (W, H):
```
snap 0: pos = D[k]/2 + off*H        snap 1: pos = (off + size)*H        snap 2: pos = D[k] - (off + size)*H
snap 3: pos = ref.pos + off*H       snap 4: pos = ref.pos - (ref.size + off + size)*H        snap 5: ... + (...)
quad scale = size*H (half extents).
Text: Scale = 9*sizeY*H (both axes), centre flag set, Position.y = 0.642857*sizeY*H
      (-> line top 0.643*sizeY*H above the centre, so capitals are vertically centred in the button)
```
* **0x6e19b0(ui, label, blueprint, action, gap)** (esi = prev): makes a new item with prev's size, snap (3,4) and offset
  (0, gap). It sits **directly below** prev, with the same x and a gap·H gap. It links prev.down(+0x7c) = new and
  new.up(+0x88) = prev.
* **Links.** 0x6dd540 links A.right(+0x70) = B and B.left(+0x64) = A. 0x6e1760 places an item right of the reference,
  0x6e1820 left of it, and 0x6e18e0 above it (same size, gap 0.007).
* **Navigation.** The d-pad moves to the neighbour in that direction, if there is one. menu+0 is the index of the item
  selected by default.
* **Mode (+0xa8).**
  * 0 = normal button (click flash);
  * 1 = left-aligned text;
  * 2 = slider (left/right adjusts the value);
  * 3 = static: titles, labels and the initials (no flash).
* **Selected-button glow.** A pulse of 0.35 + 0.65·(sin(2.5t)+1)/2, scaled by the hover amount, goes into the
  button-mesh shader bytes (0x6a7720).
* **Backdrop.** Name entry, and a results screen opened from a finish (0x6df630), spawn **HudBlackSquare** at
  (5000, 5000, -40) px with scale 80000, i.e. a full-screen quad behind the menu. Its colour is in the blueprint
  (not provided).
  * **Name entry:** sets its RenderMesh glow byte (+0x63) to 0. The name-entry menu update (0x6ec460) then adds 1
    per frame while glow·0.1 < 0.7, so it fades 0 -> 0.7 over 7 frames.
  * **Results:** this copy keeps the blueprint value.

  Assuming the HUD material uses the glow as alpha (unverified), the 3D scene stays visible at about 30%; see 12.4.

### 11.3 Results screen 0x6e6290(ui, category, rank, fromMenu) + rows 0x6e5dc0 / 0x6e08d0

**Layout.** Positions are item centres. 1 px = 1/H of the screen height.

| element | size (half, ·H) | snap / offset | centre | text Scale | 960x544: centre (x, y_top) / capital px | 1280x720 |
|---|---|---|---|---|---|---|
| "Results" (menu_text, mode 3) | 0.3 x 0.08 | (0,2) / (0, 0.025) | W/2, y_up 0.895H | 0.72H | 480, 57.1 / 36.0 | 640, 75.6 / 47.6 |
| `results_category` "<Cat> Scores" (menu_text, mode 3) | 0.18 x 0.048 | below the title, gap -0.06 | W/2, 0.827H | 0.432H | 480, 94.1 / 21.6 | 640, 124.6 / 28.6 |
| left arrow (action 0x41) | 0.1625 x 0.0325 | (0,2) / (-0.45, 0.14) | W/2 - 0.45H, 0.8275H | 0.2925H | 235.2, 93.8 / 14.6 | 316, 124.2 / 19.3 |
| right arrow (action 0x40) | same | (0,2) / (+0.45, 0.14) | W/2 + 0.45H, 0.8275H | 0.2925H | 724.8, 93.8 | 964, 124.2 |
| Quit (0x3f) | 0.1625 x 0.0325 | left of Continue, gap 0.007 | W/2 - 0.332H, 0.0825H | 0.2925H | 299.4, 499.1 | 401, 660.6 |
| Continue (0x33) | same | (0,1) / (0, 0.05) | W/2, 0.0825H | 0.2925H | 480, 499.1 | 640, 660.6 |
| Restart (0x1f) | same | right of Continue, gap 0.007 | W/2 + 0.332H, 0.0825H | 0.2925H | 660.6, 499.1 | 879, 660.6 |
| Post Result (0x42), only if IsRealFinish | same | above Continue, gap 0.007 | W/2, 0.1545H | 0.2925H | omit on Vita | |
| FriendText (global string 0x7ff934) | 0.8 x Continue's size | below Continue, gap 0 | W/2, 0.024H | 0.234H | omit on Vita (online/friends) | |

* **Arrow captions.** 0x6df160 overwrites the "<< Cat" / "Cat >>" captions after every rebuild:
  * left (0x41) = `"< " + name(prev category) + " %LB"`;
  * right (0x40) = `"%RB " + name(next category) + " >"`.

  0x6f4570 cycles categories 1..5 with wrap-around and skips empty ones (Default is always kept). Category names
  (0x6f2980): 0 Temporary, 1 Local, 2 Default, 3 Developer, 4 Imported, 5 Online.
* **Buttons.**
  * **From a finish (fromMenu = 0):** left to right **Quit, Continue, Restart**, plus Post Result above (PC online
    only) and the two arrows. Default focus: Continue.
  * **From a menu (fromMenu = 1):** a single **Back** (0x0b) at the Continue position, plus the arrows. No Quit or
    Restart, no highlight and no [YOU] row.
  * **Story vs Arcade:** 0x6e6290 has no Story/Arcade branch, so both show the same buttons. Story mode only changes
    what Continue does: no "Select Ghosts to Challenge" step; it goes to the next story level, or to CampaignOutroLevel
    after the last one. Quit (0x3f) and Restart (0x1f) open the usual confirm dialogs in both modes.
* **Headers.** Three `HudTextPrimary` texts, Scale 0.3H (line 0.0422H, capital 0.0275H; 14.98 px at 544), line top at
  y_up = 0.65H (y_top 190.4 at 544, 252 at 720):
  * "Time": left edge at 0.12W.
  * "Score": **right edge at 0.64W**.
  * "Best Trick": **right edge at 0.90W**.

  The last two are right-aligned by 0x607d50, which sets the object translation x to X - extent·Sx, while
  Position.x = X is still added (X = 0.32W and 0.45W). The doubled X is how PC actually lays it out, so a port should
  copy these right edges.
* **Rows (0x6e5dc0 -> 0x6e08d0 per row).** Text Scale 0.3H for all four columns.
  * **Columns:**
    * `"%s. %s"` = rank word (0x6f2870: "1st".."10th", "11th") + initials, **right edge at 0.11W** (X = 0.1W plus
      Position.x = 0.01W);
    * Time = FormatTime(t, hundredths) "mm:ss.hh", **left edge at 0.12W**;
    * Score: integer, right edge 0.64W;
    * Best Trick: integer, right edge 0.90W.

    At 960 px wide those edges are 105.6 / 115.2 / 614.4 / 864.
  * **Vertical:** the first row's line top is y_up = 0.65H - (0.3H·60/512 + 0.005H) = **0.6098H** (y_top 212.25 at 544,
    280.9 at 720). Each next row moves down by (rank+name text's lowest quad bottom)·0.3H + 0.005H:
    * 0.04016H (21.84 px at 544) for normal rows (lowest glyph bottom 60/512);
    * 66/512 if the initials contain 'Q';
    * 70/512 if they contain '_' (e.g. the default "TM_"): 25.0 px at 544.
  * **Assumption (unverified):** the `HudTextPrimary` blueprint transform is at (0,0). The left-aligned Time column
    and every y add that translation.
  * **Row count:** min(entries, 11). If the index is >= 10 the label is forced to "[YOU]", so row 11 reads
    "11th. [YOU]" whatever the real rank.
  * **Colour:** the blueprint's colour (unknown; HUD texts are (1,1,1,0.9)). **Highlight:** the row whose index ==
    ui+0xb4 gets Color = (0.3, 0.8, 0.3, 1.0) on all four texts at build time. Their **object handles** (not
    positions) are stored at ui+0x64/+0x70/+0x7c/+0x88. The results menu update (menu id 11, 0x6ed766 ->
    0x6ddcb0 x4) then **pulses** them every frame:
    ```
    p = 0.35 + 0.65 * (sin(4 t) + 1) / 2;
    Color = (1 - 0.9p, 1 - 0.05p, 1 - 0.9p, 1);
    ```
    t is real time (QPC). The colour runs between pale green (0.685, 0.98, 0.685) and green (0.1, 0.95, 0.1), with a
    period of 1.57 s. No bar or other marker.
  * **Which row is highlighted** (ui+0xb4 = RankFor(category), or -1 when fromMenu):

    | category | what happens | highlighted row |
    |---|---|---|
    | Local (1) | the run is already in the table, which is capped at 10 on Accept by 0x6f4ae0 | if rank <= 9: its row, with the player's initials. If rank >= 10: a copy named "[YOU]" is appended and shown as row 11 |
    | Developer (3) | the run is always inserted as "[YOU]" at its sorted position (0x6f3750) | the "[YOU]" row |
    | Default (2) / Imported (4) | nothing is inserted | the entry at the would-be rank (PC quirk: it is someone else's row). Nothing if rank >= the number of rows |
    | Online (5) | never inserted | not on Vita |
  * **Rank voice:** rank 0, 1 or 2 plays Play_FirstPlace / SecondPlace / ThirdPlace (rank 0 also unlocks "First!").
    The AchievementDisplay is spawned at the end.

### 11.4 Name entry 0x6e5770(ui) - menu id 12
**Layout** (all centre-snapped (0,0) unless noted; sizes are half extents in units of H):

| item | size | centre (y_up) | text Scale | 960x544 centre (x, y_top) | 1280x720 |
|---|---|---|---|---|---|
| "Enter Name..." (menu_text, mode 3, action 0) | 0.2 x 0.05 | W/2, 0.65H | 0.45H (capital 22.5 px at 544) | 480, 190.4 | 640, 252 |
| "^" (menu_text), columns c = 0, 1, 2 | 0.03 x 0.0325 | W/2 + (-0.065, 0, +0.065)H, 0.55H | 0.2925H | 444.6 / 480 / 515.4, 244.8 | 593.2 / 640 / 686.8, 324 |
| letter `NameEntry_Initial<c>` (menu_button, mode 3) | 0.03 x 0.0325 | same x, 0.478H | 0.2925H (capital 14.6 px at 544, 19.3 at 720) | ..., 284.0 | ..., 375.8 |
| "v" (menu_text) | 0.03 x 0.0325 | same x, 0.406H | 0.2925H | ..., 323.1 | ..., 427.7 |
| "Accept" (menu_button, action 0x3e), snap (2,1), offset (0.05, 0.05) | 0.1625 x 0.0325 | W - 0.2125H, 0.0825H | 0.2925H | 844.4, 499.1 | 1127, 660.6 |

The vertical gaps are 0.007H (0x6e19b0 chain "^" -> letter -> "v"). There is a HudBlackSquare backdrop. On open,
**Play_NewHighScore** (Wwise 0x43288d72, FNV-1 checked) is queued. The HUD is hidden by the ShowResults handler.

**Actions** (switch 0x6ea740; the letter is changed by 0x6f5300(dir, column)):

| item | column 0 | column 1 | column 2 | effect |
|---|---|---|---|---|
| "^" | 0x36 | 0x38 | 0x3a | dir 0: **previous** character |
| "v" | 0x35 | 0x37 | 0x39 | dir 1: **next** character |
| letter | 0x3b | 0x3c | 0x3d | moves the cursor to the letter's right neighbour (I0 -> I1 -> I2 -> Accept) |
| Accept | 0x3e | | | 0x6f4ae0 commits, then 0x6e13e0, then MenuButtonUsedMessage(0x1a, 0x3e) -> BaseLevel 0x713640 -> results 0x6e6290(ui, category, RankFor(category), 0) |

After every "^" / "v" press the letter's text is rebuilt. 0x6f4ae0 copies the initials into the run entry, inserts it
into the Local table, keeps the top 10 and saves.

* **Navigation and input.** This replaces the earlier text, which was wrong. The links are:
  * left/right: I0 <-> I1 <-> I2 <-> Accept (0x6dd540);
  * up/down: a letter's up neighbour is its "^", its down neighbour its "v" (0x6e19b0);
  * the arrows have no left/right links.

  Every frame the name-entry menu update **0x6ec460** (menu id 12, dispatched from 0x6ecdb0) does the following:
  * **Default selection:** if nothing is selected, item 2 (Initial0) is selected.
  * **Typing:** if a key character was typed (`[0x7ff760]+0x771`, not a space) and a letter is selected,
    0x6f5270(char, column) stores it:
    * A-Z as is, a-z upper-cased, '_' as '_';
    * digits and anything else are rejected.

    On success the text is rebuilt and the cursor moves right.
  * **"MenuBack"** (edge): moves to the left neighbour, if there is one. It never cancels.
  * **"MenuStart"** (edge): fires Accept (0x3e) at once.
  * **D-pad up/down on a letter:** if not in mouse mode (ui+0x2c == 0) and the selection is now on a "^"/"v"
    (actions 0x35..0x3a), the update fires that action and puts the selection back on the letter (items 2/5/8).
    So **d-pad up = previous character and down = next character, with the cursor staying on the letter**.
  * **Mouse:** the arrows are simply clicked.
  * **Backdrop:** fades in (11.2).
* **Character set and cycling** (0x6f5300 works on the PlayerData string in place):
  * Characters: **A-Z and '_' only**. Digits are not reachable, and any other character becomes 'A' on the next press.
  * Next ("v"): A -> B ... Z -> **_** -> A.
  * Previous ("^"): A -> **_** -> Z -> Y ...
  * A column > 2 returns '?'.

  So "TM_" is a legal entry.
* **Starting initials.** PlayerData (`[0x7ff6dc]`) +0x38 = **"username"**, loaded from the PlayerData save (0x6f85a0).
  It defaults to **"AAA"** (0x6f5120 / 0x6f85c1) when the save has no username. The letters are edited **in place**,
  so the last-entered name persists (saved with PlayerData) and pre-fills the next entry. The same string names every
  run entry (0x6f3460).
* **When it appears.** ShowResults handler 0x713730 requires:
  * the "Car" has TrickSystem2 (component 0x6f);
  * the level's score_style != 0;
  * the message's success flag. Success leads to `IsRealFinish = "1"`, then name entry, then results. A failed finish
    (dev key) goes straight to results.

  The handler has **no Story/Arcade test**. ui/levelsets.txt (last field = score_style) gives:
  * Campaign_Level1..6, Hardcore, Challenge, Old and Community levels: 7;
  * stunt levels: 6 (StuntLevel::DisplayResults always goes to name entry);
  * **Tutorial and AdvancedTutorial: 0**.

  So name entry and results **are shown in Story mode** after every campaign level. They are **never shown for the
  tutorials**: BaseLevel::Finish sends score_style-0 levels to state 5, so ShowResults is never sent, and the handler
  would refuse anyway. Inferred: Story uses the same LevelDesc entries as set 1_Campaign.

---------------------------------------------------------------------------------------------------------------------
## 12. Self ("S") replays behind the results: FailedRunCamera

Component ids used here, from the ComponentCreator registrations (`0x67a960(id, name)`):

| id | component | id | component |
|---|---|---|---|
| 0x0a | Camera | 0x47 | RenderMesh |
| 0x1b | ControllerCar | 0x4f | ChaseCameraLogic |
| 0x20 | CarLogic | 0x68 | IntroCarLogic |
| 0x36 | MenuButton | 0x80 | FailedRunCameraLogic |
| | | 0x81 | FailedReplayCar |

Level vtable slots (BaseLevel 0x7952bc / CampaignLevel 0x795334 / StuntLevel 0x7953ac / ChallengeLevel 0x79716c /
TutorialLevel 0x796284):

| slot | role | per class |
|---|---|---|
| 4 | **Init** (every load, including the reload done by a restart) | all classes 0x714ac0 |
| 7 | **Exit** (level unload) | all classes 0x712400; it calls slot 23 first |
| 19 | called at the end of Init (0x714a94) | Challenge: registers message 0x23 |
| 22 | **PostUpdate** (end of the level update, 0x7147dc) | Campaign 0x7110d0 / Stunt 0x724130 / Challenge 0x710d40 / Tutorial 0x70d920 |
| 23 | **OnExit** | Campaign 0x711080 / Stunt 0x723ca0 / Challenge 0x710c50 / Tutorial 0x70d590 (sounds only) |

Which class runs a level is chosen by sd_level: 0 Campaign (Story, Hardcore, Old and Community levels), 1 Stunt,
2 Tutorial, 3 Challenge.

### 12.1 When the copies are made, and what accumulates
* **`level+0x178` = "self-replay setup pending for this load".**
  * Init (slot 4, 0x714b3f) sets it to 1 on every load. Init also resets the rank caches (scoreMgr+0x10c..0x120 = -1)
    and gfx+0x3d0 = 0.
  * PostUpdate clears it right after doing the setup once.
* **Setup trigger (PostUpdate)**. Each class does the same thing; only the level state differs:
  ```
  if (+0x178) { RM_SetupFailedRunReplay(rm) /*0x6fe860*/;
                cam = Spawn("FailedRunCamera"); Init(cam); GC(cam,0x80)->restartGhostsToo(+0x99) = 1; +0x178 = 0; }
  ```

  | class | level state that triggers it | when that is |
  |---|---|---|
  | Campaign | **3 or 4** (so the first update in state 3) | the frame of the finish line: BaseLevel::Finish sets state 3, then 1.7 s later state 4 + ShowResults. The copy and the camera therefore exist about 1.7 s before name entry appears |
  | Challenge | **4** | the ShowResults frame, together with name entry |
  | Stunt | **4** | Finish(true), when the car has settled after time-up, together with name entry |
  | Tutorial / Autogen | never | no self replays |
* **Which recording is copied:** the RM's current recording (A, B, C, D, names at rm+0x1c/+0x34/+0x4c/+0x64/+0x9c).
  It covers GO to the point where recording stopped:
  * races: BaseLevel::Finish sets rm->state = 3 at the line;
  * stunt: Finish(true), after the coast;
  * a Challenge death-restart: the recording is still running at Exit, so the copy includes the explosion and the
    ~2 s until the reload.
* **RM_SetupFailedRunReplay 0x6fe860**:
  ```c
  if (rm->A.size < 50) return;                          // too short: nothing (also no rewind)
  if (rm->self.size < 20) { lf = new LF(index = self.size, isSelf = 1, colour = rm->palette[3] /*unused*/, "???");
                            rm->self.push_back(lf); rm->failedCars.push_back(null handle); }
  lf = rm->self.back();                                 // at 20: the LAST slot is overwritten
  LF_Reset(lf); CopyRecordingAsSelf(lf, ...);           // 0x682a20: copies A,B,C,D,names; names[i] += 'S'; state = 2
  rm->failedCars.back() = null;
  for (each self lf) LF_Rewind(lf);                     // objects destroyed, cursors to start, state 2 (not playing)
  ```
* **Exit / slot 23** decides what survives a reload. Self LFs live in the global RM and survive level loads.
  ClearGhosts and ResetRecording do not touch them.
  * **Campaign and Stunt:** RM_ClearFailedReplays 0x6ff180 (delete every self LF and handle) on **every** exit: Restart
    (pause menu or results), Continue/next level, Quit. **Only the run that reached the finish is ever replayed** (one S
    car). Failed or abandoned attempts produce nothing.
  * **Challenge:** `if (+0x191) ClearFailedReplays(); else RM_SetupFailedRunReplay(rm);`
    * Init -> slot 19 (0x710ce0) sets +0x191 = 1 and +0x190 = 0, and registers handler 0x710c10 for message 0x23.
    * Challenge slot 17 sets CarLogic+0x564 = 0, so there is no checkpoint respawn. When the car has been dead for
      more than 0.5 s, CarLogic (0x70aa7c) sends message 0x23. The handler sets +0x190 = 1.
    * Next PostUpdate: send RestartSdLevelMessage (0x1c), then +0x190 = 0 and **+0x191 = 0**. The restart reloads the
      level; Exit then keeps the dead attempt as a new S LF.
    * So **each death-restart in a Challenge level adds one attempt**: at least 50 actions, max 20, and the 20th slot
      is overwritten by later ones. A pause-menu Restart, Quit, Continue or a results Restart clears them all
      (+0x191 is still 1). At the final finish the successful run is appended last.
* Recording itself is reset on every load (LoadLevelGhosts), so each attempt starts a fresh recording.

### 12.2 FailedRunCameraLogic (0x80): vtable 0x79725c, ctor 0x40a800, Init 0x68bf00, serialize 0x68be20, update 0x68c9b0
**Blueprint fields** (all floats, no code defaults; `Objects/FailedRunCamera.xml` is **not provided**, so fetch it):

| offset | field | offset | field |
|---|---|---|---|
| +0x34 | Leash Distance | +0x44 | Fast Camera FoV |
| +0x38 | Camera Height (computed but **multiplied by 0**: no effect) | +0x48 | Fade Speed |
| +0x3c | Target Height | +0x4c | Initial Wait Time |
| +0x40 | Slow Camera FoV | +0x50 | Loop Wait Time |

The ChaseCamera equivalents (re_K): Leash 6, Target Height 1.2, Slow/Fast FoV 60/75, Fade Speed 3.

**Runtime fields:**
* +0x54 target handle; +0x60 up U (= (0,0,1) in practice); +0x6c unused blend (forced to 1);
* +0x70 state; +0x74 focus index; +0x78 fade (Init 1 = clear); +0x7c timer;
* +0x98 restart flag; +0x99 also restart normal ghosts (set to 1 by the level).

**Update(dt)**: does nothing while dt == 0, i.e. while paused.
```
if (fade < 1) { fade = min(1, fade + FadeSpeed*dt); gfx+0x3d0 = 1 - fade; }    // final-screen fade, 1 = black
switch (state) {
0: timer += dt; if (timer > InitialWait) { focus = selfCount-1; timer = 0; state = 1; restart = 1; }   // newest first
1: gfx+0x3cc = 1 (renderer one-shot "cut" flag); fade = 0 (black); state = 2;
2: if (restart) { RewindFailed 0x6fea00; StartFailed 0x6fcc80 /* every self LF 2->3 */;
                  if (+0x99) { RewindGhosts 0x6fec60; StartGhosts2 0x6fcf50 } }  // normal ghosts restart from frame 1 too
   fade = 0; state = 3; CarName [0x7ff6d4] = "ReplayCar";
3: fade = 0; car = failedCars[focus] (0x6fe7c0); if (!car) break;               // waits for the first spawn tick
   target = car; car->name = "ReplayCar"; self->object->name = "ChaseCam"; state = 4;
4: Camera.FoV = FastFoV; U = (0,0,1); Camera.FoV = SlowFoV; Snap 0x68bf70 (no-op: needs ControllerCar+CarLogic,
   which S cars lack); Activate camera 0x605520 (priority = ++counter -> this becomes the rendering camera); state = 5;
5: if (target alive) Follow(dt) else state = 6;  if (selfLF[focus].state != 3) state = 7;
6: FoV += 10*dt; if (failedCars[focus]) { state = 1; restart = 0; }  if (selfLF[focus].state != 3) state = 7;
7: timer += dt; FoV += 10*dt;
   if (timer > LoopWait) { focus = (focus+1) % selfCount; timer = 0; state = 1; restart = 1; }
}
```
* **Focus order:** newest attempt first (the finishing run), then wrapping to the oldest: 0, 1, ... It **loops
  forever** until the level is left.
* **Restarts:** at the start of every focus cycle **all** self replays **and** all normal ghosts are rewound and
  restarted from frame 1 together. The cycle only waits for the focused attempt to end. Ghost labels stay hidden: the
  label fade went to 0 at the player's finish (0x71348b).
* **Replay car explodes** (its object is destroyed by a destroy action):
  1. state 6: the camera freezes where it is and the FoV widens 10 deg/s while the gibs fly;
  2. when the recorded respawn spawns a new CarGhostS (FailedReplayCar.Init 0x6a4bb0 -> 0x6fd6d0 stores
     failedCars[lfIndex] = this object), the camera cuts: state 1, black, then fade-in;
  3. it re-acquires without restarting the replays.
* **Replay finishes** (LF state 4, frozen at its last pose just before the line): state 7, the camera holds, the FoV
  widens 10 deg/s for LoopWait, then a black cut to the next attempt.
* **Fade:** gfx+0x3d0 is a float parameter of the final-screen effect (semantic 0x2e, as in ChaseCam re_K). The screen
  is **black from state 1 until the car is acquired**, then fades in at FadeSpeed per second.
* **Edge case:** if no self LF exists (run < 50 actions), the focus is -1, state 3 never finds a car and the 3D view
  stays black.
* **Follow 0x68c200** (ChaseCam's "tumble" branch with T = 1 and W = 0; per-frame factors tuned for 60 fps):
  ```
  T = target world pos; C = camera world pos; U = (0,0,1); L = Leash
  D = T - C
  D = Rot(camRight /*cam col0*/, -0.3*Axis("CamTurnY")*L*dt) * Rot(U, 0.5*Axis("CamTurnX")*L*dt) * D   // right stick orbits
  h = |D - U(U.D)|;  h2 = h < L ? 0.9h + 0.1L : L;  r = h2 / L
  d = |D|;           d2 = d > L ? 0.7d + 0.3L : 0.9d + 0.1L
  want = T - normalize(D)*d2                    // no pitch clamp; (Camera Height + 3*CamTurnY) term x 0
  C = Raycast(static world 0x63ba80, from T, along want - T) ? hit.point : want     // no +0.07 normal push
  LookAt(T + U*TargetHeight*r, up = U)          // 0x678120
  ```
  This is a lazy string-leash: the camera keeps its current bearing and gets dragged behind the car to distance L.
  There is no snap behind the car on acquire. After a cut the camera starts where it was (first time: the
  FailedRunCamera blueprint position), and the 0.7/frame pull reaches L within about 0.3 s while the screen is still
  black.
* **Side effects:**
  * CarName "ReplayCar" is read by MortarGunLogic, DropShipLogic and BlockerLogic (they look the car up by that global
    name), so **those hazards target the focused replay car** during the results.
  * Renaming itself "ChaseCam" makes FindObject("ChaseCam") ambiguous (unverified which one wins).
* **Sound:** none. No sound call in 0x68bac0..0x68cd56; S cars have no sound components.

### 12.3 S blueprints
* **CarGhostS** (provided):
  * Transform; RenderMesh `car_full_no_wings.msh`, material **car_dead**, Color1 **(0.3, 0.6, 0.95, 3.0)**, i.e.
    bytes B 242, G 153, R 76 and glow byte 30;
  * ReplayPlayer; ChildObjectLogic (Copy Color 1: `CarGhostWingInnerLeftS`, `CarGhostWingInnerRightS`);
  * AnimBasic (Start 0, End 0.6, State 2, no deltas); **FailedReplayCar**;
  * **no PlayerName**.
* **Colours left alone:** LF_Spawn applies the palette only when !isSelf, and DimGhosts only touches cars with a
  PlayerName. So S cars keep the blueprint Color1 exactly: no palette, no dimming, no label. The wings copy the car's
  RenderMesh colour at Init (Copy Color).
* **Material:** car_dead is opaque (not the additive shockwave), so it is drawn in the normal shaded pass. Materials/
  car_dead.txt is not in my data. If it is phong_ref with the car textures, the port's FS_REF already gives the
  meaning: diffuse x light + emit.rgb x mix(1, Color1.rgb, emit.a) x (glow 3.0 x strength) + sphere-map reflection. That
  is the normal lit car with its emissive parts tinted light blue at glow 3.0 (inferred, not verified against the
  .txt).
* **Wreck pieces exist:** the copy appends 'S' to every recorded name, so a death in the replay spawns
  `DeadCarGhostBodyS`, `...BackS`, ... `DeadCarGhostTireS` (x4). These blueprints, their material and colour are
  **not provided**. Presumably the same car_dead look. Each spawn of a missing blueprint would hit the LF_Spawn assert
  (unverified behaviour).
* **Vehicles:** other vehicles record their own ghost names (e.g. `VanGhost`), which become `VanGhostS` etc. Every
  such blueprint needs a FailedReplayCar component or the camera never finds it (stuck black in state 3).
* **Blueprints to fetch from the user's Objects/ folder:**
  * `FailedRunCamera.xml`, `HudBlackSquare.xml`;
  * `CarGhostWingInnerLeftS.xml`, `CarGhostWingInnerRightS.xml`, `CarGhostWingOuterLeftS.xml`,
    `CarGhostWingOuterRightS.xml`;
  * `DeadCarGhost{Body,Back,BackFenderLeft,BackFenderRight,BackLeftLight,BackLightRight,Front,FrontFenderLeft,
    FrontFenderRight,FrontLightLeft,FrontLightRight,Tire}S.xml`;
  * `VanGhostS.xml` (plus the S names for the other vehicles; grep Objects/ for `Replay Object Name`);
  * the player car blueprint(s) for **"Finish Car Object Name"** (CarLogic+0x544) and that stand-in blueprint;
  * `Materials/car_dead.txt`;
  * optionally `menu_text.xml`, `menu_button.xml`, `HudTextPrimary.xml` (section 11).

### 12.4 What is on screen behind name entry and results
* **The world keeps running and rendering.** States 3, 4 and 5 are not a pause (dt > 0), so the RM ticks the
  replays, hazards animate and the level draws normally through the active camera.
  * Until the FailedRunCamera reaches state 4 (after Initial Wait Time; unknown, fetch the blueprint), the view is the
    normal ChaseCam. With an EndTunnel that is finish mode 6, tracking the stand-in.
  * After that it is the replay camera.
* **What dims it:** the HUD is hidden by the ShowResults handler (HideHud 0x712040(0)). The only overlay is the
  full-screen **HudBlackSquare** (11.2): it fades to glow 0.7 in name entry and keeps its blueprint value on the
  results screen. Nothing else blocks the view, so the replay is visible through the square (about 30% brightness if
  glow = alpha; verify with HudBlackSquare.xml).
* **Menu fade:** the black-cut fade (gfx+0x3d0) is applied in the final-screen pass. Whether the HUD and menus are drawn
  after it (and therefore are not faded) is unverified; most likely they are.
* **The player's own car is hidden from the finish on.** CarFinish -> 0x705da0 does the following:
  1. spawns the stand-in named by CarLogic **"Finish Car Object Name"** (+0x544) at the car's pose, copying its linear
     and angular velocity (an IntroCarLogic 0x68 on it drives it on, towards the EndTunnel when there is one);
  2. moves the real car +100 in z;
  3. runs respawn 0x702380 and then the hide sequence 0x702820 (components and recorder off).

  The stand-in stays wherever it rolls to, and is only seen if the replay camera passes it. The S car is a separate
  object (no collision with anything).

### 12.5 Port plan
1. On the success finish, or on a Challenge death-restart exit, copy the current recording into a self slot if it has
   at least 50 actions:
   * up to 20 slots, overwriting the last when full;
   * append "S" to the names;
   * keep it across the Challenge reload;
   * clear it on every other exit.
2. At the PostUpdate trigger (Campaign state 3, Challenge/Stunt state 4), start a FailedRunCamera object running the
   12.2 state machine. Restart self replays and normal ghosts together on each cycle; cut to black and fade in at
   FadeSpeed.
3. Render S cars with the car shader (opaque) and the blueprint colour. No label, no palette, no dimming.
4. During name entry and results keep rendering the 3D scene from that camera, under a black overlay that fades
   0 -> 0.7 (name entry) or uses the blueprint value (results). The player car is already hidden by the finish
   stand-in logic.

## Unverified / open (all sections)
* Exact meaning of the PC sample-time offset (4.2) at runtime; system update order (physics / replay / animation).
* Right-wing and other DeadCarGhost*/S/_Gold blueprints (not provided); gib-model mapping by name.
* Additive base material states for `shockwave`. (Text Draw Mode 2 sizing: resolved in 11.1.)
* Menu/HUD blueprint colours and fonts (`HudTextPrimary`, `menu_text`, `menu_button`, `HudBlackSquare`) - not in the data (11).
* Self-replay clearing: resolved in 12.1. Whether quitting before name Accept desyncs table and ghost files: still open.
* FailedRunCamera / HudBlackSquare / S-wing / S-gib / car_dead / Finish Car data (12.3): not in the provided data.
