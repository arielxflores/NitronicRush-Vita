# RE group E: story finale (Campaign_Level5 / Campaign_Level6 / outro)

All addresses are VAs in NitronicRush.exe. "GetComp(id)" = `0x66e730` (ecx = GameObject). GameObject layout used below:
GO+0x18..+0x20 = handle {index, serial, pool*}; GO+0x6c = Transform; GO+0xa8 = position; GO+0xf0 = scale (vec3);
GO+0xfc = 3x3 orientation (row-major, so column k = local axis k in world: col0 = +0xfc/+0x108/+0x114,
col1 = +0x100/+0x10c/+0x118); GO+0x1b4 = "transform dirty" byte. Transform+0x148 = dirty byte, +0x14c = version counter.
Component IDs used: 0x0b Light, 0x10 BodyRigid, 0x33 Text, 0x47 RenderMesh, 0x4c SparkBetweenLogic, 0x5d SpinFocusCamera,
0x6e CoreExploder, 0x78 CoreLightningLogic.

Shared helpers (found here, useful elsewhere):
| addr | meaning |
|---|---|
| `0x67c8e0(name, 0)` ecx=[0x7fe804] | create GameObject from blueprint name (not yet in world) |
| `0x66ee40(0)` ecx=obj | add object to world / initialize its components |
| `0x67b340(name)` | find object by SingleName (returns GO* or 0) |
| `0x67b220` eax=obj / `0x66f880` ecx=obj | destroy object (deferred) |
| `0x44d310` ebx=&handle | resolve handle {index,serial,pool} -> GO* or 0 |
| `0x677d50` ecx=transform | returns &position (vec3) |
| `0x672af0` | 3x3 matrix multiply ; `0x675470(angle)` ebx=mat: mat = RotZ(angle) composed with mat |
| `0x678e70` ebx=transform | rebuild 3x4 world matrix at transform+0xc0 (rows +0xc0/+0xd0/+0xe0, translation in 4th column) |
| `0x4151c0` ecx=BodyRigid, eax=&v | set linear velocity (writes both copies +0xe0 and +0xec) ; BodyRigid+0xd4 = angular velocity, +0x74 = inverse mass |
| `0x63ba80(world,&origin)` ebx=&segment, edi=&result | physics raycast along `segment` (not normalised); result+4 = t, result+0x14 = hit point = origin + segment*t; returns bool |
| `0x402180(vk)` | keyboard "key went down this frame" ([0x7ff764]: +0x72c focus, +0x1c+vk now, +0x1a6+vk previous) |
| `0x401150()` / `0x66e430(eax=mgr, &msg)` | message manager singleton / broadcast message |
| `0x6fb620(id)` ecx=[0x7fe7d0] | unlock achievement `id` (name table at 0x7f56c8) |
| [0x7ff6c4]+0x5577fc/800/804 | active camera object handle (index/serial/pool) |
| [0x7ff6c4]+0x557a38 | global scene brightness/fade (1 = normal, 0 = black) |
| [0x7ff788] | physics world: +8 = fixed sub-step (default 0.005), +4 = accumulator, +0x5c/+0x60 = list of "TV_" force volumes, +0x2c/+0x30 = list of physics colliders |
| RNG xorshift128 at 0x7f4824..0x7f4830 | `t=s0^(s0<<11); s0=s1; s1=s2; s2=s3; s3=s3^(s3>>19)^t^(t>>8)`; float = fmod(u32/32768.0, range) (`0x5ea5aa` = fmod); `0x429540` = same RNG on the FPU stack. Plain CRT `rand()` (import 0x75a30c) is used for integer picks |

RenderMesh (0x47) fields used: +0x60..+0x63 = Color1 as bytes **B,G,R,A** where A = intensity*10 (Color1 w=6.8 is stored as 68),
+0x68 = Float1, +0x6c = Float2, +0x70 = Float3 (shader params). Light (0x0b): +0x3c..+0x48 = Color (rgb, w = intensity).

---------------------------------------------------------------------------------------------------------------------

## 0. Big picture / event flow

* **Campaign_Level5** ("Into the Belly of the Beast"): the core (`core_building` + rotating `core_arms_upper` / `core_arms_lower`,
  `core_eye_nologic`) is scenery. `CoreLightningLogic` on the upper arms makes purple bolts between the arm tips.
  The `CoreExploder` on `core_building` **does nothing in this level**: its update returns at once unless the active camera has a
  `SpinFocusCamera` (only the outro scene has one). A `TriggerCylinderZone` (r=219, h=180, OnEntryOnly) at the top of the core
  fires `TopOfTheCORE` -> `TriggerAchievement` (ID 40 = "Top of the CORE").
* **Campaign_Level6** ("End to a Violent Heart"): at the end of the track sits the ball `corecorecore` (CoreBallLogic bolts that ray-cast
  to the surroundings, Light, tri-mesh collider). `CoreForceField` (TV_RadialForce, R=30) around it is a **drag zone** that brakes the car.
  A `TriggerSphereZone` (r=23, OnEntryOnly, 33 units in front of the ball) fires `EndStory` -> `TriggerEndStory`:
  replace `corecorecore` by `corecorecore2` (adds CoreBallExplosionLogic: pulse/explode/fall shader animation) and broadcast
  `LevelFinishedMessage(success=true)` -> normal level-finish (results, profile progress).
* Continuing after finishing the **last level of the level set** while the setting `"In Story Mode"` != 0 switches to the level class
  **`CampaignOutroLevel`** (`0x70da04`, `0x710e5e`, `0x7111aa`; otherwise `MainMenuLevel`). The outro loads `Levels/core_demo.xml`,
  flies the `SpinFocusCamera` around the core, calls `CoreExploder::Start` (the destruction sequence: fire rings, claws fall off, eye blown
  off, building breaks into 61 rigid pieces + `CoreExplosion`), fades out and goes to **`CreditsLevel2`** (credits; unlocks achievement 30
  "Made From Scratch" on entry).
* No Lua script references any of these objects/events.

---------------------------------------------------------------------------------------------------------------------

## 1. CoreExploder (type 0x6e, vtable 0x796c74, size 0x64, ctor 0x40fd60) - blueprint `core_building`

### Fields
| off | name | blueprint value (core_building) | notes |
|---|---|---|---|
| +0x34 | TimeRing0 | 1.6 | no ctor default (all times come from the blueprint) |
| +0x38 | TimeRing1 | 2.0 | |
| +0x3c | TimeSmallRing0 | 2.5 | |
| +0x40 | TimeSmallRing1 | 2.65 | |
| +0x44 | TimeSmallRing2 | 2.8 | |
| +0x48 | TimeEye | 4.0 | |
| +0x4c | TimeBuilding | 6.0 | |
| +0x50 | timer | 0 | runtime |
| +0x54 | state | 0 | runtime, 0..8 |
| +0x58/+0x5c/+0x60 | sparker handle {index, serial, pool} | 0 | CoreDeathSparker, later CoreEyeSparker |

Serialize = slot 10 `0x6ce060` ("unrecognized version on CoreExploder Serializing" if version != 0); slot 7 `0x6cddb0` = editor tweak UI.
Initialize (slot 9, `0x6cdd90`): `timer = 0; state = 0;`

### Start (`0x6ce320`, esi = this) - called by CampaignOutroLevel (and by the debug key)
```c
state++;            // 0 -> 1
timer = 0;
GO* s = Create("CoreDeathSparker"); AddToWorld(s);   // particle emitter, SingleName "core_death_sparker"
sparker = s->handle;
```
Note: CoreDeathSparker is created at its blueprint position, then moved every frame by MoveSparker (below).

### Update (slot 15, `0x6ceef0`)
```c
GO* cam = Resolve(camHandle at [0x7ff6c4]+0x5577fc);
if (!cam) return;
SpinFocusCamera* sc = cam->GetComp(0x5d);
if (!sc) return;                            // -> never runs in normal gameplay levels
timer += dt;
switch (state) {
case 0: if (KeyPressed('U' /*0x55*/)) Start(); break;   // debug only
case 1: if (timer > TimeRing0)      { Shake(sc,0.02); state++; SpawnAt("FireRing", {0,1.104,45.946}); } break;
case 2: if (timer > TimeRing1)      { Shake(sc,0.02); state++; SpawnAt("FireRing", {0,1.104,52.007}); } break;
case 3: if (timer > TimeSmallRing0) { Shake(sc,0.01); state++; SpawnAt("FireRing", {0,1.104,60.112}); } break;
case 4: if (timer > TimeSmallRing1) { Shake(sc,0.01); state++; SpawnAt("FireRing", {0,1.104,62.409}); DropClaws(); } break;
case 5: if (timer > TimeSmallRing2) { Shake(sc,0.01); state++; SpawnAt("FireRing", {0,1.104,65.144}); } break;
case 6: if (timer > TimeEye) {                       // no shake kick here
          SpawnAt("FireRing", {0,1.104,86.23});
          state = 7;
          GO* eye = SpawnAt("DeadCoreEye", {0,0,0});   // 0x6ce9f0: same as SpawnAt without offset
          BodyRigid* rb = eye->GetComp(0x10);
          SetLinearVelocity(rb, -20 * eye.col1);      // col1 = (GO+0x100, +0x10c, +0x118) = local Y axis
          rb->angVel(+0xd4) = 20 * eye.col0;          // col0 = (GO+0xfc, +0x108, +0x114) = local X axis
          Destroy(Resolve(sparker));                   // CoreDeathSparker
          GO* es = SpawnAt("CoreEyeSparker", {0,-6.741,89.104});
          es->scale = (1,1,1); es->dirty(+0x1b4) = 1; AddToWorld(es) /*again*/;
          sparker = es->handle;
          if (GO* e = FindByName("coreeye")) Destroy(e);   // the intact eye of the scene
        } break;
case 7: Shake(sc,0.003);                             // every frame while waiting
        if (timer > TimeBuilding) {
          Shake(sc,0.15);
          Destroy(Resolve(sparker));                   // CoreEyeSparker
          BreakBuilding();                             // 0x6cec10
          state++;                                     // 8 = finished (no case)
        } break;
}
if (state <= 6) MoveSparker();                      // 0x6ce140
```
`Shake(sc,a)`: `sc->amp1(+0x3d4) = max(sc->amp1, a); sc->amp2(+0x3d8) = max(sc->amp2, a);` (see 1.5).

### SpawnAt(name, offset) (`0x6ce780`; `0x6ce9f0` is the same with offset 0)
```c
GO* o = Create(name);
local = o.blueprintPos + offset;                   // offset is in the building's local, unscaled space
o.pos   = owner.pos + owner.R * (owner.scale * local);   // componentwise scale, then rotate
o.R     = owner.R * o.R;                            // 0x672af0
o.scale = owner.scale * o.scale;                    // componentwise
AddToWorld(o);
if (BodyRigid* rb = o->GetComp(0x10)) rb->pos(+0xf8) = rb->pos(+0x104) = o.pos;   // teleport
return o;
```
So everything inherits the building's transform (scale 5 in core_demo, i.e. FireRing heights 230..431 units).

### MoveSparker (`0x6ce140`): the CoreDeathSparker runs up the side of the building towards the eye
```c
GO* s = Resolve(sparker); if (!s) return;
float z = (timer / TimeEye) * 83.128 + 5.976;       // 5.976 at Start ... 89.104 at TimeEye
float y;
if (z < 72.318)       y = -11.136;
else if (z > 82.641)  y = -6.741;
else { float f = (z - 72.318) * 0.09687109;  /* = 1/10.323 */  y = f * -6.741 - (1 - f) * 11.136; }
s.pos = owner.pos + owner.R * (owner.scale * vec3(0, y, z));
s.dirty(+0x1b4) = 1;
```
(follows the building silhouette: y=-11.136 on the shaft, tapering to -6.741 near the top; ends exactly where CoreEyeSparker spawns.)

### DropClaws (`0x6ce6d0`) - at TimeSmallRing1
```c
if (GO* up = FindByName("CoreArmsUpper")) {
  Debris("DeadCore_Claw0", up, false); Debris("DeadCore_Claw0", up, true);
  Debris("DeadCore_Claw1", up, false); Debris("DeadCore_Claw1", up, true);
  Destroy(up);                                       // its CoreLightningLogic dies with it
}
if (GO* lo = FindByName("CoreArmsLower")) {
  Debris("DeadCore_Claw2", lo, false); Debris("DeadCore_Claw2", lo, true);
  Debris("DeadCore_Claw3", lo, false); Debris("DeadCore_Claw3", lo, true);
  Destroy(lo);
}
```
Debris(name, parent, mirror) = `0x6ce370`:
```c
GO* o = Create(name);
if (mirror) { o.localPos.x = -o.localPos.x; o.localPos.y = -o.localPos.y;  // 180 deg about Z
              o.R = RotZ(PI) (x) o.R; o.eulerZ(+0x68 of transform) += PI; }
vec3 rel = parent.R * (parent.scale * o.localPos);          // world-space offset from the arm centre
o.pos = parent.pos + rel; o.R = parent.R * o.R; o.scale = parent.scale * o.scale;
AddToWorld(o);
if (rb = o->GetComp(0x10)) {
  rb->pos(+0xf8,+0x104) = o.pos;
  rb->linVel(+0xe0 and +0xec) = -0.5 * rel;
  rb->angVel(+0xd4)           = cross(rel, vec3(0,0,0.05)) = (0.05*rel.y, -0.05*rel.x, 0);
}
```
The claws are dynamic BodyRigid (Inverse Mass 0.1, Rotational Factor 0.1, convex collider, mesh `DeadCore_ClawN.msh`, material core_building).

### BreakBuilding (`0x6cec10`) - at TimeBuilding
SpawnAt(name, 0) for, in this order: `DeadCoreBase`, `DeadCore_A_0..A_8`, `DeadCore_B_0..B_6`, `DeadCore_C_00..C_14`,
`DeadCore_D_0..D_7`, `DeadCore_E_00..E_18`, `DeadCore_F_0..F_4` (61 pre-fractured pieces, each its own BodyRigid with InvMass 0.1 +
ColliderConvexMesh `<name>C.mdc` + RenderMesh with material `core_building`; their blueprint positions are already in building-local
space, so they spawn exactly in place), then `CoreExplosion`, then `Destroy(owner)` (the intact core_building).
No velocity is given here; the pieces fall under gravity and get the one-shot TV_RadialImpulse of CoreExplosion (1.4).

### 1.1 Spawned effect blueprints
| blueprint | components | look |
|---|---|---|
| FireRing | RenderMesh FireRing.msh (mat FireRing, Color1 (1,1,1,3)) + EnumLogic Type 4 (Time 0.5, Start (1,1,1,3), End (1,1,1,0), Speed 20) | expanding ring that fades out in 0.5 s |
| CoreDeathSparker | ParticleEmitter (Color Index 10, Texture Index 16, Size 5 +-1, Rotation 65 +-8, dRot 5, ...) + SingleName "core_death_sparker" | spark shower climbing the tower; also the camera's focus target |
| CoreEyeSparker | ParticleEmitter (Color Index 10.5 +-0.5, Intensity 2.5, Texture 16, Size 7.5 +-2.5, Rotation 0 +-360) | spark shower at the broken eye socket |
| DeadCoreEye | BodyRigid (InvMass 0.1), ColliderCylinder (half height 1.05, radius 3.136), DeadCoreEye.msh | the eye flies off |
| CoreExplosion | StaticMesh+RenderMesh splosion.msh (additive, mat "splosion"), EnumLogic Type 4 (Time 1.5, Start (1,0.15,0.1,20) -> End (1,0.15,0.1,0), Speed 95), Light (0.8,0.3,0.2,2) near 5 far 15, Phantom (event "Play_CarEngine", Begin Playing 0 = silent), TV_RadialImpulse (R 80, coeffs (0.1,1,1), Max 1.5) | big red flash sphere |

### 1.2 EnumLogic Type 4 (case at `0x6a9f72`, run by EnumLogicManager_; fields: +0x38 remaining, +0x3c Time, +0x40 Speed, +0x44 Start Color, +0x54 End Color)
```c
if (remaining < 0) Destroy(owner);
float s = max(remaining / Time, 0);                  // 1 -> 0
color = Start * s + End * (1 - s);  RenderMesh.SetColor(color);
owner.scale += vec3(Speed * dt);                      // linear growth on all axes
if (Light* l = GetComp(0x0b)) l.intensity(+0x48) = color.w;
remaining -= dt;
```

### 1.3 TV_RadialImpulse (vtable 0x7af29c) - used by CoreExplosion
Fields: +0x34 Radius, +0x38/+0x3c/+0x40 Impulse Coeffients (c0,c1,c2), +0x44 Max Impulse, +0x48 "done" byte.
Init (slot 9 `0x62ac80`) done=0; after the first physics frame slot 16 (`0x62ac70`) sets done=1 -> **applied once**.
Per collider in the physics world (slot 15 `0x62aea0(dt, collider)`):
```c
if (done) return;  rb = collider->owner->GetComp(0x10); if (!rb || rb->invMass == 0) return;
vec3 d = collider.worldPos(+0x7c) - owner.pos; float d2 = dot(d,d);
if (d2 == 0 || d2 >= Radius*Radius) return;
float inv = 1/sqrt(d2);
float m = c0 + c1*inv + c2*inv*inv;                  // (0.1, 1, 1)
if (fabs(m) > MaxImpulse) m = (m < 0) ? -MaxImpulse : MaxImpulse;   // 1.5
vec3 J = d * (m / rb->invMass * inv);                // = mass * m * normalize(d)  (delta-v = m)
vec3 p = collider->shape->Support(-d*inv);           // vfunc +4 of shape at collider+0x34
ApplyImpulse(rb, J, 0.5 * (p - collider.worldPos));  // 0x638740: linear + angular
```
(Radius is not scaled by the object scale.)

### 1.4 Timeline (outro, times after Start; Start happens when the outro camera reaches t=0.3 = 7.5 s)
1.6 ring @z45.9 + shake 0.02 / 2.0 ring @52.0 + shake 0.02 / 2.5 ring @60.1 + 0.01 / 2.65 ring @62.4 + 0.01 + both arms replaced by 8
falling claws / 2.8 ring @65.1 + 0.01 / 4.0 ring @86.2, eye flies off (v = -20 * local Y), eye-socket sparks, continuous shake 0.003 /
6.0 shake 0.15, building -> 61 pieces + CoreExplosion flash. All heights are building-local (x scale 5 in core_demo).
**No sound events are posted by CoreExploder** (the explosion's Phantom has Begin Playing = 0); audio = outro music (ChangeMusicMessage 15).

### 1.5 Camera shake (SpinFocusCamera, `0x6d0520(dt)`, called each frame of the outro camera)
Fields: +0x3d4 amp1, +0x3d8 amp2, +0x3dc freq1 = 50, +0x3e0 freq2 = 70, +0x3e4 phase1, +0x3e8 phase2, +0x3ec decay1 = 5, +0x3f0 decay2 = 7
(set in `0x6d3550`, amps/phases 0).
```c
amp1 -= amp1 * decay1 * dt;
if (amp1 > 0.0001) { phase1 += freq1*dt; cam.RotateAboutLocalX(sin(phase1) * amp1); }   // 0x678800 (pitch)
else { amp1 = 0; phase1 = 0; }
amp2 -= amp2 * decay2 * dt;
if (amp2 > 0.0001) { phase2 += freq2*dt; cam.RotateAboutLocalY(sin(phase2) * amp2); }   // 0x678760
else { amp2 = 0; phase2 = 0; }
```
(angles in radians, applied after the look-at each frame.)

**To reproduce:** a state machine driven by the outro (or a debug key); spawn prefab pieces relative to the building transform exactly
as above; keep a handle to the sparker and move it with the piecewise y(z) profile; kick the camera shake amps with max();
EnumLogic-4 = grow + fade + auto-destroy. The building only breaks in the outro scene, never in Level5 gameplay.

---------------------------------------------------------------------------------------------------------------------

## 2. CoreLightningLogic (type 0x78, vtable 0x796e8c, ctor 0x40a6b0) - `core_arms_upper` (Level5) / `core_arms_upper_intro` (core_demo)

### Fields
| off | name | Level5 core_arms_upper | core_demo (_intro) | blueprint core_arms_upper |
|---|---|---|---|---|
| +0x34 | list<{handle(12), j, i}> of live bolts (20-byte elements) | | | |
| +0x4c | max_lightning_distance_ | 350 | 100 (outro also forces 100) | 350 |
| +0x50 | lightnings_per_sec_ | 60 | 60 | 60 |
| +0x54 | spawn accumulator | init 0 (slot 9 `0x6b9880`) | | |
| +0x58 | min_life_ | 0.2 | 0.2 | 0.2 |
| +0x5c | max_life_ | 0.5 | 0.5 | 0.5 |
| +0x60 | spark_width_ | 0.9 | 0.7 | 0.9 |
| +0x64 | spark_brightness_ | 20 | 10 | 20 |
No ctor defaults for the floats (blueprint values). Serialize slot 10 `0x6b98a0`.

### Endpoints (`0x6b9990`)
Two sets of 4 world points, recomputed every frame from the current (rotating) arm transforms (`0x678e70` world 3x4 incl. scale):
* upper U[k] = owner(CoreArmsUpper).World * Lk, with L = (19.869, 17.238, -6.229)
* lower D[k] = FindByName("CoreArmsLower").World * Mk, with M = (19.874, 18.409, 6.746) (no null check)
* sign pattern for k = 0..3: (+x,+y,z), (+x,-y,z), (-x,+y,z), (-x,-y,z)  -> the 4 claw tips of each arm.
(The arms counter-rotate: StaticSawLogic RotSpeed +0.3 / -0.3 about Z, so tip pairs come and go in range.)

### Update (slot 15, `0x6ba1e0`)
```c
ComputeEndpoints(U, D);
acc += lightnings_per_sec_ * dt;
while (acc > 1) {
  acc -= 1;
  int i = rand() % 4;          // lower tip
  int j = rand() % 4;          // upper tip
  if (dist2(U[j], D[i]) > max_lightning_distance_^2) continue;
  char name[] = "sparkdead1"; name[9] = '1' + rand() % 4;     // sparkdead1..4
  GO* s = Create(name); AddToWorld(s); if (!s) continue;
  bolts.push({s->handle, j, i});
  SparkBetweenLogic* sb = s->GetComp(0x4c);
  float lo = min(min_life_, max_life_), hi = max(...);
  sb->life(+0x38)       = lo + frand(hi - lo);
  sb->width(+0x70)      = spark_width_;
  sb->brightness(+0x3c) = spark_brightness_;
}
for (k = 0; k < bolts.size; k++) {                 // track the moving tips
  GO* s = Resolve(bolts[k].handle);
  if (!s) { swap-remove bolts[k]; k--; continue; }
  sb = s->GetComp(0x4c);
  sb->p1(+0x58) = U[bolts[k].j];  sb->p2(+0x64) = D[bolts[k].i];  sb->mode(+0x74) = 2;
}
```
Note the bolts are only re-targeted by this list; a newly spawned bolt gets its endpoints on the same frame (the loop runs after spawning).

### 2.1 SparkBetweenLogic (type 0x4c, vtable 0x7a24a4) - the bolt itself (blueprints sparkdead1..4)
Fields: +0x34 age, +0x38 life, +0x3c brightness, +0x40/+0x4c object handles (modes 1/3), +0x58 p1, +0x64 p2, +0x70 width, +0x74 mode
(1 = between two objects' positions, 2 = two world points, 3 = two local points of two objects; other = assert "Spark Mode Not Set").
Update (slot 15 `0x6a6c20`):
```c
if (age > life) { Destroy(owner); Place(); return; }
float I = (1 - age/life) * brightness;               // linear fade
RenderMesh.alphaByte(+0x63) = (int)(I * 10);         // i.e. Color1.w = I
age += dt;
Place();
```
Place (`0x6a66b0`), requires a valid camera object:
```c
vec3 d = p2 - p1; float len = |d|; if (len < 0.01) return;
vec3 dir = d / len;
vec3 n = normalize(cross(dir, camPos - p1));           // camPos = camera GO+0xa8
R.col0 = n; R.col1 = dir; R.col2 = cross(n, dir);      // camera-facing ribbon, local +Y along the bolt
owner.pos = p1; owner.R = R;
owner.scale = (width * len, len, len);
```
Meshes `spark1..4.msh` are flat (z=0) jagged strips from y=0 to y~0.99, x in about [-0.19, 0.30]
(103/261/157/128 verts; vertex format "p3n4ccu2t3b3", 64-byte stride). So a bolt = one of 4 pre-modelled zig-zag shapes, chosen at
random, stretched between the endpoints, width proportional to length; there is **no per-frame jitter or procedural segmentation**.
Colour: RenderMesh Color1 (0.4, 0.15, 0.6) (purple), material "sparks" (additive, texture arrow.tga in StaticMesh); intensity
w = (1 - age/life) * spark_brightness_ (blueprint 6.8 is overwritten on the first frame).

**To reproduce:** per frame accumulate rate*dt, spawn camera-facing textured quads/strips using the 4 meshes, life uniform in
[min,max], intensity fades linearly from brightness to 0, endpoints follow the arm tips; reject pairs farther than max distance.

---------------------------------------------------------------------------------------------------------------------

## 3. CoreBallLogic (type 0x85, vtable 0x7972fc, size 0x50, ctor 0x4108c0) - `corecorecore` (Level6), also on `corecorecore2`

### Fields
| off | name | blueprint corecorecore | Level6 instance | corecorecore2 bp |
|---|---|---|---|---|
| +0x34 | max_lightning_distance_ | 25 | 52 | 50 |
| +0x38 | lightnings_per_sec_ | 20 | 25 | 15 |
| +0x3c | spawn accumulator (init 0, slot 9 `0x6b8480`) | | | |
| +0x40 | min_life_ | 0.8 | 0.5 | 0.8 |
| +0x44 | max_life_ | 2.0 | 1.0 | 2.0 |
| +0x48 | spark_width_ | 2.0 | 1.0 | 0.8 |
| +0x4c | spark_brightness_ | 25 | 20 | 20 |
Serialize slot 10 `0x6b84a0`.

### Update (slot 15, `0x6b8550`)
```c
acc += lightnings_per_sec_ * dt;
while (acc > 1) {
  acc -= 1;
  float a = frand(2)-1, b = frand(2)-1, c = frand(2)-1;   // each in [-1,1)
  vec3 D = vec3(c, b, a) * max_lightning_distance_;        // x uses the 3rd draw, z the 1st
  if (|D.x|+|D.y|+|D.z| < 0.001) continue;
  vec3 O = owner.pos + owner.scale * normalize(D);        // point on the (scaled, unrotated) unit sphere
  if (!Raycast(physWorld, O, D, &hit)) continue;          // segment O -> O+D
  name = "sparkdead" + ('1' + rand()%4);
  GO* s = Create(name); AddToWorld(s); if (!s) continue;
  sb = s->GetComp(0x4c);
  sb->life = lo + frand(hi-lo);  sb->width = spark_width_;  sb->mode = 2;  sb->brightness = spark_brightness_;
  sb->p1 = O;  sb->p2 = hit.point;
}
```
Bolts are fixed in world space (no tracking list). They hit whatever collider is in range (walls, floor, the car).
**To reproduce:** random direction on a cube, normalised, start on the ball surface (ball radius = object scale, 10.69 in Level6),
segment ray-cast of length |D| <= maxDist*sqrt(3); spawn a SparkBetween bolt to the hit point.

---------------------------------------------------------------------------------------------------------------------

## 4. TV_RadialForce@N (type 0x42, vtable 0x7af21c, ctor 0x40e370) - `CoreForceField` (Level6)

Fields: +0x34 Radius (30), +0x38/+0x3c/+0x40 "Force Coeffients" c0,c1,c2 = (15, 3.02, 0.105) (ctor default 0), +0x44 Max Force (1000,
**read by serialize but never used**). Serialize slot 10 `0x62b310`. Slots 11/12 (`0x6166b0`/`0x6166e0`) link/unlink the component
in the physics world's TV list (world+0x5c, count at world+0x64). Slot 17 `0x62b520` = debug draw (3 circles of Radius, colour (0.5,0,1)).

Call site: physics update `0x63bf..`: once per **render frame** (dt clamped to 0.1, before the fixed sub-steps)
`0x63a2e0`: for each TV in the list, for each collider in world list +0x2c: `tv->vfunc15(dt, collider)`; then `tv->vfunc16()`.

Slot 15 `0x62b370(dt, collider)`:
```c
BodyRigid* rb = collider->owner->GetComp(0x10); if (!rb) return;
vec3 d = collider.worldPos(+0x7c) - owner.pos; float d2 = dot(d,d);
if (d2 == 0) return;
if (Radius*Radius <= d2) return;                     // inside the sphere only (Radius not scaled by object scale 13)
vec3 v = rb->linVel(+0xec); float s = |v|; if (s <= 0) return;
float dec = (c0 + c1*s + c2*s*s) * dt;               // 15 + 3.02 s + 0.105 s^2  (units/s^2)
if (dec > s) dec = s;                                 // never reverses
SetLinearVelocity(rb, v * ((s - dec) / s));           // 0x4151c0
```
It is a **speed-dependent drag (braking) zone**, not a push/pull: at 100 u/s the car loses 1367 u/s^2, at 30 u/s 200 u/s^2.
Direction is preserved; angular velocity untouched.
**To reproduce:** each frame, for every dynamic body whose collider centre is within R of the field centre, scale its velocity
by max(0, 1 - (c0 + c1 s + c2 s^2) dt / s).

---------------------------------------------------------------------------------------------------------------------

## 5. TriggerAchievement@N (type 0x67, vtable 0x796ee4, ctor 0x40f760)

Fields: +0x34 Incoming (std::string), +0x50 Outgoing (std::string), +0x88 Achievement ID (int). Serialize `0x726d50`.
Level5 instance: Incoming "TopOfTheCORE", Achievement ID 40. It is fired by a `TriggerCylinderZone` (Outgoing "TopOfTheCORE",
Radius 219, Height 180, OnEntryOnly, at (55.92, 5403.4, 1363.26) = above the core).
Fire handler (slot 16, `0x726fc0`) - overrides TriggerBase::Fire (`0x68d520`, which would broadcast Outgoing), so nothing is chained:
```c
Achievements(0x7fe7d0)->Unlock(AchievementID);       // 0x6fb620
```
Unlock (`0x6fb620`): if profile flag [0x7ff6dc]+0x82+id is 0: set it, show the "AchievementDisplay" blueprint (unless an object
"achievement_display" exists) with the name from table 0x7f56c8 (id 40 = **"Top of the CORE"**), broadcast a message, if all achievements
are now unlocked unlock the meta one (and the "Goldenceptor" car), save the profile.

## 6. TriggerEndStory@N (type 0x82, vtable 0x7972b4, ctor 0x4105f0)

Fields: +0x34 Incoming, +0x50 Outgoing (serialize `0x7269d0`). Level6 instance: Incoming "EndStory"; fired by a TriggerSphereZone
(Outgoing "EndStory", Radius 23, Recheck Delay 0.1, OnEntryOnly, at (-2.51, 2540.21, 65.03); the ball is at (-1.82, 2573.61, 65.33)).
Fire handler (slot 16, `0x726af0`):
```c
GO* old = FindByName("corecorecore"); if (!old) return;
save pos, scale, R(9 floats) of old;
RenderMesh* rm = old->GetComp(0x47);  col = (rm[+0x62]/255, rm[+0x61]/255, rm[+0x60]/255, rm[+0x63]*0.1);  // R,G,B,intensity
Light* li = old->GetComp(0x0b);       lightCol = li[+0x3c..+0x48];
Destroy(old);
GO* n = Create("corecorecore2"); if (!n) return;
n.pos = pos; n.scale = scale; n.R = R; n.dirty = 1;
n.RenderMesh bytes = col (x255, intensity x10);  n.Light colour = lightCol;   // Level6: Color1 (0.576,0.212,0.122,7.2), Light (0.8,0.365,0.22,1.2)
AddToWorld(n);                                       // runs CoreBallLogic / CoreBallExplosionLogic init with the copied values
Broadcast(LevelFinishedMessage{ id 0x13, success = true, float 0 });   // msg vtable 0x793c94
```
Effect of LevelFinishedMessage (handlers registered for id 0x13):
* BaseLevel `0x711780`: stores success at +0x152, the level clock (+0x48, double) at +0x154, calls level vfunc 14 (OnLevelFinished(success))
  -> normal finish / results flow.
* Profile `0x6f5890`: if success, unlocked progress (+0xe4) = max(+0xe4, current level index) and saved (userdata.xml).
`corecorecore2` has no collider, so the car can drive through where the ball was.

### 6.1 CoreBallExplosionLogic (type 0x8d, vtable 0x79750c) - on corecorecore2
Fields: +0x34 Crazy Pulse Time (3.25), +0x38 Explosion Time (0.275), +0x3c Fall Time (1.0), +0x40 timer, +0x44 base colour intensity,
+0x48 base light intensity. Serialize `0x68ace0`.
Init (slot 9 `0x68ad40`): `timer = 0; I0 = RenderMesh.alphaByte * 0.1; L0 = Light.intensity(+0x48);`
Update (slot 15 `0x68ad80`, skipped while `[0x7ff754]+8 != 0`):
```c
timer += dt;  RenderMesh* rm = GetComp(0x47);
if (timer < Pulse) {
  float p = timer / Pulse * 1.25;
  rm->Float3(+0x70) = p;
  rm->alphaByte = (int)(((1 - p) * I0 + 2) * 10);
  GetComp(0x0b)->intensity = L0 * (1 - p) + 1;
  return;
}
if (timer < Pulse + Expl)        rm->Float1(+0x68) = (timer - Pulse) / Expl * 15;
if (timer < Pulse + Expl + Fall) { float f = (timer - Pulse - Expl) / Fall; rm->Float2(+0x6c) = f*f*13; }
// note: Float2 is also written during the explosion phase (f negative, squared)
```
The visible pulse/explosion/fall is done by the `corecorecore2` shader using Float1..3 (shader not reversed here).
CoreBallLogic keeps shooting bolts from corecorecore2 meanwhile (50 / 15 per s / 0.8..2 s / width 0.8 / brightness 20).

---------------------------------------------------------------------------------------------------------------------

## 7. End-of-story sequence: CampaignOutroLevel (vtable 0x796e00, ctor 0x40b990, registered by name at `0x738ee3`)

Chosen when finishing/continuing from the last level of a level set with setting "In Story Mode" != 0 (`0x70da04`, `0x710e5e`, `0x7111aa`;
`0x6f8c30` = "current level is last in set").
Fields: +0x38 fade value, +0x3c fade rate, +0x40 phase duration, +0x44 phase timer, +0x4c phase, +0x50 cinematic helper object
(shared intro/outro code at 0x6d4xxx-0x6d6xxx: letterbox text `hud_cinema`, object `postprocessintro`, `coreeye`), +0x25c handle of
`skip_intro_text`, +0x268 skip prompt shown, +0x26c input map, +0x29c skip-hold timer.

Enter (slot 4, `0x724bf0`):
```c
fade = 0; fadeRate = 0.1; phaseDur = 0; phaseTimer = 0; phase = 0;
Broadcast(ChangeMusicMessage{id 0x17, track 15});
LoadLevel("Levels/core_demo.xml");
physWorld->fixedStep(+8) = 0.03;                  // default 0.005, restored by slot 7 (0x7246a0) on exit
reset render globals ([0x7ff6c4]+0x557a38 brightness = 0, +0x557a60..0x557a74 = 0, +0x557a94 = 1)
skip_intro_text = Create("skip_intro_text"); AddToWorld;
FindByName("CoreArmsUpper")->GetComp(0x78)->max_lightning_distance_ = 100;
helper.Init();                                     // 0x6d4590 (finds "coreeye", "postprocessintro")
bind "SkipIntro" (input codes 0x59 (= VK 'Y' if these are VK codes; the debug key above uses VK 'U'=0x55) and pad 0x116) and "ButtonPressed" (almost every key / pad button)
```
Update (slot 9, `0x724880`):
```c
if (fade < 1) { brightness = fade; fade += fadeRate*dt; }      // 0 -> 1 in 10 s (brightness gets the pre-increment value)
else if (fade < 2) { brightness = 1; fade = 2.5; }
helper.Update(dt);                                           // 0x6d6050
sc = camera->GetComp(0x5d); if (!sc) return;
if (SkipIntro || ButtonPressed) {
  if (!skipPromptShown) { holdTimer = 0; skipPromptShown = 1; }
  else if (SkipIntro held) { holdTimer += dt; if (holdTimer > 1.5) GoToCredits(); }
} else holdTimer = 0;
switch (phase) {
case 0: phaseTimer += dt; if (phaseTimer > phaseDur /*0*/) {
          phaseTimer = 0; phaseDur = 1; helper.StartCinema();    // 0x6d60b0: spawns "hud_cinema"
          sc->StartOutroPath();                                  // 0x6d3550
          sc->mode(+0x2e8) = 1; sc->(+0x64) = 5; phase = 1; } break;
case 1: if (sc->Progress() /*clamp(t/25)*/ > 0.3)          // 7.5 s into the fly-by
          if (GO* b = FindByName("corebuilding")) { b->GetComp(0x6e)->Start();
            phaseTimer = 0; phaseDur = 25; phase = 2; } break;
case 2: phaseTimer += dt; if (phaseTimer > 25) { phaseTimer = 0; phaseDur = 5; phase = 3; } break;
case 3: phaseTimer += dt; brightness = 1 - phaseTimer/5;            // fade to black over 5 s
        if (phaseTimer >= 5) { phaseTimer = 0; phaseDur = 2; phase = 4; } break;
case 4: phaseTimer += dt; brightness = 0;
        helper.FadeTexts(1 - clamp(phaseTimer/2, 0, 1));            // 0x6d47f0: Text(0x33) colour.w *= value
        if (phaseTimer >= 2) GoToCredits(); break;
}
```
GoToCredits (`0x724740`): request level "CreditsLevel2" (if not found and name == "Quit" -> quit), helper end, unbind input, destroy
skip_intro_text, reset render globals. CreditsLevel2 (vtable 0x7971e4, `0x710900` PostBaseLevelEnter) unlocks achievement 30
"Made From Scratch", sets "Currently In Credits", reads `data/credits.txt`, loads its level, looks up objects "StartTunnel" /
"EndTunnel" and spawns `HudTextCredits` text objects (credits level not reversed further).

### 7.1 SpinFocusCamera outro path (`0x6d3550` setup, `0x6d14e0` update mode 1) - object `corespincam` in core_demo
Setup: time(+0x3b4) = 0, duration(+0x3b8) = 25; position spline (+0x2ec, built by `0x71ae60`) through
(15.158, 565.451, -77.154), (278.586, 430.220, -14.752), (216.916, 348.226, -0.945), (88.184, 333.840, 114.810), (-4, 371.836, 300),
(-421.500, -72.664, 521.045); a second 2-point spline (+0x350): (0, 595.45, -99), (0, 640.2, 337.34) (not used by mode 1);
focus(+0x3c8) = (-0.133, 650.921, -20.262); shake params as in 1.5.
Mode 1 per frame:
```c
time += dt; t = time / 25;                              // not clamped here
cam.pos = SplineEval(posSpline, t * posSpline.length(+0x30));   // 0x71a710
GO* sp = FindByName("core_death_sparker");
vec3 target = (-0.133, 650.921, 337.262);                // the core eye
if (t > 0.3 && sp && sp.pos.z > -20.262) target = sp.pos;    // follow the climbing sparker
float k = (t < 0.315) ? 0.0001 : 0.1;                   // per-frame smoothing (frame-rate dependent)
focus = focus * (1-k) + target * k;
cam.LookAt(focus);                                       // 0x678310
Shake(dt);                                               // 0x6d0520
```

---------------------------------------------------------------------------------------------------------------------

## 8. Port checklist
1. Level5: CoreLightningLogic bolts between counter-rotating arm tips (sec. 2); TopOfTheCORE achievement; CoreExploder inert.
2. Level6: CoreBallLogic ray-cast bolts (sec. 3); drag zone (sec. 4); EndStory -> swap to corecorecore2 + CoreBallExplosionLogic
   shader params (sec. 6.1) + LevelFinished(success).
3. After the results screen of the last story level: outro scene core_demo.xml (sec. 7): 10 s fade-in, 25 s spline fly-by,
   CoreExploder starts at 7.5 s (sec. 1 timeline), fade out 5 s at ~32.5 s, 2 s black, credits. Skippable by holding SkipIntro 1.5 s.
4. Needed assets: FireRing.msh, splosion.msh, DeadCore*.msh/.mdc (61 + 4 claws + eye + base), spark1..4.msh, corecorecore2.msh +
   its shader (Float1..3 semantics unknown), particle settings of CoreDeathSparker / CoreEyeSparker.
