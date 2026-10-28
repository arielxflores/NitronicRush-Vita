# RE group F2: triggers, particles, misc components (Nitronic Rush PC exe)

Helpers written for this group (in `$HOME/nrw/F2/`): `lvcomps.py <tag-regex> [levelglob]` (dump component fields of level
instances), `trig_all.txt` (every Trigger* instance in every level), `chain.py <level>` (zone -> receiver chains),
`tuttab.py <level>` (tutorial text tables), `d3dsm.py file.fxo` (**D3D9 SM3 shader disassembler for the .fxo effects**),
`pe_upd.txt` (ParticleEmitter spawn function listing).

## 0. Shared facts found here

| thing | meaning |
|---|---|
| `[0x7ff700]` | **TriggerManager**. +0x04 vector<TriggerBase*> "receivers" (list A), +0x1c vector "self-updating triggers" (list B: Sphere/Cylinder/AABox/OrientedBox zones, TriggerDelay). Slot 11 of a trigger registers it, slot 12 unregisters. |
| `0x7238a0(std::string* ev)` | **Broadcast(ev)**: if ev empty return; for every trigger in list A, then list B: `if (trigger->Incoming == ev) trigger->Fire()` (vtable slot 16). Exact, case-sensitive std::string compare. Synchronous and recursive (a Fire may broadcast again). The list sizes are sampled before the loops. |
| TriggerBase@N (type 0x5f) | +0x34 `Incoming` (std::string, 0x1c bytes), +0x50 `Outgoing`, +0x6c debug label (not serialized; if non-empty Fire prints it in magenta). Size 0x88. **TriggerBase::Fire (0x68d520) = Broadcast(Outgoing)**. Blueprint `TriggerChainer`. |
| Trigger blueprints | all 14 `Objects/Trigger*.xml` carry a `block.msh` RenderMesh with `Play="0" Pause="0" DevPause="1"` -> **editor only, never drawn in game**. |
| `0x67b340("Car")` | every zone tests only the object named **"Car"** (AABox/OrientedBox: field `Object Name`, default and all instances = "Car"). Position used = car GameObject world position (`0x677d50` on GO+0x6c), i.e. the car origin, no extents. |
| CarLogic (0x20) fields used here | +0x34 max heat (50 on PC), +0x5c heat, +0x190 Boost Force, +0x568 Heat Multiplier (boost heat rate/s = BoostForce/30*HeatMult), +0x1a4 respawn position, +0x1b0 respawn 3x3 orientation, +0x254..+0x260 "cool glow" colour vec4, +0x268 glow param (0.75), +0x26c glow timer, +0x270 "Play_CoolDown already posted" flag. `0x702380(eax=CarLogic)` = **respawn the car at +0x1a4/+0x1b0** (ray-cast 25 units down the respawn up-axis, put the car on the hit point + suspension height, zero velocities). |
| `0x6dc6e0(announcer=[0x7ff6ec], eventId, float offset, fmt, ...)` | queue a line in the **announcer queue** (+0x70). Entries are played one after another with `PostEvent(eventId)`. |
| Sound table `0x792f38` | `Sound` integer fields (TriggerTutorialText, TriggerAnnouncer) index this table of Wwise event ids (sorted event names). Useful: 0 Play_Ain_tNothin_ButChickenWing, 5 Play_Bangin_, 221 Play_TutorialDisplayDown, 222 Play_TutorialDisplayUp, 223 Play_HoldRTForGas, 224 Play_HoldLTForBrake, 225 Play_ForwardArrowKeyForGas, 226 Play_BackArrowKeyForBrake, 227 Play_UseLSForTurning, 228 Play_ArrowKeysForSteering, 229 Play_UsLSAndRSForTightTurning, 230 Play_HoldEForRollMode, 231 Play_UseLBAndAToBoost, 235 Play_PressXToHop, 237 Play_PressBToFly. |
| `rand()` (CRT, import `0x75a30c`) | particles use `rnd(x) = ((rand() % 2048) - 1024) / 1024 * x`  -> uniform in [-x, x). |
| Text component (0x33) | fields +0x34 Text, +0x50 Font, +0x80 Color (vec4, +0x8c = alpha), +0x90 Scale (vec2), +0x98 Position, +0x74 Letter Gap, +0x7c Word Gap, +0x78 Line Gap, +0x6c Wrap Flag, +0x70 Wrap Value, +0xa4 Draw Mode. `'|'` or `'\n'` = new line. **Button glyphs**: `%A`->0x7f, `%B`->0x80, `%X`->0x81, `%Y`->0x82, `%LT`->0x83, `%RT`->0x84, `%LB`->0x85, `%RB`->0x86, `%LS`->0x87, `%RS`->0x88, `%s`(start)->0x89, `%b`(back)->0x8a (font glyph codes). |
| graphics `[0x7ff6c4]` | +0x557a2c / +0x557a30 = screen width / height (pixels, float). |

---------------------------------------------------------------------------------------------------------------
## 1. Trigger zones

All four: slot 15 `VUpdate(dt)`; Fire = TriggerBase::Fire (broadcast Outgoing). Radius/extents are **NOT multiplied by
the object scale** (Tutorial zones have scale 7 and use Radius 20 as is). **Every zone instance in every level has
OnEntryOnly = 1.** If `dt == 0` and the object is selected in the editor, gizmos are drawn (ignore).

### 1.1 TriggerSphereZone@N (type 0x61, vt 0x7a2644, ctor 0x40f380, size 0x9c; update 0x723680, serialize 0x7232f0)
| field | off | default |
|---|---|---|
| Incoming / Outgoing | 0x34 / 0x50 | "" |
| Radius | 0x88 | 10 |
| Recheck Delay | 0x8c | 0.1 |
| OnEntryOnly (bool) | 0x90 | (set by file) |
| timer (runtime) | 0x94 | 0 |
| wasInside (runtime) | 0x98 | 0 |

### 1.2 TriggerCylinderZone@N (type 0x62, vt 0x7a26a4, ctor 0x40f450, size 0xa0; update 0x722ce0)
Radius 0x88 (def 10), **Height 0x8c (def 30)**, Recheck Delay 0x90 (0.1), OnEntryOnly 0x94, timer 0x98, wasInside 0x9c.

### 1.3 TriggerZoneAABox@N (type 0x86, vt 0x7a2e1c, ctor 0x6a3dc0; update 0x6a3e90)
Object Name 0x88 (std::string, def "Car"), Half Extents 0xa4 (vec3, def (20,20,20)), Recheck Delay 0xb0 (0.1),
OnEntryOnly 0xb4, timer 0xb8, wasInside 0xbc.

### 1.4 TriggerZoneOrientedBox@N (type 0x87, vt 0x7a2e7c, ctor 0x71d8e0; update 0x71e240)
Object Name 0x88 ("Car"), Half Extents 0xa4 ((20,20,20)), +0xb0 copy of inverse orientation (taken in init, unused by the
test), Recheck Delay 0xd4 (0.1), OnEntryOnly 0xd8, timer 0xdc, wasInside 0xe0.

### Pseudo-C (identical skeleton; only `inside()` differs)
```c
void Zone::VUpdate(float dt) {
    timer += dt;
    if (!(timer > RecheckDelay)) return;          // strict >
    timer = 0;
    GO* car = FindByName(ObjectName /* "Car" */); if (!car) return;
    vec3 d = car->worldPos - this->owner->worldPos;
    if (inside(d)) {
        if (OnEntryOnly && wasInside) return;      // already fired for this visit
        wasInside = 1;
        Fire();                                    // vtable slot 16 -> Broadcast(Outgoing)
    } else wasInside = 0;
}
// Sphere:      inside = R*R > dot(d,d)
// Cylinder:    inside = Height > fabs(d.z) && R*R > d.x*d.x + d.y*d.y
//              -> WORLD-Z axis, centred on the object origin: z in (oz-Height, oz+Height); Height is a HALF height;
//                 object rotation ignored.
// AABox:       inside = all(c - H <= p) && all(p <= c + H)   (inclusive, world axes, rotation ignored)
// OrientedBox: l = inverse(R_owner) * d  (= (dot(axisX,d), dot(axisY,d), dot(axisZ,d)), live orientation each check)
//              inside = all(-H <= l) && all(l <= H)          (inclusive)
```
With OnEntryOnly = 0 the zone would fire every recheck while inside (never used).
A zone also sits in list B, so a broadcast equal to its *Incoming* calls Fire directly (only `trigger_test`).

### 1.5 Where zones are used (level sets: Campaign = Tutorial, Campaign_Level1..6; Hardcore = AdvancedTutorial, Hardcore_*, Gladiator, nature, NitroTunnel; Stunt = Arena, vert_park, christmas, neon_nights, sky_city)
* Tutorial (36 texts) / AdvancedTutorial (17) / tutorial_hover: SphereZone(R 20) -> TriggerControlScheme or TriggerTutorialText (tables in section 2.5).
* Campaign_Level5: Cylinder R219 H180 -> `TopOfTheCORE` -> TriggerAchievement 40; Sphere R52 `CoreEye_Unused` (no receiver).
* Campaign_Level6: Sphere R23 -> `EndStory` -> TriggerEndStory (re_E.md).
* Hardcore_TealLevel: Cylinder R118 H109 -> TopOfTheCORE -> achievement 40.
* Stunt levels: Cylinder/Sphere/AABox zones -> TriggerRespawnLocation (99 instances: Arena 15, christmas 8, neon_nights 54,
  sky_city 14, vert_park 8). e.g. sky_city: Cylinder R350 H700 per area.
* neon_nights: OrientedBox `sawzilla` and AABox `childofsawzilla` -> TutorialText "Sawzilla!!" (3 s) / "Child of Sawzilla!" (3.5 s).
* Gladiator / fantastic / train_wreck: TriggerAtFinish chains (2.3).

**To reproduce:** one global list of (Incoming, receiver) and `Broadcast(name)`; zones test only the car origin every
`RecheckDelay` (+1 frame); radius/extents unscaled; cylinder = world-Z, half-height; don't draw trigger meshes.

---------------------------------------------------------------------------------------------------------------
## 2. Receivers

### 2.1 TriggerRespawnLocation@N (type 0x66, vt 0x7a28dc, ctor 0x40f6c0; Fire 0x7216e0)
Fields: Absolute Positioning 0x88 (bool, def 0), Respawn Location 0x8c (vec3, def 0,0,0; never present in level files).
```c
void Fire() {                                    // does NOT broadcast Outgoing
    GO* car = FindByName("Car"); if (!car) return;
    CarLogic* cl = car->GetComponent(0x20);
    vec3 p = AbsolutePositioning ? RespawnLocation : owner->worldPos + RespawnLocation;
    if (cl) { cl->respawnPos(+0x1a4) = p;  cl->respawnRot(+0x1b0) = owner->orientation (3x3 copy); }
}
```
It only stores the point. The car is moved there the next time CarLogic respawns it (`0x702380`, called from the
death/reset paths `0x706277`, `0x708588`, `0x70283f`, `0x6c6e31`): ray-cast from respawnPos along `-25 * respawnRot.col2` (down),
on hit put the car at hit point + up * suspension rest height, else at respawnPos; orientation = respawnRot; all velocities 0.
At level start (`0x70bab0`) the respawn point = the car's start transform. (Campaign checkpoints use a separate
component `CheckpointLogic@N` (type 0x48, vt 0x7962fc), not covered here; when passed it spawns 5 `checkpoint_ghost`
objects (EnumLogic type 1, section 3) at pos + localY*i with Speed = 40*i, Time = (5-i)*0.3.)

**To reproduce:** on fire, respawnPos = trigger object position, respawnRot = trigger object rotation (all instances are relative with zero offset).

### 2.2 TriggerControlScheme@N (type 0x65, vt 0x7a2864, Fire 0x721dd0)
Fields: 0x88 `Outgoing (Basic Controls)`, 0xa4 `Outgoing (Advanced Controls)`, 0xc0 `Outgoing (Keyboard Controls)`.
```c
void Fire() {
    Broadcast(Outgoing);                                     // TriggerBase::Fire, empty in all levels
    if (input[0x7ff760]->gamepadConnected(+0x644)) {
        switch (profile[0x7ff6dc]->controlScheme(+0x54)) {   // 0 = Basic, 1 = Advanced, else nothing
            case 0: Broadcast(OutgoingBasic); break;
            case 1: Broadcast(OutgoingAdvanced); break; }
    } else if (!OutgoingKeyboard.empty()) Broadcast(OutgoingKeyboard);
}
```
**Vita:** gamepad always present -> pick Basic or Advanced from the port's control-scheme setting.

### 2.3 TriggerAtFinish@N (type 0x97, vt 0x7a2ec4) / TriggerBranchOnVehicle@N (type 0xa4, vt 0x7a2f4c) / TriggerAchievement
* AtFinish: no extra fields. Slot 11 registers a **message handler for LevelFinishedMessage (id 0x13)** (it is *not* put in the
  trigger lists); handler `0x6a3900` = Fire() = Broadcast(Outgoing). It does **not** check the success flag.
* BranchOnVehicle: field 0x88 `Vehicle Needed` (string). Fire (`0x71d440`): `if (vehicles[0x7ff6e4].list[current].name == VehicleNeeded) Broadcast(Outgoing);`
* Chains in levels: Gladiator `race_finished` -> achievement 45; fantastic `level finish` -> Branch(Commander) -> `outta time` -> achievement 47;
  train_wreck `anthracite` -> Branch(Commander) -> `dust` -> achievement 46. (Achievement unlock: re_E.md section 5.)

### 2.4 TriggerTutorialText@N (type 0x63, vt 0x7a2744, Fire 0x722650) + TutorialInfoDisplay (type 0x37, vt 0x7a2194)
Fields: Hides Tutorial 0x88 (bool), Text 0x8c (string), Sound 0xa8 (int, index into the sound table), Duration 0xac (float).
```c
void TriggerTutorialText::Fire() {
    Broadcast(Outgoing);                                  // empty in all levels
    GO* d = FindByName("Tutorial_Display"); if (!d) return;   // object from blueprint `tutorialinfodisplay`, spawned by CarLogic (0x708a6a)
    TutorialInfoDisplay* t = d->GetComponent(0x37);
    if (HidesTutorial) { t->active(+0x1e0) = 0; t->showReq(+0x34) = 0; return; }   // unused in levels
    // Show (0x6a8f30):
    t->duration(+0x1e4) = Duration; t->elapsed(+0x1e8) = 0; t->active = 1; t->showReq = 1;
    d->Text.text = Text;  Text::Relayout();
    Announcer.Queue(soundTable[Sound], offset 0.0f, no text);   // voice line goes through the announcer queue
}
void TutorialInfoDisplay::VUpdate(float dt) {                    // 0x6a8c80
    if (active) { elapsed += dt; if (elapsed > duration) active = 0; }
    float tgtSize, tgtAlpha;
    if (!showReq && !active) {                                   // hidden
        if (shown(+0x1d8)) { PostEvent(Play_TutorialDisplayDown); shown = 0; }
        tgtSize = 0; tgtAlpha = 0;
    } else {
        float W = screenW, H = screenH;
        owner.pos = (W*0.5, H - H*0.35*0.25 /* =0.9125H */, 0);   // Text draw mode 2 (centred)
        Text.drawMode(+0xa4) = 2;
        tgtSize = H*0.35; tgtAlpha = 1;
        if (!shown) { PostEvent(Play_TutorialDisplayUp); shown = 1; }
    }
    size(+0x1d0)  = size *0.875 + tgtSize *0.125;               // per FRAME (frame-rate dependent)
    alpha(+0x1d4) = alpha*0.875 + tgtAlpha*0.125;
    Text.color.a = (0.35 + 0.65*(0.5 + 0.5*sin(time*6))) * alpha; // pulsing 0.35..1, time = global seconds (0x4152a0)
    Text.scale = (size, size);                                   // blueprint default Scale is 200 (so 0.35*H ~ 252 at 720p)
    showReq = 0;
}
```
Blueprint `tutorialinfodisplay`: Text font `hemihead.fnt`, colour white, Draw Mode 2, + a Phantom (3D emitter). The
blueprint also has old "Case N Keyboard/Xbox String" fields that the trigger path does not use.
Note: `Sound=0` posts `Play_Ain_tNothin_ButChickenWing` (table entry 0); `Sound=221/222` re-post the UI sounds through the announcer.

**To reproduce:** text box centred horizontally at y = 0.9125*H in a y-up screen space (so about 8.75% of the height
below the top edge if the Text space is y-up; check against the PC game), grows from 0 and fades in with a 0.125/frame
lerp (use `1-0.875^(dt*60)` for a frame-rate independent version), alpha pulses at 6 rad/s, stays `Duration` s, then shrinks
and fades out. Substitute `%RT` etc. with Vita button glyphs; `|` = line break.

### 2.5 Tutorial level contents (Campaign "Tutorial"; all zones SphereZone, Recheck 0.1, OnEntryOnly)
| zone pos (x,y,z) | R | event | Basic scheme text [voice, duration] | Advanced scheme (if different) |
|---|---|---|---|---|
| 0,-196,253 | 20.0 | Gas -> ControlScheme | "Hold %RT for gas" [Play_HoldRTForGas, 5s] | same |
| 2,299,252 | 20.0 | Brake -> ControlScheme | "Hold %LT for brake and reverse" [Play_HoldLTForBrake, 8s] | same |
| 0,606,252 | 20.0 | Steer -> ControlScheme | "Use %LS for turning" [Play_UseLSForTurning, 5s] | same |
| 182,2319,251 | 20.0 | Tight -> ControlScheme | "Use %LS and %RS for tight turning <br> It is best used when boosting" [Play_UsLSAndRSForTightTurning, 8s] | "Hold %LB and use %LS for tight turning <br> It is best used when boosting" [Play_TutorialDisplayDown, 10s] |
| 182,879,252 | 20.0 | Boost -> ControlScheme | "Hold %A or %LB to Boost" [Play_UseLBAndAToBoost, 5s] | "Hold %A or %RB to Boost" [Play_TutorialDisplayDown, 5s] |
| -469,2738,280 | 20.0 | Hop1 -> ControlScheme | "Press %RB or %X to Hop" [Play_PressXToHop, 5s] | "Press %X to Hop" [Play_TutorialDisplayDown, 5s] |
| -1594,2741,280 | 20.0 | Fly1 -> ControlScheme | "Press %B to Fly!" [Play_PressBToFly, 5s] | same |
| -2668,2740,264 | 20.0 | Trick1 -> ControlScheme | "Do a Barrel roll by <br> moving %RS to the left or right" [Play_TutorialDisplayDown, 10s] | "Hold %LB and move %LS left or right <br> to do a barrel roll" [Play_TutorialDisplayDown, 10s] |
| -3251,2737,264 | 20.0 | Trick2 -> ControlScheme | "Do a Backflip by <br> moving %RS down" [Play_Ain_tNothin_ButChickenWing, 10s] | "Hold %LB and pull %LS back to do a backflip" [Play_TutorialDisplayDown, 10s] |
| 0,32,252 | 20.0 | Checkpoint -> ControlScheme | "This is a Checkpoint <br> Press %b to reset to the Checkpoint" [Play_TutorialDisplayUp, 10s] | same |
| -3884,2899,264 | 20.0 | NewStuff -> ControlScheme | "Discover other tricks by using %RS <br> and be sure to <br> hold %A or %LB to get more air off jumps!" [Play_Ain_tNothin_ButChickenWing, 20s] | "Discover new tricks by using %LS <br> and be sure to Boost to get more air off jumps!" [Play_TutorialDisplayDown, 15s] |
| 182,1442,252 | 20.0 | BoostDeath | "Boosting for too long will blow you up! <br> Keep an eye on your heat meter" [Play_TutorialDisplayDown, 10s] | same |
| 222,2740,252 | 20.0 | Jump | "Boost into and over Jumps!" [Play_TutorialDisplayDown, 5s] | same |
| -960,2739,280 | 20.0 | Secret | "Watch for hidden roads <br> They can speed up your journey" [Play_TutorialDisplayDown, 10s] | same |
| -59,2739,262 | 20.0 | Adjust -> ControlScheme | "You can adjust your car in the air by using %LS" [Play_TutorialDisplayUp, 5s] | same |
| -3880,3259,264 | 178.0 | TrickReduce | "Tricks reduce your heat meter!" [Play_TutorialDisplayDown, 5s] | same |
| -2054,2741,220 | 20.0 | Fly2 -> ControlScheme | "Press %B to Fly!" [Play_PressBToFly, 5s] | same |
| 0,510,219 | 20.0 | '' | (nothing: empty Outgoing is never broadcast) | same |

AdvancedTutorial (Hardcore set):

| zone pos (x,y,z) | R | event | Basic scheme text [voice, duration] | Advanced scheme (if different) |
|---|---|---|---|---|
| 0,-47,88 | 20.0 | HighJump -> ControlScheme | "Hold %A to boost and then <br> Tap %RB just before leaving a jump to get more air" [Play_TutorialDisplayDown, 5s] | "Hold %A to boost and then <br> Tap %X just before leaving a jump to get more air" [Play_TutorialDisplayDown, 5s] |
| -3,551,98 | 20.0 | WallRide -> ControlScheme | "Tap %RB then move %RS to the right to rotate onto the side road" [Play_TutorialDisplayDown, 10s] | "Tap %X then hold %LB then move %LS <br> to the right to rotate onto the wall" [Play_TutorialDisplayDown, 10s] |
| -3,697,99 | 20.0 | Faster | "The faster you do this the better <br> as the car's stickyness to walls is based on speed" [Play_TutorialDisplayDown, 10s] | same |
| -20,1135,114 | 20.0 | GetOffWall -> ControlScheme | "Tap %RB and move %RS to the left to rotate onto the road" [Play_TutorialDisplayDown, 5s] | "Tap %X and hold %LB then move %LS <br> to the left to rotate onto the road" [Play_TutorialDisplayDown, 5s] |
| -2,1763,97 | 20.0 | Again | "Time for a try on the right side" [Play_TutorialDisplayDown, 5s] | same |
| 18,2096,115 | 20.0 | BarrelRoll | "Do a barrel roll onto the road!" [Play_TutorialDisplayDown, 5s] | same |
| 3,2606,100 | 20.0 | Flip -> ControlScheme | "When Vertical, Tap %RB then move %RS to the left or right <br> to do a 180 onto the other road <br> Be sure to be boosting" [Play_TutorialDisplayDown, 10s] | "When Vertical, Tap %X then<br>hold %LB then move %LS to the left or right <br> to do a 180 onto the other road <br> Be sure to be boosting" [Play_TutorialDisplayDown, 10s] |
| -1,3251,433 | 20.0 | AgainF | "Do it sideways now, <br> GET HIGH UP ON THE WALL TO COMPENSATE FOR GRAVITY" [Play_TutorialDisplayDown, 10s] | same |
| -25,4143,433 | 20.0 | Done | "You are now prepared for the Hardcore Levels <br> Good Luck!" [Play_TutorialDisplayDown, 5s] | same |

tutorial_hover (not in a level set): Sphere R14 `text0` -> "Angle your car up|while boosting to hover." [Play_TutorialDisplayUp, 10 s].

### 2.6 Other receivers
* **TriggerAnnouncer@N** (type 0x64; only `trigger_test`): fields Text 0x8c, Color 0xa4 (vec4), Sound 0xb4, Offset 0xb8.
  Fire (`0x728600`) = Broadcast(Outgoing) + `Announcer.Queue(soundTable[Sound], Offset, Text, Color)`.
* **TriggerDelay@N** (type 0x60, blueprint `TriggerDelayer`; only `trigger_test`): Delay 0x88, flag 0x8c, timer 0x90, running 0x94.
  Fire: `if (!running || flag) { timer = 0; running = 1; }`. Update: `if (running && (timer += dt) > Delay) slot17()` (= broadcast Outgoing, stop).
* **TriggerAchievement / TriggerEndStory**: see re_E.md.

### 2.7 TurnOffBlur (type 0x8c, vt 0x7974b4; blueprint `turn_off_blur`, only in `credits_level`)
No fields. Every frame (`0x68aed0`): `if (activeCamera has ChaseCameraLogic(0x4f)) chase->speedBlur(+0x90) = 0;`
ChaseCameraLogic+0x90 is the 0..1 "speed" factor that drives motion blur (Blur Speed Low/High, fade in/out) and lerps
FoV between `Slow Camera FoV` and `Fast Camera FoV` (also RTPC 0x24bfd816). **Port:** in the credits level force the
chase-camera speed factor to 0 (no speed blur, slow FoV).

### 2.8 FlickerOnHack (type 0x5e, vt 0x7968bc; on blueprint `core_eye` = Evil_Carwash, Hardcore_TealLevel, core_demo)
No serialized fields. Init (`0x6bd8d0`): low 0x34 = 0.1, high 0x38 = 2.0, 0x40 = 6 (unused), active 0x3c = 0, timer 0x44 = 0,
pattern 0x48 = `"10101110101"`, index 0x64 = 0.
```c
void VUpdate(dt) {                                   // 0x6bd9a0
    float v;
    if (active) { v = pattern[idx]=='1' ? high : low;  if (++idx >= len) idx = len-1; }   // one char PER FRAME, ends on '1'
    else        { v = low; idx = 0; }
    if (Light* l = GetComp(0x0b)) l->intensity(+0x48) = v;
    if (RenderMesh* m = GetComp(0x47)) m->glowByte(+0x63) = (int)(v*10);   // overrides Color1.w (1.7) every frame
}
```
`active` is only set by **NarrativeLevel** (the story-intro cinematic, `0x72af2a` toggle / `0x72b004` set, object "coreeye").
**In playable levels the eye just has glow intensity 0.1** (core_eye has no Light). Port: set core_eye's glow intensity to
0.1 and skip the flicker unless the intro cinematic is ported.

---------------------------------------------------------------------------------------------------------------
## 3. EnumLogic (type 0xd, vt 0x7a1ecc, ctor 0x40c5a0, serialize 0x6a9bb0; updated by EnumLogicManager_, `0x6a9d50(this, dt)`)

| field | off | notes |
|---|---|---|
| Type | 0x34 | int |
| Time | 0x38 | serializer sets 0x38 = 0 first, then reads Time into it; **for types 1, 3, 4 it is also copied to 0x3c** (total). 0x38 is then the remaining / elapsed time. |
| total Time | 0x3c | |
| Speed | 0x40 | read only for type 4 (type 1 uses it when set from code; type 6 uses 0x40 as runtime "previous side") |
| Start Color | 0x44 (vec4) | read for types 0, 1, 4 only (ctor = 0) |
| End Color | 0x54 (vec4) | same |
| 0x64 | 0 | |
The serializer reads: type 0 -> Time, Start, End (no copy); 1 -> Time, Start, End; 2 -> nothing; 3 -> Time; 4 -> Time, Start, End, Speed;
5, 6, 7 -> **nothing** (so EndTunnel/FinishPoint's Start/End colours in the XML are ignored and stay 0).
Asserts: type 0 "checkpoint is being used via EnumLogic (deprecated)", type 5 "CarStart ... deprecated".

```c
switch (Type) {                                   // update switch is on Type-1, types 1..7
case 0: break;                                    // deprecated checkpoint: no update
case 1: /* fade + rise: checkpoint_ghost */
    if (rem < 0) Destroy(owner);
    s = max(rem / total, 0);
    RenderMesh.SetColor(Start*s + End*(1-s));
    if (Light) Light.intensity = s*3;
    owner.TranslateLocal(0, Speed*dt, 0);         // along own local Y
    rem -= dt; break;
case 2: /* spin: fansaw */  body.angularVel = owner.localY;  break;   // every frame, 1 rad/s about own Y (BodyRigid needed)
case 3: /* lifetime: Phaser 3s, ShardTarget/ball/box/cone/cylinder/pill 10s or 3s */
    if (rem < 0) Destroy(owner);  rem -= dt; break;
case 4: /* grow + fade: explosions, shockwaves, FireRing, ring_particle */  see re_E.md 1.2; break;
case 5: Assert("CarStart deprecated"); DestroyImmediate(owner); break;
case 6: /* FINISH LINE: EndTunnel, FinishPoint */
    GO* car = FindByName("Car"); if (!car) break;
    if (t(0x38) == 0) {                           // not finished yet
        vec3 l = transpose(R_owner) * (car.pos - owner.pos);   // local coords
        if (fabs(l.y) > 20)            { prev(0x40) = l.y; break; }
        if (l.y * prev > 0)            { prev = l.y;       break; }   // same side: no crossing
        prev = l.y;                                                // crossed the local XZ plane (or prev==0)
        if (fabs(l.x / scale.x) > 8.6) break;
        float z = l.z / scale.z; if (z > 17.6 || z < 1.2) break;  // window 1.2..17.6 (scaled) above the origin
        t = 0 + dt;                                // finished state
        Broadcast message LevelFinishedMessage{ id 0x13, success = 1, 0.0f };   // -> results screen etc. (re_E.md)
    } else if (t < 1) {                            // post-finish colour fade (Start/End are 0 -> sets the mesh tint to 0;
                                                   //  EndTunnel also has a ColorPaletteComponent that may re-colour it - unverified)
        RenderMesh.SetColor(Start*(1-t) + End*t);  t += dt;
    }
    break;
case 7: /* hud_buttons: pin to top-right */
    owner.pos.x = screenW - 20 - owner.scale.x;  owner.pos.y = owner.scale.y + 20;  owner.dirty = 1; break;
}
```
Blueprint usage: type 1 `checkpoint_ghost` (Time 1, spawned by CheckpointLogic); 2 `fansaw`; 3 Phaser(3), ShardTarget(10), ball/box(3)/cone/convexmesh/cylinder/pill(10);
4 CoreExplosion, FireRing, JumpShockwave, MineExplosion(2), PhaserExplosion, ShardShockwave, TestForceField, ring_particle;
6 **EndTunnel** (levels CRUIS'N EGYPT, NIGHTMARE_ON_CHRISTMAS_STREET, TOTALLY_TUBULAR, enter_the_matrix, Tutorial_Level1/2, NewFirstPlayableAddon) and FinishPoint (datatest); 7 hud_buttons.
**To reproduce (type 6):** keep `prevSide` per finish object, finish when the car origin crosses the object's local XZ plane
within |y|<=20, |x|<=8.6*sx, 1.2*sz<=z<=17.6*sz; then send the level-finished event once. Initialise prevSide = 0 (PC leaves it uninitialised).

---------------------------------------------------------------------------------------------------------------
## 4. Particles

### 4.1 ParticleEmitter (type 0xe, vt 0x7b0f54, ctor 0x5f4de0, XML serialize 0x5f46f0; all fields float)
| field | off | used as |
|---|---|---|
| Color Index | 0x34 | palette row, 1-based |
| Color Index Randomness | 0x38 | +-rnd |
| Color Intensity | 0x3c | colour multiplier (may be negative = darkens, additive blend) |
| Texture Index | 0x40 | volume-texture slice, 1-based |
| Texture Index Randomness | 0x44 | +-rnd |
| Texture Delta | 0x48 | slices advanced over the particle's life |
| Size Intensity | 0x4c | half-size of the quad (world units) |
| Size Randomness | 0x50 | +-rnd |
| Position | 0x54 vec3 | spawn offset in emitter space (world matrix incl. scale) |
| Position Randomness | 0x60 vec3 | +-rnd per axis, in emitter axes (rotated, scaled) |
| Rotation | 0x6c | degrees*... (see below) |
| Rotation Randomness | 0x70 | |
| Delta Rotation | 0x74 | radians over the whole life |
| Rotation Dampening | 0x78 | **copied but never used** |
| Direction | 0x7c vec3 | rotated by the emitter matrix, NOT normalised |
| Angle | 0x88 | degrees, cone jitter amount sin(Angle) |
| Speed / Speed Randomness | 0x8c / 0x90 | |
| Gravity | 0x94 | added to vel.z per second (world Z) |
| Speed Dampening | 0x98 | **copied but never used** |
| Particle Lifetime | 0x9c | seconds |
| Spawn Frequency | 0xa0 | particles per second |
| spawn accumulator | 0xa4 | runtime (reset on load) |
| previous spawn pos | 0xc0 vec3 | runtime (init = current world spawn pos) |

Registration: slot 11 links the emitter into the graphics emitter list (sentinel graphics+0x48), slot 12 unlinks.

### 4.2 Global pool and per-frame update (graphics `0x609640(gfx, dt)`)
* Pool: `gfx+0x1b7bb8`, **25000 (0x61a8) particles x 0x4c bytes**, count `gfx+0x55777c`. Burst queue: `gfx+0x387998`, count `+0x557778`.
* Particle layout: +0 pos, +0x0c age (0..1), +0x10 tex w, +0x14 palette v, +0x18 size, +0x1c intensity, +0x20 rotation,
  +0x24 vel, +0x30 gravity, +0x34 life left (s), +0x38 dTex/s, +0x3c dRot/s, +0x40 1/Lifetime, +0x44 speed damp, +0x48 rot damp.
```c
for (i = 0; i < count; ++i) {                      // 1. simulate
    p.age += p.invLife*dt;  p.rot += p.dRot*dt;  p.life -= dt;  p.tex += p.dTex*dt;
    if (p.life <= 0) { p = pool[--count]; --i; continue; }      // swap-remove
    p.vel.z += p.gravity*dt;  p.pos += p.vel*dt;                // no damping, no collision
}
append min(25000-count, queued) particles from the burst queue;  queued = 0;   // 2.
for each registered emitter: count += Emit(emitter, dt, &pool[count], 25000-count); // 3. continuous emitters
copy pos, age, tex, colorV, size, intensity, rot (9 floats) of every particle to the instance buffer (gfx+0xdc018);
```
Emit (`0x5f4eb0`, returns number spawned):
```c
M = owner world matrix (3x4 rows, GO+0x12c, includes scale);  sa = sin(Angle * pi/180);
dirW = M3 * Direction;   posW = M * Position;
acc += SpawnFrequency * dt;  n = (int)acc;  step = 1/n;
for (k = 0, t = 0, tsub = 0; n >= 1 && free > 0; --n, ++k) {
    acc -= 1;
    r   = M3 * (rnd(PosRand.x), rnd(PosRand.y), rnd(PosRand.z));
    pos = posW*(1-t) + prevPosW*t + r;           // spread between last and current emitter position
    tex   = (TexIndex   + rnd(TexRand)   - 0.5) / 64;
    colV  = (ColorIndex + rnd(ColorRand) - 0.5) / 64;
    inten = ColorIntensity;
    size  = SizeIntensity + rnd(SizeRand);
    rot   = (Rotation + rnd(RotRand)) * (pi/360);  // note pi/360, not pi/180
    vel   = (dirW + sa*(rnd(1),rnd(1),rnd(1))) * (Speed + rnd(SpeedRand));
    gravity = Gravity; life = Lifetime; invLife = 1/Lifetime;
    dTex = TexDelta*invLife/64;  dRot = DeltaRotation*invLife;  (damp fields copied)
    pos += vel*tsub; age = invLife*tsub; life -= tsub;   // sub-frame pre-advance
    tsub += dt/n_initial;  t += 1/n_initial;
}
prevPosW = posW;
```
(When the pool is full the accumulator keeps growing and is emitted later as a burst.)

### 4.3 Rendering (effect `Effects/particlesnew.fx`, textures `Textures/particles.ddsv`, `Textures/particle_color.tga`)
Instanced draw of mesh `quad` (corners at +-1) with the 9-float instance data; `d3dsm.py` output:
```
VS: ang = rot; (c,s) = (cos ang, sin ang)
    pal  = tex2Dlod(particle_color, u = age, v = colV)          // 64x64 RGBA palette: ROW = Color Index (1-based), COLUMN = age
    half = size * pal.a
    o    = corner.xy * half;  o' = (c*o.x + s*o.y, -s*o.x + c*o.y)
    viewPos = WorldView * pos;  clip = Proj * (viewPos + (o',0))  // camera-facing billboard in view space
    color = pal.rgb * intensity;  uvw = (cornerUV, tex)
PS: t = tex3D(particles.ddsv, uvw)   // 64x64x64 A8R8G8B8 volume texture, no mips; slice k-1 = Texture Index k
    out = t * float4(color, 1);  clip(|r|+|g|+|b| - 0.004)
States: ZENABLE 1, ZWRITE 0, CULL NONE, ALPHABLEND 1, SRCBLEND ONE, DESTBLEND ONE  -> purely ADDITIVE
```
Palette rows used: Color Index 1 (sparks: white -> orange -> red -> dark red -> black), 8 (VolcanoSmoke: dim teal -> black,
with intensity -0.2 it darkens), 10 (RedParticles: orange -> dark), 12/13 (Gray/Snow: black -> grey -> black). Alpha = 255 in these rows (size constant).
**Port:** convert the 64 slices to a 2D atlas (8x8 of 64x64); tex w -> slice = w*64-0.5 (blend the two nearest slices
or just round); palette lookup (age, row); additive blending with depth test and no depth write. For negative intensity
(VolcanoSmoke) use subtractive blending (or clamp, losing the darkening).

### 4.4 Level emitters (blueprints; no level overrides; transform scale 0.4098 everywhere)
| blueprint | levels (playable sets) | Color idx | Inten | Tex idx | TexDelta | Size | PosRand | Speed | Grav | Life | Freq | CameraFollower |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| GrayParticles | Gray (Challenges), KillItWithFire (Community) | 12.5+-0.5 | 0.9 | 50+-1 | 0 | 0.5+-0.1 | (650,650,300) | 50+-50, Angle 14 | -4 | 4 | 4000 | 200 |
| RedParticles | Hardcore_GoldLevel, NIGHTMARE_ON_CHRISTMAS_STREET (x2), train_wreck | 10 | 1.0 | 52+-2 | 5 | 1.0+-0.2 | same | same | -4 | 4 | 4000 | 200 |
| Snow | christmas (Stunt), ski_slope (Challenges), NIGHTMARE | 12.5+-0.5 | 5.0 | 50+-1 | 0 | 0.5+-0.1 | same | same | -4 | 4 | 4000 | 200 |
| VolcanoSmoke | MountainTop (Challenges) | 8 | -0.2 | 49 | 0 | 250+-5 | 0 | 50+-50 | +50 | 30.5 | 2.5 | none (fixed at (-3,2274,702)) |
All: Position (0,0,100) (x0.41 = 41 up), Direction 0 (pure random 0.24*[-1,1]^3*speed), Rotation 0+-360, Delta Rotation 1 (VolcanoSmoke 0).
Weather keeps ~16000 particles alive (4000/s x 4 s) inside a box +-266 x +-266 x +-123 around a point 200 units ahead of
the camera. **Vita:** reduce the frequency (e.g. 1/4..1/8) and raise size slightly; it is decoration only.

### 4.5 CameraFollower (type 0x89, vt 0x7973ac; field Distance 0x34)
Every frame (`0x6b8110`): `owner.pos = cam.pos - Distance * cam.orient.col2;` (camera col2 = BACK, so the point is
`Distance` units in FRONT of the active camera); orientation untouched (stays identity), dirty = 1.

### 4.6 Car sparks (`SparkBitch`, burst-spawned from CarLogic::HandleCollisions, `0x7094xx`)
SparkBitch: Color 1 (white->red->black ramp), Inten 0.6, Tex 55, Size 0.05+-0.1, PosRand 0.25, Speed 1+-0.1, Angle 14,
Grav -5, Life 0.75, Freq 0 (never spawns by itself). Per contact the car does:
`spark.pos = contact point; n = contact normal; spark.Direction = n/|n|; spark.Speed = |n| (=1);
 vt = relVel - n*dot(n,relVel); s = |vt|^2;  x = (s/3 < 15) ? sqrt(s)/3 : 15;  spark.acc = 5*x;
 Emit(spark, dt = 0.015, -> burst queue)`  (so up to ~11 sparks for slow scrapes, 75 when |vt| >= ~6.7); `x >= 2` also
starts the scrape sound. (Re-check against the CarLogic report.)
Other effect objects (MineExplosion, CoreExplosion, ShardShockwave, FireRing, JumpShockwave) are **meshes animated by
EnumLogic type 4**, not particles.

---------------------------------------------------------------------------------------------------------------
## 5. Cooling

### 5.1 CooldownPadLogic (type 0x3f, vt 0x7960cc; blueprint `coolpad`; levels CRUIS'N EGYPT 42, christmas 37, skyscraper 34, train_wreck 6, vert_park 44 - **no campaign level**)
| field | off | value in all 121 instances |
|---|---|---|
| Scale Offset | 0x34 vec3 | (0.35, 1.5, 0.2) |
| Scaled Pos Offset | 0x40 vec3 | (0, 0, 0.95) |
| On Pad Color | 0x5c vec4 | (0.2, 1.0, 0.3, 1.0) |
| saved RenderMesh colour | 0x4c vec4 | taken in init (0x690030) |
```c
void VUpdate(float dt) {                                   // 0x6900e0
    GO* car = FindByName("Car"); if (!car) return;
    vec3 h   = ScaleOffset * owner.scale;                  // half extents, per axis
    vec3 c   = owner.pos + ScaledPosOffset * h;            // offset added in WORLD axes (= +0.95*h.z up)
    vec3 d   = car.pos - c;
    vec3 l   = (dot(R.col0,d), dot(R.col1,d), dot(R.col2,d));     // pad local axes
    if (any(l > h) || any(l < -h)) { RenderMesh.SetColor(saved); return; }   // inclusive box
    CarLogic* cl = car.GetComp(0x20); BodyRigid* rb = car.GetComp(0x10);
    rand();                                               // result unused
    RenderMesh.SetColor(OnPadColor);                      // pad turns green while the car is on it
    if (!cl || !rb) return;
    SetRTPC(0x75bd8ad4, cl.heat/cl.maxHeat);
    cl.heat = max(0, cl.heat - 0.5 * |rb.linearVel| * dt);     // proportional to CAR SPEED, nothing when stopped
    SetRTPC(0x75bd8ad4, cl.heat/cl.maxHeat);
    cl.SetCoolGlow(color = (0.2, 1.0, 0.3, 50), param = 0.75, timer = 1.5);   // 0x426a20, refreshed every frame
}
```
CarLogic shows the glow while `timer > 0` (timer -= dt each frame): first frame posts **Play_CoolDown**, then writes
`glowColor*timer` into the car parts' RenderMesh second colour (+0x64..+0x67) (see the CarLogic report).
**Port fix:** replace "box + margins (2,2,3), -60/s" by: half = (0.35,1.5,0.2)*padScale, centre +0.95*half.z up, pad-local
test, `heat -= 0.5*speed*dt` on the PC scale (= `heat -= speed*dt` with the port's max 100); pad colour -> (0.2,1,0.3,1) while inside.

### 5.2 CarCooler (type 0x7e, vt 0x7a2ae4; blueprint `car_cooler`; levels **Campaign_Level6**, credits_level, moto, neon_nights, ski_slope, sky_city, NitroTunnel, NIGHTMARE, TOTALLY_TUBULAR)
Field Intensity 0x34 (default 45; no level sets it). Global, no area test:
```c
void VUpdate(float dt) {                                  // 0x6a4ca0
    CarLogic* cl = FindByName("Car")->GetComp(0x20); if (!cl) return;
    cl.heat = max(0, cl.heat - cl.BoostForce/30 * cl.HeatMultiplier * dt * 6);   // 6x the boost heating rate
    cl.SetCoolGlow((0.1, 1.0, 0.3, Intensity), 0.75, 1.5);                      // permanent green glow (+Play_CoolDown once)
    if (GO* hud = FindByName("hud_main")) {               // HUD pulses cyan <-> magenta
        s = 0.5 + 0.5*sin(time*5);
        hud.RenderMesh colour1 = colour2 = (R=s, G=1-0.8*s, B=1, I=1);
    }
}
```
Net effect: boosting can never overheat in these levels. ProximityCarCooler@N (type 0xa7) is only on the network
"NetDummy" car blueprints (multiplayer) - not needed.

---------------------------------------------------------------------------------------------------------------
## 6. Armature / skinned creatures (type 0x6c, vt 0x796a64; init 0x5fead0, update 0x5fddb0)
* Blueprints `crow` (crow_tb.mop + crow_arm.arm), `joe` (joe_tb.mop + joe_arm.arm), `salamander`. Placed only in
  **credits_level** (crow, joe) and **nature** (Hardcore set: 2 crow, 1 joe, 2 salamander). The .arm files live in the game root.
* Fields: ArmFile 0x34 (string); runtime: speed 0xb4 = 1, time 0xb8, action 0xc0 = 0, blend-to action 0xc4 = 0,
  blend 0x100 = 0, editor-draw flag 0x108. Init also stores per-action durations for joe_arm.arm (5.4, 3.6, 3.6) and
  scorpion_arm.arm (0.7, 0.7) - not used by the update.
* Update: `time += dt*speed; frame = time*fps; i = floor(frame) % (frames-1); interpolate keys i and i+1;` build the
  bone palette for the skinned shader (`phong_emit_skinned`, `depth_skinned`). Nothing in the game changes the action,
  so **each creature loops action 0 forever**: crow `LookAround` (43 frames, 15 fps = 2.8 s), joe `Run` (13 f), salamander
  `FishFlail` (25 f). Other actions: crow Squawk/RuffleFeathers, joe Crab/Creep, salamander Walk/Idle.
* .arm format: `"BASEPOSE\0" u32 size, u32 boneCount, boneCount names\0, bind data...`, then per action
  `"ACTION\0" u32 size, name\0, u32 frames, f32 fps, u32 tracks, tracks x { u32 bone, u32 channels, channels x { "Qfloat4"|"Pfloat3"|"Sfloat" \0, u32 bytes, frames x value } }`
  (one key per frame).
* **Worth porting?** Low priority: only the credits scene and one Hardcore level use them, as decoration. Drawing the
  .mop in bind pose (or skipping them) is acceptable; full support needs the .mop skinned-mesh loader + this simple
  per-frame keyed animation.

---------------------------------------------------------------------------------------------------------------
## 7. Port checklist
1. Trigger system: global list + `Broadcast(name)`; zones (sphere / world-Z half-height cylinder / AABox / oriented box) on
   the car origin, Recheck Delay, OnEntryOnly; don't draw trigger meshes.
2. TriggerRespawnLocation -> set respawn pos + rotation (used on the next respawn). ControlScheme -> Basic/Advanced.
3. Tutorial text HUD (table 2.5) with button-glyph substitution, grow/fade/pulse, voice via an announcer queue.
4. EnumLogic types 1-4, 6 (finish line!), 7; type 2 = spin about local Y at 1 rad/s.
5. Particles: 25000 pool, emitter maths above, additive billboards, 64-slice texture + 64x64 lifetime palette; weather
   emitters 200 units in front of the camera (reduce counts on Vita); sparks burst on scraping contacts.
6. Coolpad: pad-local box (0.35,1.5,0.2)*scale, heat -= 0.5*speed*dt (PC units). CarCooler: heat -= 6*boostHeatRate*dt, green glow.
7. core_eye glow intensity 0.1; TurnOffBlur in credits; armatures optional.
