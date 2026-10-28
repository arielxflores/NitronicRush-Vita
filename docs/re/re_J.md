# RE group J: GunHumvee enemy + HumveeLogic (Nitronic Rush PC exe)

All addresses are VAs. Conventions as in re_F1: `GC(id)` = GetComponent 0x66e730, `owner` = this+4, GO+0xa8 pos, GO+0xfc 3x3 orientation
(row-major, column k = local axis k; **local +Y = forward, local +Z = up**), GO+0x1b4 dirty. Transform component T = GO+0x6c
(T+0x3c local pos, T+0x90 local orientation, T+0x148 dirty), `WorldPos(T)` = 0x677d50. BodyRigid(0x10): +0x74 invMass, +0xd4 angVel,
+0xe0/+0xec linVel (2 copies), +0xf8/+0x104 pos (2 copies), +0x128 world centre of mass, +0x134 kinematic.
`Spawn(name, parent)` = 0x67c8e0 (ecx=[0x7fe804]), `Init(go)` = 0x66ee40, `Find(name)` = 0x67b340, `Post(go,id)` = 0x664630,
`DestroyObject(go)` = 0x67b220 (deferred: pushes the handle to world+0x167c), `DestroyChildren(go)` = 0x66e650.
Working files: `~/nrw/J/` (function dumps `f_*.txt`, `upd_full.txt`, `phaser.txt`, helpers `bp.py`, `lv.py`).

## 0. New shared facts found here

| thing | meaning |
|---|---|
| `0x672af0(ecx=A, edx=B, eax=out)` | returns **B*A** (checked numerically, J/mm.py). So `0x675470(ebx=&M, a)` = `M = RotZ(a) * M` (rotation about the PARENT's Z). |
| `0x671a00(a, eax=out)` | RotX(a) = [1 0 0; 0 c -s; 0 s c] (row-major). RotX(+a) turns local +Y toward +Z (nose up). |
| `0x678db0(esi=T, a)` | `T.orient = RotZ(a)*T.orient; T+0x68 += a; dirty`. |
| `0x638740(eax=body, ecx=&J, edx=&r)` | ApplyImpulse: `linVel += J*invMass; angVel += Iinv_world*(r x J)`. |
| `0x4151c0(ecx=body, eax=&v)` | set both linVel copies. |
| `0x63b750` raycast (used by PhaserLogic) | mask 0xffff (everything, including the car), one excluded collider (ecx), segment start + t*delta with t <= 1; result +0 collider, +4 t, +8 normal, +0x14 hit point. |
| `0x418b80(min,max)` | random vec3, each component uniform in [min,max]. |
| ControllerCar (0x1b) inputs | +0x74 **steer** [-1,1] (setter 0x62ce60), +0x78 **gas** [-1,1], negative = reverse (0x62ce30), +0x7c **brake** [0,1] (0x62ce10). Wheel steer angle = steer * wheel "Steer Factor"; the wheel forward becomes `R*(-sin a, cos a, 0)`, so **positive steer turns left (counter-clockwise about up)** (0x62dfc0). Wheel records: vector at +0x90, 0xc8 bytes each (+0x30 on ground, +0x54 axle vector, +0x9c grip/slip factor (unverified name), +0xc0 wheel spin rate). |
| Collision material **"Default"** | not in CollisionMaterials.xml; it is a built-in entry (0x63c167): restitution 0.3 (+0), friction 0.5 (+4), layer bit0, collides bit0, i.e. it behaves like `Metal`. The struct is {restitution, friction, u16 layer, u16 collides} (parser 0x63c2f8). |
| Wwise | RTPC **CarEngineRPM** = 0x549792de (from Sounds/Nitronic/GeneratedSoundBanks/Wwise_IDs.h; that header has every event/RTPC name). Events used: Play_HumveeSiren 0xfcd349f7, Play_HumveePhaser 0x65489e75, Stop_HumveePhaser 0xe2e7c157, Play_Rand_TireSkid 0xdb03fff1, Stop_Rand_TireSkid 0x026ba453, Play_Rand_Scrape 0xfbe20f9e, Stop_Rand_Scrape 0x6ef291a0, Play_Rand_SmallCrash 0xc135b338, Play_Rand_MediumCrash 0x6d65f5b0 (dead code here), Play_Rand_Humvee_Explosion 0x0ffd5878, Play_Rand_Humvee_Death_Voice 0xacd4ba40. |
| **Correction to re_F1 sec. 7** | GarageLogic does not destroy its old humvees on a checkpoint respawn, but **every HumveeLogic subscribes to PlayerResetAtCheckpointMessage (0x16) and silently deletes itself** (sec. 7 below). So after a respawn only the new pair exists. |

## Where it appears

| source | count | notes |
|---|---|---|
| GarageLogic spawns (re_F1 sec. 7) | 2 per garage, once per checkpoint-reset cycle, when the car is within 340 u | Arena 3 garages, christmas 1, nature 4 (two pairs of garages stacked at the same XY, z 222.7 and 238.5), sky_city 3, vert_park 1. Spawn pose: garage pos +-X*2*s.x + Z*0.25*s.z, orientation (-X,-Y,Z) (facing out of the door). |
| playground_addendum.xml (placed) | 11 | scale 1. Positions (-65.8,374.4,81.6) yaw -45 deg, (-139.8,374.4,81.6) yaw +45, (-65.8,301.4,81.6) yaw -135, (-139.8,301.4,81.6) yaw +140 (yaw = RotZ angle), and 7 at z 73.82 with identity orientation: (74.8,445.2) (106.7,476.9) (74.3,509.8) (42.4,477) (-275.3,398) (125.8,329.8) (-265.9,226.7). HumveeLogic = blueprint defaults (empty element). Override of the first (red) CircularSpinningLightLogic: N 5, R 5, spin -7, Color (.95,.05,.05,1.5), spec .9, amb .01, att 2/5. The second (blue) light ring is unchanged. |
| moto.xml | 23 `GunHumveeTireL` | giant static decoration only (scale 22-40, StaticMesh/RenderMesh). Not related to the enemy. |

No other level places GunHumvee or overrides HumveeLogic.

---------------------------------------------------------------------------------------------------
## 1. Blueprints

### GunHumvee (Objects/GunHumvee.xml)
| component | values |
|---|---|
| Transform | bp pos (0,0,1), scale 1, identity |
| StaticMesh + RenderMesh | GunHumvee.msh, material `humvee`, Color1 (1, .3, .15, 2.0) (Diffuse/Normal/Emit GunHumvee*.tga, Reflect city_ref.tga) |
| ColliderOrientedBox | material **Default** (built-in: restitution .3, friction .5, layer bit0, collides bit0), Size (1.0635, 2.507, 1.1025) (half-extents, inferred from the wheel layout; unverified) |
| BodyRigid | **dynamic** (Is Kinematic 0), Inverse Mass **1** (same as the player Car), Drag (1, .02, .005) (same as Car), Rotational Factor .16 (Car .52), Center Of Mass (0, .125, -0.5) |
| ControllerCar | Driving Restitution -.5, Rolling Restitution .2, Driving Friction .5, Rolling Friction .6, side/front roll coeff .4/.8, Wheel Radius .5, Spring min/max/rest .1/.4/.4, Spring Strength 70, Max Spring Force 15000, damping 1.8/1.8, **Forward Force Factor 12, Reverse Force Factor 3.5** (Car: 20/15). 4 wheels, all drive wheels, all "Can Steer": front (+-0.905, 1.731, -0.738) Steer Factor **.45**, Brake .004 (max .1); rear (+-0.905, -1.481, -0.738) Steer Factor **-0.1**, Brake .016 (max .4); friction static 2.5 / kinetic 2.0 front and side. |
| CarWheelLogic | wheel models GunHumveeTireL, TireR, TireL, TireR (no flip). Tire blueprints: scale .5, mesh only. |
| CollisionInfoCollector | (needed by the contact pass) |
| HumveeLogic | Brake Time .2, Steer Reduction Start Velocity 10, Steer Reduction Factor .2, Minimum Gun Pitch -0.4, Maximum Gun Pitch 0.3, Gun Rotate Speed 1.0, Shots Per Second 3.0 |
| CircularSpinningLightLogic #1 (red) | N 4, R 5.5, Spin -6, Color (1, .05, .05, 1.1), spec .9, amb .01, att 1.5/5 |
| CircularSpinningLightLogic #2 (blue) | N 3, R 2.5, Spin +4, Color (.05, .05, 1, 1.1), spec .9, amb .01, att 1.5/5 |
| Phantom | `Play_CarEngine`, 3D, Begin Playing 1 (engine loop; pitch via RTPC CarEngineRPM, sec. 4.4) |

So the humvee is a full ControllerCar vehicle, driven by HumveeLogic writing the same steer/gas/brake inputs the player's car uses.
Note: AirMineLogic explodes on contact with any object that has a ControllerCar (re_D), so humvees also set off air mines.

### Turret parts (spawned by HumveeLogic::init, visual only, no collider, no body)
* `GunHumveeBase`: GunHumveeBase.msh, `humvee` Color1 (1,.3,.15,2), local pos **(0, -0.445, 0.674)** relative to the humvee.
* `GunHumveeGun`: GunHumveeGun.msh, same material, local pos **(0, 0.168, 0.247)** relative to the base.

### Projectile `Phaser` + `PhaserExplosion`
* `Phaser`: Pill.msh additive, material `splosion`, Color1 (1, .15, .1, 2), scale (0.1, 0.1, 1) (long axis = local Z), **PhaserLogic Speed 200**,
  **EnumLogic Type 3 Time 3** (removal after 3 s, re_F2 sec. 3). **No collider, no body, no light, no sound.**
* `PhaserExplosion`: splosion.msh additive (1,.15,.1,2), scale .25, EnumLogic Type 4 (Time .3, Start (1,.15,.1,2) -> End alpha 0, Speed 10),
  Light (0.8,0.3,0.2, I 2) spec .9 amb .1 att 5/15, Phantom Play_CarEngine with Begin Playing 0 (silent),
  **TV_RadialImpulse R 5, coef (2, 20, 0), Max 15** (unscaled).

### Destruction pieces (all: dynamic BodyRigid invMass **10**, Drag (0.995,0,0), Rotational Factor .2, TimedDeathLogic **3-4 s**, smoke ParticleEmitter, `humvee` material)
| blueprint | collider (material) | bp local pos | bp orientation |
|---|---|---|---|
| DeadGunHumveeBack | OrientedBox (1.037,.113,.522) Metal | (0,-2.135,-0.078) | I |
| DeadGunHumveeFront | OrientedBox (1.049,.696,.132) Metal | (0,1.563,-0.19) | rot X ~ -9.8 deg (rows 1 0 0 / 0 .985 .171 / 0 -.171 .985) |
| DeadGunHumveeTop | OrientedBox (1.012,1.518,.05) Metal | (0,-0.711,0.503) | I |
| DeadGunHumveeSideBack (x2) | OrientedBox (.205,.423,.359) Metal | (+-0.852,-1.588,0.082) | I |
| DeadGunHumveeSideFront (x2) | ConvexMesh DeadGunHumveeSideFront.mdc Metal | (+-0.852,1.084,-0.455) | I |
| DeadGunHumveeSideMid (x2) | OrientedBox (.05,.957,.582) Metal | (+-1,-0.146,-0.16) | I |
| DeadGunHumveeWindow | OrientedBox (.928,.041,.204) Metal | (0,0.81,0.239) | rot X ~ +5.3 deg |
| DeadGunHumveeBase | Cylinder r .697 hh .061 Metal | placed at the live base | |
| DeadGunHumveeGun | ConvexMesh GunHumveeGun.mdc Metal | placed at the live gun | |
| DeadGunHumveeTireL (x4, also used for the right wheels) | Cylinder r 1 hh .6 **CarGibTire**, scale .5 | at the wheel models | |
`ShardShockwave` (re_D/re_F1): JumpShockwave.msh ring (1,.6,.2,1), EnumLogic 4 (0.5 s, Start (1,.5,.2,1.5), Speed 60), Light (.8,.2,.1,3) att 15/19,
**TV_RadialImpulse R 10, coef (10, 100, 0), Max 25**.

---------------------------------------------------------------------------------------------------
## 2. HumveeLogic (type 0x3c, vt 0x79601c, size >= 0xc0)

vtable: 1 0x6c0eb0, 7 0x6c0d10 (tweak UI), 9 **init 0x6c2bf0**, 10 **serialize 0x6c0ec0**, 13/14 empty, 15 **update 0x6c3100**.
ctor 0x408d60 (sets no field defaults), dtor 0x6c1dc0 (unsubscribes). Message handler **0x6c1d80** (PlayerResetAtCheckpointMessage 0x16, subscribed via 0x42b390).

### Fields
| off | name | bp default | notes |
|---|---|---|---|
| +0x34 | Brake Time | 0.2 | s; low-speed wait before forward/reverse may switch |
| +0x38 | Steer Reduction Start Velocity | 10 | u/s forward speed |
| +0x3c | Steer Reduction Factor | 0.2 | |
| +0x40 | Minimum Gun Pitch | -0.4 | rad (-22.9 deg) |
| +0x44 | Maximum Gun Pitch | 0.3 | rad (+17.2 deg) |
| +0x48 | Gun Rotate Speed | 1.0 | rad/s (yaw and pitch) |
| +0x98 | Shots Per Second | 3.0 | |
| +0x4c | steerCmd | 0 | runtime, raw steering wish |
| +0x50 | throttleCmd | 0 | runtime, +1 forward / -1 reverse / 0 coast |
| +0x54 | brakeTimer | 0 | |
| +0x58 / +0x59 | canForward / canReverse | 1 / 1 | |
| +0x5c..64 | spawn body pos | body+0x104 | unused |
| +0x68..88 | identity 3x3 | | unused |
| +0x8c | 0 | | unused |
| +0x90 | scrapePlaying | 0 | |
| +0x91 | skidPlaying | 0 | |
| +0x92 | sirenStarted | 0 | |
| +0x93 | phaserLoopPlaying | **not initialised** | port: 0 |
| +0x94 | gunPitch | 0 | rad |
| +0x9c | shotTimer | 0 | |
| +0xa0 | state | 0 | 0 drive, 1 shoot, 2 flipped |
| +0xa4 | stateTime | 0 | |
| +0xa8/ac/b0 | handle GunHumveeBase | | {index, serial, table} |
| +0xb4/b8/bc | handle GunHumveeGun | | |

Serialize order (all "Float"): Brake Time, Steer Reduction Start Velocity, Steer Reduction Factor, Minimum Gun Pitch, Maximum Gun Pitch, Gun Rotate Speed, Shots Per Second. Version must be 0.

### 2.1 init (0x6c2bf0, arg = "clone/reinit" flag)
```c
steerCmd = throttleCmd = brakeTimer = 0; canForward = canReverse = 1;
pitch = shotTimer = stateTime = 0; state = 0; scrape = skid = siren = 0;
if (!arg) {
  base = Spawn("GunHumveeBase", parent = owner);         // child of the humvee, keeps bp local pos (0,-0.445,0.674)
  gun  = Spawn("GunHumveeGun",  parent = base);          // child of the base, local pos (0,0.168,0.247)
  hBase = base.handle; hGun = gun.handle;                // no explicit Init on them (unverified whether Spawn/parent init does it)
  Subscribe(PlayerResetAtCheckpointMessage, this, 0x6c1d80);
}
```

### 2.2 update (0x6c3100, dt)
```c
if (dt == 0) return;                                    // paused / editor
car = Find("Car");  if (!car) return;                   // literal "Car" (not [0x7ff6d4]); no car -> nothing runs, not even contacts
up_z = owner.orient[2][2];                              // GO+0x11c = world Z of the local up axis
switch (state) {
case 0: /* drive */
  stateTime += dt;
  if (stateTime > 60)  { HumveeDestroy(); return; }     // 60 s lifetime without catching the car
  if (up_z < 0.5)      { state = 2; stateTime = 0; }    // tipped more than 60 deg
  else if (stateTime > 3 && |owner.pos - car.pos|^2 < 625) { state = 1; shotTimer = 0; stateTime = 0; }   // within 25 u
  SteerToCar();  AimTurret(dt);                          // also on the frame of the transition
  break;
case 1: /* shoot burst */
  stateTime += dt;
  if (stateTime > 3) { state = 0; stateTime = 0; }
  SteerToCar();  AimTurret(dt);  Shoot(dt);
  throttleCmd = 0;                                      // coasts while shooting (no brake), still steers
  break;
case 2: /* flipped */
  stateTime += dt;
  if (stateTime > 3) { HumveeDestroy(); return; }       // could not right itself in 3 s -> explodes
  SpinTurret(dt);                                       // 0x6c1660
  if (up_z > 0.7) { state = 0; stateTime = 0; }
  break;                                                // steer/throttle keep their last values
}
if (WorldPos(owner).z <= -70) SilentRemove();           // 0x6c1d50 (fell off the level). The rest still runs this frame (deferred delete).
ApplySteer(dt);      // 0x6c1150
ApplyThrottle(dt);   // 0x6c0f90
ContactPass();       // 0x6c2d70
SkidSound();         // 0x6c1530
EngineRPM();         // 0x6c1450
SirenAndPhaserLoop();// 0x6c12a0
```
Behaviour: it chases the car. After at least 3 s of driving, if it is within 25 u, it stops accelerating for a 3 s burst of fire, then drives
for at least 3 s again. It explodes by itself after 60 s of uninterrupted state 0 (the timer restarts on every burst or flip),
or 3 s after tipping over. There is **no road following, path finding or obstacle avoidance**: it steers straight at the car's origin and
can drive off ledges (removed silently at z <= -70).

---------------------------------------------------------------------------------------------------
## 3. Driving AI

### 3.1 SteerToCar (0x6c1b10, edi = this)
```c
P = WorldPos(owner); C = WorldPos(car); d = C - P;  n = d / |d|;      // 3D direction
F = owner.orient.col1 (forward), U = owner.orient.col2 (up);
s = dot(cross(F, n), U);                       // sin of the bearing, > 0 when the car is to the left (CCW)
if (dot(d, F) >= 0) { steerCmd = 1.3f * s;  throttleCmd = +1; }      // car ahead: drive forward
else                { steerCmd = -5.0f * s; throttleCmd = -1; }      // car behind: reverse with opposite lock
```
### 3.2 ApplySteer (0x6c1150, esi = this): speed-dependent steering with a slew limit
```c
v  = dot(body.linVel(+0xec), owner.orient.col1);                 // signed forward speed
x  = max((v - SteerRedStart) * SteerRedFactor, 0);               // (v-10)*0.2
k  = 1 / (1 + x);                                                // 1 up to 10 u/s, 1/3 at 20, 1/5 at 30, 1/9 at 50
target = steerCmd * k;  maxStep = k * dt * 5;
cur = cc.steer(+0x74);
if      (target - cur >  maxStep) cur += maxStep;
else if (target - cur < -maxStep) cur -= maxStep;
else    cur = target;
cc.steer = clamp(cur, -1, 1);
```
### 3.3 ApplyThrottle (0x6c0f90, esi = this): gas / brake / reverse gating (same idea as a player car)
```c
v_fwd = dot(body.linVel, owner.orient.col1);  t = throttleCmd;
if (t > 0) { if (canForward) { cc.gas = clamp(t,-1,1); cc.brake = 0; canReverse = 0; } else goto BRAKE; }
else       { if (canReverse) { cc.gas = clamp(t,-1,1); cc.brake = 0; canForward = 0; } else goto BRAKE; }
goto SPEED;
BRAKE: cc.gas = 0; cc.brake = min(|t|, 1);            // t = 0 gives brake 0 (pure coasting)
SPEED:
if (|body.linVel|^2 < 36) {                           // slower than 6 u/s
   brakeTimer -= dt;
   if (brakeTimer <= 0) { brakeTimer = 0; canForward = canReverse = 1; }
} else {
   brakeTimer = BrakeTime;                            // 0.2
   if      (v_fwd >  0.1f) { canForward = 1; canReverse = 0; }
   else if (v_fwd < -0.1f) { canReverse = 1; canForward = 0; }
}
```
So to switch from forward to reverse it brakes (brake = 1) until it has been below 6 u/s for 0.2 s, then reverses with full gas -1 (ControllerCar
Reverse Force Factor 3.5 makes reversing weak). The top speed comes from ControllerCar (Forward Force Factor 12 and Drag (1,.02,.005)); it is not
limited in HumveeLogic (exact top speed not measured, unverified).

### 3.4 Flip handling
`up_z = GO.orient[2][2]`. Below 0.5 in state 0 -> state 2. In state 2 HumveeLogic does not try to right the vehicle. It only spins the turret
at 5 rad/s (SpinTurret 0x6c1660: `base.orient = RotZ(5*dt) * base.orient`; base T+0x68 += 5*dt) while the last steer/gas commands stay applied.
If physics brings `up_z` above 0.7 within 3 s, it goes back to state 0. Otherwise HumveeDestroy (full explosion).

### 3.5 SilentRemove (0x6c1d50) / checkpoint reset (0x6c1d80)
```c
CarWheelLogic(0x3d).DestroyWheels();   // 0x690c80: destroys the 4 tire model objects and clears its vector
DestroyChildren(owner);                // 0x66e650: GunHumveeBase -> GunHumveeGun
DestroyObject(owner);                  // CSL destructors (0x6cb3b0) also delete their spawned lights
```
No explosion, no sound. It is used when `z <= -70` and when **PlayerResetAtCheckpointMessage (0x16)** is received. All humvees, including the 11 placed
ones in playground_addendum, vanish on a checkpoint respawn. Garage humvees come back because GarageLogic clears its flag on the same message
(new pair when the car is again within 340 u). Placed ones do not come back (until a level reload).

---------------------------------------------------------------------------------------------------
## 4. Turret, shooting and sounds

### 4.1 Pose hierarchy (each frame)
`world(base) = world(humvee) * [T(0,-0.445,0.674) * Rb]`, `world(gun) = world(base) * [T(0,0.168,0.247) * RotX(pitch)]`.
`Rb` (the base's local orientation) is accumulated: every yaw step does `Rb = RotZ(step) * Rb` (about the humvee's up axis). The gun's local orientation is
**overwritten** each frame with `RotX(pitch)` (0x671a00), so pitch is the only gun DOF. Starting pose: Rb = I, pitch = 0.

### 4.2 AimTurret (0x6c1700, ebx = this) - called in states 0 and 1
```c
if (!base || !gun || !car) return;
L  = Rh^T * (WorldPos(car) - WorldPos(owner));          // car in humvee space (origin of the humvee, not of the base)
f  = Rb.col1;                                           // base forward in humvee space
c  = f.x*L.y - f.y*L.x;                                 // (f x L).z, an un-normalised lateral offset in world units
if (|c| < 0.2f && dot(L, f) > 0) yaw = GunRotateSpeed * c * dt * 5;     // proportional when nearly on target
else                             yaw = (c > 0 ? 1 : -1) * GunRotateSpeed * dt;   // otherwise constant 1 rad/s
Rb = RotZ(yaw) * Rb;                                    // 0x678db0 on the base transform
B  = Rb^T * (L - basePos(0,-0.445,0.674));              // car in base space (uses the new Rb)
g  = RotX(pitch).col1 = (0, cos p, sin p);
c2 = g.y*B.z - g.z*B.y;                                 // (g x B).x, > 0 means the target is above the barrel
if (|c2| < 0.2f && dot(B, g) > 0) pitch = clamp(pitch + GunRotateSpeed*c2*dt*5, MinPitch, MaxPitch);
else if (c2 > 0) pitch = min(pitch + GunRotateSpeed*dt, MaxPitch);
else             pitch = max(pitch - GunRotateSpeed*dt, MinPitch);
gun.localOrient = RotX(pitch); gun.dirty = 1;
```
Yaw is unlimited (full 360 deg). Pitch limits are -0.4..+0.3 rad relative to the humvee body. The aim point is the car origin, with **no lead**.
The proportional zone is 0.2 world units of lateral miss distance, so the turret tracks the car very tightly once it is on target.

### 4.3 Shoot (0x6c2920, this on the stack)
```c
shotTimer += ShotsPerSecond * dt;
if (shotTimer <= 1) return;
shotTimer = 0;                                         // reset, remainder lost -> interval = ceil((1/SPS)/dt) frames (0.35 s at 60 fps)
G = Rh * Rb * RotX(pitch);                             // gun world orientation (0x672af0 twice)
p = Spawn("Phaser", 0);
M = WorldPos(gun) + 1.5f * G.col1;                     // muzzle point, 1.5 u ahead of the gun origin
p.pos = M;  p.orient = [G.col2 | G.col0 | G.col1];     // columns: phaser local Z = barrel forward, X = gun up, Y = gun right
GC(p, PhaserLogic 0x44)->ignore(+0x38) = GC(owner, ColliderOrientedBox 0x12);   // never hits its own humvee
Init(p);
ApplyImpulse(humveeBody, J = -G.col1 (|J| = 1), r = M - body.worldCOM(+0x128));  // recoil: 1 u/s back (invMass 1) + a little torque
```
A 3 s burst at 60 fps gives about 8 shots (first after ~0.35 s). **There is no muzzle flash object and no per-shot sound**: the bolt itself is the glowing
red pill. The phaser sound is the proximity loop of sec. 4.4.

### 4.4 Sound/RTPC helpers
* **SirenAndPhaserLoop 0x6c12a0**: on the first update `Post(owner, Play_HumveeSiren)` once (+0x92). Then every frame, if `|owner.pos - car.pos|^2 < 2500` (50 u)
  and the loop is not playing, `Post(owner, Play_HumveePhaser)`. If the car is beyond 50 u and the loop is playing, `Post(owner, Stop_HumveePhaser)`.
  This is independent of the shoot state.
* **EngineRPM 0x6c1450**: `v = clamp(2*|dot(body.linVel, fwd)|, 0.125, 50); SetRTPC(CarEngineRPM, v*0.02 + 0.1, owner)` (range 0.1025..1.1)
  for the blueprint's `Play_CarEngine` Phantom loop.
* **SkidSound 0x6c1530**: look for the first ControllerCar wheel with `onGround(+0x30) && wheel+0x9c < 0.9999` (sliding). If one is found and the sound is not playing:
  `Post(owner, Play_Rand_TireSkid)`. If none is found and it is playing: `Post(owner, Stop_Rand_TireSkid)`.
* Scrape/crash sounds: see the contact pass (sec. 6).

---------------------------------------------------------------------------------------------------
## 5. PhaserLogic (type 0x44, vt 0x7a2414) - the projectile

Field: +0x34 Speed (Phaser bp 200). +0x38 ignored collider (set by the shooter). init 0x6a6d60 = nothing; serialize 0x6a6d80; **update 0x6a6e00**:
```c
if (dt == 0) return;
P = owner.pos;  D = Speed * dt * owner.orient.col2;           // straight line along local +Z, no gravity, no drag
if (Raycast(P, D, exclude = ignoreCollider, mask 0xffff, &hit)) {   // 0x63b750: anything (road, walls, car, other humvees, debris)
   e = Spawn("PhaserExplosion", 0); Init(e); e.pos = hit.point; dirty;    // orientation/scale = blueprint
   DestroyObject(owner);
} else owner.pos = P + D;                                     // teleport (no body)
// lifetime: EnumLogic type 3, 3 s -> range 600 u
```
**Effect on the player's car**: no heat, no kill and no special message. The only effect is the one-shot TV_RadialImpulse of PhaserExplosion
(R 5, coef (2,20,0), Max 15). Every dynamic body within 5 u of the hit point gets `dv = clamp(2 + 20/d, +-15)` away from the point (impulse = dir*dv*mass,
applied off-centre, so it adds spin; re_D). If the bolt hits the car's collider, d is about the distance from the hit point to the car's centre:
d <= 1.54 gives the full 15 u/s, d = 2 gives 12, d = 3 gives 8.7. So each hit is a sideways shove of 10-15 u/s with spin. A burst can push the car off the road
or into walls, where the normal crash rules (re_A, > 60 against static or kinematic geometry) kill it. The explosion also pushes the humvees and debris.

---------------------------------------------------------------------------------------------------
## 6. Contact pass (0x6c2d70, edi = this)

Collector entry layout as in re_A (+0xc n into this body, +0x18 relVel = v_other - v_self, +0x24 pre-solve linVel, +0x30 pre-solve angVel, +0x3c other collider).
```c
cc = GC(owner,0x1b); col = GC(owner,0x1e); if (!col) return;
bool scraping = false;
for (c : col->contacts) {
  other = c.collider->owner;
  if (other && (m = GC(other, Material 0x1f)) && m->id == 1) { HumveeDestroy(); break; }   // lava / killblock: explode, stop the loop
  d = dot(c.n, c.relVel);                                    // closing speed (x2.33386 = HUD MPH)
  if (d > 30) {                                              // 70 MPH
     ob = c.collider->body(+0x54);
     if (ob && !ob->kinematic) {                             // hit by / hitting a DYNAMIC body (player car, other humvee, gibs, mines)
        if (d > 68) {                                        // 158.7 MPH
           body.linVel = c.preVel; body.angVel = c.preAngVel; // keep the pre-impact momentum for the debris
           if (GC(other, CarLogic 0x20)) Unlock(24 /*"I Know Car Fu"*/);   // 0x6fb620, only the player car has CarLogic
           HumveeDestroy();
        }                                                    // 30 < d <= 68 vs dynamic: nothing at all (no sound)
     } else {                                                // static collider (no body) or kinematic body
        body.linVel = c.preVel; body.angVel = c.preAngVel;
        HumveeDestroy();                                     // crashed into the world at > 30 u/s: no achievement
     }
     continue;
  }
  // d <= 30
  tang = c.relVel - c.n * d;
  if (|tang| / 3 > 2) {                                      // tangential speed > 6 u/s
     scraping = true;
     if (!scrapePlaying) { scrapePlaying = 1; Post(owner, Play_Rand_Scrape); }
  }
  if (d > 15) Post(owner, Play_Rand_SmallCrash);             // (a "d > 30 -> Play_Rand_MediumCrash" branch exists but cannot be reached)
}
if (scrapePlaying && !scraping) { Post(owner, Stop_Rand_Scrape); scrapePlaying = 0; }
```
Multiple qualifying contacts in one frame are safe: HumveeDestroy returns at once when CarWheelLogic's wheel list is already empty.
A humvee is fragile against the world: hitting a wall or landing hard (box contact) at > 30 u/s along the normal destroys it. Normally its suspension
wheels (raycasts) carry it, so ordinary driving does not touch the road with the box.

**Player car vs humvee** (the car's own handler, re_A): the humvee has a dynamic body, so a car contact with it **never kills the car** (any speed, wings or not).
If the closing speed is > 68, the humvee explodes, achievement 24 unlocks and the ShardShockwave then kicks the nearby car (sec. 7: up to 25 u/s within 6.7 u).
If the closing speed is between 30 and 68, both bounce normally and nothing else happens. Ramming is therefore the intended way to kill them.

---------------------------------------------------------------------------------------------------
## 7. HumveeDestroy (0x6c2480, this on the stack)
```c
cc = GC(owner,0x1b); cwl = GC(owner,0x3d);
if (cwl.wheels.empty()) return;                              // already destroyed this frame
P = WorldPos(owner); Rh = owner.orient; hb = GC(owner,BodyRigid);
sw = Spawn("ShardShockwave"); sw.pos = P; Init(sw);          // ring + light + radial impulse R10 (10,100,0) max 25, unscaled, bp orientation
// 10 body pieces, SpawnGib 0x6c2200(name, mirror):
for (name, mirror) in [Back,0 Front,0 Top,0 SideBack,0 SideBack,1 SideFront,0 SideFront,1 SideMid,0 SideMid,1 Window,0]:
   g = Spawn("DeadGunHumvee"+name); lp = g.bpPos; if (mirror) lp.x = -lp.x;    // only the position is mirrored, not the orientation
   g.pos = P + Rh*lp;            // NOT multiplied by the humvee scale (always 1 in data)
   g.orient = Rh * g.bpOrient;  Init(g);
   gb.linVel = hb.linVel + rand3(-5,5);  gb.pos = gb.prevPos = g.pos;  gb.angVel = hb.angVel;
// turret pieces (0x6c1e40), only if both handles are valid:
   DeadGunHumveeBase: pos = WorldPos(base), orient = Rh*Rb;              same velocity rule (hb.linVel + rand3(-5,5), hb.angVel)
   DeadGunHumveeGun : pos = WorldPos(gun),  orient = Rh*Rb*RotX(pitch);  same velocity rule
// tires: one per ControllerCar wheel (4)
for i in wheels:
   t = Spawn("DeadGunHumveeTireL");                           // L mesh for all four
   t.pos = cwl.WheelModelWorldPos(i);                         // 0x690680
   W = cc.WheelOrientation(i);                                // 0x62d620
   t.orient = [W.col1 | W.col2 | W.col0];  Init(t);           // columns permuted
   tb.linVel = hb.linVel (no random);  tb.angVel = hb.angVel + wheel[i].axle(+0x54) * wheel[i].spin(+0xc0);   // keeps rolling
last = last tire spawned (or the shockwave if there are no wheels);
Post(last, Play_Rand_Humvee_Explosion); Post(last, Play_Rand_Humvee_Death_Voice);
cwl.DestroyWheels(); DestroyChildren(owner); DestroyObject(owner);   // lights of both CSLs go with it
```
Shockwave impulse on the car: `dv = clamp(10 + 100/d, +-25)`, i.e. 25 u/s within 6.7 u and 20 u/s at 10 u, away from the humvee centre. It also throws the 16 fresh gibs.
All debris disappears after 3-4 s (TimedDeathLogic). Debris is dynamic (Metal / CarGibTire), so it can never kill the car.

---------------------------------------------------------------------------------------------------
## 8. The two CircularSpinningLightLogic instances (re_F1 sec. 6)
Each one spawns N `CircularSpinningLight` point lights at Init and every frame places light i at
`(H.x + R cos a_i, H.y + R sin a_i, H.z)` with `a_i += Spin*dt` (H = humvee position; **world XY circle, humvee rotation ignored**, same height as the humvee origin).
Blueprint: red ring 4 lights, R 5.5, -6 rad/s (clockwise), colour (1,.05,.05) intensity 1.1, att 1.5/5. Blue ring 3 lights, R 2.5, +4 rad/s, (.05,.05,1) 1.1, att 1.5/5.
Both rings have spec .9 and amb .01. With att far = 5, the red lights mostly light the ground around the humvee and the blue ones light the body. This is the police-light effect.
playground_addendum replaces the red ring with 5 lights, R 5, -7 rad/s, (.95,.05,.05) I 1.5, att 2/5.
Initial phase = uninitialised member at the first build (use 0, spacing 2*PI/N). They have no message handler of their own: on a checkpoint reset the humvee deletes itself
and the CSL destructor (0x6cb3b0 via 0x408590) deletes its lights. The same happens on HumveeDestroy and SilentRemove.

---------------------------------------------------------------------------------------------------
## 9. Port checklist
1. **Blueprint**: dynamic body invMass 1, drag (1,.02,.005), rotFactor .16, COM (0,.125,-.5), oriented box (1.0635,2.507,1.1025) with restitution .3 / friction .5
   ("Default" = Metal-like layer), CollisionInfoCollector, ControllerCar with the humvee's wheel table (sec. 1). Reuse the player car's ControllerCar code;
   +steer = turn left.
2. **Init**: child meshes GunHumveeBase at (0,-0.445,0.674) and GunHumveeGun at (0,0.168,0.247) on the base. Rb = I, pitch = 0, state 0, timers 0,
   canForward = canReverse = 1. Subscribe to the checkpoint-reset message -> silent remove.
3. **Update** (skip if dt == 0 or there is no "Car"): run the state machine of sec. 2.2 with the exact constants (60 s, 3 s, 25 u, up_z 0.5 / 0.7, z <= -70).
4. **Steering**: sec. 3.1-3.3 (1.3*sin forward, -5*sin reverse; k = 1/(1+max(0,(v-10)*0.2)); slew 5*k/s; gas/brake gating with the 6 u/s + 0.2 s rule).
   In state 1 throttle = 0 (coast). In state 2 keep the last inputs.
5. **Turret**: yaw `Rb = RotZ(step)*Rb`, pitch clamp [-0.4, 0.3], speed 1 rad/s, proportional gain 5 inside 0.2 u miss distance (sec. 4.2).
   In state 2 spin the base at 5 rad/s.
6. **Shooting** (state 1 only): timer += 3*dt, fire when > 1 and reset to 0. Spawn the bolt at gunWorldPos + 1.5*barrelFwd, moving at 200 u/s along the barrel,
   life 3 s, ray-swept each frame against everything except its own humvee box. On a hit: PhaserExplosion (0.3 s flash, light, radial impulse R 5 (2,20,0) max 15).
   Recoil impulse 1 on the humvee at the muzzle.
7. **Contacts** (sec. 6): kill material -> destroy. d > 30 vs static/kinematic -> destroy. d > 68 vs dynamic -> destroy (+ achievement 24 if the other has CarLogic).
   Scrape (tangential > 6 u/s) and SmallCrash (d > 15) sounds. The car never dies from touching a humvee.
8. **HumveeDestroy** (sec. 7): ShardShockwave (impulse R 10 (10,100,0) max 25), 10 body gibs (+mirrored ones), dead base and gun, 4 tires with their roll spin,
   inherit velocity (+ random +-5 per axis except the tires), gibs live 3-4 s, two sounds, then remove the humvee, its wheels, children and lights.
   Guard against double calls.
9. **Silent remove**: fell below z = -70, or checkpoint reset. No effects.
10. **Lights**: two spinning rings (sec. 8) following the humvee position in the world XY plane.
11. **Audio**: Play_CarEngine loop + RTPC CarEngineRPM = clamp(2|v_fwd|, .125, 50)*0.02 + 0.1. Play_HumveeSiren once on the first update.
    Play_HumveePhaser / Stop_HumveePhaser loop on a 50 u car distance. TireSkid start/stop. Scrape start/stop, SmallCrash. Explosion and death voice on destroy.
12. Unverified: the exact Size semantics of ColliderOrientedBox (half-extents assumed), the meaning of wheel+0x9c (grip ratio < 0.9999 = skidding), whether Spawn-with-parent
    initialises the child meshes, and the humvee's top speed (comes from ControllerCar). Level-file component matching (which CSL the playground_addendum override hits)
    is inferred from the colours (red one).
