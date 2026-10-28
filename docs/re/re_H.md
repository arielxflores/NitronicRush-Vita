# re_H - Nitronic Rush achievements: exact unlock conditions (PC exe)

All addresses are VAs in NitronicRush.exe. "HUD MPH" = world speed x 2.33386 (GetSpeedMPH 0x62e260: forward
speed dot(vel, carForward) when >=2 wheels touch, else |displacement|/dt, times 2.33386, abs).

## 0. Machinery

### Tables
* Names `const char* name[50]` at 0x7f56c8, descriptions `desc[50]` at 0x7f5790 (NULL = "secret"), ids 0..48 are real
  (loops run `id < 0x31`); index 49 "2012_12_21" is never used.
* Visible list: 0x6cf4a0 builds `visible[]` (0x7f5858) = ids whose desc != NULL, count -> [0x7f56c4] (= 31).
  The PC achievements menu (0x6b1644) iterates ONLY this list, so the 18 NULL-description achievements are never shown.

### Unlock(id) - 0x6fb620 (`this` = [0x7fe7d0], __thiscall, ret 4)
```c
void Unlock(int id) {
  PlayerData* pd = *(PlayerData**)0x7ff6dc;
  if (pd->achieved[id]) return;                       // byte at pd+0x82+id
  pd->achieved[id] = 1;                               // (inlined SetAchieved 0x6f5460 -> recursive no-op)
  if (!FindByName("achievement_display"))             // only one popup object
      Spawn("AchievementDisplay")->Init();
  Broadcast(new AchievementMessage(name[id]));        // 0x6ff680 (vt 0x799eac) -> AchievementDisplayComponent queue
  int n = 0; for (int i = 0; i < 49; i++) n += pd->achieved[i] != 0;
  if (n >= 10) Unlock(19 /*Bleep Bloop*/);            // recursion; n counts the one just set
  if (AllUnlocked())                                  // 0x6f55e0: count(achieved[0..48]) == [0x7f56c4] (31 visible)
  { DispatchEvent(new VehicleUnlockedEvent("Goldenceptor")); /*0x6ff530 -> "Vehicle Unlocked" -> 0x6fb170*/
    ShowVehicleUnlocked(); }                          // 0x6fb2b0
}
```
* Silent setter 0x6f5460(eax=id): sets the flag with no popup (no callers found).
* Profile save (PlayerData::SaveData ~0x6f76e0): `<Achievements><Achievement name="<name[id]>" .../>` for every set flag
  (stored by NAME, not id). LifetimeData block written by name too (see 0.3).

### Popup / sound (AchievementDisplay blueprint + AchievementDisplayComponent type 0x6b, vt 0x796a0c)
* Objects/AchievementDisplay.xml: Text component, font `hemihead.fnt`, colour (0.427, 1.0, 0.266, 1), scale 200x200,
  text = the achievement NAME (no prefix); RenderMesh quad.msh material `zz_achievement_display`, Diffuse1 swapped to
  `achievement_unlocked.png` (also used for `vehicle_unlocked.png` / `new_level_unlocked.png` popups, 0x6fa7e0);
  SingleName `achievement_display`.
* Messages are queued (vector at +0x60); one shown at a time for **3.3 s** (+0x3c, 0x6fa6a0). On show (0x6fa700):
  mesh visible, text mode 2, timer 0, posts Wwise event **Play_ViperGTS_Rev** (0xe0631744) on the global sound object.

### 0.3 Persistent stats - PlayerData [0x7ff6dc] (XML `<LifetimeData>` in userdata, names exactly as below)
| off | type | XML name | updated by |
|---|---|---|---|
| +0x82..+0xb2 | bool[49] | (Achievements) | Unlock |
| +0xc0 | int | Overheat Deaths | PlayerDeathMessage cause 4 (0x6f7dd0) |
| +0xc4 | int | Suicides | PlayerDeathMessage cause 1 |
| +0xc8 | int | High-Speed Deaths | PlayerDeathMessage cause 3 |
| +0xcc | int | Points | TrickCompleteMessage (0x6f58e0): += trick points |
| +0xd0 | int | Biggest Trick | max(trick points) |
| +0xd4 | int | Tricks Landed | ++ per trick |
| +0xd8 | int | Barrel Rolls | ++ if trick sound == Play_BarrelRoll |
| +0xdc | float | Wall Ride Distance (miles) | TrickSystem2 trick end (0x6dc37c) += ts+0x144 |
| +0xe0 | float | Ceiling Ride Distance (miles) | += ts+0x158 |
| +0xe4 | int | Best Score On One Level | LevelFinishedMessage(success) (0x6f5890): max(run.points) |
| +0xe8 | float | Time Played (MINUTES) | level update, state 2 only: += dt/60 |
| +0xec | int | Streetlights Broken | StreetlightBreaker (0x6bd290) / 0x6fbb90 |
| +0xf0 | int | Screens Broken | 0x6fbb30 |
| +0xf4 | int | Most Screens Broken On One Level | max(run.screens) |

### 0.4 Per-run stats - PlayerFeedbackTracking [0x7fe7d0] ("PlayerFeedback.cpp")
| off | meaning | written by |
|---|---|---|
| +0x00 | overheat deaths (Car Death cause 4) | "Car Death" event 0x6fae50 |
| +0x04 | self-terminations (cause 1) | " |
| +0x08 | high-speed crash deaths (cause 3) | " |
| +0x0c | other deaths (cause 2 kill material, 5 fell out, ...) | " |
| +0x10 | points (sum of landed trick points) | "Trick Complete" 0x6fb7f0 |
| +0x14 | tricks landed | " |
| +0x18 | biggest trick | " |
| +0x1c | screens broken | 0x6fbb30 |
All 8 are zeroed by the **"Race Start"** game event (0x6fac80; emitted by BaseLevel slot 13 0x715080 = start / restart
of a level attempt). "Race Finish" (0x6fac70) zeroes only +0x1c. Checkpoint respawns do NOT reset them.

### 0.5 Event / message order for a landed trick (TrickSystem2 0x6dbcf0)
A trick "lands" when TrickSystem2 (type 0x6f, update 0x6dc3e0) sees >= 3 wheels on the ground (+0x60 > 2) with base
points (+0x44) > 0 (and no wall/ceiling ride pending unless +0x78). Points = round(base(+0x44) * multiplier(+0x48));
multiplier = number of distinct trick components (each non-zero counter adds 1.0, 0x6da470).
1. game event "Trick Complete" (TrickCompleteEvent: +0x38 Wwise sound id, +0x40 bool simple, +0x44 points) is dispatched
   synchronously first -> 0x6fb7f0 (achievements 20, 23, 14, 2, 1, 0);
2. then TrickCompleteMessage (msg 0x19, +0x2c sound, +0x34 points) -> PlayerData 0x6f58e0 updates lifetime stats.
So inside 0x6fb7f0 the lifetime stats do NOT yet include the current trick.

Death causes ("Car Death" event + PlayerDeathMessage msg 0x15, +8 = cause): 1 hard reset (CarResetHard, 0x708f44),
2 kill material (0x7093a6), 3 crash > death speed (0x709c25), 4 overheat (0x708c10), 5 fell below z = -70 (0x70aa3f).

## 1. Summary table (id = index in 0x7f56c8)
| id | name | where | exact condition |
|---|---|---|---|
| 0 | Million Point Mark | 0x6fbafa (Trick Complete) | lifetime Points (+0xcc, before adding this trick) >= 1,000,000 |
| 1 | 100 Grand | 0x6fbabd | run.points (+0x10, incl. this trick) >= 100,000 AND level score_style != 0 AND (score_style & 1) == 0 (= Stunt Mode, style 6) |
| 2 | Over 9000 | 0x6fbaa7 | this trick's points > 9000 |
| 3 | Self-Termination | 0x6f7e4d (PlayerDeathMessage) | any cause-1 death (hard reset while driving): Suicides++ then unlock |
| 4 | Wreckless | 0x6f7e2a | cause-3 death: High-Speed Deaths++; unlock when >= 10 |
| 5 | No Hands | - | never unlocked (no code, hidden) |
| 6 | Spinsanity | - | never ("Scratch Event" 0x6fb240 only prints "Spinsanity Test!!") |
| 7 | Rush Hour | 0x713fb4 (level update) | Time Played (+0xe8, minutes, counted only in level state 2 = racing) > 60.0 |
| 8 | Wings Deployed | 0x706b52 (CarLogic wings) | frame where wings flag (+0x58) goes false->true (static prev 0x801ac4) |
| 9 | Wall Ride | 0x6dc5bb (TrickSystem2 update) | wall-ride time of current trick (+0x140) > 3.0 s |
| 10 | Gravity? No Care. | 0x6dc5d9 | ceiling-ride time of current trick (+0x154) > 1.5 s |
| 11 | 88 MPH | - | never |
| 12 | High Speed | - | never |
| 13 | First! | 0x6e6b28 / 0x6edcdc | results screen rank == 0 (local table incl. shipped default scores; or online rank 0 after publishing) |
| 14 | Tricks Make You Cool | 0x6fba88 | lifetime Tricks Landed (+0xd4, before this trick) + 1 >= 100 |
| 15 | Toasty | 0x6f7e07 | cause-4 (overheat) death: Overheat Deaths++; unlock when >= 3 |
| 16 | 1337 | - | never |
| 17 | Make It Fast | - | never |
| 18 | Track Maniac | - | never |
| 19 | Bleep Bloop | inside Unlock | >= 10 achievement flags set (ids 0..48) |
| 20 | Do A Barrel Roll! | 0x6fba55 | landed trick whose announcer sound == Play_BarrelRoll (0xc4129ecb) |
| 21 | Terrible, Terrible Damage | - | never |
| 22 | Conquistador | - | never |
| 23 | Hat Trick | 0x6fba6f | run.tricks (+0x14, after ++) == 3 |
| 24 | I Know Car Fu | 0x6c2e36 (HumveeLogic contacts) | GunHumvee hit by a dynamic body that has CarLogic with closing speed d > 68 |
| 25 | Still Alive | 0x714e8b (BaseLevel::Finish) | finish from racing state, level score_style != 0, run deaths (+0..+0xc sum) == 0, not StuntLevel |
| 26 | Aileron Roll | - | never |
| 27 | Victory Roll | - | never |
| 28 | Better Handling | 0x73371d (main menu enter) | XInput pad #0 connected ([0x7ff760]+0x644) when MainMenuLevel starts |
| 29 | 360 Gamepad, Please | same | NO pad connected when main menu starts (not in Vita list) |
| 30 | Made From Scratch | 0x710900 / 0x734530 | entering CreditsLevel2 (post-story) or CreditsLevel (menu credits) |
| 31 | Initiate | 0x70dbe0 | TutorialLevel finish (Tutorial.xml or AdvancedTutorial.xml, sd_level 2) |
| 32 | Hardcore I | - | never |
| 33 | Hardcore II | - | never |
| 34 | Photo Finish | - | never |
| 35 | Impermanence | 0x6f7e80 | menu Reset Achievements -> "Reset ALL Achievements?" yes: clear flags[0..48], then Unlock(35) |
| 36 | Ninja | - | never (Play_Ninja is only an announcer line, see 20) |
| 37 | Wheelie of Fortune | 0x6dc59d | wheelie time of current trick (+0xc0) > 5.0 s |
| 38 | Wheelie Master | - | never |
| 39 | Boost Machine | - | never |
| 40 | Top of the CORE | TriggerAchievement 0x726fc0 | enter TriggerCylinderZone "TopOfTheCORE" (Campaign_Level5 and Hardcore_TealLevel) |
| 41 | Down, Not Out | 0x70ac37 (CarLogic update) | car z < -60 but never < -70, then back above z > -40 while alive |
| 42 | Rainbow Dash | - | never |
| 43 | Rampage | 0x6bd608 / 0x6fbb90 / 0x6fbb6f | lifetime Streetlights Broken + Screens Broken >= 20 |
| 44 | Smash TV | 0x6fbb41 | run.screens (+0x1c) >= 5 |
| 45 | Homage | Gladiator.xml trigger chain | LevelFinishedMessage in Gladiator (any vehicle) |
| 46 | Great Scott! | train_wreck.xml chain | finish "Choochoo Chase" with vehicle "Commander" |
| 47 | Outta Time | fantastic.xml chain | finish "Clockwork" with vehicle "Commander" |
| 48 | LAN Party | 0x6d874b (net) | LAN auto-discovery found another player (not in Vita list) |

"Never" = no code path and no level/Lua data references the id (checked: all `call/jmp 0x6fb620` sites, all
`Achievement ID` values in Levels/*.xml are only 40, 45, 46, 47; LuaScripts contain no achievement calls). These 18
have NULL descriptions, so the PC menu does not list them either. To match the PC exactly the port should hide them
and never unlock them.

Goldenceptor: needs count(flags 0..48) == 31, i.e. ALL visible ones, including 29 "360 Gamepad, Please" and
48 "LAN Party" (both need hardware the Vita lacks) - the port must decide (e.g. treat the visible set minus 29/48).

## 2. Pseudo-C per achievement

### Trick Complete handler 0x6fb7f0 (ids 20, 23, 14, 2, 1, 0) - registered on game event "Trick Complete"
```c
bool OnTrickComplete(TrickCompleteEvent* e) {           // e->sound +0x38, e->simple +0x40, e->points +0x44
  Run* r = *(Run**)0x7fe7d0; PlayerData* pd = *(PlayerData**)0x7ff6dc;
  r->points += e->points; r->tricks++; r->biggest = max(r->biggest, e->points);
  ShowTrickPopup(e);                                    // "%s\n%s Points" or random awesome phrase
  if (e->sound == 0xc4129ecb /*Play_BarrelRoll*/) Unlock(20);         // Do A Barrel Roll!
  if (r->tricks == 3)                     Unlock(23);                // Hat Trick (exactly 3rd trick of the run)
  if (pd->tricksLanded /*+0xd4*/ + 1 >= 100) Unlock(14);             // Tricks Make You Cool (100th trick)
  if (e->points > 9000)                   Unlock(2);                 // Over 9000
  LevelDesc* L = (*(LevelSetMgr**)0x7ff6d8)->current /*+0x20*/;
  if (r->points >= 100000 && !pd->achieved[1] && L && L->score_style /*+0x7c*/ && !(L->score_style & 1))
                                          Unlock(1);                 // 100 Grand: stunt mode (style 6) only
  if (pd->points /*+0xcc*/ >= 1000000)    Unlock(0);                 // Million Point Mark (lifetime, previous tricks)
  return true;
}
```
Barrel-roll trick definition (sound pick in 0x6dbcf0): multiplier (+0x48) in (0, 1] (exactly one trick component) AND
(rollLeftCount +0x90 == 1 OR rollRightCount +0x94 == 1) -> Play_BarrelRoll. I.e. exactly one 360-degree barrel roll and
nothing else in that trick (2+ rolls, or roll + flip/twist/wheelie, etc. do not count). Points of 1 roll = 150.
Other single-component sounds: +0x98 Backflip, +0x9c FrontFlip, +0xa0/+0xa4 Frisbee, +0xa8+0xac==1 FrontTwist,
+0xb0+0xb4==1 BackTwist, ==2 TwistAndShout.
Lifetime "Points" grows only from tricks (any mode, any level; also tutorials), so Million Point Mark unlocks at the first
trick landed after the lifetime total reached 1,000,000 (one-trick lag). Port: check `pd->points >= 1e6` before adding.

### PlayerDeathMessage handler 0x6f7dd0 (ids 3, 4, 15) - PlayerData, msg 0x15
```c
void OnPlayerDeath(PlayerDeathMessage* m) {
  switch (m->cause /*+8*/) {
  case 1: pd->suicides++;                               Unlock(3);  break;  // Self-Termination
  case 3: if (++pd->highSpeedDeaths >= 10)              Unlock(4);  break;  // Wreckless
  case 4: if (++pd->overheatDeaths  >= 3)               Unlock(15); break;  // Toasty
  }
}
```
* cause 1 = "CarResetHard" action (Back / T on PC; port: Select) accepted only when car state +0x1f0 == 1 (driving)
  and CarLogic+0x60 > 0.5; Die() then cause 1.
* cause 3 = crash death in the car collision handler (closing speed d > 60 on static/kinematic geometry, > 20 with wings
  out, > Custom Death Speed, barrels > 50; and dot(n, carUp) < 0.7) - see re_A sec. 1.
* cause 4 = heat >= Heat Max (50) (0x708c10). Goldenceptor has Infinite Heat, so it can't earn Toasty.
* Counters are lifetime (+0xc0/+0xc4/+0xc8) and never reset (Impermanence does not clear them).

### Rush Hour (7) - level update 0x713f94 (BaseLevel; also used by StuntLevel)
```c
case LEVEL_STATE_RACING /*+0x3c == 2*/:
  level->raceTime += dt;
  pd->timePlayedMin /*+0xe8*/ += dt * (1/60.f);
  if (pd->timePlayedMin > 60.0f) Unlock(7);
```
Only racing time counts (not menus, countdown, pause or results).

### Wings Deployed (8) - CarLogic wings update ~0x706ae4
```c
static bool prevWings /*0x801ac4*/;
if (cl->wingsOut /*+0x58*/ && cl->wingsOut != prevWings) { ...open wing meshes...; Unlock(8); }
... prevWings = cl->wingsOut;
```

### TrickSystem2 timers (9, 10, 37) - update 0x6dc3e0, per frame after the timers are advanced
```c
if (ts->wheelieTime /*+0xc0*/ > 5.0f) Unlock(37);   // Wheelie of Fortune
if (ts->wallTime    /*+0x140*/ > 3.0f) Unlock(9);   // Wall Ride
if (ts->ceilTime    /*+0x154*/ > 1.5f) Unlock(10);  // Gravity? No Care.
```
All three accumulate within the current trick and are zeroed by TrickSystem2::Reset 0x6db060 (trick landed / 5 s
idle / death / respawn). Defaults (0x6db1d0; Car.xml uses defaults):
* Wheelie (0x6dadb0): requires speed >= 16 HUD MPH (+0x118); front wheels 0,1 off the ground and rear wheel 2 or 3
  touching (0x6d9320); dot(carUp (orientation column 2), contact normal of wheel 2 (or 3)) <= 0.95 (+0xec, 0x6da990). Gaps shorter than 0.25 s (+0x11c)
  are bridged and added. Timer counts from the first frame (no minimum; 0.15 s min is only for scoring).
* Ceiling ride (0x6d9370): >= 2 wheels touching (+0x60 > 1), car up-vector world z < -0.6 (+0x124 "Ceil. Ride Limiter");
  starts only if speed > 20 HUD MPH (+0x184). Pending time/distance (+0x14c/+0x150) are committed when distance >
  0.0340909 mi (180 ft, +0x134); then ceilTime = pending time + time since; distance(miles) += mph*dt/3600.
  A break shorter than 0.4 s (+0x18c "overlook") resumes the ride.
* Wall ride: same but up.z < 0.18 (+0x120) and not in ceiling mode, commit distance 0.01 mi (52.8 ft, +0x12c),
  timer +0x140. (Min speed to break +0x188 = -1: never breaks on speed.)

### First! (13) - results screen 0x6e6290(arg1, style, rank, flag)
```c
// rank = LocalScores::Rank(level->score_style)  (0x6f3b60 -> 0x6f3750)
if (rank == 0) { Post("Play_FirstPlace"); Unlock(13); }   // 1 -> Play_SecondPlace, 2 -> Play_ThirdPlace
```
Called on LevelFinishedMessage for normal levels (0x7137b0..0x713822) and from StuntLevel results (0x723f0d).
Rank = insertion index of the current run in the LOCAL score table of that level for its score_style (bit mask:
1 shortest time, 2 highest score, 4 biggest trick; priority time > score > trick; ties go AFTER existing entries,
strict comparisons). The local table is seeded from the shipped `data/default_scores.xml` / `data/default_scores/Default/
<level>.xml/scores.NrScoreSet` (2 dev entries each for Campaign_Level1-5, Evil_Carwash, Hardcore_Blue/Red/Teal); levels
without defaults are empty, so their first finish is rank 0. Second path 0x6edcdc: online leaderboard rank 0 after
"Publish Score" (ecx=5) - not applicable on Vita.
Port: rank 0 = beat (time ascending for style 7; score then trick for style 6) every entry in the local table
including the default entries, else First!.

### Bleep Bloop (19) - inside Unlock (see 0): unlocked when >= 10 flags are set after setting a new one.

### Rampage (43) / Smash TV (44)
```c
void StreetlightBroken() { pd->streetlights++; if (pd->streetlights + pd->screens >= 20) Unlock(43); }   // 0x6bd608
void ScreenBroken() {                                                      // 0x6fbb30
  r->screens++; pd->screens++;
  if (r->screens >= 5) Unlock(44);                                         // per level attempt (reset on Race Start/Finish)
  pd->mostScreens = max(pd->mostScreens, r->screens);
  if (pd->streetlights + pd->screens >= 20) Unlock(43);                    // lifetime
}
```
Break rules: streetlight = Material ID 3 contact with d > 15; screen = Material ID 4 contact with d > 15 (re_A sec. 2-3).

### I Know Car Fu (24) - HumveeLogic (type 0x3c, GunHumvee) contact pass 0x6c2d70
```c
for (Contact* c : humvee.collector->contacts) {
  if (Material(other) && Material(other)->id == 1) { ...; continue; }
  float d = dot(c->n, c->relVel);                       // closing speed, world units/s (x2.33386 = HUD MPH)
  if (d <= 30) continue;                                // (lower branch: no effect)
  BodyRigid* ob = other collider body (+0x54);
  if (ob && !ob->kinematic /*+0x134*/) {                // hit by a dynamic body
     if (d > 68) { copy velocities; if (GC(otherGO, 0x20 /*CarLogic*/)) Unlock(24); HumveeDestroy(); /*0x6c2480*/ }
  } else HumveeDestroy();                               // humvee crashed into static/kinematic at d > 30: no achievement
}
```
"CORE drone" = GunHumvee (Objects/GunHumvee.xml, HumveeLogic), spawned by `garage` objects (GarageLogic, Spawn Distance
340) in Arena, sky_city (Floating Free Zone), christmas, vert_park (stunt) and nature (Observatory). Taking one out "with
force" = ramming it with the player car at a closing speed > 68 (~159 HUD MPH). Humvees that crash on their own give nothing.

### Still Alive (25) - BaseLevel::Finish(bool success) 0x714d60 (vtable slot 14; StuntLevel overrides it -> excluded)
```c
if (level->state != 2) return;                          // must be racing
if (success) { rank/score bookkeeping; }
level->state = finishState;
LevelDesc* L = levelSetMgr->current;
if (!L || L->score_style == 0) level->state = 5;        // tutorials (style 0): no achievement
else if (r->overheat + r->selfTerm + r->highSpeed + r->other == 0) Unlock(25);
level->OnFinished(success) /*vslot 26: TutorialLevel -> Initiate*/; EmitEvent("Race Finish");
```
"Wrecked" = any death since Race Start (all causes 1-5, including hard reset). Checkpoint respawns don't reset the count.

### Better Handling (28) / 360 Gamepad, Please (29) - MainMenuLevel enter 0x733430
```c
Unlock(input->xinputConnected /*[0x7ff760]+0x644*/ ? 28 : 29);   // every time the main menu loads
```
Vita: always 28 at the main menu (29 has no Vita meaning; not in the port list).

### Made From Scratch (30)
`CreditsLevel2::PostBaseLevelEnter` 0x710900 (credits after the story ending, re_E) and `CreditsLevel` slot 4 0x734530
(credits_level from the menu): unconditional Unlock(30) on entering the credits.

### Initiate (31) - TutorialLevel vslot 26 0x70dbe0 (called from BaseLevel::Finish)
```c
level->state = 5; Post("Play_TutorialComplete" /*0x78bf841d*/); Unlock(31);
```
Tutorial levels = sd_level 2: "Tutorial: Controls and Basics" (Tutorial.xml), "Hardcore Tutorial: Advanced Topics"
(AdvancedTutorial.xml). The success flag is not checked.

### Impermanence (35) - 0x6f7e80, menu "Reset Achievements" -> "Reset ALL Achievements?" -> yes (0x6ec0e7)
```c
memset(pd->achieved, 0, 49); Unlock(35);   // lifetime stats are NOT cleared
```
Consequence (PC behaviour): stat-based ones re-unlock at their next check (Rampage at next break, Tricks Make You Cool /
Million Point Mark at the next trick, Rush Hour next racing frame, Wreckless/Toasty at the next such death, ...).

### Top of the CORE (40), Homage (45), Great Scott! (46), Outta Time (47) - level data + TriggerAchievement
TriggerAchievement@N (type 0x67) fire 0x726fc0: `Unlock(AchievementID /*+0x88*/)` on its Incoming message.
* 40: TriggerCylinderZone Outgoing "TopOfTheCORE", OnEntryOnly:
  Campaign_Level5 at (55.92, 5403.4, 1363.26) R 219 H 180; Hardcore_TealLevel ("Hardcore Death Machine") at
  (-3774.18, 340.72, 1081.96) R 118 H 109.
* 45 Gladiator.xml: TriggerAtFinish (Outgoing "race_finished") -> TriggerAchievement 45.
* 46 train_wreck.xml ("Choochoo Chase", Challenge set): TriggerAtFinish "anthracite" -> TriggerBranchOnVehicle
  (Vehicle Needed "Commander") "dust" -> TriggerAchievement 46.
* 47 fantastic.xml ("Clockwork", Challenge set): TriggerAtFinish "level finish" -> TriggerBranchOnVehicle "Commander"
  "outta time" -> TriggerAchievement 47.
TriggerAtFinish (type 0x97, vt 0x7a2ec4) subscribes to LevelFinishedMessage (msg 0x13) and fires on ANY of them (success
flag ignored; success=0 only from DevKey RaceFinish / stunt timeout). TriggerBranchOnVehicle (0xa4, 0x71d440): fires
iff `VehicleData[current].name == "Commander"` (the Delorean object). Gladiator: any vehicle.

### Down, Not Out (41) - CarLogic::VUpdate 0x70ac1f
```c
if (cl->state /*+0x1f0*/ != 0xb && cl->state != 7) {     // not dead / respawning
  float z = carTransform.worldPos.z;                    // Z-up world
  if (z > -40.f) { if (cl->nearlyFell /*+0x28c*/) { cl->nearlyFell = 0; Unlock(41); } }
  else if (z < -60.f) { cl->nearlyFell = 1;
                        if (z < -70.f) { Die(cl); /*clears +0x28c*/ SendDeath(cause 5); } }
}
```
So: drop below z = -60 (fall-out kill height is -70, absolute world z, same in every level) and climb back above
z = -40 without dying. Die() (0x707db0) and level reset clear the flag.

### Smash TV / Rampage / Top of the CORE etc. above; LAN Party (48): unlocked by the LAN discovery code (0x6d8690) when
another game is found - out of scope for Vita.

## 3. Port checklist
* Implement `Unlock(id)` exactly as sec. 0 (flag, popup with name text 3.3 s queued, Play_ViperGTS_Rev, Bleep Bloop >= 10,
  Goldenceptor when all visible are set) and persist flags by name plus the 14 LifetimeData fields.
* Keep run stats reset on level (re)start; Smash TV/Hat Trick/100 Grand/Still Alive are per attempt.
* Hide and never award ids 5, 6, 11, 12, 16, 17, 18, 21, 22, 26, 27, 32, 33, 34, 36, 38, 39, 42 (PC has no code for them
  and does not list them).
