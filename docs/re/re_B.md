# Nitronic Rush RE, group B: things that move

All addresses are VAs in NitronicRush.exe. "this" = the component, `owner` = this+4 (GameObject).
Conventions used below:
- GameObject: `+0xa8` position, `+0xf0` scale (vec3), `+0xfc` 3x3 orientation (row-major; column k = local axis k in world),
  `+0xcc` accumulated euler-ish vector, `+0x1b4` transform-dirty byte. `owner+0x6c` is the Transform sub-object
  (`0x677d50(T)` returns &position; `0x677ea0` gives the world orientation).
- **World is Z-up** (level objects are rotated about Z; drones/blimp steer in the XY plane).
- BodyRigid (comp id 0x10): `+0x90` 3x3 orientation (built from the quaternion), `+0xb4/+0xc4` saved/current quaternion,
  `+0xd4` angular velocity, `+0xe0/+0xec` saved/current linear velocity, `+0xf8/+0x104` saved/current position,
  `+0x134` Is Kinematic (bool).
- Global mode `[[0x7ff754]+8]`: 0 = playing, 2 = editor. 1 is probably paused.
- Component ids (from the CID table and `typeids.txt`): 0xb Light, 0x10 BodyRigid, 0x20 CarLogic, 0x47 RenderMesh,
  0x2d StaticSawLogic, 0x2e BackAndForthSawLogic, 0x32 FakeSpotLightLogic, 0x52 SweepLaserLogic, 0x76 SearchlightLogic.
- Helpers: `0x67c8e0(name, parent)` on `[0x7fe804]` spawns a blueprint (parent=0 means a world object). `0x66ee40(0)` initializes
  the new object. Handles are 12 bytes `{index, generation, pool*}`; `0x67b340("Car")` finds an object by name.
  `0x418b80(lo,hi)` is a random vec3 with each component in [lo,hi); the RNG is xorshift128 `0x429540`.
- The active camera GameObject handle is at `[0x7ff6c4]+0x5577fc` (written when a Camera renders). The projection is
  `D3DXMatrixPerspectiveFovRH`, so the camera looks along its local −Z, and **camera column 2 points backward**.
- Vtable slots 16/17 on these classes form a "reset to authored state" / "(re)start" pair, probably editor play/stop or a level
  restart. For the port, call reset+start on level (re)start.
- RenderMesh colour: bytes `+0x60`=B, `+0x61`=G, `+0x62`=R, `+0x63`=A. The code writes `R,G,B = c*255` and **`A = c.w*10`**,
  so the "alpha" byte stores an HDR intensity × 10.
- CarLogic `+0x5c` = heat and `+0x34` = max heat (= 50.0, set in CarLogic init 0x7087f0). The car overheats when heat > max.

## Physics integration of kinematic bodies (confirmed)
Integrator `0x639560(dt)` (eax = BodyRigid):
```
pos(+0x104) += linVel(+0xec) * dt;
q(+0xc4)    += 0.5*dt * quat(angVel(+0xd4), 0) * q;  normalize (0x660d00); rebuild matrix +0x90 (0x639120)
if (!IsKinematic) { forces, drag, angVel *= 0.995 ... }   // all skipped for kinematic bodies
owner+0xcc += angVel*dt                                      // accumulator only
```
So a kinematic body moves at a constant linear/angular velocity, with **no damping**. Logic code either sets velocities
(saws' spin, drones, blimp, sweep arch) or writes the position directly (back-and-forth saw, train cars).

---------------------------------------------------------------------------------------------------
## 1. StaticSawLogic (vt 0x795c44, size 0x60, ctor 0x40d9b0)
Fields: `RotSpeed` f32 @0x58, `RotAxis` int @0x5c (serialize 0x691ae0). The constructor sets no defaults.
Other members: 0x34..0x54 = saved orientation (3x3).
```
init (slot 9, 0x691d00):   saved = owner.orient;  if (mode!=2) body.angVel = RotSpeed * owner.orient.col(RotAxis);
update (15, 0x691b20):     only if mode==2 (editor): if (RotAxis<0) RotAxis=2; else if (RotAxis>2) RotAxis=0;  // wrap
reset (16, 0x691b50):      owner.T.orient = saved; dirty; body.angVel = 0
start (17, 0x691c60):      saved = owner.orient; body.angVel = RotSpeed * owner.orient.col(RotAxis)
```
There is no other runtime behaviour: the kinematic integrator spins the body at RotSpeed rad/s about the chosen local axis.
Blueprint `saw`: saw.msh / saw.mdc collider (material "Obsticle"), BodyRigid kinematic, RotSpeed −10, RotAxis 0, CustomDeathSpeed
"45.0f". The same component also drives `fan2` (L3/L4: RotSpeed 1, RotAxis 1) and **SpinningLaser** (RotSpeed −1, RotAxis 0).
To reproduce in the port: angVel = RotSpeed·axis_world, integrate the rotation each step. RotSpeed is in rad/s.

## 2. BackAndForthSawLogic (vt 0x795c9c, ctor 0x4086d0)
Fields (serialize 0x691a70): `RotSpeed` f32 @0x70, `MoveExtents` f32 @0x74, `MoveSpeedMultiplier` f32 @0x78, `RotAxis` int @0x7c.
The constructor sets no defaults. Blueprint defaults: −100 / 100 / 1 / 0.
Members: 0x34 basePos, 0x40 moveAxis (= body orient column 1 = **local Y**), 0x4c saved orient (9 f), 0x80 t, 0x84..0x8c handle of the
"saw_path" object.
```
init (9, 0x692230):
  basePos = owner.pos; moveAxis = body.orient.col1; saved = owner.orient;
  if (mode!=2) body.angVel = RotSpeed * owner.orient.col(RotAxis);
  t = 0;
  p = spawn("saw_path", parent=0); init(p); handle = p;
  p.pos = owner.pos;  p.orient = owner.orient;
  p.scale = (owner.scale.x*0.125, owner.scale.y*1.25 + MoveExtents, 0.01);
update (15, 0x691ef0):
  if (mode==2) { re-read basePos/moveAxis from the object and refresh saw_path pos/scale/orient as in init; return; }
  s = sin(MoveSpeedMultiplier * t);            // x87 fsin, t in seconds
  body.angVel = (RotSpeed * s) * body.orient.col(RotAxis);   // spin reverses with the direction of travel
  P = basePos + moveAxis * (s * MoveExtents);
  body.pos(+0x104) = body.savedPos(+0xf8) = P;               // position written directly, linVel stays 0
  t += dt;
reset (16, 0x691db0): owner.orient = saved; angVel = 0; body orientation = saved; t = 0; moveAxis = saved.col1;
                      body pos = owner pos = basePos
start (17, 0x691bc0): saved = owner.orient; angVel = RotSpeed*col(RotAxis); t = 0
destroy (19, 0x6921d0): delete the saw_path object
```
- **Path**: a straight line along the saw's local Y axis, centred on the authored position, amplitude ±MoveExtents in world units
  (not scaled).
- **Motion curve**: pure sine, `pos = base + Y*ME*sin(MSM*t)`, period 2π/MSM s. **Phase**: t=0 at level start or reset, so every
  saw starts at the centre and moves toward +localY. Saws with the same MSM stay in sync.
- **Spin**: the angular velocity is `RotSpeed*sin(...)`, which is 0 at the centre and ±RotSpeed at the ends, and reverses each half
  cycle. At t=0 the init value (RotSpeed, not ×sin) holds for one frame only.
- Level values: L3 ME≈8.7–10, MSM 1–2.5, RotSpeed −100. L4 ME 10, MSM 1.5–2.5.
- Blueprints: `backandforth_saw` (saw.msh, collider saw.mdc with material "Obsticle", kinematic BodyRigid, Phantom "Play_Saw" 3D
  looping, red point Light 7/20, CustomDeathSpeed 45). `saw_path` (saw_path.msh, additive "blocker_holo", colour
  (1,0.3,0.1,4.5), DrawMode 2): a glowing track decal. Its unit mesh is stretched to span the travel range.
- Port: compute the position analytically each frame and set the kinematic body's position. Use the velocity
  `Y*ME*MSM*cos(MSM t)` for collision response if needed.

## 3. DroneLogic (vt 0x797564, size 0x54, ctor 0x410c60)
Fields (serialize 0x6b7770): `speed` f32 @0x34, `radius` f32 @0x44. No constructor defaults; blueprint defaults 2.0 / 20.0.
Level values: speed 5–8, radius 55–130.
Members: 0x38 basePos; 0x48 a random unit vec3 (normalize(rand[0,1]^3)) set in init and **never used**.
```
init (9, 0x6b77f0): basePos = owner.pos; rnd = normalize(random vec3 in [0,1)); 
update (15, 0x6b7850): only when mode==0 (in the editor, mode==2, it draws 3 debug circles of `radius` around the current pos)
  cam = active camera object; if (!cam) return;
  Zc  = cam.orient.col2;                         // camera BACK vector (RH camera)
  d.x = basePos.x - Zc.x*radius - owner.pos.x;
  d.y = basePos.y - Zc.y*radius - owner.pos.y;  d.z = 0;  d = normalize(d);
  turn = 1.0f;
  if (dot(owner.col0, d) > 0)  turn = -1.0f;      // goal is to the drone's right: turn clockwise
  dy = dot(owner.col1, d);
  if (dy > 0.8f) turn = (1 - dy) * turn * 5.0f;  // ease the turn when almost facing the goal
  body.linVel = body.savedLinVel = speed * owner.col1;   // fly forward along local +Y
  body.angVel = (0, 0, turn);                            // yaw about world Z at up to 1 rad/s
reset (16): owner pos = basePos.   start (17): basePos = owner pos.
```
- The goal point is `basePos + radius * cameraForward_xy`, i.e. `radius` units beyond the drone's home on the far side from the
  camera. "radius" is that offset, not an orbit radius. The drone flies at constant speed with a capped turn rate of 1 rad/s, so
  it loops and circles around the goal. Its turning circle radius is speed/1, and the circles follow the camera. Height
  (Z) never changes, there is **no bobbing**, and it always faces its velocity (local +Y).
- The rotation is only about world Z, so authored orientations must be yaw-only. All story levels satisfy this.
- Blueprints: drone_dome / drone_core / drone_burger / drone_hawk / drone_fly / drone_hawk2. Each has a RenderMesh `<name>.msh`,
  kinematic BodyRigid and Phantom "Play_Drone" (3D, looping). There is no collider in drone_hawk, so drones are scenery.
- Port: each frame compute turn and set the velocities as above, then integrate (pos += v dt, yaw += turn dt).

## 4. BlimpLogic (vt 0x796dac, ctor 0x40fe90)
Fields (serialize 0x6baad0): `speed_` @0x34, `rot_speed_` @0x38, `target_dist_` @0x3c. Blueprint values 120 / 0.65 / 500,
used once in L4.
```
init (9, 0x6bad90):
  k = (|scale.x|+|scale.y|+|scale.z|) / 3;
  for i in 0..3: o = spawn("searchlight"+('1'+i), parent=owner); L = o.Light(0xb);
                 L.attNear(+0x54) *= k; L.attFar(+0x58) *= k; init(o);
update (15, 0x6bab40):  (no mode check: it also runs in the editor)
  cam = active camera; if (!cam) return;
  g = cam.pos - target_dist_ * cam.col2.xy;     // a point 500 units in FRONT of the camera (XY only)
  d = normalize((g - owner.pos).xy, z=0);
  turn = 1; if (dot(owner.col0,d) > 0) turn = -1; dy = dot(owner.col1,d); if (dy > 0.8) turn = (1-dy)*turn*5;
  body.linVel = body.savedLinVel = speed_ * owner.col1;
  body.angVel = (0, 0, rot_speed_ * turn);
```
This is the same steering law as the drone, chasing a point 500 units ahead of the camera, so the blimp keeps cruising into
the player's view at 120 units/s. Its Z is constant. It has no reset/start slots.
Blueprint `blimp`: blimp.msh, ColliderTriMesh blimp.mdc ("Building"), kinematic body, Phantom "Play_Blimp", scale 3.
Children searchlight1..4 (local offsets about (−4.3, 11.3/2.3/…, −12.8), scale 17, searchlight.msh, spot Light 50/150) each run
**SearchlightLogic** (vt 0x796d54; fields max_angle_ @0x58 = 50°, interpolation_time_ @0x74 = 0.15, rest_time_ @0x78 = 0.5):
- **Rest state:** keeps its aim. When t > rest_time it picks a new target:
  - a = rand[0,max_angle]°, b = rand[0,2π);
  - local target = (sin a·sin b, −sin a·cos b, cos a), a cone of half-angle a around saved local +Z;
  - world target = savedOrient·local;
  - `from = previous target`, and t restarts at rand[−0.2·interp, 0].
- **Move state:** the aim direction is `normalize(lerp(from,to,t/interp))`. When t > interp it goes back to rest with t = rand[−0.2·rest, 0].
- **Aim function** (0x6bb390): orients the cone along that direction (in parent space) and rolls it to face the camera.
  The exact column mapping is not fully decoded.
- **Init** (0x6bb070): from = to = −savedOrient.col2.

## 5. TrainLogic (vt 0x7a2d9c, ctors 0x71e9b0/0x71f6b0)
Fields (serialize 0x7208a0):

| name | offset | blueprint | L3 instance |
|---|---|---|---|
| Speed | 0x34 | 10 | 11.75 |
| Start Distance | 0x38 | 1 | 1 |
| Engine Length | 0x3c | 7 | 7 |
| MidCar Length | 0x40 | 3.5 | 3.5 |
| Caboose Length | 0x44 | 4 | 4 |
| Track Lead | 0x48 | 20 | 20 |
| Train Color (vec4) | 0x4c | (0.9,1,0.6,3) | (0.518,0.471,0.227,4) |
| Track Color (vec4) | 0x5c | (1,0.3,0.1,0.5) | (0.698,0.255,0.110,0.2) |
| Mid Car Count | = cars.size()−2 | 4 | 18 |
| Track Piece Count | = pieces.size() | 25 | 56 |
| BezierPath | 0x70 (object) | – | 33 points, closed (first==last) |

Other members: 0x6c `pieceLen` = 2.0 (hard-coded in init); 0xd4 = lastPointIndex; 0xd8 = cached last point; 0xe4 vector<handle> cars
(begin @0xf0); 0xfc vector<handle> track pieces (begin @0x108); **0x114 dist** (distance of the engine's front along the path);
0x118 trackHead (path distance of the front-most track piece); 0x11c index of the head piece. The L3 object has uniform scale
s = 23.44, and every length is multiplied by `s = owner.scale.x` except where noted.

### BezierPath (BezierPath.cpp)
Object at this+0x70:
- +0x00 vector<(param f32, dist f32)> arc-length table (begin @+0xc)
- +0x18 vector<vec3> samples
- +0x30 total length L
- +0x34 vector<vec3> points P (begin @+0x40)
- +0x4c vector<vec3> handles H (begin @+0x58)

Functions:
- `push_back(p)` (0x71b260):
  - P.push(p);
  - if n>1: H.push(P[n−2]); H.push(p); then smooth(n−2).
- `smooth(k)` (0x71a850), k>0:
  - d = P[k+1]−P[k−1];
  - if |d|² < 1e-4: H[2k−1] = H[2k] = P[k];
  - else H[2k−1] = P[k] − 0.25·d and H[2k] = P[k] + 0.25·d.
  - The end handles stay equal to the end points (no smoothing at P0 or P[n−1], even for a closed loop).
- `eval(seg,t)` (0x71a4c0): cubic Bezier P0=P[seg], C1=H[2seg], C2=H[2seg+1], P3=P[seg+1], standard Bernstein basis.
- `build()` (0x71ae60):
  - table = [(0,0)], samples = [P0], L = 0;
  - for each seg: addSample(seg, 0.5, eval(seg,.5)); addSample(seg, 1.0, P[seg+1]).
- `addSample(seg,t,pt)` (0x71acd0), recursive:
  - lt = last.param − seg; mid = eval(seg,(lt+t)/2);
  - if |mid − (lastSample+pt)/2|² < 0.001: L += |pt−lastSample|; table.push(seg+t, L); samples.push(pt);
  - else recurse on (tmid, mid) then (t, pt).
- `posAt(dist)` (0x71a710):
  - 0 points → 0; 1 point → P0; dist ≤ 0 → P0; dist ≥ L → last sample;
  - else param = binary search in the table with linear interpolation (0x71a3a0/0x71a280); seg = int(param); eval(seg, param−seg).

### TrainLogic code
```
init (9, 0x720cc0):
  pieceLen(0x6c) = 2.0;
  if (P.empty) path.push_back(owner.pos);  if (P.size<2) path.push_back(P[0] + (0,10,0));
  path.build();  lastIdx = n-1; cachedLast = P[n-1]; owner.pos = P[n-1];   // the object sits on the LAST control point
  cars[0] = spawn("choochoo"); cars[1..n-2] = spawn("choocart"); cars[last] = spawn("choocaboose");   // all parent=0
  pieces[i] = spawn("Railroad") for all i;
  dist = StartDistance;                        // NOT scaled
  placeCars(); layTrack(); setScales(); setColours();
update (15, 0x720760):
  s = owner.scale.x;
  if (mode==2) { if owner.pos != cachedLast: path.setPoint(lastIdx, owner.pos), cache; path.rebuild(0x71aa80); setScales; setColours; layTrack; placeCars; }
  dist += Speed * s * dt;  if (dist > L) dist -= L; else if (dist < 0) dist += L;     // loops forever (closed path)
  if (dt != 0) { placeCars(); setCarVelocities(); updateTrack(); }
placeCars (0x71fe40):
  d = dist; front = posAt(d); d -= EngineLength*s (wrap +L); back = posAt(d); place(cars[0], front, back);
  for mid cars: front = back; d -= MidCarLength*s (wrap); back = posAt(d); place(...);
  caboose: front = back; d -= CabooseLength*s (wrap); back = posAt(d); place(...);      // cars are contiguous, no gaps
place(obj, A=front, B=back) (0x71f3b0):
  obj.pos = body.pos = body.savedPos = (A+B)/2;
  f = normalize(B-A);                    // local +Y points toward the REAR
  r = normalize((f.y, -f.x, 0));         // = cross(f, Z)
  u = cross(r, f);
  obj.orient = columns [r, f, u];  body orientation = quat(orient)
setCarVelocities (0x71f0a0 -> 0x71ea40 per car):
  body.linVel = body.savedLinVel = -col1 * (car.scale.x * Speed);   // forward velocity (for collisions)
  body.angVel = 0.1*Speed * (correction toward the front point) ...  // small, for collision response only
layTrack (0x720180):
  head = dist + TrackLead*s (wrap -L);  head -= fmod(head, 2.0);    // note: snapped with the UNSCALED 2.0
  b = posAt(head);
  for i in 0..N-1: a = b; d -= 2.0*s (wrap); b = posAt(d); place(pieces[i], a, b);
  headIdx = 0
updateTrack (0x71f790), Speed >= 0 branch:
  gap = dist + TrackLead*s - trackHead;  if (gap > L/2) gap -= L;
  if (gap > 2*s) {                       // at most ONE piece recycled per frame
      idx = (headIdx - 1 + N) % N;       // the tail piece
      place(pieces[idx], posAt(trackHead + 2s wrap), posAt(trackHead));
      trackHead += 2s (wrap); headIdx = idx; gap -= 2s; }
  f = gap / (2s);
  for i in 0..4:  p = pieces[(headIdx+i)%N]:  p.RenderMesh.A = int((i+f)*0.2*TrackColor.w*10);
                                           p.scale = (2 - (i+f)*0.2) * s  (uniform)     // new pieces pop in large and faint, then shrink
  pieces[(headIdx+5)%N].A = int(TrackColor.w*10)
  for i in 0..4:  pieces[(headIdx+N-1-i)%N].A = int((i+f)*0.2*TrackColor.w*10)  // tail fade (as coded)
  (Speed < 0 has a mirrored branch that moves the head piece to the tail; it uses the unscaled 2.0*N. Unused in the story.)
setScales (0x71eec0): every car and piece gets a uniform scale = owner.scale.x.
setColours (0x71ec90): cars' RenderMesh colour = Train Color; pieces' = Track Color (RGB*255, A = w*10).
```
- The track extends from TrackLead·s **ahead** of the engine (469 units in L3) back over N·2s (2625 units in L3). The train
  itself is (7+18·3.5+4)·s = 1735 units long. Pieces are laid ahead of the train as it advances and recycled from behind, so
  only a moving stretch of rail exists.
- Blueprints, each with collider material "Building" and a kinematic body:
  - `choochoo`: choochoo.msh/.mdc, material "choochoo", Phantom "Play_TrainEngine".
  - `choocart`: choocart.msh/.mdc, Phantom "Play_TrainCar".
  - `choocaboose`: choocaboose.msh/.mdc, Phantom "Play_TrainCar".
  - `Railroad`: Railroad.msh, material "shockwave", RenderMesh only, no collider.
- Port:
  - Implement BezierPath exactly, including the 0.25 handles and the adaptive arc-length table. Build it from the XML
    `<BezierPath>` child list of `Vector3` points (the names are ignored).
  - Then implement placeCars and the track recycling. The cars are kinematic: set positions directly and give them the
    forward velocity for collisions.

## 6. FakeSpotLightLogic (vt 0x795bec, size 0x40, ctor 0x40e1d0)
Fields (serialize 0x6cab90): `Start Counterclockwise` bool @0x34 (also the live direction flag), `Rotation Speed` f32 @0x38,
`Swap Threshold` f32 @0x3c. The constructor sets no defaults. Blueprint values 1 / 3.0 / 0.75; levels use the same values,
with CCW 0 or 1. Init (0x6ca6b0) does nothing.
```
update (15, 0x6cb280): if (mode==2) return;
  c1 = owner.col1 (old), c2 = owner.col2 (old);
  w = RotationSpeed; if (!ccw) w = -w;
  angle = (fabs(c2.z - SwapThreshold) + 0.1f) * w * dt;
  owner.T.rotateLocalY(angle);              // 0x678760: orient = orient * RotY(angle), RotY=[[c,0,s],[0,1,0],[-s,0,c]]
  if (c2.z < SwapThreshold)                 // tilted beyond acos(threshold) (41.4 deg for 0.75)
      ccw = dot(c1, cross(c2, (0,0,1))) >= 0;   // = c1.x*c2.y - c1.y*c2.x; pick the direction that swings back up
```
The beam (local Z) swings like a pendulum about its local Y axis between about ±acos(Threshold) from vertical. It is fastest at
vertical ((0.25+0.1)·3 = 1.05 rad/s) and slowest at the ends (0.1·3 = 0.3 rad/s). The transform is rotated directly (no body).
Blueprint: fakespotlight.msh, additive texture fakespotlight.png, DrawMode 2, colour (1,1,1,1). It is a fake volumetric cone.

## 7. SpinningLaserLogic (vt 0x79680c) and SingleLaserLogic (vt 0x7967b4)
SpinningLaserLogic serializes **nothing** (0x68d800 is `ret 8`). The `RotSpeed`/`RotAxis` in the SpinningLaser blueprint belong to
its **StaticSawLogic**, which spins the kinematic hub (−1 rad/s about local X). In L6 local X is world Y, the road direction,
so the fan rotates in the plane across the road. The update is empty.
```
init (9, 0x68d990):  for i in 0..14:
    o = spawn("SingleLaser", parent=owner); init(o); lasers(+0x34 vector).push(o);
    a = i * 0.418867f;                         // ~2*pi/15 = 24 deg
    o.orient = RotX(a) * o.orient;             // RotX=[[1,0,0],[0,c,-s],[0,s,c]]; o.cc.x += a
    o.pos = (0,0,0) + 3 * o.orient.col1;       // local to the hub: 15 spokes, starting 3 units out
destroy (19, 0x68d830): delete the children
```
SingleLaserLogic: field `Max Distance` f32 @0x34 (blueprint 125). Init (0x68dee0) spawns a "LaserDot" (world object, renamed
"LaserDot") with its handle at @0x38.
```
update (0x68dcb0):
  o = world pos; dir = worldOrient.col1 * MaxDistance;
  if (raycast(physWorld [0x7ff788], o, dir) -> hit {t, collider, point}) {
      if (car = hit.object.GetComponent(CarLogic 0x20)) car.heat = max(0, car.heat + dt);    // +1 heat/s (max heat 50)
      owner.scale.y = MaxDistance * hit.t;      // beam stops at the first surface
      LaserDot.pos = hit.point; }
  else { owner.scale.y = MaxDistance; LaserDot.pos = o; }
```
Blueprints:
- `SpinningLaser`: Crusher.msh/.mdc, material "Obsticle", colour (0.5,1,0.1,2), kinematic body, Phantom "Play_LaserObstacle".
- `SingleLaser`: laser.msh, scale (25,1,25), colour (1,0.15,0.1,3). The beam is along local +Y with unit length; scale.y is the
  length.
- `LaserDot`: splosion.msh, scale 0.1, a point Light and a ParticleEmitter.

Contact with a spinning laser only heats the car slowly. The hub itself is a deadly "Obsticle" collider.

## 8. SweepLaserLogic (vt 0x795a34, ctors 0x408030/0x4080f0)
Fields (serialize 0x6925d0): `Laser Speed` f32 @0x4c, `Laser Rotation Speed` f32 @0x50, `Number Of Lasers` int @0x68.
Blueprint values 10 / 0.5 / 8. In L6: 15 / ±0.5 or −1 / 6, arch scale 55.6.

Members:
- 0x34 vector<handle> lasers
- 0x54 t, 0x58 phase duration, 0x5c sweep offset (world units), 0x60 phase, 0x64 sweep direction (1 = +X)
- 0x6c current laser count, 0x70 saved orientation

Let sx/sz = owner.scale.x/z, X/Y/Z = columns of the (body) orientation, and top = pos + 0.87·sz·Z.
```
init (9, 0x693ec0): count = N; saved = orient; rebuild(this, spawn=true)
rebuild(spawn) (0x693910): t=0; dur=1.0; sweep=0; phase=0; dir=1;
  (in the editor, adds or deletes laser objects to match N)
  A = top - 0.6*sx*X;  step = 0.1*sx*X;
  for i<N: if (spawn) lasers[i] = spawn("laser", parent=0) + init;
           L.orient = columns [X, -Z, Y];  L.scale = owner.scale * 1.74;  L.pos = A + i*step;
  SetRTPC("laser_volume" = 0x7f5d03f3, 1.0, owner)
update (15, 0x693ef0):
  if (mode==2) { rebuild(false); return; }   if (N==0) return;
  t += dt;  car = FindObject("Car"); if (!car) return;
  body.angVel = LaserRotationSpeed * owner.col1;   // the whole arch ROLLS about its local Y (the road axis)
  Pp = top + X*(1.8*sx/N);  Pm = top - X*(1.8*sx/N);  A = top - 0.6*sx*X;   (from the body pose)
  for i<N: L = lasers[i];
      visuals(L, phase, owner, t, dur);                 // 0x6930b0 (see below)
      if (phase==3 && hitsCar(L)) car.CarLogic.heat = max(0, heat + dt*3000);   // instant overheat: KILL
      L.orient = [X,-Z,Y];  L.pos = A + sweep*X + i*0.1*sx*X;
  advancePhase(phase, t, dur, owner);                   // 0x692f20
  if (dir==1) { sweep += LaserSpeed*dt; if (|lasers[0].pos - Pp|^2 < lasers[0].scale.y) dir = 0; }
  else        { sweep -= LaserSpeed*dt; if (|lasers[N-1].pos - Pm|^2 < lasers[N-1].scale.y) dir = 1; }
hitsCar(L) (0x6935c0): raycast from L.pos along L.col1 * L.scale.y; true if the hit object has CarLogic.
advancePhase (0x692f20):
  if (phase in {1,2,4} && t > dur) { t = 0;  1->2 (dur 0.5);  2->3 (dur 3.0);  4->0 (dur 1.0);  return; }
  d2 = |owner.pos - camera.pos|^2;
  if (phase==0) { if (d2 < 40000) { phase=1; dur=0.1; } }      // camera within 200 units: arm
  else if (d2 > 40000) { phase=4; dur=0.1; }                     // camera left: shut down
visuals (0x6930b0): f = sin(50 t) + 1;   mesh = laser RenderMesh, Ll = laser Light, La = arch Light
  0 off:     t=0; mesh hidden (+0x55=0); Ll.int=0; La.int=1; RTPC laser_volume=0
  1 warm:    k=t/dur; col=(0.5kf,0.5kf,0.05kf,0.25kf) on mesh, Ll, La; Ll.int=La.int=t; RTPC=0.35k
  2 charge:  col=(0.5f,0.5f,0.05f,0.25f); Ll.int=0.75; La.int=2; RTPC=0.35f          (flicker, 0.5 s)
  3 fire:    t=0; col=(1,0.1,0.1,3); Ll.int=15; La.int=4; RTPC=1                     (deadly until the camera leaves)
  4 cool:    k=max(0,1-t/dur); col=(k,0.1k,0.1k,3k); Ll.int=1; La col alpha 3k; RTPC=k
```
- **What it spawns:** N "laser" objects (laser.msh, material "laser", colour (1,0.15,0.1,3), a Light of Type 5). They are world
  objects, not children, and get repositioned every frame.
- **Laser layout:** they hang from the top of the arch (0.87·sz up along its local Z) and point down through the arch (−Z).
  They are spaced 0.1·sx apart along the arch's X.
- **Motion:** the group slides along X at LaserSpeed units/s and reverses when the first or last laser comes within
  sqrt(scale.y) of ±1.8·sx/N. Meanwhile the arch (Lgate.msh/.mdc collider "Obsticle", kinematic body) rotates about its local Y
  at LaserRotationSpeed rad/s, carrying the lasers with it.
- **Kill:** any laser touching the car in phase 3 adds 3000·dt heat (≈50 per frame against max 50), so the car **dies**. The
  phases depend on the camera distance: 200 units arms them (0.1 s warm-up, 0.5 s charge) and leaving shuts them down (0.1 s fade).
- **Sound:** Phantom "Play_LaserObstacle" on the arch, with its volume driven by RTPC `laser_volume` (FNV-1 hash 0x7f5d03f3,
  confirmed).

## Notes and uncertainties
- The slot 16/17 trigger (editor play/stop or checkpoint restart) is not confirmed.
- I identified 0x5577fc as the active camera handle from the renderer copying a Camera component's view and projection
  matrices and its owner handle (0x613861). Drones and blimp therefore steer relative to the camera, not the car.
- In the train, `Start Distance` and the 2.0 snap in layTrack are unscaled, while everything else is ×owner.scale.x.
- The SearchlightLogic aim column mapping is not fully decoded.
