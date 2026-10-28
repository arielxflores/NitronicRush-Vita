# Group D – hazards that attack the player (AirMine, ShardBuilding/Shard, BarrelSpawner/Barrel)

Nitronic Rush PC exe, 32-bit MSVC. Addresses are VAs. All values below were read from the code and the blueprints.

## 0. Shared engine facts (new or confirmed while doing this group)

| thing | meaning |
|---|---|
| `[0x7ff754]+8` | game mode. **0 = playing**, **2 = editor** (BarrelSpawner runs gameplay only when it is ==0 and draws editor gizmos when it is ==2). AirMine/ShardBuilding treat ==2 as "editor: don't simulate". |
| `0x67b340("Car")` | FindObjectByName. Every hazard looks up the player object called **"Car"** every frame. |
| `0x67c8e0(world=[0x7fe804], name, 0)` | SpawnBlueprint(name), which returns a GameObject*. |
| `0x66ee40(ecx=GO, bool)` | GameObject::Initialize(arg): calls slot 9 (init) of every component once. Spawned objects get arg=0. |
| `0x67b220(eax=GO)` / `0x66f880(ecx=GO)` | DestroyObject (deferred: pushes GO+0x18 handle into `world+0x167c`). |
| `0x41cea0(edi=vector, push &handle)` | vector<Handle>::push_back (handles are 12 bytes: index, serial, table). |
| `0x67ab80(esi=world, ebx=&handle)` | resolve handle (world+0x161c table) to GameObject* (or 0 if stale). |
| GameObject | +0x18 handle(12B); +0x6c embedded Transform; +0xa8 position; +0xf0 scale; +0xfc 3x3 orientation (row-major, column k = local axis k); +0x1b4 transform-dirty byte. `0x677d50(ecx=GO+0x6c)` = world position. |
| BodyRigid (id 0x10) | +0x34..3c accumulators; +0x74 inverse mass; +0xd4 angular vel; +0xe0 and +0xec linear vel (two copies, `0x4151c0` sets both); +0xf8 and +0x104 position (both are written on teleport); `0x6388c0(eax=rb, ecx=&quat)` set orientation; `0x661060` mat3 to quat; `0x4010b0(eax=rb)` zero all motion; +0x134 = "Is Kinematic" (confirmed from the BodyRigid serializer); the body's rotation matrix is probably at +0x90. |
| RenderMesh (0x47) | **+0x60..0x63 = per-object tint bytes B,G,R,I** where I = glow intensity x10 (ShardBuilding writes `R*255 → +0x62, G*255 → +0x61, B*255 → +0x60, A*10 → +0x63`). It is the blueprint `Color1` (r,g,b,intensity) at runtime. `0x4052e0` sets it from a vec4. |
| CollisionInfoCollector (0x1e) | contacts vector at +0x40/+0x44, 64-byte entries: +0x0c contact normal, +0x18 relative velocity (normal·relvel = impact speed), +0x24 pre-impact linear vel, +0x30 pre-impact angular vel, +0x3c other collider (collider+4 = other GameObject, collider+0x54 = its body). |
| `0x664630` with `push hash; push [audio+0x14]`, edx = GameObject | **Wwise PostEvent(eventID, gameObject)**. The IDs are FNV-1 hashes of the lower-cased event name. I resolved them with `$HOME/nrw/wwise_hash.txt` (all Play_/Stop_ names found in the game files): 0xe606835b = Play_Rand_Mine_Explosion, 0xef3b7b72 = Play_BuildingDestruction, 0x714b1940 = Play_Rand_ShardImpact, 0x63c2ecd3 = Play_Rand_Barrel_Impact. |
| `0x429540` | xorshift128 RNG that returns a float in [0, 131072) (uint32 · 2^-15). `0x6612a0(lo,hi)` = `lo + fmod(rnd, hi-lo)` (via a CRT import, probably `_CIfmod`). |
| `0x672af0(ecx=A, edx=B, eax=out)` | out = **B·A** (row-major 3x3). |
| `0x60a230` | debug-draw circle (editor gizmos only). |
| Phantom (0x23) | gives the object a 3D Wwise emitter, and plays `Event Name` at spawn if `Begin Playing?=1`. |

**How the car dies (CarLogic contact handler, about 0x7093a6–0x709f7f; KillCar = 0x707db0).** None of the group-D hazards calls KillCar directly. They hurt the car only through physics contact, which CarLogic judges as follows:
* impact = contact normal · relative velocity.
* If the other object has a `Material` component with ID 1, the car is killed at once.
* If the other object has CustomDeathSpeed (0x83), the car is a crash candidate when impact > that speed.
* Otherwise, if the other object has `Material` ID 2 (this is the **spike barrel**), the car is a crash candidate when |impact| > 50, or when a wheel-contact metric (`0x7009c0`, over at most 4 wheels) is < 0.5.
* Otherwise, if the other body is dynamic (not kinematic), the car is never a crash candidate. If the other is static or kinematic (mines, shards, buildings), the car is a crash candidate when impact > 60.
* A crash candidate actually dies only if `dot(contact normal, car body column 2 (body+0x98/+0xa4/+0xb0, which I take to be the up axis)) < 0.7`, so landing on top of something is survivable. The CarLogic group should verify this.

The mine explosion's TV_RadialImpulse can also fling the car.

TV_RadialImpulse (0x43). Fields: 0x34 Radius, 0x38..0x40 "Impulse Coeffients" c0,c1,c2, 0x44 Max Impulse, 0x48 fired flag. For every body with invMass != 0 and distance d < R: `dv = clamp(c0 + c1/d + c2/d², ±Max)`, pushed away from the centre. The impulse = dir·dv·mass, applied at half-way to the body's support point (this adds spin). It fires only once: slot 16 sets the flag.

---
## 1. AirMineParentLogic (type 0x54, vt 0x7a2a54) + AirMineLogic (type 0x53, vt 0x7965cc)

### Blueprints / placement
* `AirMineParent` = Transform (default scale 5) + `AirMineParentLogic` (no fields). It is the only thing placed in levels: L4 12, L5 34, L6 51 instances (97 total). Uniform scale 4.3–10.5 (e.g. L4 10.5/8.78, L5 7.78, L6 4.3–9.76). z from -1.7 to 172. 38 of them have a tilted orientation (some upside-down, e.g. L5 col2.z = -0.97).
* `AirMine` (spawned by the parent, never placed directly, so its parameters are always the blueprint defaults):
  * RenderMesh `mine.msh`, material `mine`, Color1 (1, .384, .176, 6.7)
  * BodyRigid **kinematic** (invMass 0)
  * ColliderTriMesh `mine.mdc`, material **Metal**
  * CollisionInfoCollector
  * Phantom (3D emitter, no event)
  * AirMineLogic: Rotation Amount 3.0, Floating Distance 0.2, Floating Speed 0.5, Attacking Distance 50, Attacking Speed **0.0**
* `MineExplosion`:
  * StaticMesh/RenderMesh `splosion.msh`, additive, material `splosion`, Color (1, .15, .1, 3)
  * EnumLogic Type 4, Time 0.8, Start (1, .15, .1, 5), End alpha 0, Speed 5
  * Light: col (.9, .3, .2, 5), spec .9, amb .2, atten 8/30
  * Phantom `Play_Rand_Mine_Explosion`
  * TV_RadialImpulse R 15, coef (10, 10, 40), Max 100
* Gibs reuse the humvee wreck pieces `DeadGunHumvee{Back,Front,Top,SideBack,SideFront,SideMid,Window}`. Each is a dynamic box: invMass 10, drag .995, Metal, TimedDeathLogic 3–4 s, smoke ParticleEmitter.

### AirMineParentLogic fields
| off | name | note |
|---|---|---|
| 0x34,0x38,0x3c | child mine handle (index, serial, table*) | |

* serialize (slot 10, 0x6a4ea0): empty.
* slot 7 (0x6a5330): editor tweak button "Reset Mine" calls 0x6a51d0, which snaps the mine to the parent.

**init (slot 9, 0x6a53b0 → 0x6a50b0)** (registers itself in the global component list first)
```c
mine = SpawnBlueprint("AirMine");  this->hMine = mine->handle;
mine->orientation = owner->orientation;  mine->pos = WorldPos(owner);  mine->scale = owner->scale;
mine->Initialize(0);
rb = mine->GetComponent(BodyRigid);  rb->SetOrientation(quat(owner->orientation));
rb->pos = rb->prevPos = WorldPos(owner);
```
**update (slot 15, 0x6a4f00)**: does something only when `mode==2` (editor). It then re-copies the parent's orientation, position and scale into the mine and its body every frame, so dragging the parent in the editor drags the mine. **In gameplay it does nothing.** The parent has no respawn logic: once the mine is destroyed it is gone (unless the level is reloaded).

### AirMineLogic fields (serialize 0x68e2f0)
| off | name | default (bp) |
|---|---|---|
| 0x34 | Rotation Amount | 3.0 |
| 0x38 | Floating Distance | 0.2 |
| 0x3c | Floating Speed | 0.5 |
| 0x40 | Attacking Distance | 50 |
| 0x44 | Attacking Speed | 0 |
| 0x48 | phase (runtime) | random |

* slot 7 (0x68e1b0) = tweak-bar UI.
* **init (0x68e370)**: `phase = rnd()` (in [0, 131072), i.e. a random phase that desynchronises the mines).

**update (slot 15, 0x68e860)**
```c
if (mode == 2) { phase = 0; return; }
T = owner->transform; up = column2(owner->orientation); s = owner->scale.x;
// contact test
cic = GetComponent(owner, CollisionInfoCollector);
for (c in cic->contacts) if (c.otherCollider->owner->GetComponent(ControllerCar /*0x1b*/)) { Explode(); break; }
car = FindObjectByName("Car");
if (car && |carPos - minePos|^2 < AttackDist^2) {            // "attack" mode
    rb = BodyRigid;
    if (dist2 != 0) rb->linVel = normalize(carPos-minePos) * AttackSpeed;   // =0 with stock data -> mine just stops bobbing
    // angular velocity is NOT touched: it keeps its last bob value (constant spin)
    phase += dt * 7.0;
    tint: B=12, G=25, R=(sin(3*phase)+1)*0.5*255, I=70 (=7.0)  // fast red pulse
} else {                                                       // idle bob
    phase += dt;
    rb->linVel = up * (FloatSpeed*FloatDist*s * cos(FloatSpeed*phase));     // z(t)=z0+FloatDist*s*sin(FS*t)
    rb->angVel = up * (FloatSpeed*RotAmount * sin(FloatSpeed*phase));       // yaw wobble ±RotAmount rad
    tint: R=255, G=(sin(3*phase)+1)*0.5*0.6*255, B=25, I=30 (=3.0)      // slow orange<->red pulse
}
```
With the stock values the bob amplitude is 0.2·scale (≈1–2 units), the period is 2π/0.5 = 12.6 s, and the yaw swing is ±3 rad. The mine body is kinematic and integrates the velocity, so its position is never re-clamped (it can drift slightly).

**Explode (0x68e5c0, edi = this)**, triggered only by **contact** with an object that has ControllerCar. "Attacking Distance" does not trigger it; that distance only changes the colour and freezes the mine.
```c
p = WorldPos(mine); s = mine->scale;
SpawnGib("DeadGunHumveeBack",0); ...Front,0; ...Top,0; ...SideBack,0; ...SideBack,1; ...SideFront,0; ...SideFront,1;
SpawnGib("DeadGunHumveeSideMid",0); ...SideMid,1; ...Window,0;      // 10 pieces, flag 1 = mirror X
ex = SpawnBlueprint("MineExplosion"); ex->Initialize(0); ex->pos = p; ex->scale = s*3;
ri = ex->TV_RadialImpulse: Radius*=s.x, Coef*=s.x, MaxImpulse*=s.x;   // e.g. s=7.78 -> R 117, coef (78,78,311), max 778
li = ex->Light: Color.intensity*=s.x, AttenNear*=s.x, AttenFar*=s.x;
PostEvent(Play_Rand_Mine_Explosion, ex);
DestroyObject(mine);
```
`SpawnGib(name, mirror)` (0x68e380):
* spawn the blueprint;
* if mirror, negate the blueprint's local x;
* `gib.pos = p + mineOri·(bpLocalPos ∘ s)`;
* `gib.ori = mineOri · bpOri`;
* `gib.scale = s*0.5`;
* Initialize, then set the body position.

The gibs get no velocity of their own; the explosion impulse throws them. TimedDeathLogic removes them after 3–4 s.

**To reproduce in the port**
* Spawn one mine per AirMineParent at level start (copy its transform).
* Each frame:
  * Look for car contact via the mine's contact list. On contact, explode: 10 gib boxes, a flash mesh scaled ×3 plus a light, a radial impulse scaled by the mine scale, the Wwise event, and remove the mine.
  * If the car is within 50 units: freeze the mine (AttackSpeed 0) and pulse red quickly.
  * Otherwise: bob and yaw-wobble with the formulas above, using the per-mine random phase.
* The kill itself comes from the car's crash rules (kinematic Metal body: impact > 60) and/or the impulse throw.
* There is no respawn.

---
## 2. ShardBuildingLogic (type 0x2f, vt 0x795d4c) + ShardLogic (type 0x30, vt 0x795da4)

### Blueprints
* `ShardBuilding`: StaticMesh/RenderMesh `ShardBuilding.msh` (material `shard`), ColliderTriMesh `ShardBuilding.mdc` (material **Building**), CulledStatic, ShardBuildingLogic, Phantom (`Play_BuildingDestruction`, not auto-played). Default scale 10. `ShardBuilding_InRedLevel` is the same except for colours (it is not used in L4–L6).
* `Shard` (6 spawned per building):
  * `Shard.msh` / `Shard.mdc`, material **Obsticle**, BodyRigid kinematic
  * ShardLogic Spin Speed 15
  * Phantom `Play_ShardHitsRoad` (not played by code)
  * Blueprint transform pos (0, 0.6, 7) and ori `[1 0 0; 0 .966 -.259; 0 .259 .966]`. This is the template T: local Z (the long axis) leans 15° inward, and local Y points outward and up.
* `ShardCore` (1 per building): `ShardCore.msh`, kinematic, Obsticle, ShardLogic. It is only tinted; it never moves.
* `ShardTarget`: additive `shard_target.msh` (material `blocker_holo`, col (1, .3, .1, 4.5)), CulledStatic, EnumLogic Type 3 Time 10 (probably timed removal after 10 s). This is the ground marker for where a shard will land.
* `ShardShockwave`:
  * additive `JumpShockwave.msh`, scale 10
  * EnumLogic Type 4: Time .5, Start (1, .5, .2, 1.5) → End alpha 0, Speed 60 (an expanding ring that fades)
  * Light (.8, .2, .1, 3), atten 15/19
  * TV_RadialImpulse R 10, coef (10, 100, 0), Max 25 (not scaled)
* `ShardStatic`: decorative shard with no logic (L5 has a field of them). L4 also has 15 free-standing `Shard` objects. Their ShardLogic stays in state 0, so they are static props.

### Placement
* **L4: 18 buildings**, scale 10, z 115.8, in two rows at x≈1074–1449, y≈5543–6056. 16 of the 18 use the limited targeting area.
  * Shard Speed 300 on 16 of them, 250 on 2.
  * Trigger Distance ≈125–208; Trigger Area Offset ≈(±80..±190, −270..−425, 0).
  * Idle Color (1, .45, .13, 2); Angry Color (1, .15, .13, 1.9).
  * Limited area radius 50–117.
* **L5: 7 buildings**, scale 12.1, z 2.3, all with limited area.
  * Trigger 300; Shard Speed 300 or default 170; Spread 20 or default 10.
  * Idle (.996, .4, 0, 2); limited radius 50–100.
  * Offsets e.g. (150, −515, 0), centre (115, −200, 0).
* The trigger offset and the targeting centre are **world-space** offsets from the building origin. They are not rotated; all buildings have identity orientation anyway.

### ShardBuildingLogic fields (serialize 0x6c7770)
| off | name | bp default |
|---|---|---|
| 0x34 | Angry Color (vec4 rgb,intensity) | (1, .15, .13, 1.9) |
| 0x44 | Shard Speed | 170 |
| 0x48 | Shard Rotation Speed | 3 |
| 0x4c | Trigger Distance | 500 |
| 0x50 | Trigger Area Offset (vec3) | 0 |
| 0x5c | Distance Predict Coef | 1 |
| 0x60 | Target Spread | 10 |
| 0x64 | byte: anger sound posted | |
| 0x68 | 3x3 template orientation (copied from a spawned Shard) | |
| 0x8c + 12·i | handles of Shard i (i = 0..5) | |
| 0xd4 | ShardCore handle | |
| 0xe0 | timer | |
| 0xe4 | Idle Color (vec4) | (1, .45, .13, .8) |
| 0xf4 | state (int) | |
| 0xf8 | Uses Limited Targeting Area (bool) | 0 |
| 0xfc | Limited Targeting Area Center (vec3) | 0 |
| 0x108 | Limited Targeting Area Radius | 250 |

The constructor zero-fills everything, so the defaults are the blueprint values. The destructor (0x6c9130) queues all 7 children for destruction.

**init (slot 9, 0x6c91e0, arg)**
```c
if (!arg) {
  state = 0;
  for i in 0..5: shard[i] = SpawnBlueprint("Shard"); Initialize(0); store handle;
  core = SpawnBlueprint("ShardCore"); Initialize(0); store handle;
  template = lastShard->orientation;            // = blueprint ori T
  Reset();                                      // 0x6c7d90
  register in global list (0x429d00);
}
soundPosted = 0;
```
**Reset (0x6c7d90)**: `state = 1; timer = 0;` then PlaceShards() (below). For the 6 shards and the core: ShardLogic.Reset (0x6c6f60), tint = Idle Color. The building's own tint is also set to Idle Color.

**PlaceShards (0x6c7860)**, the crown on the roof:
```c
O = building ori; S = building scale; P = building pos; M = template; v = (0, 0.6, 7);
R = Rz(60°) = [cos -sin 0; sin cos 0; 0 0 1];
for i in 0..5:
   shard.scale = S;  shard.ori = O·M;  rb.SetOrientation(quat(O·M));
   shard.pos = rb.pos = rb.prevPos = P + O·(v∘S);
   M = R·M;  v = R·v;                       // next shard 60° further round
core: scale = S; rb.ori = quat(O); rb.pos = P + O·(0,0,5·S.z)
```
With S = 10, the shards sit on a ring of radius 6 at height 70 and the core at height 50.

**update (slot 15, 0x6c92e0)**
```c
if (editorSelected(owner)) debug-draw trigger sphere (3 circles, r=TriggerDistance at pos+TriggerAreaOffset) and limited area circle;
if (mode == 2) { if (state==0) state=1; if (state==1) PlaceShards(); return; }   // editor: follow the building
switch (state) {
case 0: state = 1; Reset(); break;
case 1: // idle, waiting for the car
  car = Find("Car"); if (!car) return;
  if (|carPos - (pos + TriggerAreaOffset)|^2 < TriggerDistance^2) {
     state = 2; timer = 0;
     for i in 0..5: sl = shard[i].ShardLogic; sl.Activate() /*0x6c6e90*/; sl.speed = ShardSpeed; sl.rotSpeed = ShardRotationSpeed;
  }
  soundPosted = 0; break;
case 2: // 1-second anger ramp
  timer += dt;
  f = (sqrt(2)*timer - timer*timer) / (sqrt(2) - 1);       // 0 at t=0, peaks 1.207 at t=.707, 1 at t=1
  col = Angry*f + Idle*(1-f)  (all 4 components)
  building tint = col;
  if (!soundPosted) { PostEvent(Play_BuildingDestruction, building); soundPosted = 1; }
  shards[0..5] + core tint = col;
  if (timer >= 1) { state = 3; timer = 0; }
  break;
case 3..8: // fire shard (state-3) every 0.15 s
  timer += dt; if (timer <= 0.15) break; timer = 0;
  if (UsesLimitedArea) {
     c = pos + LimitedCenter;  p = c + (RandRange(-R/2,R/2), RandRange(-R/2,R/2), 0);   // square of side R (the in-circle test never fails)
     fwd = (0,1,0);
  } else {
     d = |carPos - buildingPos|;  k = d * PredictCoef / ShardSpeed;
     p = carPos + carRb.linVel * k;  p.x += RandRange(-Spread,Spread); p.y += RandRange(-Spread,Spread);
     fwd = car column1 (local Y = car forward);
  }
  if (!Raycast(world, from (p.x,p.y,p.z+30), delta (0,0,-60), &hit)) break;   // miss: retry in 0.15 s
  n = hit.normal; P = hit.point;
  side = fwd × n;
  marker = SpawnBlueprint("ShardTarget"); Initialize(0); marker.pos = P; marker.ori = axes(X=side, Y=n×side, Z=n);
  sl = shard[state-3].ShardLogic; sl.target = P; sl.targetNormal = n; sl.Launch() /*state=2*/; sl.hMarker = marker;
  state++;   // after state 8 -> 9: building is spent (no reload)
}
```
Timeline: trigger at t=0 → anger flash until t=1.0 → shards launch at about t = 1.15, 1.30 … 1.90 s. Each building fires exactly once (6 shards).

### ShardLogic fields (serialize 0x6c6ef0)
| off | name | |
|---|---|---|
| 0x34 | state 0 idle / 1 rising / 2 flying / 3 stuck | |
| 0x38 | target point | |
| 0x44 | target normal | |
| 0x50 | rise axis (shard local Y at activation) | |
| 0x5c | timer | |
| 0x60 | speed (from building) | |
| 0x64 | rot speed (from building) | |
| 0x68 | **Spin Speed** | bp 15 |
| 0x6c | ShardTarget handle | |
| 0x78 | byte impact-sound posted | |

* init (0x6c6f40, arg): `if(!arg) state=0; hMarker = 0`.
* Activate (0x6c6e90): `state=1; timer=0; riseAxis = column1(ori)`.
* Launch (0x6c6f10): `state=2`.
* Reset (0x6c6f60): `state=0; timer=0; rb.ZeroMotion(); destroy marker; hMarker=0`.

**update (slot 15, 0x6c7000)** (returns at once if dt == 0)
```c
a = column2(owner->ori);  // long axis
switch (state) {
case 0: soundPosted = 0; break;
case 1: timer += dt;
   if (timer > 1) { rb.ZeroMotion(); rb.angVel = a * SpinSpeed; }
   else { rb.linVel = riseAxis * (owner->scale.z * timer * 2.5);  rb.angVel = a * (SpinSpeed * timer); }
   break;     // slides ~1.25*scale outward/up over 1 s while spinning up
case 2: d = target - pos;  c = a × d;
   if (dot(n,d) > 0 && dot(n,a) < 0) {          // passed below the target surface plane, pointing into it
       rb.ZeroMotion(); state = 3;
       if (!soundPosted) { PostEvent(Play_Rand_ShardImpact, shard); soundPosted = 1; }
       if (marker = resolve(hMarker)) { sw = SpawnBlueprint("ShardShockwave"); Initialize(0); sw.pos = shardPos; sw.ori = marker.ori; Destroy(marker); hMarker = 0; }
       break;
   }
   k = 1; if (|c|^2 > 0) c = normalize(c);
   r = |c_unnormalized|^2/|d|^2  (= sin^2 of angle between a and d);
   if (r < 0.15 && dot(d,a) > 0) k = r * 6.66667;      // ease off when within ~23°
   rb.angVel = c * (rotSpeed * k) + a * SpinSpeed;     // turn nose toward target + spin about own axis
   rb.linVel = a * speed;                              // always flies nose-first, no gravity (kinematic)
   break;
case 3: nothing (stuck in the road; it stays a kinematic obstacle).
}
```
* There is no hit-the-car logic. A flying or stuck shard is a kinematic "Obsticle" body, so the car crashes into it under the generic rule (impact > 60 and contact normal not "from below").
* The shockwave impulse (Δv ≤ 25 within 10 units) nudges nearby bodies.
* Shards are never reused: one launch per building per level load.

**To reproduce in the port**
* Spawn the 6-shard crown plus the core per building (60° steps).
* State machine, all world-space, Z-up:
  * idle: trigger sphere at pos + offset
  * 1 s colour ramp with overshoot, playing BuildingDestruction
  * shards rise and spin for 1 s
  * then one shard every 0.15 s: aim at a raycast ground point (random point in the limited square, or the car position predicted by `dist/speed`, ± spread)
  * fly at constant speed along the nose, steering at RotSpeed rad/s; stick when past the ground plane
  * play ShardImpact and the shockwave, and remove the marker
* Tint is RenderMesh colour·intensity.

---
## 3. BarrelSpawnerLogic (type 0x71, vt 0x796b14) + BarrelLogic (type 0x72, vt 0x796b6c)

### Blueprints / placement
* `barrel_spawner`: RenderMesh `barrel_spawner.msh` (material `stomper2`, col (.5, 1, .1, 2)), ColliderTriMesh `barrel_spawner.mdc` (material Obsticle), BarrelSpawnerLogic (trigger_dist_ 200, delay_between_spawns_ 2, delay_before_first_spawn_ 1, max_barrels_ 10, barrel_rot_vel_ 12).
* `spike_barrel`:
  * RenderMesh `spike_barrel.msh`, material `Crusher`
  * BodyRigid **dynamic**: invMass 0.04 (mass 25), Drag (.995, 0, 0), Rotational Factor .2
  * ColliderCylinder Half Height 3.4, Radius 2.0, physics material **spike** (friction .95, restitution .05)
  * BarrelLogic (no fields)
  * **Material ID=2**
  * Phantom `Play_SpikeBarrel` with Begin Playing=1 (rolling loop starts at spawn)
  * CollisionInfoCollector
* **L5 only, 7 spawners** at x≈37–65, y≈3374–3723, z=44, scale 1. Yaw is about 180° (two at 175°), so barrels roll toward +Y.
  * trigger_dist_ = 400, 425, 375, **−450**, 350, 400, 400
  * delay_between = 1 (2 on two spawners); delay_before_first = 0
  * max 10; rot_vel 20, 30, 40, 20, 20, 20, 20

### BarrelSpawnerLogic fields (serialize 0x6bc5d0)
| off | name |
|---|---|
| 0x34 | trigger_dist_ |
| 0x38 | trigger_dist_² (runtime) |
| 0x3c | delay_between_spawns_ |
| 0x40 | delay_before_first_spawn_ |
| 0x44 | max_barrels_ (int) |
| 0x48 | barrel_rot_vel_ |
| 0x4c | timer |
| 0x50.. | vector<Handle> of live barrels (begin 0x5c, end 0x60, cap 0x64) |

* **init (0x6bc960)**: `trigDist2 = trigger_dist_²; timer = 0;` then register itself.
* **The sign of trigger_dist_ does nothing.** Only its square is ever used (at init and recomputed every editor frame), and the editor circles are drawn with radius trigger_dist_. So −450 behaves exactly like 450; it is a harmless designer typo.

**update (0x6bcc20)**
```c
if (mode == 2) { if selected: debug circles r=trigger_dist_ (grey .8); trigDist2 = trigger_dist_^2; return; }
if (mode != 0) return;
car = Find("Car");
if (car) {
  if (|spawnerPos - carPos|^2 < trigDist2) { if (timer > 0) Spawn(); }
  else timer = -delay_before_first_spawn_;
}
timer += dt;
```
**Spawn (0x6bc9c0)**
```c
if (liveBarrels.size() >= max_barrels_) return;          // timer not reset -> spawns as soon as a slot frees
b = SpawnBlueprint("spike_barrel");
bl = b.BarrelLogic; bl.hSpawner = spawner handle; bl.rotVel = barrel_rot_vel_;
bl.SetPath(spawnerPos, -column1(spawnerOri));            // 0x6bbef0: stores XY origin and normalized XY dir
liveBarrels.push_back(b.handle);
timer = -delay_between_spawns_;
b.pos = spawnerPos;
b.ori = spawnerOri with columns  (c0,c1,c2) -> (-c2, c1, c0)  // 90° about local Y: barrel axis (local Z) = spawner X, lying flat
b tint = spawner tint (bytes copied);
b.Initialize(0);
```
The barrel's axis is horizontal along the spawner's X. Its roll direction is `(a.y, −a.x)`, which equals −spawner Y, i.e. the path direction.

### BarrelLogic fields (no serialized fields)
| off | name |
|---|---|
| 0x34 | spawner handle |
| 0x40 | rotVel |
| 0x44,0x48 | path origin XY |
| 0x4c,0x50 | path dir XY (unit) |
| 0x54 | age |
| 0x58 | 4.0 (steer-flip time) |

* init (0x6bbeb0): `age = 0; flipTime = 4.0`.
* destructor (0x6bc080): if the spawner is still alive, remove this barrel's handle from its list (0x6bc7b0). **max_barrels_ is therefore a cap on barrels alive at the same time.**

**update (0x6bc100)**
```c
if (pos.z < -70) { Destroy(self); return; }                       // despawn when it falls off the world
a = column2(ori) (rolling axis); L = 20 * rotVel;
q = pos.xy + L*(a.y, -a.x);                                        // look-ahead in roll direction
t = dot(q - origin, dir); e = origin + dir*t - pos.xy;           // pure-pursuit point on the spawn line
steer = (|e| > 0.001) ? dot(e/|e|, a.xy) : 0;
if (age > 4.0) steer = -steer;                                     // after 4 s it steers AWAY from the line (barrels veer off)
rb.angVel.z += steer * dt * 30;                                    // yaw correction
if (|rb.angVel| < rotVel) rb.angVel += column2(ori) * (rotVel - |rb.angVel|);   // keep rolling at >= rotVel rad/s
age += dt;
ImpactSound();   // 0x6bbf90: for each contact, if normal·relVel > 30 -> PostEvent(Play_Rand_Barrel_Impact, barrel) (once per frame)
```
* Everything else is real physics: gravity, rolling friction (spike friction .95) and bouncing. The roll speed is ≈ rotVel·radius = 40–80 u/s at rotVel 20–40.
* Barrels do not damage the car in their own code. The car's contact handler special-cases Material ID 2: the car is a crash candidate if |impact| > 50, or if the wheel-contact metric is < 0.5. It then dies if `normal·carUp < 0.7`. (`0x7009c0` is a per-wheel metric I did not decode.) So a hard side or front hit by a barrel kills; driving over one is usually survivable. Ordinary dynamic bodies never kill the car.

**To reproduce in the port**
* A spawner fires while the car is within |trigger_dist_| (3D): the first barrel after `delay_before`, then one every `delay_between`, with at most `max_barrels_` alive.
* Spawn a dynamic cylinder (r 2, half-length 3.4, mass 25, friction .95) at the spawner, axis = spawner X.
* Each frame, set a minimum spin rotVel about the axis, plus pure-pursuit yaw steering (gain 30) back to the spawn line for the first 4 s, and reversed steering after that.
* Destroy it below z = −70. Play the impact sound when normal speed > 30. Loop the Play_SpikeBarrel sound.
* Treat the Material ID 2 car-kill rule as above.
