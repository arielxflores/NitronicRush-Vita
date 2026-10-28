# re_K - Chase camera (ChaseCameraLogic, type 0x4f) - Nitronic Rush PC
Blueprint `Objects/ChaseCamera.xml` (object name "ChaseCam"): Transform + Camera(0xa, "Field of View" 45 -> Camera+0xb4, overwritten every frame) + ChaseCameraLogic.
vtable 0x79652c, ctor 0x409420, init 0x6ee9b0 (+defaults 0x6ee8b0), serialize 0x6eeae0, tweak-bar 0x6ee3b0, VUpdate 0x6f1ef0.
Conventions: Z-up world; car R columns X=right, Y=forward, Z=up (GO+0xfc row-major); camera looks along -col2. Disasm dumps: `$HOME/nrw/K/`.

## Fields (blueprint value / ctor default)
+0x34 Target Name "Car" | +0x50 Leash Distance 6.0 | +0x54 Camera Height 1.2 | +0x58 Target Height 1.2 | +0x5c Fly Distance 10.0
+0x60 Fly Camera Sensitivity 0.12 | +0x64/68 Blur Speed Low/High 4000/7000 (compared with SPEED SQUARED) | +0x6c/70 Blur Fade In/Out 0.5/0.25
+0x74/78 Slow/Fast Camera FoV 60/75 | +0x9c Fade Speed 3.0 | +0xc0/+0xd0 Shake Freq/Damping X 50/6 | +0xc4/+0xd4 Shake Freq/Damping Y 70/7.
Code-only: +0x108 LongLeashDistancePlus_ 6.0, +0x10c LongLeashHeightPlus_ 0.7, +0x100 view-blend rate 2.0. Tumble*/Forward* tweaks (+0x124..+0x14c) and +0x118 are never read.
Per-car (CarLogic): "Added Camera Leash Distance" +0x4c8 (Car 0, Cycle -0.5, Humvee 2, Van 1.33); "BumperCamPos_" +0x1f4 (Car/Gold/Delorean (0,1.0,0.35), Humvee (0,1.2,0.35), Van (0,1.92,0.42), Cycle (0,-0.135,0.702)), copied to cam+0xec at car init (0x707738).
Runtime: +0x7c up vec, +0x88 tumble T, +0x8c wing blend W, +0x90 speed factor s, +0x94 mode, +0x98 screen fade, +0xa0 focus list {handle,weight},
+0xb8/bc shake amp X/Y, +0xc8/cc phase, +0xd8 finish ref point, +0xf8 bumper flag, +0x104 far flag, +0xfc bumper blend bb, +0x110 far blend lb, +0x114 reset pending.

## 1. Collision - YES, a segment raycast, no sphere cast
Raycast `0x63ba80(world=[0x7ff788], origin, seg=ebx, hit=edi)`: AABB-tree query of **world+0xbc = static colliders only** (colliders without BodyRigid: roads, buildings, posters...). Kinematic hazards (saws, crushers, blockers, mines, NitroTunnel; tree +0xf8) and dynamic bodies (car, gibs; tree +0x134) are NOT tested. Filter {ignore=none, mask 0xFFFF} vs material Layer word (all materials pass). Closest hit, fraction t in [0,1); hit = {+4 t, +8 normal, +0x14 point = origin+seg*t}. Trimesh triangles use a scalar-triple-product test with sign rejects (0x72fff0) -> **one-sided** (faces wound away from the ray origin are ignored).
* Origin = car world position (+ car forward*5*bb while blending to bumper view); end = origin + final offset (sec. 2). NOT from target height.
* Hit -> camPos = hit.point + **0.07** * hit.normal. Miss -> camPos = origin + offset.
* Pull-in is instant (no smoothing). Release is implicit: next frame starts from the actual (pulled-in) camera, and the leash lerps back out 10%/frame (FL' = 0.9FL+0.1leash), with camera and look-at heights scaled by FL'/leash (so a pulled-in camera also sits lower).
* The same ray (+0.07) is used by the snap/reset (0x6efd60); death cam uses it to drop occluded focus objects. (Correction to re_F1: 0x63ba80 does not hit the car.)

## 2. VUpdate 0x6f1ef0 (all lerps are PER FRAME, tuned for 60 fps: port k' = 1-(1-k)^(60dt))
```
if dt==0 return; if (+0x114) {+0x114=0; Reset();}
if (fade<1) { fade=min(1,fade+FadeSpeed*dt); gfx[0x7ff6c4]+0x3d0 = 1-fade; }   // black fade-in
mode 1: HeatTint(); if(!Find(TargetName)) return; if (bb<0.9) Chase(dt); SpeedFov(dt); ViewSwitch(dt); ShakeAbs(dt);
mode 3 (CarLogic_Die): HeatTint(); DeathCam(dt); ShakeInc(dt)      mode 4 (respawn 0x7014d0): HeatTint(); gfx+0x3cc=1 (one-shot renderer flag); fade=0; mode=5
mode 5: HeatTint(); mode=1; +0x114=1 (snap next frame)             mode 6 (finish/EndTunnel 0x706154): FinishCam(dt); ShakeInc(dt); heat-tint alpha -= 2.3dt (>=0)
```
**Chase 0x6f0c10**
```
if Held("CamCenter") Reset();      // 0x403f40 = held state (CarBoost uses it); recentres every frame while held
car=Find(TargetName); cc=ControllerCar; wg=ControllerWings; cl=CarLogic; body=cc+0x9c; R=car rot (X,Y,Z); Cprev=cam rot (start of frame)
P = car.worldPos + Y*5*bb;  C = cam.worldPos
leash = Leash + cl.Added + 6*lb;  h = CamHeight + 0.7*lb;  fly = FlyDist + cl.Added + 6*lb
v=body.linVel(+0xec) w=body.angVel(+0xd4); fwdV=Y.v; latV=|X.v|; upV=|Z.v|
if (cc.wheelsOnGround(+0x80) >= 3) T -= dt;                        // tumble factor
else if (fwdV<25 || latV>20 || upV>20 || |w|^2>35) T = 1;          // set instantly
else if (fwdV>28 && latV<10 && upV<10 && |w|^2<10) T -= dt;   T=clamp(T,0,1)
tx=Axis("CamTurnX"), ty=Axis("CamTurnY")                            // right stick
dist=|C-P|; D = norm((C-P)/dist - X*tx*FlySens + Z*ty*FlySens)
d = dist>fly ? fly : 0.9*dist+0.1*fly;   D2 = 0.9*D - 0.1*Y            // fly offset dir (not renormalised)
k = min(1, 2-2*|wg.pitchInput(+0x44)|);  Uf = lerp(Cprev.col1, (0,0,1), 0.1*k)
W = (fwdV<2) ? 0.9W : 0.9W + 0.1*wg.deploy(+0x40)                   // wings blend
up = (T<0.9) ? (1-T)*(0.9up+0.1Z) + T*up : 0.97up + (0,0,0.03)        // not normalised; Reset sets (0,0,1)
hgt = h + 3*ty
v2 = Rot(Cprev.col0, -0.3*ty*leash*dt) * Rot(up, 0.5*tx*leash*dt) * (P-C)   // RH Rodrigues 0x676f10
flat = v2 - up*(up.v2)
if (|v|^2>1) { nv=norm(v-up*(up.v)); a=-norm(flat); dd=a.nv;               // swing behind when car drives at camera
  if (dd>0.75) { c=cross(a,up); s=(dd-0.75)*dt*10; flat += (c.nv>0 ? c*s : -c*s); } }
FL=|flat|; fN=flat/FL; FL2 = FL<leash ? 0.9FL+0.1leash : leash; ratio=FL2/leash   // hard leash, eases out from below
L=|v2|;  L2 = L>leash ? 0.7L+0.3leash : 0.9L+0.1leash
n=norm(v2); if (|n.z|/|n.xy| > 5) n = norm(n.xy/|n.xy|, sign*5)                  // pitch clamp ~78.7 deg
ground = -fN*FL2 + up*hgt*ratio;   tumble = -n*L2
off = (1-W)*((1-T)*ground + T*tumble) + W*d*D2
camPos = RaycastStatic(P, off) ? hit.point+0.07*hit.normal : P+off
LookAt(target = P + up*TargetHeight*ratio*(1-W), upHint = up*(1-W) + Uf*W)
```
LookAt 0x678120: col2 z=norm(pos-target); col0 x=norm(cross(upHint,z)) (x=(1,0,0) if 0); col1 y=cross(z,x).
Behaviour summary: grounded -> string-leash 6 behind/1.2 above, up follows car up 10%/frame (walls, loops, upside-down roads all rotate the camera with the car). Airborne slow/unstable/upside-down off the ground -> T=1: camera holds its line of sight, distance relaxes to leash, up drifts to world Z 3%/frame. Wings out -> fly cam: distance -> FlyDist 10, direction eased 10%/frame toward behind the car, right stick orbits (0.12), look at car centre, up eased toward world Z unless pitching hard.
**Reset 0x6f0b50**: T=W=s=0; up=(0,0,1); heat-tint alpha=0; Camera.FOV=SlowFoV; snap 0x6efd60: up=carZ; off=-Y*leash + up*h; ray from car pos; LookAt(car+up*TargetHeight, up); zero transform rot accumulators. Called by CamCenter, car init (0x707733), replay start (0x7152dc), respawn (via modes 4->5; 0x6ee960 also clears the focus list).
**SpeedFov 0x6f09f0**: `q=|vel|^2; t= q<=4000?0 : q>=7000?1 : (q-4000)/3000; a=clamp(dt/(t>s?0.5:0.25),0,1); s+= (t-s)*a; FOV=60+15*s; RTPC 0x24bfd816=s` (s also drives motion blur).
**ShakeAbs 0x6eede0** (mode 1): `ampX-=6*ampX*dt; if(ampX>1e-4){phX+=50dt; RotLocalX(sin(phX)*ampX);} else ampX=phX=0;` same for Y (70/7, RotLocalY). Mode 3/6 (0x6eeca0) rotates incrementally by cos(ph)*amp*freq*dt. Kicks (CarLogic): amp=max(amp,k) on X and Y; explosion 0.035; crash min(0.05*impact/60, 0.10).

## 3. Projection (0x605590, every render from 0x61383b)
`D3DXMatrixPerspectiveFovRH(fovY = 2*atan(tan(FOV/2)/sqrt(aspect)), aspect = backbuffer W/H, zn = 0.1, zf = SkyFar + 0.01)`; SkyFar from the skybox FogTweaks (default 1600; skybox values 1600/2000/3100). FOV is a geometric-mean FOV (60..75 in race). Camera init (0x605410) uses FOV direct, 4/3, 0.1, 800 (replaced).

## 4. Change View ("SwitchCarCam", 0x6eef50), cycle Normal -> Far -> Bumper -> Normal
on press (edge, 0x4021b0): `if (bumper){bumper=0;far=0} else if (far){far=0;bumper=1} else far=1`. r=2.0*dt: lb -> far?1:0 (clamped); bb: bumper ? min(1,bb+r) : (bb-=r, if <0 {bb=0; return}).
* Normal: leash 6 (+Added), height 1.2, target 1.2, fly 10.  * Far: leash 12, height 1.9, fly 16 (0.5 s blend).
* Bumper: H = car.pos + R*(BumperCamPos_ * car.scale) (Car: 1.0 fwd, 0.35 up); rotation Q = car frame (cam col0=X, col1=Z, col2=-Y).
  `cam.pos = lerp(cam.pos, H, bb); cam.rot = nlerp(quat(cam.rot), Q, bb)` (hemisphere-corrected, not renormalised). Chase update is skipped when bb>=0.9; while bb<0.9 the chase target is pushed 5*bb ahead.

## 5. Death (mode 3, 0x6f00c0) and finish (mode 6, 0x6ef740)
Camera position frozen. Focus list = DeadCarBody + gibs (weight 1, pushed by CarLogic_Die 0x70807a/0x708151). Each frame one entry (round-robin, static 0x801ad0) is raycast from the camera; if a static hit -> removed. M = weighted centroid, S = mean |p-M| per axis.
Roll level about cam Z: `a = up.z>0 ? -right.z*3dt : (right.z>0 ? -3dt : 3dt)`. Aim (f=-col2, h=horiz dir to M, rh=(f.y,-f.x,0)/|f.xy|): if h.f>0 { pitch = cross(f,norm(M-cam)).rh*3dt about rh; yaw = -(rh.h)*3dt about world Z } else { pitch 0; yaw = +-3dt }.
FOV += (clamp(1.5*deg(atan((|S|+2)/|M-cam|)),10,120) - FOV)*3dt. Mode 6: same tracking of the FinishCar (pushed at 0x70615e, ref +0xd8), no raycast.
HeatTint 0x6ef4c0: screen tint gfx+0x557a68..74 = lerp(base, (0.7,0.025,0.01,3s), a), base = 0.65*"tronics" RenderMesh Color1 (no "tronics" object: (0.3,0.4,1,0)) with a from the car's smoothed remaining heat (0.97/0.03 per frame) - cosmetic.

## 6. Hiding / fading near the camera: NONE
No alpha fade of objects between camera and car, no car hiding in bumper view, no near-distance cull in ChaseCameraLogic or its callers (GetComponent(0x4f) callers: 0x68af12 credits, 0x701530 respawn, 0x706140/0x706226 finish, 0x707724 init, 0x707f6f death, 0x708e2f/0x709734 shake, 0x7152cd replay). Only protections: 0.07 wall offset + zn 0.1. (color_xray.fx = debug-draw only.)

## 7. Port notes
Implement RaycastStatic against static level collision only (one-sided tris), 0.07 offset, no smoothing on pull-in. Convert per-frame lerps for 30 fps. Use FOV formula + zn 0.1 / zf SkyFar.
