# Group A - collisions, hazards, breakables (Nitronic Rush PC exe)

Notation: `GC(id)` = `GameObject::GetComponent(id)` (0x66e730, ecx = GameObject).
`dt` = frame delta. All offsets are hex. "car" = player car GameObject; `CL` = its CarLogic (type 0x20).
World is **Z-up**. The car's local frame is X = right, Y = forward, Z = up (wheel `Pos` in Car.xml: x=+-0.7, y=+-1.1, z=-0.243).

## 0. Shared facts discovered

### Component type ids (slot 0 of every component vtable returns it; full list in `$HOME/nrw/typeids.txt`)
0xb Light, 0xe ParticleEmitter, 0x10 BodyRigid, 0x11-0x1a Collider*, 0x12 ColliderOrientedBox, 0x14 ColliderTriMesh,
0x16 ColliderCylinder, 0x1b ControllerCar, 0x1e CollisionInfoCollector, 0x1f Material, 0x20 CarLogic, 0x24 LaserLogic,
0x47 RenderMesh, 0x4f ChaseCameraLogic, 0x5b SingleLaserLogic, 0x6a StreetlightBreaker, 0x79 ScreenBreaker,
0x83 CustomDeathSpeed. (Script: `$HOME/nrw/typeids.py`.)

### Engine layout bits
* GameObject: +0x6c Transform; +0xa8..b0 world position, +0xf0..f8 scale, +0xfc 3x3 orientation (row-major, column k = local axis k), +0x1b4 "transform dirty" byte. +0x12c..0x158 = 3x4 world matrix rows.
* Generic component header: +0x20 current mode (0 play/1 pause/2 devpause), +0x24/25/26 Play/Pause/DevPause enable flags, +0x28, +0x2c "disabled" (1 = disabled; vtbl+0x34 = OnDisable, vtbl+0x38 = OnEnable). First derived member = +0x34.
* BodyRigid: +0x4c..6c world inverse inertia, +0x70 Rotational Factor, +0x74 Inverse Mass, +0x90..b0 orientation (row major), +0xd4..dc angular vel, +0xe0..e8 (copy of) linear vel, +0xec..f4 linear vel, +0x134 **Is Kinematic** (bool).
  `0x638740(body=eax, impulse=ecx, r=edx)` = ApplyImpulseAtPoint: `v += J*invMass; w += Iinv*(r x J)`.
* RenderMesh +0x55 = visible (byte). Light +0x99 = enabled (byte); Light +0x3c..0x48 = Color (r,g,b,intensity).
* `0x67c8e0(world=[0x7fe804], name, 0)` = spawn blueprint, `0x66ee40(obj,0)` = initialize spawned object, `0x67b340(name)` = find object by name.
  World+0x167c = "objects to destroy" queue (push a 12-byte handle {index, generation, table}).
* Audio: `0x664690(name)` = post Wwise event by name on object; `0x664630(id)` = post by FNV-1 id; `0x66bd60(id,val)` = set RTPC on object; `0x48e800(group,state)` = SetState. Decoded ids (FNV-1 lowercase): 0xfbe20f9e Play_Rand_Scrape, 0x6ef291a0 Stop_Rand_Scrape, 0xc135b338 Play_Rand_SmallCrash, 0x6d65f5b0 Play_Rand_MediumCrash, 0xbec1cc50 Play_Rand_Car_Explosion, 0x8c9a715a Play_Rand_Electric_Arc, state Crash_States/Announcer_Crash = Crashed, RTPC 0x7f5d03f3 = Laser_Volume.
* `0x605980` is an empty stub (stripped debug print) - ignore.

### Collision materials (physics only - NO game logic keys off these names)
`Materials/CollisionMaterials.xml` gives Friction / Restitution / Layer / Collides. The string is parsed left-to-right: '1' sets bit n, '0' just advances, spaces ignored (0x639e40). Two shapes collide if `(A.layer & B.collides) || (B.layer & A.collides)` (0x63af20).

| name | friction | restitution | layer bit | collides |
|---|---|---|---|---|
| Car | 0.5 | 0.5 | 1 | all except 1,2 |
| CarGib / CarGibTire | 0.8 / 0.95 | 0.3 / 0.95 | 2 | all except 1 |
| Road | 0.5 | 0.3 | 3 | all |
| Building | 0.5 | 0.5 | 4 | bit0 |
| Streetlight | 0.5 | 0.5 | 4 | bit0 |
| Obsticle | 0.5 | 0.5 | 5 | bit0 |
| Lava | 1.0 | 0.1 | 5 | bit0 |
| Metal (debris) | 0.5 | 0.5 | 0 | bit0 |

The string "Obsticle" does not even appear in the exe. **Deadliness comes from the `Material` component** (type 0x1f, field `Integer "Material ID"` at +0x34; no default), plus BodyRigid kinematic flag and CustomDeathSpeed:

| Material ID | used by | meaning in the car collision handler |
|---|---|---|
| (none) | roads, buildings, arches, saws | generic impact rules |
| 1 | Lava, LavaSpike, killblock, **killblock_laser** | instant death on any contact |
| 2 | spike_barrel | special barrel rule |
| 3 | streetlight, streetlight_no_flicker, streetlight_pulse | breakable, car passes through |
| 4 | all *Poster / screen billboards (81 blueprints) | ScreenBreaker, car still collides |

### CollisionInfoCollector entry (0x40 bytes, vector at collector+0x40..0x44), built in 0x63a9xx for each body of a contact
```
+00 vec3 contact point
+0C vec3 normal n   (points INTO this object, i.e. away from the other; for the car: the push-out direction)
+18 vec3 relative velocity at the contact  (v_other - v_self), so d = dot(n, +18) > 0 means closing
+24 vec3 this body's linear velocity before the solver (BodyRigid+0xec), 0 if no body
+30 vec3 this body's angular velocity before the solver (+0xd4)
+3C ptr  other collider (collider+4 = other GameObject)
```

## 1. Car collision handler  `CarLogic::HandleCollisions` = 0x709290 (called from CarLogic::VUpdate 0x70ad61)

Per-frame order in CarLogic::VUpdate: heat update 0x708d00(dt) -> 0x703af0(dt) -> **0x709290** -> 0x703ff0(dt) -> ... -> at the end it saves the body velocity: `CL+0x21c = body.vel(+0xec)`, `CL+0x228 = body.angvel(+0xd4)` (0x70ae1c). The collector holds the contacts of the last physics step.

Relevant CarLogic members: +0x34 Heat Max (set to **50.0** in initialize 0x7087f0), +0x38 Infinite Heat (false), +0x58 **wings deployed** (toggled by the "CarWings" action with a 0.3 s cooldown at +0x1d8, cleared by CarResetSoft, by death, and by ANY contact - see end of handler), +0x59 boosting, +0x5c heat, +0x80/84 handle of the sparks object, +0x98/9c handle of the chase camera, +0x124 scrape-sound-playing, +0x13c/140, +0x144/148, +0x15c/160 rumble accumulators (see below), +0x21c/0x228 last-frame velocities, +0x274 Explode Shake Amount (0.035), +0x278 Crash Shake Mult (0.05), +0x27c Crash Shake Max (0.10), +0x2fc "Explosion Object Name" (splosion), +0x318 "Dead Car Body Name" (DeadCarBody).

```c
void CarLogic_HandleCollisions(CarLogic* cl) {
  ControllerCar* cc = GC(cl->owner, 0x1b);
  CollisionInfoCollector* col = GC(cl->owner, 0x1e); if (!col) return;
  GameObject* sparks = HandleLookup(cl+0x80);             // car's sparks child
  ParticleEmitter* em = GC(sparks, 0x0e);
  bool impactSoundDone = false, scraping = false;
  int matId;                                              // NOTE: stale between iterations (see quirk)
  for (Contact* c : col->contacts) {
    GameObject* other = c->otherCollider->owner;
    Material* mat = GC(other, 0x1f);
    if (mat) { matId = mat->id; if (matId == 1) goto INSTANT_DEATH; }

    float d  = dot(c->n, c->relVel);                      // closing speed along normal
    vec3  vt = c->relVel - d*c->n;  float vtLen = |vt|;   // sliding speed
    // --- sparks at the contact
    sparks->pos = c->point; sparks->dirty = 1;
    em->dirMagnitude(+0x8c) = |n|;  em->dir(+0x7c) = n/|n|;
    float rate = min(vtLen/3.0f, 15.0f);
    em->rate(+0xa4) = rate*5.0f;
    Transform_Update(sparks); ParticleEmitter_Emit(em, 0.015f);   // 0x5f4eb0, global pool of 25000 particles
    if (rate >= 2.0f) {                                    // i.e. sliding speed >= 6
      scraping = true;
      if (!cl->scrapePlaying) { cl->scrapePlaying = 1; Post(car, "Play_Rand_Scrape"); }
      float s = |carBody.vel|^2 * 0.002f;  cl->rumbleL += s; cl->rumbleR += s;
    }
    // --- camera shake (every contact)
    ChaseCameraLogic* cam = GC(HandleLookup(cl+0x98), 0x4f);
    if (cam) { float k = min(cl->crashShakeMult * d * (1/60.f), cl->crashShakeMax);
               cam->shakeA(+0xb8) = max(cam->shakeA, k); cam->shakeB(+0xbc) = max(cam->shakeB, k); }

    // --- breakables
    if (matId == 4 && d > 15.0f) { ScreenBreaker* sb = GC(other,0x79); if (sb) ScreenBreaker_Break(sb); }
    if (matId == 3) {
      if (d > 15.0f) {
        StreetlightBreaker* sl = GC(other,0x6a); if (sl) StreetlightBreaker_Break(sl, c, &c->selfVel /*+0x24*/);
        // car passes through without losing speed: restore last frame's velocities
        carBody.vel = carBody.vel2(+0xe0) = cl->lastVel(+0x21c); carBody.angVel = cl->lastAngVel(+0x228);
        goto SOUNDS;                                       // no death check
      }
    }
    // --- death rules (first match wins)
    bool tooFast;
    if (cl->wingsOut)                         tooFast = d > 20.0f;
    else if (CustomDeathSpeed* cds = GC(other,0x83)) tooFast = d > cds->deathSpeed;
    else if (mat && mat->id == 2)             tooFast = fabs(d) > 50.0f || BarrelClearance(cl, other) < 0.5f;
    else { BodyRigid* ob = GC(other,0x10);
           if (ob && !ob->isKinematic)        tooFast = false;      // dynamic bodies never kill
           else                               tooFast = d > 60.0f; } // static geometry & kinematic movers
    if (tooFast) {
      vec3 carUp = column2(carBody.orientation);         // BodyRigid +0x98,+0xa4,+0xb0
      if (dot(c->n, carUp) < 0.7f) goto CRASH_DEATH;     // within ~45.6 deg of "landing on the wheels" survives
    }
  SOUNDS:
    if (!impactSoundDone) {
      if (d > 30.0f)      { Post(car,"Play_Rand_MediumCrash"); cl->rumbleBig = d;   cl->rumbleBigT = 0.35f; impactSoundDone = true; }
      else if (d > 15.0f) { Post(car,"Play_Rand_SmallCrash");  cl->rumbleSmall = d; cl->rumbleSmallT = 0.25f; impactSoundDone = true; }
    }
  }
  if (col->contacts.size() > 0) cl->wingsOut = 0;          // any contact folds the wings
  if (cl->scrapePlaying && !scraping) { Post(car,"Stop_Rand_Scrape"); cl->scrapePlaying = 0; }
  return;

INSTANT_DEATH:   // Material ID 1 (no speed / orientation test, rest of the contacts ignored)
  carBody.vel = carBody.vel2 = c->selfVel(+0x24); carBody.angVel = c->selfAngVel(+0x30); // keep pre-impact momentum
  CarLogic_Die(cl);                                        // 0x707db0
  SendEvent("Car Death", cause = 2, cl);
  return;

CRASH_DEATH:
  CarLogic_Die(cl);
  GameObject* cr = Spawn("crater");                        // scorch decal
  if (cr) { cr->pos = c->point;
            vec3 a = cross((0,0,1), n); if (|a| < 0.01) { a=(1,0,0); b=(0,n.z,0); } else { a=normalize(a); b=cross(n,a); }
            cr->orientation = [a b n] (columns) * RotZ(random(0..2*pi));   // 6.28318
            Init(cr); }
  SendEvent("Car Death", cause = 3, cl);
  return;
}
```
Rumble: 0x6ffff0 each frame, while timer +0x140 (or +0x148) > 0: timer -= dt, `rumbleL(+0x15c) += strength`, `rumbleR(+0x160) += 0.75*strength` (strength = the stored impact speed d). Purely gamepad vibration - can be skipped/mapped to nothing on Vita.

`BarrelClearance` (0x7009c0, approximate decode): takes the car's first <=4 wheel suspension end points (ControllerCar wheel records, 0xc8 bytes each: pos +0x3c + dir +0x48 * len +0x34) and averages them per axle. Returns 1200 (= never kills) if those points are above the barrel centre height (barrel pos.z) or outside the cylinder's half height along its axis; otherwise returns `(distance of the axle-midpoint from the barrel axis - ColliderCylinder radius(+0xb0)) * barrel.scale.x`. So a spike barrel kills on side hits with the wheels near its surface (< 0.5), or on any hit with |d| > 50; driving over it from above does not. spike_barrel is not placed in the story levels.

`CarLogic_Die` (0x707db0): wings/boost flags cleared, +0x128 = 1, spawns "Explosion Object Name" (splosion) at the car centre, sets chase camera mode (+0x94) = 3, spawns DeadCarBody + gibs, posts Play_Rand_Car_Explosion, Play_Rand_Electric_Arc, Wwise states Crash_States=Crashed and Announcer_Crash=Crashed. "Car Death" event causes: 2 = kill material, 3 = crash, 4 = overheat (0x708c10).

Heat (needed for lasers), 0x708d00: `if (!wingsOut && !boosting) heat -= 2.0*dt; heat = max(heat,0); if (heat > HeatMax(50)) { heat = HeatMax; if (!InfiniteHeat) { Die(cl); SendEvent("Car Death", 4); } }`.

### Summary of the death thresholds (d = closing speed along the contact normal, world units/s)
* Any contact with an object having `Material ID 1` -> instant death (cause 2). This is the only "instant" hazard; it is NOT tied to the Obsticle collision material.
* Otherwise a crash needs `d > threshold` **and** `dot(normal, carUp) < 0.7`:
  * wings deployed: 20 (overrides everything below, even CustomDeathSpeed)
  * other has CustomDeathSpeed: its "Death Speed" (45 on saw / backandforth_saw)
  * Material ID 2 (spike_barrel): |d| > 50, or a side hit with clearance < 0.5 (see BarrelClearance)
  * other has a non-kinematic BodyRigid (barrels, debris, crates): never
  * everything else (static colliders with no body, kinematic bodies: roads, buildings, arches, Obsticle-material blockers, platforms): 60
* Streetlight (ID 3) at d > 15: broken, car keeps its velocity, no death check. At d <= 15: ordinary rules (threshold 60, it has no body).
* Poster (ID 4) at d > 15: screen breaks, then ordinary rules apply (car bounces normally).
* Road / Building / Streetlight / Obsticle collision materials only differ in friction/restitution/layers (table above).

Quirk: `matId` is only refreshed when the other object has a Material component; a contact with a Material-less object later in the same contact list reuses the previous contact's id for the ==3 / ==4 tests (the ==1 and ==2 tests are guarded). Port: treat "no Material" as id 0.

**Port notes**: after the physics step, iterate the car chassis contacts with the data above (normal into the car, relative velocity, pre-solve car velocity). Keep the order of tests. Store `lastVel/lastAngVel` at the end of the car update so streetlights can be "passed through". Material ID 1 kills before any other processing and stops the loop.

## 2. StreetlightBreaker (type 0x6a, vtable 0x7969b4; ctor around 0x409f20, dtor 0x6bd0d0)
Fields: none serialized (serialize 0x6bd0a0 is `ret 8`). Members: +0x34 bool broken; +0x38 container (vector of 12-byte object handles at +0x44..0x4c) - never filled by any code (debris cleans itself up, see below).
Slots: 9 initialize 0x6bd090 `broken = 0`; 15 VUpdate 0x6bd0c0 = nothing; 16 0x6bd280 = Reset (if broken -> 0x6bd120); hit = 0x6bd290 (called only from the car handler).

```c
void StreetlightBreaker_Break(StreetlightBreaker* s, Contact* c, vec3* carVelBefore) {   // 0x6bd290
  if (s->broken) return;
  GameObject* o = s->owner;
  RenderMesh* rm = GC(o,0x47); Light* li = GC(o,0x0b);
  Post(o, "Play_SmashPole");
  static const char* parts[4] = {"streetlight_dead1","streetlight_dead2","streetlight_dead3","streetlight_dead4"};
  for (int i = 0; i < 4; i++) {
    GameObject* p = Spawn(parts[i]); if (!p) continue;
    p->scale       = o->scale * p->scale;                 // component-wise
    p->pos         = o->worldMatrix * p->localPos;        // blueprint position is an offset in streetlight space
    p->orientation = o->orientation * p->orientation;     // 0x672af0
    GC(p,0x47)->tint(+0x60..+0x70) = rm->tint;            // keep palette colour
    if (Light* pl = GC(p,0x0b)) CopyLight(pl, li);        // 0x42bbb0 (only dead1 has a Light)
    BodyRigid* b = GC(p,0x10);
    ApplyImpulseAtPoint(b, 0.3f * *carVelBefore, c->point - p->pos);   // pieces: Inverse Mass 100 -> dv = 30 * v_car
    Init(p);
  }
  ColliderTriMesh* tm = GC(o,0x14); if (!tm->disabled) { tm->disabled = 1; tm->OnDisable(); }   // car & others pass through now
  rm->visible = 0; li->enabled(+0x99) = 0;
  s->broken = 1;
  stats->streetlightsBroken(+0xec)++;                    // [0x7ff6dc]
  if (stats->streetlightsBroken + stats->screensBroken(+0xf0) >= 20) UnlockAchievement(0x2b /*"Rampage"*/);
}
void StreetlightBreaker_Reset(s) { // slot 16, 0x6bd120 (caller not identified - likely editor play->edit; level restart reloads the level)
  queue all handles in s+0x38 for destruction; clear list;
  re-enable ColliderTriMesh (disabled=0, OnEnable); rm->visible = 1; light->enabled = 1; s->broken = 0;
}
```
Debris blueprints (`Objects/streetlight_dead1..4.xml`): BodyRigid non-kinematic, Drag 0.995, Rotational Factor 0.2, **Inverse Mass 100**, ColliderOrientedBox material Metal, sizes (0.159,0.299,2.011) (0.162,0.264,1.216) (0.174,0.264,1.437) (0.212,0.147,1.572); local positions (0,0.620,-0.143) [rotated ~93 deg about X] (0,-0.924,-1.784) (0,-0.915,-4.438) (0,-1.295,-6.754); meshes streetlight_dead1..4.msh; dead1 also has the lamp Light + SimpleStreetLightFlickerLogic (2 s on / 10 s flicker / mod 10); all have **TimedDeathLogic 13-14 s** (they delete themselves). Being dynamic bodies they never kill the car (rule above); being Metal they still collide with the car.
Other streetlight components: OneShotRadiusAudioLogic "Play_Woosh_Checkpoint" radius 17.5 (whoosh when passing), Material ID 3, Light (Color 1,0.867,0.117,1.2, near 7 far 25).

**Port**: on car contact with a Material-3 object and d > 15: hide mesh/light, disable collider, spawn the 4 debris rigid bodies with the transforms above, give them `dv = 0.3*invMass*v_car` (+ spin from r x J) - consider clamping because Δv = 30·v_car looks extreme (the original engine may clamp velocities; check visually), play Play_SmashPole, restore the car's previous-frame velocity, bump the stat. Debris despawns after random 13-14 s.

## 3. ScreenBreaker (type 0x79, vtable 0x796f2c, size 0x44, ctor 0x4100a0)
Fields: none. Members: +0x34 broken; +0x38..0x40 handle of the spawned "poster_dead" object.
Slots: 9 initialize 0x6b93b0 `broken = 0`; 15 = nothing; 16 = Reset 0x6b9430 (`if broken: destroy poster_dead handle, mesh visible = 1, broken = 0`); hit = 0x6b9470.
```c
void ScreenBreaker_Break(ScreenBreaker* s) {          // 0x6b9470, from car handler when MaterialID==4 && d>15
  if (s->broken) return;
  RenderMesh* rm = GC(s->owner,0x47);
  GameObject* p = Spawn("poster_dead");
  Post(s->owner, "Play_ScreenGlassBreak");
  if (p) { s->deadHandle = p->handle;
           p->pos = owner->pos; p->orientation = owner->orientation; p->scale = owner->scale;
           GC(p,0x47)->colorRGBA8(+0x60..63) = rm->colorRGBA8;   // same tint
           rm->visible = 0; Init(p); }
  s->broken = 1;
  ScreensBrokenStat();                                // 0x6fbb30
}
// 0x6fbb30: run.screens([0x7fe7d0]+0x1c)++, stats.screens(+0xf0)++;
//   if run.screens >= 5 -> achievement 0x2c "Smash TV"; stats.bestScreensInRun(+0xf4) = max(..);
//   if stats.streetlights + stats.screens >= 20 -> achievement 0x2b "Rampage".
```
Poster blueprints: RenderMesh board5x7.msh with materials (building_cr2, <name>_poster), ColliderTriMesh material **Building** (stays enabled - the car bounces off normally, restitution 0.5), Phantom loop "Play_ScreenHum" (not stopped on break), Material ID 4. `poster_dead`: same mesh with material 2 = "poster_dead" (dead-screen texture), no collider, no timeout.
**Port**: swap the screen texture to poster_dead (or hide + spawn), play Play_ScreenGlassBreak, no physics change; the impact still goes through the normal death rules (60 static).

## 4. CustomDeathSpeed@N (type 0x83, vtable 0x7a2bc4, size 0x38, ctors 0x4106c0 / 0x6a4a90)
Field: `Float "Death Speed"` at +0x34 (serialize 0x6a4a50). **No default** (constructor leaves it uninitialised; every blueprint sets it). Used values: saw.xml and backandforth_saw.xml "45.0f" (parsed as 45). Only 15 vtable slots, no init/update logic - it is pure data read by the car handler: `if (!wingsOut && other has CDS) crash if d > DeathSpeed` (checked before the barrel and kinematic rules, so it also works on dynamic bodies). The saws are kinematic BodyRigid + ColliderTriMesh "Obsticle" - without CDS they would use 60.
**Port**: per-object override of the 60 threshold (still subject to the wings=20 rule and the 0.7 orientation test).

## 5. killblock_laser (blueprint)
Transform scale (3,3,3); RenderMesh block.msh material "GroundLaser" Color1 (1,0.5,0.1,2.2); ColliderOrientedBox material Obsticle size (1,1,1); CulledStatic; **Material ID 1**. No logic component.
Story use: one huge instance as a death floor - Level5 pos (53.6,1840.2,-35.9) scale (28.1,1163.2,3); Level6 pos (-1.7,1306.4,-106.0) scale (33.8,10000,100).
Behaviour: any car contact -> instant death (cause 2): car velocity reset to its pre-contact value (no bounce), Die(), "Car Death". (Same for killblock, Lava, LavaSpike.) Port: trigger box = instant kill.

## 6. LaserLogic (type 0x24, vtable 0x7959dc) - blueprint `laserarch`
No serialized fields (serialize 0x6925a0 only checks the version). Members: +0x34 container of the 5 spawned "laser" object handles (vector at +0x40..0x44, 12-byte handles), +0x4c timer, +0x50 phase duration, +0x54 state.
Slots: 9 initialize 0x6949a0 = `Setup(this, spawn=1)`; 15 VUpdate 0x6949b0; 19 0x693900 -> 0x6937b0 = destroy the 5 lasers.

laserarch blueprint: scale (2,2,2), Lgate.msh (material laserarch, Color1 1,1,1,1.2), ColliderTriMesh **Obsticle** Lgate.mdc (frame only; no Material comp -> hitting the frame uses the 60 rule), Phantom loop "Play_LaserObstacle" (its volume is driven by RTPC Laser_Volume), Light Color (0.9,0.1,0.1,3.0) near 1.5 far 15. Used 6x L4, 9x L5, 2x L6.
`laser` blueprint: laser.msh (StaticMesh DrawMode 2 + RenderMesh material "laser" Color1 (1,0.15,0.1,3)), Light Color (1.3,0.8,0.5,1.0) Type 5 near 0.015 far 0.1. No collider.

```c
void LaserLogic_Setup(L, bool spawn) {             // 0x694640; spawn=1 at init, spawn=0 each frame in editor mode
  L->timer = 0; L->duration = 1.0f; L->state = 0;
  Transform* t = owner; vec3 P = t->worldPos; vec3 S = t->scale; mat3 O = t->orientation; X=col0(O),Y=col1(O),Z=col2(O)
  vec3 base = P + Z*(S.z*0.87f);  vec3 step = X*(S.x*0.3f);
  for (k = -2; k <= 2; k++) {                     // 5 beams, index i = k+2
    laser = spawn ? Spawn("laser") (+push handle, Init) : lookup handle i;
    laser->orientation = columns(X, -Z, Y);      // beam local +Y points down along -arch Z
    laser->scale = S * 1.74f;                    // per component
    laser->pos = base + step*k;
  }
}
void LaserLogic_VUpdate(L, float dt) {             // 0x6949b0
  if (globalMode([0x7ff754]+8) == 2) { LaserLogic_Setup(L, 0); return; }   // editor: follow the arch
  L->timer += dt;
  if (!FindObject("Car")) return;
  uint mask = CarInBeams(L);                       // 0x6929f0, bit i = car box crosses beam i
  for (i = 0; i < 5; i++) LaserBeam_Update(laser[i], &L->state, owner, &L->timer, &L->duration, 1, mask, i); // 0x6930b0
  LaserLogic_StateMachine(&L->state, &L->timer, &L->duration, owner); // 0x692f20
}
```
State machine (0x692f20), car = global player car handle ([0x7ff6c4]+0x5577fc), D2 = |carPos - archPos|^2:
```
if (state != 0 && state != 3 && timer > duration) {
   timer = 0;
   1 -> 2, duration 0.5 ;  2 -> 3, duration 3.0 (unused, state 3 zeroes timer) ;  4 -> 0, duration 1.0 ;  return;
}
if (state == 0) { if (D2 < 40000) { state = 1; duration = 0.1; } }     // car within 200 units -> arm
else            { if (D2 > 40000) { state = 4; duration = 0.1; } }     // car farther than 200 -> fade out
```
So the lasers are NOT on a timer cycle: they power up when the car comes within 200 units (0.1 s ramp + 0.5 s flicker) and stay solid until the car is > 200 units away (0.1 s fade). 

Per-beam visuals (0x6930b0). f = sin(timer*50) + 1. Colours are (r,g,b,intensity). laserLight = Light of the laser object, archLight = Light of the arch; "Laser_Volume" RTPC posted on the arch:
| state | laser mesh colour (visible) | laser Light | arch Light | Laser_Volume |
|---|---|---|---|---|
| 0 off | hidden; timer = 0 | intensity 0 | intensity 1.0 | 0 |
| 1 ramp (0.1 s), r = timer/duration | (0.5rf, 0.5rf, 0.05rf, 0.25rf) | same colour, intensity = timer | same colour, intensity = timer | 0.35r |
| 2 flicker (0.5 s) | (0.5f, 0.5f, 0.05f, 0.25f) | same colour, intensity 0.75 | same colour, intensity 2.0 | 0.35f |
| 3 on; timer = 0 | (1.0, 0.1, 0.1, 3.0) | same, intensity 15 (30 if car in this beam) | same, intensity 4 (5 if car in beam) | 1.0 |
| 4 fade (0.1 s), a = max(0,1-timer/duration) | (a, 0.1a, 0.1a, 3a) | same colour, intensity 1.0 | (a,0.1a,0.1a, 3a) | a |
(mesh colour set via RenderMesh 0x4052e0.)

Damage (state 3 only, for each beam i with `mask & (1<<i)`): `CL->heat += 3000 * frameDt` (global clock [0x7ff774]+0x30), clamped >= 0. With Heat Max 50 this overheats the car within one frame -> Die + "Car Death" cause 4 (unless Infinite Heat). Lasers kill only through heat; they have no collider.

`CarInBeams` (0x6929f0): r = |(meshAABBmax - meshAABBmin)/2| of the car's RenderMesh (mesh +0x6c..0x80). Car centre in arch-local coords (lx,ly,lz) relative to the arch origin; return 0 unless `ly^2 < r^2 && |lx| < 0.8*S.x + r && |lz| < 0.8*S.z + r`. Then (0x6926a0) in car-local space test the 5 infinite lines `p_k = P + Z*(0.6*S.z) + X*(0.3*S.x*k)`, direction Z (k=-2..2) against the car's box (half extents = mesh AABB half size): bit set when the line passes through the box (slab test on the dominant axis, both faces checked).

**Port**: per arch keep {state,timer,duration}; 5 beam quads/meshes at the listed transforms; the colour/intensity table above; when on, a cheap test "car OBB intersects any of 5 vertical lines in the arch plane" -> kill (heat jump to max). Hitting the arch frame itself is a normal 60-threshold crash.

## 7. SingleLaserLogic (type 0x5b, vtable 0x7967b4) - blueprint `SingleLaser`
Field: `Float "Max Distance"` +0x34 (no default; SingleLaser.xml = 125). Member +0x38..0x40 handle of the "LaserDot" object. Not placed in any story level directly (used by SpinningLaser/Sweep variants or custom levels).
```c
void SingleLaser_Init(S) { GameObject* dot = Spawn("LaserDot"); Init(dot); RegisterName(dot,"LaserDot"); S->dot = dot->handle; } // 0x68dee0
void SingleLaser_VUpdate(S, float dt) {          // 0x68dcb0
  vec3 P = owner->worldPos; vec3 dir = col1(owner->worldOrientation);   // local +Y
  RayHit h;
  if (Raycast(physWorld, P, dir * S->maxDist, &h)) {        // 0x63b750
     if (CarLogic* cl = GC(h.collider->owner, 0x20)) cl->heat = max(0, cl->heat + dt);   // only +1 heat/s!
     owner->scale.y = S->maxDist * h.fraction;              // beam length = hit distance
     dot->pos = h.point;
  } else { owner->scale.y = S->maxDist; dot->pos = P; }
}
```
SingleLaser blueprint: scale (25,1,25), RenderMesh laser.msh Color1 (1,0.15,0.1,3). LaserDot: tiny splosion mesh + Light (0.8,0.3,0.2,2) + ParticleEmitter sparks. Note heat +1/s is less than the passive cooling (2/s), so a single laser can only overheat a car that is boosting / flying.

## 8. Files
* `$HOME/nrw/typeids.txt` component id -> class (+vtable); `$HOME/nrw/all.asm` whole .text linear disassembly (grep-able); `$HOME/nrw/ld.py <va> [n]` linear disassembly with annotations.
* Key functions: 0x709290 car collision handler, 0x707db0 CarLogic::Die, 0x708d00 heat update, 0x708c10 overheat death, 0x7009c0 barrel clearance, 0x6bd290 / 0x6bd120 streetlight break/reset, 0x6b9470 / 0x6b9430 screen break/reset, 0x6fbb30 screen stats, 0x694640 / 0x6949b0 / 0x6930b0 / 0x692f20 / 0x6929f0 / 0x6926a0 LaserLogic, 0x68dcb0 SingleLaserLogic update.
