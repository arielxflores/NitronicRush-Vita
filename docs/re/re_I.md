# re_I - Nitronic Rush STUNT MODE (`StuntLevel`) and the results / local score flow (PC exe)

All addresses are VAs in NitronicRush.exe. Helper dumps: `~/nrw/I/` (u713830.txt = BaseLevel update, f_0x712a10.txt =
countdown, f_0x714d60.txt = BaseLevel::Finish, st_upd.txt = StuntLevel clock 0x7241df.., st_rest.txt = StuntLevel
Finish/DisplayResults tail, r6290.txt = results screen 0x6e6290, res5770.txt = name entry, cl_upd.txt / die.txt = CarLogic).
Globals: `[0x7ff6dc]` PlayerData (pd), `[0x7ff6d8]` LevelSetMgr (`+0x20` = current LevelDesc), `[0x7ff754]` level manager
(`+8` = game mode 0 play / 1 pause / 2 editor), `[0x7fe7d0]` PlayerFeedbackTracking (run stats, re_H 0.4), `[0x7fe7d4]`
menu/results UI object (`+0x4e` = "level finished, pause disabled"), `[0x7ff6e0]` ScoreManager, `[0x7ff6ec]` announcer/
AudioLogic, `[0x7ff6d0]` ReplayManager. Wwise names from wwise_hash.txt.

---------------------------------------------------------------------------------------------------------------
## 0. Summary (the whole mode in 12 lines)
* Level class is chosen by the level-set field **`sd_level`**: 0 CampaignLevel, **1 StuntLevel**, 2 TutorialLevel,
  3 ChallengeLevel, other -> CampaignLevel (0x6f8840). All 5 Stunt Mode levels have sd_level 1, score_style 6.
* Round length = table `{120, 300, 600, 1200}[pd+0x170]` seconds (default 1200 if >3). Every shipped menu path that
  launches a level sets pd+0x170 = **1 -> 300 s (5:00)**. Nothing in the level XML / level set changes it.
* Clock counts down only in level state 2 (racing, after the normal 3-2-1 countdown) and not while paused.
  HUD timer (hud_timer, same element as the race timer) shows `MM:SS` (truncated seconds, no hundredths).
* Voice countdown `Play_10 .. Play_1` once each when timeLeft < 11, 10, ..., 2. (5-min/1-min/30-s lines exist in code but
  never fire - bug, sec. 2.4.)
* At **timeLeft <= 0.8 s**: broadcast LevelFinishedMessage(success = **false**), car gets no more input (CarLogic state 10)
  and coasts; when the car has settled (sec. 2.6) -> StuntLevel::Finish(true): "Play_Finish", HUD hidden, run inserted
  into the LOCAL table, name entry (3 initials) -> results screen.
* Score = PlayerFeedback points = sum of landed trick points (round(base x multiplier), re_H 0.5). No time bonus, no death
  penalty, no stunt-specific multiplier. Deaths: car respawns at the last TriggerRespawnLocation after 2.0 s (or at once
  on CarResetHard); clock keeps running; score kept; trick in progress lost.
* Ranking for style 6: higher score first, then higher biggest trick; complete tie -> new run placed ABOVE the old one.
  No shipped default scores for stunt levels -> the first stunt finish on a level is always rank 0 ("First!").
* **There are no trophies/medals in the PC exe** (HudTrophy*.xml, hud_trophy_* materials and trophy_*.png exist but no
  code references them; no gold/silver/bronze thresholds anywhere).

---------------------------------------------------------------------------------------------------------------
## 1. Registration, class selection, vtable, fields

### 1.1 Factory registration (0x738a00.. level class table)
`size, ctor, vtable, name` (pattern: alloc(size); BaseLevel ctor 0x40bb90; *this = vtable; register(name)):
| class | size | vtable | note |
|---|---|---|---|
| TutorialLevel | 0x198 | 0x796284 | |
| CampaignLevel | 0x190 | 0x795334 | plain race level = BaseLevel + no-op overrides |
| **StuntLevel** | **0x1b0** | **0x7953ac** | name string 0x7943a4, RTTI `.?AVStuntLevel@N@@` (type desc 0x7f9a84, never dynamic_cast) |
| ChallengeLevel | 0x198 | 0x79716c | |

### 1.2 Which class a level gets - LevelClassName(LevelDesc) 0x6f8840 (callers 0x6f91a8 play level, 0x70dab8 restart)
```c
switch (levelDesc->sd_level /*+0x3c*/) { case 1: return "StuntLevel"; case 2: return "TutorialLevel";
                                          case 3: return "ChallengeLevel"; default /*0, >3*/: return "CampaignLevel"; }
```
LevelSets/README.TXT documents `sd_level` 0 Campaign / 1 Stunt Mode / 2 Tutorial. Other LevelDesc fields: +0x78 music,
+0x7c score_style. LevelSets/5_StuntMode.xml (set name "Stunt Mode", description "Get the highest score before the clock
runs out!"): every level `unlocked 1, sd_level 1, score_style 6, wings_enabled 1`:
| level name | xml_file | music | Wwise (AudioLogic 0x6dc880 switch, index-1 table 0x6dcab4) |
|---|---|---|---|
| Arena | Arena.xml | 7 | Play_StuntMode_1 (0xfa7ba977) |
| Neon Skateway | vert_park.xml | 21 | Play_PurpleEighties (0xffdb769e) |
| Christmas Court | christmas.xml | 22 | Play_ItsChristmas (0x2e52a91c) |
| Car-kour | neon_nights.xml | 9 | Play_NewPurple (0xc463ed8e) |
| Floating Free Zone | sky_city.xml | 12 | Play_Neon_Skyway (0xe4c71731) |

(Full music index table: 1,2 MainMenuPlaylist, 3 Level1Playlist, 4 CreditsPlaylist, 5 Level1Playlist2, 6 Level1Playlist3,
7 StuntMode_1, 8 GreenRMX, 9 NewPurple, 10 CoreFinal, 11 First_Playable, 12 Neon_Skyway, 13 Beta_Red, 14 IntroCutscene,
15 Finale, 16 Core8Bit, 17 CoreHeart, 18 BlueWubstep, 19 PinkClockwork, 20 CreditsOldPlaylist, 21 PurpleEighties,
22 ItsChristmas, 23 Gladiator, 24 Nature, 25 Nitroception, 26 TurboTunnel, 27 EnterTheMatrix.)

### 1.3 vtable 0x7953ac (StuntLevel) vs 0x795334 (CampaignLevel) - only the bold slots differ
| slot | StuntLevel | CampaignLevel | meaning |
|---|---|---|---|
| 1 | 0x7160d0 | same | BaseLevel Enter/load: ... 0x7118f0 field init, vslot 16, vslot 15, **vslot 17** |
| 9 | 0x713830 | same | BaseLevel per-frame update (state machine, HUD timer, pause key); calls **vslot 22** at the end |
| 12 | 0x715440 | same | BeginCountdown (state 1, clock -6 s) |
| 13 | 0x715080 | same | StartRace (state 2, "Race Start" event -> run stats zeroed) |
| **14** | **0x723f40** | 0x714d60 | Finish(bool success) |
| **17** | **0x723c10** | 0x711070 (ret) | InitRound: set the stunt clock |
| **19** | **0x723cc0** | 0x7110a0 (log only) | PostBaseLevelEnter |
| 20 / 21 | 0x7113a0 / 0x712a10 | same | state 0 / state 1 (countdown) update |
| **22** | **0x724130** | 0x7110d0 | PostUpdate(dt): stunt clock + states 4/5/8 |
| **23** | 0x723ca0 | 0x711080 | identical code: ReplayMgr 0x6ff180; return 1 |
| 26 | 0x40b640 | 0x40b640 | OnFinished (no-op; StuntLevel::Finish never calls it) |
| **28** | **0x723d70** | - | DisplayResults(bool) (new virtual) |

### 1.4 Fields
BaseLevel (used here): +0x3c state, +0x40 prev state, +0x48 double race clock, +0x50 double aux timer, +0x58 replay
recording active, +0x148 show timer (1), +0x149 timer shows hundredths (BaseLevel init 0x7118f0 = 1), +0x14c float value
shown by the HUD timer, +0x151 countdown/announcer enabled (1), +0x152 finish success, +0x154 finish time, +0x158 results
shown, +0x174 music index (LevelDesc+0x78), +0x178 failed-run-camera request, +0x179 music-started flag.
StuntLevel additions (size 0x1b0):
| off | type | meaning |
|---|---|---|
| +0x190 | float | **time left** (s) |
| +0x194 | float | always 6.0 (meant to be the initial duration - see 2.4) |
| +0x198 | byte | clock stopped; set 0 in 0x723c10, never set to 1 |
| +0x19c | int | 1 = Finish(true) done (results started); 0 after timeout until the car settles |
| +0x1a0..+0x1ac | byte[13] | "voice line played" flags: 5 min, 1 min, 30 s, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1 |

Level states (+0x3c): 0 intro, 1 countdown, 2 racing, 3 race finished (normal), 4 results, 5 continue/fade, 6 restart
fade, **8 stunt time-up / leaving** (not in the BaseLevel switch 0x7147f8, handled only by StuntLevel slot 22).

---------------------------------------------------------------------------------------------------------------
## 2. The round clock

### 2.1 Duration - StuntLevel::InitRound (slot 17, 0x723c10, called at the end of Enter 0x7160d0 -> every load / restart)
```c
this->stopped(+0x198) = 0; this->timeLeft(+0x190) = 6.0f; this->f194 = 6.0f; this->resultsDone(+0x19c) = 0;
float d; switch (pd->stuntTimeChoice /*+0x170*/) { case 0: d = 120; case 1: d = 300; case 2: d = 600; default: d = 1200; }
this->timeLeft = d;  this->hudTime(+0x14c) = d;  this->hudHundredths(+0x149) = 0;      // HUD shows MM:SS
```
pd+0x170 writers: PlayerData ctor 0x6f7f54 (= 1); main-menu actions 70/71/74 (all Arcade/level launches: 0x6ebdd3,
0x6ebee6, 0x6ec03b, = 1); menu actions 36/37/38/39 (0x6eb4e1/0x6eb5ab/0x6eb66f/0x6eb6fe, = 0/1/2/3, then launch level set
"Stunt Mode" as StuntLevel). No menu button with action ids 36-39 is created anywhere (all `0x6df700` button calls
checked; no table either) -> **dead code; shipped duration is always 300 s** (unverified only in that a computed id
could exist). pd+0x170 is not read from userdata (not persisted - unverified).

### 2.2 Start
Same start sequence as a race (shared slots 12/21/13): clock +0x48 = -6.0 if the config bool `PlayCountdown` (default 1);
at +0x48 >= -3.0 ChangeMusicMessage(0x17, music index) + `Play_NitronicRushStart` (0x91cf2d06); countdown_3 / _2 / _1
objects spawned once around t = -3 / -2 / -1 (FPU tests against -3/-2.9, -2/-1.9, -1/-0.9; exact window bounds
unverified; only if pd+0x7f and +0x151); at +0x48 > 0 StartRace (state 2,
"Race Start" -> per-run stats zeroed) and `countdown_rush`. **The stunt clock starts at GO** (it only runs in state 2).

### 2.3 Per-frame clock - StuntLevel::PostUpdate(dt) (slot 22, 0x724130; called last in BaseLevel update 0x7147d3)
BaseLevel's own state-2 code (0x713f94) runs first each frame: raceTime(+0x48) += dt; hudTime(+0x14c) = raceTime;
lifetime Time Played; then this function overwrites hudTime with the time left:
```c
void StuntLevel::PostUpdate(float dt) {
  if (!stopped && state == 2 && levelMgr->mode /*[0x7ff754]+8*/ == 0) {     // frozen in pause (1) and editor (2)
    timeLeft -= dt;  hudTime(+0x14c) = timeLeft;
    if (f194 > 300 && !flag[0] && timeLeft < 301) { Post(Play_FiveMinutesLeft 0xa120f97e); flag[0] = 1; }  // never (f194 == 6)
    if (f194 >  60 && !flag[1] && timeLeft <  61) { Post(Play_OneMinuteLeft   0xb83aae6d); flag[1] = 1; }  // never
    if (f194 >  30 && !flag[2] && timeLeft <  31) { Post(Play_ThirtySecondsLeft 0xfbe5b20a); flag[2] = 1; } // never
    static const uint32 num[10] = { Play_10 0xbde0c9f5, Play_9 0x69be7e4f, Play_8 0x69be7e4e, Play_7 0x69be7e41,
        Play_6 0x69be7e40, Play_5 0x69be7e43, Play_4 0x69be7e42, Play_3 0x69be7e45, Play_2 0x69be7e44, Play_1 0x69be7e47 };
    for (k = 0; k < 10; k++) if (!flag[3+k] && timeLeft < 11 - k) { Post(num[k]); flag[3+k] = 1; }  // <11 "10" ... <2 "1"
  }
  if (state == 2 && !stopped && timeLeft <= 0.8f) {                           // TIME UP
    Broadcast(LevelFinishedMessage{ id 0x13 /*vt 0x793c94*/, success = 0, 0.0f });
    if (CarLogic* cl = Find("Car")->Get(0x20)) { cl->wingsOut(+0x58) = 0; cl->state(+0x1f0) = 10;
                                                  cl->stateTime(+0x60) = 0; cl->f1ec = 0; }
  }
  if (state == 8 && !resultsDone) {                                           // wait for the car to settle
    GO* car = Find("Car");
    if (!car || (car->Get(0x20) && CarSettled(cl) /*0x6ffdc0*/)) { resultsDone = 1; this->Finish(true); }
  }
  if (state == 5 && auxTimer(+0x50) > 0.0) { ChangeLevel("MainMenuLevel"); state = 8; }   // after "Continue"
  if (state == 4 && failedRunCamera(+0x178)) { ReplayMgr 0x6fe860; spawn "FailedRunCamera" (+0x99 = 1); +0x178 = 0; }
}
```
* `Post(id)` here = `0x664630(id, [0x402ae0()]+0x14)`: direct post on the global sound object (not the announcer queue).
* The voice "10" is heard when the HUD turns to 00:10 (timeLeft < 11, display = trunc), ..., "1" at 00:01; the HUD reads
  00:00 for the last 0.2 s; the round ends at 0.8 s left -> effective playing time = duration - 0.8 s (299.2 s).
* +0x14c is set to timeLeft only while the clock runs; after time-up it stays at <= 0.8 ("00:00").

### 2.4 Bug: 5-minute / 1-minute / 30-second lines never play
+0x194 is written only in 0x723c10 (= 6.0; xrefs of `+0x194]` checked). The three checks require +0x194 > 300 / 60 / 30,
so `Play_FiveMinutesLeft`, `Play_OneMinuteLeft`, `Play_ThirtySecondsLeft` are dead on PC. (Intended: +0x194 = duration;
with 300 s the first would fire at 300.999.. -> immediately at GO.) Port: reproduce PC (skip them) or fix behind an option.

### 2.5 Pause
Pause key (BaseLevel update 0x714542, action "Pause"): allowed only when mode == 0 and `[0x7fe7d4]+0x4e == 0`; sets mode 1
(0x67d760 also sets time system [0x7ff774]+0x41 = 1), pause menu 0x6e9500, `Stop_MainMenuPlaylist` (0x2d99bf04) when AudioLogic music index (+0x68) != 1,
`Pause_Master_Audio_Bus` (0xae84e192); resume -> `Resume_Master_Audio_Bus` (0x6885573d). The stunt clock is explicitly
frozen while mode != 0. Both Finish paths set `[0x7fe7d4]+0x4e = 1` -> **no pausing after time-up** (coast + results).
Pause menu itself is the generic one (Resume / Restart / ... ) - nothing stunt specific. Restart -> RestartSdLevelMessage
(0x1c, 0x714b80) -> state 6, after 1.5 s LevelRestarterLevel reloads the level -> clock reset to the full duration.

### 2.6 Time-up -> results
1. LevelFinishedMessage(0x13, success=0) handlers: BaseLevel 0x711780 (success -> +0x152, clock -> +0x154, then
   vslot 14 = **StuntLevel::Finish(false)**); TriggerAtFinish objects (none in stunt levels); PlayerData 0x6f5890 does
   nothing because success == 0 (**stunt runs never update lifetime "Best Score On One Level" pd+0xe4** - PC quirk).
2. StuntLevel::Finish(bool s) 0x723f40:
```c
ui->finished(+0x4e) = 1;                                  // disables pause
if (!s) { if (state == 8) return; state = 8; raceClock = 0; resultsDone = 0; aux = 0; Ui_0x6f94b0(); return; }
if (state == 4 || state == 3) return;
raceClock = 0; state = 4; aux = 0; resultsDone = 1;
if (replayRecording(+0x58)) replayMgr->state(+0xc) = 3;
CarFinish 0x713410(0): if car not dead (state != 0xb): TrickSystem2::Reset (trick in progress discarded),
                       0x705da0(car, 0) (finish pose; flag 0 = no "EndTunnel" placement), replayMgr+0x154 = 0;
camera: finish camera object (+0x88) placed at the car (+0x70) position/orientation (same block as BaseLevel::Finish);
Announcer queue 0x6dc6e0: Play_Finish (0x6aeaaf3b);
this->DisplayResults(s);                                  // vslot 28
if (replayRecording) SaveGhost 0x6f4260;   Ui_0x6f94b0();
```
   Unlike BaseLevel::Finish it does NOT: emit "Race Finish", award Still Alive (25), call vslot 26, go through state 3 /
   the 1.7 s delay / ShowResults message 0x14.
3. CarSettled(cl) 0x6ffdc0 = `cl->stateTime(+0x60) > 30 || cl->f1ec > 1.5 || cl->state == 7 (dead) || body(comp 0x1b)
   +0x27 == 0 (asleep)`. In car state 10 (0x70ab2f) inputs are cleared every frame (0x6ff880: throttle/brake/steer =
   0), +0x1ec accumulates dt while |v|^2 < 100 and |angVel|^2 < 3 (else reset to 0). So results appear after the car has
   been slower than 10 u/s for 1.5 s, or is asleep, or 30 s max. TrickSystem2 keeps running (its update 0x6dc3e0 has no
   level/car-state gate) -> a trick that lands during the coast still adds points (likely; unverified in game).
4. DisplayResults(bool s) 0x723d70 ("StuntLevel> Local Ranking is %d"):
```c
float dur = {120,300,600,1200}[pd+0x170];                // recomputed
int rank = ComputeRank(dur);                              // 0x6f3d80, LOCAL table, current run = {dur, run.points, run.biggestTrick}
if (s) { InsertLocal(rank, dur) /*0x6f3460*/; rank = RankFor(sm->category /*+0x144*/) /*0x6f3b60*/; }
if (sm->category != 1 /*Local*/) rank = rank of the run in that category's set (0x6f38f0 + 0x6f3750);
if (Car has TrickSystem2) {
  HideHud 0x712040(0);  input+0x66c = 1, +0x66d = 1 (rumble suppressed);
  if (s) { Settings["IsRealFinish"] = "1"; NameEntry 0x6e5770(ui); }       // always taken (s == 1)
  else   ResultsScreen 0x6e6290(ui, sm->category, rank, 0);
}
this->resultsShown(+0x158) = 1;
```
   The stored/compared "time" of a stunt entry is the round duration (300.0), so the Time column reads 05:00.00.
5. Name entry (0x6e5770): "Enter Name...", three letter buttons NameEntry_Initial0..2, "Accept" (menu action 0x3e).
   Accept -> MenuButtonUsedMessage (0x1a) -> BaseLevel 0x713640: `rank = 0x6f3b60(category); 0x6e6290(ui, category,
   rank, 0)` (results screen). Results "Continue" (action 0x33) -> aux = 2.5, state 5, `Continuing to Next Level` = "1"
   -> StuntLevel slot 22 -> **MainMenuLevel** (the Arcade launch set `MainMenuLevel: Jump to Arcade menu` = 1, so the
   player lands back in the Arcade menu). Restart -> state 6 reload as in 2.5.

---------------------------------------------------------------------------------------------------------------
## 3. Scoring, ranking, results screen

### 3.1 Score
* Run score = `[0x7fe7d0]+0x10` (PlayerFeedbackTracking points) += trick points in "Trick Complete" 0x6fb7f0 (+0x14 tricks
  landed ++, +0x18 biggest trick = max). Trick points = round(base(+0x44) x multiplier(+0x48)), multiplier = number of
  distinct trick components (re_H 0.5). No other writer: no time bonus, no checkpoint bonus, no death penalty ("Car Death"
  0x6fae50 only bumps the death counters +0..+0xc), nothing stunt-specific (no code tests sd_level/StuntLevel besides 1.2).
* Zeroed by "Race Start" (each attempt / restart). Respawns do not touch it.
* HUD: TrickCompleteMessage (0x19) handler 0x712170 (registered in Enter 0x71635a): hud_main item 2 (`hud_score_num`) =
  itoa(points) (and an optional "Score: N" text object at level+0xb4 if present). Same in races.
* Achievements tied to stunt: 100 Grand (1) needs run points >= 100,000 and style 6; Still Alive (25) never in stunt;
  Rush Hour (7) counts stunt racing time (re_H).

### 3.2 Local ranking - ComputeRank(time) 0x6f3d80 and comparator 0x420320
```c
sm->cur.time(+0xe0) = time; sm->cur.score(+0xe4) = run.points; sm->cur.trick(+0xe8) = run.biggestTrick;
if (levelId empty || level->score_style == 0) return -1;
ScoreSet* s = GetSet(sm, 1 /*Local*/, 0);  if (s->style != style) { clear(s); s->style = style; }   // entries 0x48 bytes
int rank = 0; while (rank < s->count && Beats(&s->e[rank], &sm->cur, style)) rank++;
sm->rank(+0x110) = rank; return rank;
bool Beats(e, c, style) {           // e = table entry, c = current run; +0x1c time, +0x20 score, +0x24 biggest trick
  if ((style & 1) && e.time  != c.time ) return c.time  > e.time;      // shorter time first
  if ((style & 2) && e.score != c.score) return e.score > c.score;     // higher score first
  if ((style & 4) && e.trick != c.trick) return e.trick > c.trick;     // bigger trick first
  return c.time > e.time;                                               // full tie: new run goes in FRONT
}
```
(Correction to re_H "ties go after": only when the fallback time differs; an exact tie inserts the new run first.)
InsertLocal(rank, time) 0x6f3460 builds the entry (initials, time, score, biggest trick, date) and inserts it at `rank`;
no size cap seen (unverified). Score categories (0x6f2980): 0 Temporary, **1 Local** (default, ScoreManager ctor
0x6f36cf), 2 Default, 3 Developer, 4 Imported, 5 Online. Shipped defaults exist only for the 9 levels in 6.2 (none for
stunt levels) -> stunt rank 0 on the first finish of each level.

### 3.3 Results screen 0x6e6290(ui, category, rank, flag) - shared by races and stunt
* Title "Results"; category label (results_category, "<< Cat" / "Cat >>" buttons = actions 0x41/0x40); buttons Back,
  Continue (0x33), Restart, Quit, "Post Result" (online; not for Vita).
* Rank voice via announcer queue: rank 0 -> `Play_FirstPlace` (0x2d7e2655) + Unlock(13 First!), 1 -> `Play_SecondPlace`
  (0x8ef12adf), 2 -> `Play_ThirdPlace` (0x42773aea), else nothing.
* Table headers (HudTextPrimary): "Time", "Score", "Best Trick". Rows (0x6e08d0): `"%s. "` rank number + initials, or
  `[YOU]` for the extra 11th row (index clamped to 10) showing the current run when outside the top 10; Time column =
  FormatTime(time, hundredths=1); Score and Best Trick = integers. No "tricks landed" column, no trophies.
  (Exact layout constants: see r6290.txt / 0x6e0900; not needed for logic.)

### 3.4 Trophies
None. Strings "Trophy"/"trophy"/"Medal"/"Bronze"/"Silver" do not occur in the exe (ASCII or UTF-16); HudTrophies.xml,
HudTrophyGold/Silver/Bronze.xml (quad.msh + materials hud_trophy_gold/silver/bronze, Textures/trophy_*.png) are unused
leftovers. No thresholds exist in LevelSets, Levels or data/. A port that wants medals must invent them.

---------------------------------------------------------------------------------------------------------------
## 4. Death and respawn in stunt levels
* The level ignores deaths (PlayerDeathMessage 0x15 is handled only by PlayerData 0x6f7dd0; no level handler): the
  clock keeps running, state stays 2, run points kept; TrickSystem2::Reset on death drops the trick in progress.
* Die 0x707db0 (re_A): explosion, DeadCarBody + gibs, `Play_Rand_Car_Explosion` (0xbec1cc50), `Play_Rand_Electric_Arc`
  (0x8c9a715a), Wwise states Crash_States = Crashed, Announcer_Crash = Crashed; then it **immediately moves the car to the
  respawn point** (0x702380: respawnPos +0x1a4 / respawnRot +0x1b0, ray-cast down 25 u), +0x14c = 1.0, heat = 0,
  car transform z -= 15 (hidden under the spot), car state 7, state timer +0x60 = 0.
* Car state 7 (0x70aa1c): inputs cleared; CarLogic+0x564 ("auto respawn", set 1 in CarLogic init 0x708b9f; only
  ChallengeLevel clears it, 0x710c47) -> when stateTime > **2.0 s** or the `CarResetHard` action is pressed:
  Respawn 0x7014d0 (Wwise SetState Crash_States = Normal (0x4bdf8ceb/0x4527c498), Announcer_Crash = Normal
  (0x346be63a), camera reset) -> state 8 -> next frame state 9 -> next frame `Play_CarEngine` (0x2ec40d6e), state 1
  (driving), +0x1ec = -5. So control returns ~2.0 s + 2 frames after the crash.
* Respawn point = last TriggerRespawnLocation fired (re_F2 2.1; 99 zones in the 5 levels) or the start transform.
  Arena additionally has one `checkpoint` object (CheckpointLogic, at (1100.83, 1562.46, 255.41)): passing it shows
  "Checkpoint", queues `Play_NitronicRushCheckpoint` (0xa9b18a64), spawns checkpoint_ghosts and sets the respawn point;
  it gives no time.
* Death at the very end: the time-up code forces car state 10 even if the car was in state 7; CarSettled then returns
  true quickly (asleep body) -> results.

---------------------------------------------------------------------------------------------------------------
## 5. Differences from a normal race level (CampaignLevel/BaseLevel)
| aspect | race (CampaignLevel) | stunt (StuntLevel) |
|---|---|---|
| start | countdown, music at -3 s, Play_NitronicRushStart, countdown_3/2/1/rush | identical |
| music | level set `music` | identical mechanism (table 1.2) |
| HUD timer (hud_timer, HudLogic item 4) | race clock counting up, `MM:SS.hh` | time left counting down, `MM:SS` |
| HUD score (hud_score / hud_score_num) | shown, = run points | same |
| end | finish line -> LevelFinishedMessage(1) -> Finish(1): Play_Finish (not Tutorial), rank vs local table by time (style 7), state 3, after 1.7 s ShowResults (0x14) -> name entry -> results | time-up (0.8 s left) -> LevelFinishedMessage(0) -> state 8 coast -> Finish(1) when settled -> Play_Finish -> name entry -> results |
| car at end | 0x705da0(car,1): placed relative to "EndTunnel" | 0x705da0(car,0): stays, coasts with no input |
| events | "Race Finish", Still Alive, vslot 26 | none of these |
| lifetime Best Score (pd+0xe4) | updated (success 1) | never updated |
| voice | Play_Finish, place lines | + Play_10..Play_1 countdown (5min/1min/30s dead) |
| Continue | next level / outro / menu (0x7110d0) | always MainMenuLevel (-> Arcade menu) |
| pause | same menu; disabled after finish | same; disabled from time-up on |
| rumble | - | PostBaseLevelEnter: app+0x72d = pd+0x7c (rumble option), clear rumble if off, input+0x66c = 0 (port: ignore) |

HUD timer element (Objects/hud_timer.xml): Text "00:12:55" placeholder, font hemihead.fnt, colour (1, 1, 1, 0.9), Text
Scale 15x15, Draw Mode 2, Transform position (0.425038, 0.053887, 0), scale 0.292661 (HudLogic on hud_main - quad at 160x90, scale 32 - looks the 6 items up by name in 0x6fc938;
how their coordinates map to the screen: see the HUD report, unverified here). hud_score "Score" at (0.1703, 0.081) scale 0.15625; hud_score_num at (0.23432, 0.0816) scale 0.2148; same font
and colour. No colour change / flashing in the last seconds anywhere in code (only the voice countdown).
BaseLevel additionally writes "Time: " + FormatTime into an optional text object (level+0x130.., at (0.01 W, 0.99 H),
scale 0.666 x H x k) when present and state != 8 - creation not found (dev overlay, unverified).

FormatTime(t, hundredths) 0x723b60:
```c
int s = (int)t;  /* __ftol2: truncate */  int mm = s / 60, ss = s % 60;
if (!hundredths) return sprintf("%02d:%02d", mm, ss);
char f[10]; vsprintf_s(f, 10, "%0f", fmod(t, 1.0)); f[4] = 0;      // "0.234567" -> "0.23"
return sprintf("%02d:%02d.%s", mm, ss, f + 2);                       // truncated hundredths
```

---------------------------------------------------------------------------------------------------------------
## 6. Normal race results and shipped default scores

### 6.1 Race flow (BaseLevel::Finish 0x714d60, update 0x713830, ShowResults handler 0x713730)
Finish(success) only from state 2: if success { Play_Finish unless TutorialLevel; rank = ComputeRank((float)raceClock);
if rank >= 0 InsertLocal(rank, time) } ; clock = 0; state = 3 (5 if score_style 0); Still Alive; CarFinish(1)
(EndTunnel); ghost save; vslot 26; "Race Finish". State 3: clock += dt; at > 1.7 s -> state 4 + ShowResults{success,
time}. Handler (only if score_style != 0 and car has TrickSystem2): HideHud; rank = RankFor(category); success ->
"IsRealFinish" = 1, name entry -> Accept -> results 0x6e6290; failure (DevKey RaceFinish) -> results directly.
Rank rule style 7: time ascending (strict, then score desc, trick desc as tie-breaks), see 3.2.

### 6.2 data/default_scores.xml (version 2, all ScoreSet style 7, date 2011-Oct-02) = Default/<level>.xml/scores.NrScoreSet
(category 2, each with NrGhostBin ghosts)
| level | #1 | #2 |
|---|---|---|
| Campaign_Level1 | TRN 185.234039 s (03:05.23), 2304, trick 550 | CLU 316.099274 (05:16.09), 19060, 3450 |
| Campaign_Level2 | SSH 317.872253 (05:17.87), 14075, 4350 | FTP 432.575745 (07:12.57), 24771, 4800 |
| Campaign_Level3 | VIM 251.966 (04:11.96), 7694, 900 | EMC 512.045 (08:32.04), 19252, 1600 |
| Campaign_Level4 | NIX 354.263 (05:54.26), 19289, 5229 | WIN 454.946 (07:34.94), 30828, 8000 |
| Campaign_Level5 | CAR 319.607 (05:19.60), 14443, 4455 | COR 455.242 (07:35.24), 9450, 3933 |
| Evil_Carwash | NES 255.438 (04:15.43), 1324, 550 | - |
| Hardcore_BlueLevel | GEN 336.983 (05:36.98), 19507, 1228 | - |
| Hardcore_RedLevel | TM_ 478.950 (07:58.94), 47497, 2787 | - |
| Hardcore_TealLevel | SFR 613.361 (10:13.36), 59992, 2148 | - |
(Exact floats in the xml; mm:ss.hh shown as the game formats them, truncated.) data/default_scores/Developer/ has score
sets only for Calcite_by_Pixelmann (_O_ 143.466 s, 8029, 1514) and Ruby_by_Pixelmann (_O_ 108.792, 6785, 1868; _O_
113.013, 4773, 686), category 1; the other Developer folders hold ghosts only. No defaults for any stunt level.
Per re_H the Local table is seeded from these defaults (not re-verified here).

---------------------------------------------------------------------------------------------------------------
## 7. Port checklist
1. Level class from `sd_level` (1 = Stunt). Stunt levels: Arena, vert_park, christmas, neon_nights, sky_city; style 6.
2. Round length 300.0 s (keep the 120/300/600/1200 table keyed by a 0..3 setting; default 1 = 300).
3. Run the normal countdown (-6 s clock, music + Play_NitronicRushStart at -3, 3-2-1-rush); start the stunt clock at GO.
4. Each racing frame, not paused: timeLeft -= dt. HUD timer text = `MM:SS` of trunc(timeLeft) in hud_timer.
5. Voice once each: timeLeft < 11 -> Play_10, < 10 -> Play_9, ..., < 2 -> Play_1 (direct post, not queued).
   Do NOT play FiveMinutesLeft/OneMinuteLeft/ThirtySecondsLeft (PC bug), unless deliberately fixing.
6. timeLeft <= 0.8: broadcast LevelFinished(success=false) (TriggerAtFinish fires), disable pause, wings in, car input
   zeroed (state 10) and let it coast.
7. Results when the car is slow (|v| < 10 u/s and |w|^2 < 3) for 1.5 s, or asleep, or dead, or 30 s passed: reset the
   trick system, queue Play_Finish, hide HUD, insert {initials, time = 300.0, score, biggest trick, date} into the local
   table of the level (style 6), name entry (3 initials) -> results screen (Time/Score/Best Trick, top 10 + [YOU]).
8. Rank = count of leading entries that beat the run: higher score, then higher biggest trick, exact tie -> new run first.
   Rank 0/1/2 -> Play_FirstPlace (+ achievement 13) / Play_SecondPlace / Play_ThirdPlace.
9. Score = sum of landed trick points; no bonuses, no death penalty; tricks landing during the coast still count.
10. Death: same CarLogic flow as races; respawn at the last TriggerRespawnLocation after 2.0 s (CarResetHard skips the
    wait); clock and score continue.
11. Continue -> main menu (Arcade); Restart -> reload level (full 300 s again).
12. Don't update lifetime "Best Score On One Level" from stunt runs (PC quirk) - or do, if you prefer the obvious intent.
13. No trophies/medals (none in PC); races show times as `MM:SS.hh` (truncated), stunt `MM:SS`.
