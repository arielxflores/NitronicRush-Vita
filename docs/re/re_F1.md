# RE group F1: bonus/custom-level logic components (Nitronic Rush PC exe)

All addresses are VAs. `GC(id)` = GetComponent (0x66e730). `owner` = this+4. GO = GameObject.
GO layout (see re_A..E): +0xa8 pos, +0xf0 scale(x,y,z), +0xfc 3x3 orientation row-major (column k = local axis k),
+0x1b4 dirty. BodyRigid(0x10): +0x90 rot matrix (row-major, col2 = +0x98/+0xa4/+0xb0), +0xc4 quat, +0xd4 angVel,
+0xe0/+0xec linVel (2 copies), +0xf8/+0x104 pos (2 copies). Light(0xb): +0x3c..48 Color (w = intensity), +0x4c spec,
+0x50 amb, +0x54 near, +0x58 far. RenderMesh(0x47): +0x55 visible, +0x63 = glow*10 (Color1.w).
`mode` = `[[0x7ff754]+8]` (0 play, 2 editor). `CarName` = `[0x7ff6d4]` = "Car" (or "ReplayCar" during replays).
`Spawn(name)` = 0x67c8e0, `Init(go)` = 0x66ee40, `Find(name)` = 0x67b340, `Post(go, id)` = Wwise PostEvent 0x664630.

## 0. New shared facts found here

| thing | meaning |
|---|---|
| Message **0x16 = `PlayerResetAtCheckpointMessage`** (ctor 0x6ff7e0, vtbl 0x799e30; 0x15 = `PlayerDeathMessage`) | broadcast when the player respawns at a checkpoint. Subscribed (via `0x42xxxx` helpers on the message manager `[0x7fe7b8]`) by DropShipLogic (0x6b4990), GarageLogic (0x6bfe50), AirMineParent (0x6a5320) and 3 others (0x6c7f00, 0x6c1d80, 0x6bc8a0). |
| `0x5991f8` | `_CIacos` (MSVCR90). `0x75a30c` = `rand`. |
| `0x4062a0(x)` | sqrtf (cdecl, result on FPU). |
| Quaternions | stored **(w,x,y,z)**, Hamilton product; `0x661060` mat3->quat, `0x660900` quat->mat3 (standard R, column k = local axis k); `0x6388c0(eax=body, ecx=&q)` sets body orientation. |
| `0x63ba80` physics raycast | layer mask 0xffff (everything, **including the car**), no exclude object. result+4 = fraction t of the segment. |
| `0x677cb0(T,&out)` | transform velocity = (pos(+0x3c) - prevPos(+0x54)) / frameDt (`[[0x7ff774]+0x30]`, double); 0 if |dt|<1e-4. |
| `0x670f50(esi=c0, edx=c1, ecx=c2)` | build 3x3 from three column vectors. |
| "fade" helper pattern (MortarGun 0x6b3f60/0x6b3fd0, DropShip 0x6b4700/0x6b4780) | `alpha` in [0,1]; fade-out: `if(alpha>0){alpha=max(alpha-dt,0); RM.glow(+0x63)=(int)(MaxAlpha*alpha*10); RM.visible=1;} else RM.visible=0`; fade-in: `if(alpha<1){alpha+=dt; RM.glow=(int)(MaxAlpha*alpha*10); RM.visible=1;}` (no clamp at 1, so it can overshoot by one dt). When `dt==0` (paused/editor) both logics show the holo at full glow `MaxAlpha*10`. |
| Level XML format | newer levels (e.g. nature, Gladiator) store only the components that differ from the blueprint. Missing component = blueprint copy with blueprint values. Empty `<StomperLogic />`/`<GarageLogic />` = blueprint defaults. |

Nothing in this group carries a `Material` component or `CustomDeathSpeed`. So the car-death rule for every collider below is the
generic one: **static or kinematic** geometry kills when closing speed `d > 60` and `dot(n, carUp) < 0.7`. Dynamic bodies never kill.

## Level usage summary

| blueprint (logic) | levels (count) | parameters there |
|---|---|---|
| MortarGun (MortarGunLogic) | NitroTunnel 39 | all scale 18.8, Mortar Name MortarMine, Spin 15, MaxAlpha 5. (Trigger/Reset/Power): 18x 200/350/200, 14x 200/350/50, 4x 200/350/10, 1x 200/300/50, 1x 100/200/200, 1x 100/200/500 |
| SlidingBlock (SliderLogic) | Gladiator 22 | Speed 1/1.5/2/2.5/4/5, MaxDist 4/4.2/4.9/8, Initial Offset 0/0.5/1; orientations identity, diag(-1,1,-1) (moves down), and X-rotated ±90 deg (moves along ±Y) |
| mine_on_pole (SwingerLogic) | Gladiator 8, nature 8 | Gladiator: s 2.15, o=I, Speed 2.1, MinCos 0.75 (6) / 0.6 (2), IO 0,0.25,...,1.75. nature: s 2.53, yaw +20 deg, Speed 2-5, MinCos 0.75, IO 0 |
| stomper_can (StomperLogic) | Evil_Carwash 6 (s 3), sky_city 3 (s 78.5, 2x 36.3 upside down), nature 10 (s 11.7, 5 upside down) | all blueprint defaults: Pattern "01230", Wait 1, Warning 0.75, Down 0.3, Up 0.5, MaxDist 1.4 |
| DropShipHolo (DropShipLogic) | NitroTunnel 8 | Block mblock2, Ship drone_hawk2, BlockOffset 0.9, ShipOffset (0,-0.36,3.1), Speed 250, Reset 300, ResetTime 5; (Trigger, Height, Delay): (300,25,2.0) (250,15,1.5) (350,25,2.5) (250,15,1.5) (250,5,1.5) (350,30,2.5) (300,25,2.0) (350,20,2.5) |
| CircularSpinningLightLogic_Test | Calcite_by_Pixelmann 12, christmas 4, credits_level 1, sky_city 6, skyscraper 1, vert_park 6 | N=5 everywhere; R 10..500, Spin -2..2 rad/s, colours/atten per level (table in section 6) |
| (component also in) GunHumvee | playground_addendum 11 placed + garage spawns | 2 instances: (N4, R5.5, spin -6, red (1,.05,.05,1.1), att 1.5/5) and (N3, R2.5, spin 4, blue (.05,.05,1,1.1), att 1.5/5); playground_addendum overrides one: N5 R5 spin -7 (.95,.05,.05,1.5) att 2/5 |
| garage (GarageLogic) | Arena 3, christmas 1, nature 4, sky_city 3, vert_park 1 | all Spawn Distance 340 (default) |
| fishzone (FishZone) + fish (Flocking) | nature 13 zones, 48 fish | InnerRadius 20 (default); zone scales e.g. (84.7,156.5,57.6), (129.7,369,77.2) |
| NitroTunnel (CopyPositionLogic + StaticSawLogic) | NitroTunnel 1 | p (0.27,12193,0), scale (51.16,190.33,51.16), RotSpeed -0.5 about local Y, Object Name "ChaseCam", Axis Flags 2 |

---------------------------------------------------------------------------------------------------
## 1. MortarGunLogic (type 0xa1, vt 0x797b84) + MortarMineLogic (type 0xa2, vt 0x797bdc)

### MortarGunLogic fields (serialize 0x6b3ec0; ctor sets no defaults)
| off | name | bp default |
|---|---|---|
| +0x34 | Mortar Name (std::string) | "MortarMine" |
| +0x50 | Trigger Distance | 200 |
| +0x54 | Reset Distance | 350 |
| +0x58 | Power | 200 |
| +0x5c | Spin | 15 |
| +0x60 | Max Alpha | 5 |
| +0x64 | armed (bool) | runtime |
| +0x68 | alpha (0..1) | runtime |

Blueprint `MortarGun`: RenderMesh `jet_flame.msh` (material jet_flame, Color1 (1,.3,.1,4.5)), CulledStatic. **No collider**: the gun itself is harmless.

```c
init (slot 9, 0x6b3e90): if (!arg) { armed = 1; alpha = 0; }
update (slot 15, 0x6b4080, dt):
  if (dt == 0) { RM.glow = (int)(MaxAlpha*10); RM.visible = 1; return; }
  car = Find(CarName); if (!car) return;
  P = owner.pos;  d2 = |P - car.pos|^2;
  if (d2 > Reset^2)        { armed = 1; FadeOut(dt); }
  else if (!armed)         { FadeOut(dt); }
  else {                    FadeIn(dt);
     if (d2 < Trigger^2) {  armed = 0;
        m = Spawn(MortarName);  m.pos = P;  m.orient = owner.orient;  m.scale = owner.scale * 0.2f;  Init(m);
        b = GC(m, BodyRigid);
        b.linVel = Power * owner.orient.col2;     // gun local +Z
        b.angVel = Spin  * owner.orient.col0;     // gun local +X
  } }
```
So the flame holo fades in (1 s) while the car is inside Reset distance, fires **one** mine when the car comes inside Trigger,
then fades out. It re-arms only after the car has gone beyond Reset distance (for example after a checkpoint respawn further back).
There is no sound at launch.

### MortarMineLogic (serialize 0x6880f0; fields +0x34 Timer Min 0.5, +0x38 Timer Max 1.5; +0x3c t)
Blueprint `MortarMine`: RenderMesh mine.msh (material mine), **dynamic** BodyRigid (invMass 2, Drag (0.5,0.01,0.0025), RotFactor 0.52),
ColliderSphere r 1 material `spike` (layer bit 2, collides with all, friction .95, restitution .05), CollisionInfoCollector, Phantom (3D, no event).
In NitroTunnel the mine scale is 18.8*0.2 = **3.76**. Gravity and drag act on it because it is a normal dynamic body.

```c
init (0x6880e0): t = 0;
update (0x688650, dt):
  if (mode == 2) return;
  t += dt;
  if (GC(owner,0x1e).contacts.size() > 0 && t > TimerMin) { Explode(); return; }   // any contact: road, tunnel, car...
  if (t > TimerMax) { Explode(); return; }
  // blink: overrides Color1 every frame
  RM.R = 255; RM.B = 25; RM.glowByte = 30 /*3.0*/; RM.G = (int)(((sin(3*t)+1)*0.5f*0.6f)*255);
Explode (0x688350):
  P = mine.worldPos;  s = mine.scale.x;
  for i in 1..20: SpawnGib("mineGib%02d")           // 0x688130, see below
  e = Spawn("MineExplosion2"); Init(e); e.pos = P; e.scale = mine.scale * 3;
  if (tv = GC(e,0x43 TV_RadialImpulse)) { tv.Radius *= s; tv.coef[0..2] *= s; tv.MaxImpulse *= s; }
  if (L = GC(e,0xb)) { L.intensity(+0x48) *= s; L.near *= s; L.far *= s; }
  Post(e, 0xe606835b /*Play_Rand_Mine_Explosion*/);
  Destroy(mine);
SpawnGib(name):
  g = Spawn(name); gb = GC(g,BodyRigid);
  g.pos = P + mine.orient * (g.bpPos * mine.scale);   // component-wise scale
  g.orient = mine.orient * g.bpOrient;  g.scale = mine.scale;
  if (pe = GC(g,0xe)) { pe[+0x4c] *= s; pe[+0x50] *= s; }    // particle size params
  Init(g); gb.pos = gb.prevPos = g.pos;               // no velocity: the radial impulse throws them
```
`MineExplosion2`: splosion.msh additive, EnumLogic type 4 (0.8 s fade, then removal), Light (.9,.3,.2,5) spec .9 amb .2 att 8/30,
Phantom `Play_Rand_Mine_Explosion`, **TV_RadialImpulse R 7, coef (10,10,80), Max 50**. With s = 3.76: **R 26.3, coef (37.6, 37.6, 300.8), Max 188**,
light intensity 18.8, att 30/113. Radial impulse rule (re_D): bodies with invMass != 0 within R get `dv = clamp(c0 + c1/d + c2/d^2, ±Max)` away from the centre, once.
`mineGib01..20`: dynamic spheres r .288 (Metal), invMass 20, drag .995, mineGib.msh, smoke ParticleEmitter, TimedDeathLogic 3-4 s.

**How it hurts the car:** the mine and gibs are dynamic, so contact never kills. The danger is the explosion impulse
(for example at 10 units: 37.6 + 3.76 + 3.0 = 44 u/s outward). That kick can throw the car into walls, which then kill it by the normal rule.

**Port:** gun = holo + trigger state machine. Mine = dynamic sphere with the blink colour and timers. Explosion = scaled radial impulse + light + 20 gibs.

---------------------------------------------------------------------------------------------------
## 2. SliderLogic (type 0x94, vt 0x79766c, size 0x50) - SlidingBlock

Fields (serialize 0x6b71c0): +0x34 Speed (1.0), +0x38 Max Distance (2.0), +0x3c Initial Offset (0.0). Runtime: +0x40 base pos, +0x4c state.
Blueprint: RenderMesh block.msh (Color1 (1,.5,.1,2.2)), ColliderOrientedBox size 1 material **Building**, BodyRigid **kinematic**.
```c
init (0x6b7440): base = owner.worldPos; Start();
Start (0x6b7350):
  axis = body.orient.col2;                                    // local +Z
  off  = (1 - |1 - InitialOffset|) * MaxDistance;             // IO 0->0, 0.5->half, 1->top, 1.5->half
  P = base + axis*off;  owner.pos = P (dirty); body.pos = body.prevPos = P;
  if (InitialOffset >= 1) GoDown(); else GoUp();
GoUp   (0x6b72c0): state = 1; body.linVel = +Speed * axis;    // both velocity copies
GoDown (0x6b7230): state = 2; body.linVel = -Speed * axis;
update (0x6b7470, dt):
  if (mode == 2) { if (state) { state = 0; body.pos = owner.pos = base; } else base = body.pos; return; }
  if (dt < 0.0001f) return;
  h = dot(body.pos - base, axis);
  switch (state) { case 0: Start(); break;
                   case 1: if (h >= MaxDistance) GoDown(); break;
                   case 2: if (h <= 0)          GoUp();   break; }
```
This is a triangle wave along the block's local Z, from base to base+MaxDistance (world units, not scaled), at constant speed `Speed` u/s.
Period = 2*MaxDistance/Speed. Turnaround happens on the first frame after crossing an end (no clamp), so it overshoots by up to Speed*dt.
**Car:** kinematic, so contact with closing speed > 60 kills (unless the contact normal is within about 45 deg of car-up). Block speeds are 1-5 u/s, so the danger is
the car's own speed against the block faces. **Port:** kinematic box with a constant velocity that flips at the ends. Base = authored position.

---------------------------------------------------------------------------------------------------
## 3. SwingerLogic (type 0x95, vt 0x7976c4, size 0x54) - mine_on_pole

Fields (serialize 0x6b68a0): +0x34 Speed (2.1), +0x38 Min Cosine (0.75), +0x3c Initial Offset (0.0). Runtime: +0x40 rest quaternion q0 (w,x,y,z), +0x50 state.
Blueprint: RenderMesh mine_on_pole.msh (material mine, Color1 (1,1,1,1.2)), BodyRigid **kinematic**, ColliderTriMesh mine_on_pole.mdc material **Obsticle**.
The body rotates about its own origin (the pivot at the top of the pole).
```c
init (0x6b6ff0): q0 = quat(owner.orient); Start();
Start (0x6b6b80):
  s  = 1 - 2*|1 - InitialOffset|;                 // IO 0 -> -1, 0.5 -> 0, 1 -> +1, 1.5 -> 0, 2 -> -1
  h  = 0.5f * s * acos(MinCosine);                // half angle
  Y  = owner.orient.col1;                         // world-space local Y of the authored orientation
  qr = (cos h, sin h * Y);
  q  = q0 (x) qr;                                 // Hamilton product, q0 first. Note: the axis is in WORLD space but is applied on the
                                                  // local side, so the effective world axis is R0*Y (only equal to Y when R0 is a
                                                  // rotation about Y or identity; nature's 20 deg yaw gives a 40 deg yaw axis).
  owner.orient = mat(q); body.SetOrientation(q);
  if (InitialOffset >= 1) Kick(+1, state=2) else Kick(-1, state=1);
Kick(sign) (0x6b6910 / 0x6b6a50):
  z0 = mat(q0).col2;  zb = body.orient.col2;  Yb = body.orient.col1;
  body.angVel = sign * Speed * max(dot(z0,zb) - MinCosine + 0.005f, 0.0001f) * Yb;
update (0x6b6e50, dt):
  if (mode == 2) { if (state) { state = 0; body.SetOrientation(q0); owner.orient = mat(q0); } else q0 = body.quat(+0xc4); return; }
  if (dt < 0.0001f) return;
  if (state == 0) { Start(); return; }
  z0 = mat(q0).col2;  zb = body.orient.col2;
  body.angVel += -(Speed * dt) * cross(z0, zb);   // restoring torque ~ sin(angle): a true pendulum
```
So it is a frictionless pendulum: angular acceleration = -Speed*sin(theta). Small-angle omega = sqrt(Speed) (Speed 2.1 gives a period of about 4.3 s).
The integrator has no damping for kinematic bodies. MinCosine only sets the start tilt `acos(MinCos)` (41.4 deg for .75, 53.1 deg for .6)
and the start kick. With IO = 0 or 1 it starts at the full tilt with almost no velocity. With IO = 0.5 or 1.5 it starts upright with
`omega = Speed*(1-MinCos+0.005)`. That gives a different (smaller) amplitude than the extreme start, because the kick is not energy-matched.
**Car:** kinematic tri-mesh, generic rule (> 60 closing speed kills). The bob speed is omega*L (L = pole length * scale), so it is mostly the car's speed that matters.
**Port:** store q0, reproduce Start() exactly (same quaternion order), then each frame `angVel += -Speed*dt*cross(z0, zb)` and let the kinematic integrator rotate it.

---------------------------------------------------------------------------------------------------
## 4. StomperLogic (type 0x22, vt 0x795984) - stomper_can + spawned stomper_foot

Fields (serialize 0x6cb9d0): +0x34 Wait Duration (1.0), +0x38 Warning Duration (0.75), +0x3c Down Duration (0.3), +0x40 Up Duration (0.5),
+0x50 Stomp Pattern (std::string, "01230"), +0x7c Max Distance (1.4).
Runtime: +0x44 foot rest pos, +0x6c pattern index, +0x70 phase t (0..1), +0x74 travel fraction, +0x78 current state, +0x80 blink timer,
+0x84 light colour target (rgba), +0x94 smoothed colour, +0xa4 target intensity, +0xa8 smoothed intensity, +0xac foot handle.
Blueprints: `stomper_can` = StaticMesh/RenderMesh stomper_can.msh, ColliderTriMesh stomper_can.mdc **Obsticle**, **no body (static)**.
`stomper_foot` = RenderMesh stomper_foot.msh, BodyRigid **kinematic**, ColliderTriMesh stomper_foot.mdc **Obsticle**,
Phantom `Play_StomperStomp` (3D, not auto-playing), Light (.2,.2,.7,10) spec .9 amb .01 att 2/9.
```c
init (0x6cc7b0): foot = Spawn("stomper_foot"); Init(foot); hFoot = foot; Reset();
Reset (0x6cc480): t = 0; travel = MaxDistance /*placeholder*/; colTarget = colSmooth = (1,1,1,1); idx = 0; Itarget = Ismooth = 1;
                  NextState(); PlaceFoot();
PlaceFoot (0x6cbf60):              // also called every frame in editor mode
  Z = can.orient.col2 * can.scale.z;  rest = can.pos - Z;     // one scaled unit below the can along -localZ
  foot.pos = foot.body.pos = foot.body.prevPos = rest; foot.orient = can.orient (body too); foot.scale = can.scale;
NextState (0x6cba90):
  t = 0; state = Pattern[idx] - '0';
  Z = foot.orient.col2;  D = foot.scale.z * MaxDistance;
  switch (state) {
   case 0 /*wait*/:    colTarget = (0.9,0.9,0.9,1); Itarget = 0; foot.body.Stop();                       break;
   case 1 /*warning*/: blink = 0; foot.body.Stop(); Post(foot, 0x809e5c83 /*Play_StomperAlarm*/);       break;
   case 2 /*down*/:    colTarget = (1,0.1,0.1,1); Itarget = 7;
                       travel = Raycast(rest, -Z*D) ? hitFraction : 1;     // all layers, incl. the car
                       foot.body.linVel = -Z * (travel*D / DownDuration);                           break;
   case 3 /*up*/:      colTarget = (1,0.1,0.1,1);
                       foot.body.linVel = +Z * (travel*D / UpDuration);                              break;
   default: nothing; }
  idx = (idx+1) % Pattern.length();
update (0x6cc110, dt):
  if (mode == 2) { PlaceFoot(); return; }
  if (dt < 0.0001f || !foot) return;
  switch (state) { case 0: t += dt/Wait; break;  case 2: t += dt/Down; break;  case 3: t += dt/Up; break;
    case 1: t += dt/Warning; blink += dt; s = sin(30*blink);
            colTarget = (1,0.1,0.1,1)*(1-s) + (1,1,0.1,1)*s; break; }       // red <-> yellow, s in [-1,1]
  if (t > 1) { EndState(); NextState(); }
  colSmooth = 0.95*colTarget + 0.05*colSmooth;   Ismooth = 0.05*Itarget + 0.95*Ismooth;   // per frame
  footLight.Color = (colSmooth.rgb, Ismooth);
EndState (0x6cbde0):
  if (state == 2) { foot.body.Stop(); Post(foot, 0x724e9b9b /*Play_StomperStomp*/); }
  if (state == 3) { foot.body.pos = foot.body.prevPos = rest; foot.body.Stop(); }  // snap home
// Stop() = 0x4010b0-style zero: linVel, angVel and force accumulators (+0x34..0x3c) = 0
```
Pattern "01230" wraps to "0" again, so the sequence is wait, warn, down, up, wait, wait, warn, ... (period 3.55 s, 2 s pause
between stomps; the very first wait is 1 s). Travel per stomp = scale.z*1.4: Evil_Carwash 4.2 u in 0.3 s = 14 u/s; nature 16.4 u = 54.6 u/s;
sky_city 110 u = **366 u/s** (s 78.5) / 50.8 u = 169 u/s (s 36.3). Upside-down cans (col2 = -Z) have the foot above the can, and it stomps upward.
If the raycast hits something (road, or the car), the foot stops there (fraction t), and the Up phase returns by the same distance.
**Car:** the foot is velocity-driven kinematic, so contact relative velocity includes the foot speed. A foot coming down has a contact normal
opposite to car-up, so `d > 60` kills (sky_city: always; nature: about 55 + car vertical speed; Evil_Carwash: it shoves rather than kills).
The can is static Obsticle geometry (generic > 60 rule).
**Port:** spawn the foot as a kinematic mesh, drive it by velocity per the state machine above, raycast at the start of Down (exclude the foot and the can), snap at the end of Up.
Sounds: Play_StomperAlarm (warning start) and Play_StomperStomp (end of down), both on the foot.

---------------------------------------------------------------------------------------------------
## 5. DropShipLogic (type 0x9f, vt 0x797a5c) - DropShipHolo

Fields (serialize 0x6b47e0): +0x34 Block Name ("mblock2"), +0x50 Ship Name ("drone_hawk2"), +0x6c Block Offset (0.9), +0x70 Ship Offset vec3 (0,-0.36,3.1),
+0x7c Trigger Distance (250), +0x80 Reset Distance (300), +0x84 Reset Time (5), +0x88 Ship Height (15), +0x8c Ship Speed (250),
+0x90 Ship Delay Time (1.5), +0x94 Max Alpha (5).
Runtime: +0x98 block handle, +0xa4 ship handle, +0xb0 holo alpha, +0xb4 state, +0xb8 reset timer, +0xbc decel (Speed/T), +0xc0 brake distance (0.5*Speed*T).
Blueprint DropShipHolo: RenderMesh blocker_holo2.msh (additive holo (1,.3,.1,4.5)) = landing marker, CulledStatic, OneShotRadiusAudioLogic `Play_Woosh` r 25, Phantom.
```c
init (0x6b4a80): handles = 0; alpha = 0; state = 0; subscribe(PlayerResetAtCheckpointMessage 0x16 -> ResetDrop 0x6b4990);
update (0x6b4b00, dt):
  if (dt == 0) { RM.glow = MaxAlpha*10; RM.visible = 1; return; }
  car = Find(CarName); if (!car) return;
  O = owner.pos; R = owner.orient; X,Y,Z = R.col0/1/2; d2 = |O - car.pos|^2;
  switch (state) {
  case 0: FadeOut(dt);
    if (d2 < Trigger^2) {
      T = sqrtf(0.08f * Height);                // time to fall Height at g = 25 (Height = 12.5*T^2)
      decel = Speed / T;  brake = 0.5f*Speed*T;
      back  = brake + (Delay - T)*Speed;        // = Speed*(Delay - T/2)
      ship = Spawn(ShipName); ship.pos = O + R*(Offset.x, Offset.y - back, Offset.z + Height); ship.orient = R; Init(ship);
      ship.body.linVel = Speed * Y;
      blk  = Spawn(BlockName); blk.pos = O + R*(0, -back, Height); blk.orient = R * blk.bpOrient; Init(blk);
      blk.body.linVel = Speed * Y;
      state = 1; }
    break;
  case 1: FadeIn(dt);
    if (dot(O - blk.pos, Y) <= brake) state = 2;            // released from the ship
    break;
  case 2: FadeOut(dt);
    blk.body.linVel -= decel*dt*Y + 25*dt*Z;                // brakes horizontally and falls (local -Z, g = 25)
    h = dot(blk.pos - O, Z) - BlockOffset;
    if (h <= 0) { state = 3; blk.body.linVel = 0; blk.body.pos -= Z*h;   // snap onto the pad
                  sw = Spawn("ShardShockwave"); sw.pos = O; sw.orient = R; Init(sw); }
    break;
  case 3: FadeOut(dt); if (d2 > Reset^2) { state = 4; timer = 0; } break;
  case 4: timer += dt; if (timer > ResetTime) ResetDrop(); break; }
ResetDrop (0x6b4900): state = 0; destroy ship and block (if any).        // also on checkpoint respawn
```
The ship never stops; it keeps flying at Speed until the reset. Timeline after the trigger: the block reaches the brake point at Delay-T, lands at about Delay s,
at O + Z*0.9 with ~0 horizontal speed. Landing vertical speed = 25*T (H 5/15/20/25/30 -> 15.8/27.4/31.6/35.4/38.7 u/s).
Start distance behind the pad: 250*(Delay - T/2) = 238-467 u. Spawned blueprints:
* `drone_hawk2`: drone_hawk.msh, scale 5, **kinematic, no collider** (cannot touch the car), Phantom `Play_Dropship` (3D, starts playing at spawn).
* `mblock2`: mblock.msh (Color1 (1,.1,.05,1.2)), scale (.483,.226,.492), bp orient (0 0 -1 / -1 0 0 / 0 1 0), **kinematic**, ColliderTriMesh mblock.mdc material **Road** (drivable).
* `ShardShockwave`: JumpShockwave.msh additive ring (EnumLogic type 4, 0.5 s), Light (.8,.2,.1,3) att 15/19, **TV_RadialImpulse R 10, coef (10,100,0), Max 25** (unscaled), at the pad.
**Car:** the falling block is kinematic. Hitting it at > 60 closing speed kills (generic rule); being under it adds its fall speed. The landing shockwave
pushes the car away from the pad (up to 25 u/s within 10 u). Once landed, the block is a drivable obstacle until the reset.
**Port:** implement the 5-state machine as written. Use velocity-driven kinematic motion for the block and the ship (per-frame velocity update in state 2), and reset on checkpoint respawn.

---------------------------------------------------------------------------------------------------
## 6. CircularSpinningLightLogic (type 0x28, vt 0x795ae4)

Fields (serialize 0x6cac60): +0x68 Number Of Lights (int), +0x6c Radius That Lights Are On, +0x64 Spin Speed Of Lights (rad/s),
+0x70 Color vec4 (ctor zeroes it), +0x80 Specular Strength, +0x84 Ambient Strength, +0x88 Attenuation Near, +0x8c Attenuation Far.
Runtime: vector<Handle> lights (+0x34, data +0x40), vector<float> angles (+0x4c, data +0x58), +0x90 phase.
Blueprint `CircularSpinningLightLogic_Test` = Transform + this logic only (defaults N 5, R 10, spin 1, Color (1.1,.1,.1,10), spec .9, amb .01, att 2/9).
Spawned per light: `CircularSpinningLight` = Transform + Light (point).
```c
init (0x6cb700): Build(create=1); phase = rnd();        // phase is set AFTER the first build, so it is only used by editor rebuilds
Build(create) (0x6cb490):
  a = phase;  step = 2*PI / N;  C = owner.pos;
  for i < N: L = create ? Spawn("CircularSpinningLight") (Init, push handle) : lights[i];
             L.pos = (C.x + R*cos a, C.y + R*sin a, C.z);          // world XY circle; owner rotation/scale ignored
             Light = GC(L,0xb): Color = this.Color; spec/amb/near/far = this.*;
             angles[i] = a; a += step;
update (0x6cb720, dt):
  if (mode == 2) { Build(0); return; }
  C = owner.pos;
  for i: angles[i] += Spin*dt;  lights[i].pos = (C.x + R*cos angles[i], C.y + R*sin angles[i], C.z);
```
Purely visual: lights only, no collision, no sound. They follow the owner position (used on the GunHumvee as police-style lights).
Initial phase: uninitialised member at the first build (use 0). Per-level values: Calcite 12x (defaults); christmas R110 spin1 (.098,1,.384,1) att50/100,
R250 spin1 (1.1,.1,.1,1) 75/150, R500 spin-1 (.098,1,.325,1) 150/300, R500 spin.5 (1.1,.1,.1,1.25) 150/300; credits R100 spin.5 (.808,.455,.455,2) 10/60;
sky_city R110 (.314,.698,.784,3) 25/100, R110 (.894,.29,.29,5) 20/110, R140 (.298,.8,.639,9) 15/130, R140 spec 3 (.843,.788,.349,2) 10/100,
R40 (.11,.667,.388,4) 3/35, R100 spin.5 (.643,.329,.769,3) 12/90; skyscraper R120 (.957,.776,.463,1) 40/100;
vert_park R120 (.098,1,.671,1) 35/100, R85 spin-2 (.2,.898,.408,1) 30/60, R85 spin-2 (.875,.608,.882,1) 30/60, R85 spin1 (1,.506,.098,1) 30/60,
R85 spin2 (.808,.737,.29,1) 30/60, R90 (.851,.145,.263,1) 35/90 (spin 1 and spec .9, amb .01 unless stated).

---------------------------------------------------------------------------------------------------
## 7. GarageLogic (type 0x46, vt 0x79622c)

Fields (serialize 0x6bff50): +0x34 Spawn Distance (340). Runtime +0x3c spawned flag.
Blueprint `garage`: StaticMesh/RenderMesh garage.msh (lbuild6, Color1 (1,1,1,1.1)), ColliderTriMesh garage.mdc material **Building**, **static** (no body), CulledStatic, scale 3.
```c
init (0x6bfff0): spawned = 0; subscribe(PlayerResetAtCheckpointMessage -> 0x6bfe50: spawned = 0);
update (0x6c0040, dt):
  if (dt == 0 || spawned) return;
  car = Find("Car"); if (!car) return;
  G = owner.pos; X = owner.orient.col0, Y = col1, Z = col2; s = owner.scale;
  if (|car.pos - G|^2 < SpawnDistance^2) {
    O = mat3(-X, -Y, Z);                                     // garage turned 180 deg about its Z (faces out of the door)
    h1 = Spawn("GunHumvee"); h1.pos = G + X*(s.x*2) + Z*(s.z*0.25f); h1.orient = O; Init(h1);
    h2 = Spawn("GunHumvee"); h2.pos = G - X*(s.x*2) + Z*(s.z*0.25f); h2.orient = O; Init(h2);
    spawned = 2; }
```
Spawns two `GunHumvee` enemies (with s=3: ±6 u sideways, +0.75 u up) once. After a checkpoint respawn it can spawn **another** pair
(the old ones are not destroyed). The humvees run `HumveeLogic` (type 0x3c, vt 0x79601c, update 0x6c3100: spawns GunHumveeBase/GunHumveeGun,
drives and shoots, Shots Per Second 3). **HumveeLogic is not covered here; it needs its own group.** The garage itself hurts only as static Building geometry (> 60 rule).

---------------------------------------------------------------------------------------------------
## 8. FishZone (type 0xa3, vt 0x7b118c) + Flocking (type 0x9e, vt 0x7b112c) - nature only, decorative

FishZone: field +0x34 InnerRadius (20). Blueprint `fishzone` = Transform (scale (25,100,25)) + FishZone; no mesh, no collider.
All zones register in a global vector (`[0x801a6c..0x801a70]`). Globals: `curZone = [0x7ff79c]`, `zoneChanged = [0x7f47f0]`.
```c
FishZone::update (0x5efd20): if (mode == 2) { debug draw; return; }
  if (zones[0] != this) return;                       // only the first zone runs the manager
  car = Find("Car"); prev = curZone;
  if (!car) { zoneChanged = 0; return; }
  curZone = index of the zone whose origin is nearest to the car; zoneChanged = (curZone != prev);
RandomPointInZone(z) (0x5efa70):
  r = rnd in [min(Inner, z.scale.x), max(Inner, z.scale.x)];  th = rnd in [-PI, PI);  h = rnd in [-z.scale.y, z.scale.y];
  return z.pos + z.col0*(r*cos th) + z.col2*(r*sin th) + z.col1*h;   // annulus around the zone's local Y axis
```
Flocking (no serialized fields). Blueprint `fish`: RenderMesh fish2.msh (fish2 shader, Color1/2 (.9,1,.6,3), Float2 .8) + Flocking + Light
(.8,.56,.2,1, att 3/10). **No collider or body**, so it cannot affect the car. Tunables (globals, editor names): MaxNeighborDist 19,
IdealNeighborDist 7.7, MaxMoveAcc 150, MoveApartMult 0.5, DefaultSpeed 300, AvoidAcc 600, MaxAcc 60.
```c
init (0x5f11f0): register transform in the global boid list; target = one of 10 hard-coded legacy points (rand()%10); PickTarget();
PickTarget (0x5f10f0): target = curZone valid ? RandomPointInZone(zones[curZone]) : car.pos + 3*car.velocity;
update (0x5f1470, dt): only if mode == 0
  neighbours = other fish within 19 u (half-space filter on local Y; 0x5f0cc0)
  if (zoneChanged) { PickTarget(); pos = target; return; }     // all fish teleport into the new zone
  if (|pos - target| < 6) PickTarget();
  for each neighbour n: d = n.pos - pos (random dir if |d|^2 < .001); f = 7.7^2/|d|^2 - 1;
       if (f >= 0) sep  += -(0.5*f*150) * d/|d|;   else coh += -(f*150) * d/|d|;   align += n.vel;
  if (N) align = (align/N - vel) * 5.5;
  seek = (normalize(target-pos)*300 - vel) * 0.05;
  avoid: raycast ahead 2*(dir*19 + vel*dt); on hit avoid = steer away * 600 * (19-hitDist)/19 (with +-0.4 random jitter), else avoid *= 0.75;
  acc = prioritized sum of [avoid, sep, coh, align, seek], truncated where |sum| reaches MaxAcc 60;
  vel = (vel + acc*dt) * 0.98;  pos += vel*dt;  orientation/banking follows vel; mesh Float params animate the swim.
```
**Port:** optional eye candy. A simple boids model with these constants, zone-nearest-to-car targeting and the teleport on zone change is enough.

---------------------------------------------------------------------------------------------------
## 9. CopyPositionLogic (type 0xa0, vt 0x797ab4) - NitroTunnel blueprint

Fields (serialize 0x688770): +0x34 Object Name (std::string), +0x50 Axis Flags (int: bit0 X, bit1 Y, bit2 Z).
```c
update (0x6887b0, dt): if (dt == 0) return;
  src = Find(ObjectName); if (!src) return;
  P = owner.pos; if (flags&1) P.x = src.pos.x; if (flags&2) P.y = src.pos.y; if (flags&4) P.z = src.pos.z;
  if (b = GC(owner,BodyRigid)) b.pos = b.prevPos = P;     // teleport, linVel stays 0
  else { owner.pos = P; dirty; }
```
`NitroTunnel` blueprint: RenderMesh NitroTunnel.msh (tronics), **kinematic** BodyRigid, ColliderTriMesh NitroTunnel.mdc material **Road**,
RoadData endpoints local y ±8 (±1523 u at scale y 190.3), StaticSawLogic RotSpeed **-0.5** about local Y (constant angVel), CopyPositionLogic
Object Name **"ChaseCam"** (SingleName of the ChaseCamera blueprint), Axis Flags **2**. Result: an endless tube that slides along Y with the camera
(teleported, so it adds no linear contact velocity) and spins at 0.5 rad/s about its axis. The surface moves at 0.5*r u/s (r ~ 51 u gives ~26 u/s),
and that tangential velocity is in the car's contact relative velocity. Being kinematic Road, the generic > 60 rule applies to impacts.
**Port:** each frame set tunnel.y = chaseCamera.y, and keep angVel = -0.5 * localY.
