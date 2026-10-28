# re_G - material shaders of Nitronic Rush (PS/VS decoded) and what the Vita port must change

Sources: `~/at/fx/<name>.txt` (disassembly), preshaders decoded with `~/nrw/fxi.py <fx>` (CTABs + FXLC),
effect parameter defaults with `~/nrw/fxp.py <fx>`, compact PS/VS view `~/nrw/fxv.py <fx> [psblock]`,
usage counts `~/nrw/G_use.py` -> `~/nrw/G_use.json`. Exe addresses are VAs in NitronicRush.exe.

## 0. Common inputs (verified in the exe)

**Color / Color2 / Float1..3 path.** RenderMesh (comp 0x47) -> per-draw item (0x5f2b4b):
`item+0x30 = RM+0x60 (Color1 packed)`, `+0x34 = RM+0x64 (Color2)`, `+0x38/+0x3a/+0x3c = D3DXFloat32To16Array(RM+0x68/+0x6c/+0x70)`
(Float1..3 as **half floats**), `+0x3e = half(ObjectID)`. Renderer 0x60c32b..0x60c4f8: Color handle [+0x90] and
Color2 handle [+0x94] get `(R/255, G/255, B/255, A*0.1)`; Float1/2/3 handles [+0x98/+0x9c/+0xa0] get
`D3DXFloat16To32Array(half)` via SetFloat; ObjectID [+0xa4] via SetInt. **No scaling** of Float1..3 anywhere:
XML load (0x5f3cc0) stores the float raw, the preshaders only `mov` them (phong_newpulse VS: c12=Color, c13=Color2,
c14.x=ObjectID, c15.xyz=Float1,Float2,Float3). Only precision loss: half floats (1.95 -> 1.9502; 30, 8.25 exact).
The instanced path is the same maths: instance stream = Color, Color2 as D3DCOLOR (VS x (1,1,1,25.5)) + half4(F1,F2,F3,ID).
**Byte packing quirks** (loader 0x5f3bf2/0x5f3c72: `byte = (int)(x*255)`, `alpha = (int)(w*10)`, no clamp, low byte kept):
w >= 25.6 wraps (ramp blueprint Color2.w = 30 -> 300&255 = 44 -> **4.4**; levels store 4.4), negative rgb wraps
(Car/Van Color2 = (1,-1,-1,3) -> (1, 1/255, 1/255, 3) = pure red).

**Time** (predefined param id 10, renderer param switch case 10 at 0x60a626 -> `[renderer+0x5579fc]`). Updated once a
frame in 0x613510: `Time += dt*0.05f; if (Time > 1.0f) Time -= 1.0f;` with `dt = (float)[[0x7ff774]+0x30]` (real frame
time, s); reset to 0 at 0x613ef8. So **Time is a 0..1 sawtooth rising 0.05/s, period 20 s.** (The accumulator at
[0x801ed4] = sum timeGetTime*0.001 set at 0x6106bf belongs to the loading screen, `screenload.fx` (0x610640), not to materials.)

**AudioBeat** = seconds since the last music beat (`[0x7ff6ec]+0x28`, case 48 at 0x60ac51; see re_C).

**LightBuffer** (light pre-pass, re_C): `L.rgb = sum diffuse*att*colour*intensity`, `L.a = sum pow(R.V,30)*att*SpecStrength`,
read with a 4-tap object-ID-matched bilateral upsample. Every "phong" PS uses
`LIT(D,g) = L.rgb*D.rgb + normalize(L.rgb)*L.a*g` (g = gloss, normally D.a).
**Output alpha** of lit shaders = `2 + glow terms` (literal 2 from the preshader; glass 1.3, simple_emit 1, unlit ones 0).
It is carried into the bloom chain; the port's `max(c)/4` bloom alpha is an approximation, not analysed further here.

Notation: `C` = Color (= RenderMesh Color1, w = glow 0..25.5), `C2` = Color2, `F1..F3` = Float1..3, `D,D2,E,N,A,S,RS` =
Diffuse1, Diffuse2, Emit1, Normal1, Anim1/Tron1, Specular1, ReflectStrength1 sampled at uv. `T` = Time, `AB` = AudioBeat.
`sat` = clamp 0..1, `fmodh(x)` = HLSL fmod(x,1) = sign(x)*fract(|x|) (C: `fmodf(x,1)`; GLSL ES: `x < 0.0 ? -fract(-x) : fract(x)`).
`band(x, p, k) = sat(1 - |(|x - p| - 0.5)| * k)`: a triangular band where x is half a cycle from p, half-width 1/k.
Sampler <- material PARAM: ColorSampler<-Diffuse1, ColorSampler2<-Diffuse2, EmitSampler<-Emit1, NormalSampler<-Normal1,
ReflectSampler<-Reflect1 (ReflectMap1 in phong_ref_spec), AnimSampler<-Anim1, TronSampler<-Tron1, SpecSampler<-Specular1,
RefStrSampler<-ReflectStrength1. The mesh vertex colour is **not** read by any of these shaders except `road` (and only .r).

## 1. Usage (Levels/*.xml objects -> blueprint/instance RenderMesh MaterialN -> Materials/*.txt Shaded/Additive pass)

| shader | objects / levels | materials (count) | top levels |
|---|---|---|---|
| phong_emit | 10609 / 67 | tut_building 3952, building_group 1672, streetlight 868, saw 721, road_cap 568, pillar 451, shard 427 | all |
| road | 5311 / 63 | road 4684, road_tut 346, road_broken 281 | all |
| building | 3902 / 67 | concrete_block 2332, building_cr2 318, tunnel 271, ... | all |
| simple_emit | 3056 / 36 | emit | |
| additive | 2266 / 51 | arrow 1452, blocker_holo 453, coolpad 163, shockwave 99, fakespotlight 50, antenna_light 47 | |
| traffic | 1843 / 34 | traffic | Hardcore_Teal 257, Calcite 186, Campaign_L2 103 |
| glass | 1756 / 45 | glass 1548, tunnel_glass 208 | Hardcore_Red 101, enter_the_matrix 99, Hardcore_Blue 98 |
| phong_newpulse | 1448 / 50 | ramp 1144, Lava 304 | neon_nights 526, NitroTunnel 305, vert_park 113 |
| phong_ref | 584 / 50 | checkpoint 237, Crusher 172, Tube 94, stomper2 43, humvee 34 | |
| block | 267 / 17 | block | Tutorial 67, neon_nights 58, Gladiator 32 |
| pulse_veins | 254 / 9 | pulse_veins2 220, pulse_veins 34 (Additive) | Campaign_L6 76, L5 47, Gladiator 30, MountainTop 29 |
| phong_emit_anim | 216 / 12 | turbine | fantastic 94, moto 84, nature 17 |
| tvscreen | 175 / 21 | blue_poster*, triposter, poster_nuke (all LOAD blue_poster1) | credits 25, Campaign_L1 23, moto 23 |
| tvscreen_red | 121 / 19 | poster1..10 | Hardcore_Red 23, MountainTop 13, Campaign_L4 12 |
| phong_ref_spec | 121 / 57 | tunnel2 102, stomper 19 | |
| phong_ref_cube | 50 / 3 | core_hall | nature 38, Campaign_L6 10, Gladiator 2 |
| fish_phong_ref | 48 / 1 | fish2 | nature |
| tronics | 43 / 42 | tronics (ground plane, ~1 per level) | |
| city | 39 / 39 | city (backdrop city, 1 per level) | |
| phong_ref_pulse | 13 / 3 + **the player car** | Van 13 (moto, menu, credits); `car` = Car.xml and all Car*/PlayerHumvee* parts; also Cycle, Delorean | |
| phong_ref_pulse_gold | 0 in levels | car_gold (Car_Gold + Car*_Gold parts) | |
| corecore, corecorecore | 1 / 1 each | corecore, corecorecore | Campaign_Level6 |
| corecorecore2 | 0 in levels | corecorecore2 (spawned; CoreBallExplosionLogic, re_E 6.1) | Campaign_Level6 |

## 2. Shaders

### phong_newpulse (ramp, Lava)
VS: passes uv, C (o4), C2 (o5), F1..F3 (o7.xyz). PS:
```
lit   = LIT(D, D.a)
u     = fmodh(F1*T + F2)                       // per-object phase, cycles
k     = band(E.b, u, F3) * E.g                  // E.b = phase map, E.g = chevron mask
rgb   = lit + E.r*C.rgb*C.w + k*C2.rgb*C2.w      // base glow uses E.r only (not E.rgb)
alpha = 2 + E.r*C.w^2 + k*C2.w^2
```
**Speed:** u advances `0.05*F1` cycles per second. Ramps: F1 = 30 (1069 of 1144 objects) -> **1.5 Hz (one sweep per
0.667 s)**; others 20 -> 1.0 Hz, 25 -> 1.25, 18 -> 0.9, 15 -> 0.75, 12 -> 0.6. Lava: 5 -> 0.25 Hz (178), 1 -> 0.05, 2 -> 0.1,
3 -> 0.15, 4 -> 0.2, 6 -> 0.3. All F1 are integers, so the 20 s Time wrap is seamless. The lit chevron is the one whose
E.b = u + 0.5 (mod 1): the light runs in order of increasing E.b. F3 = 8.25 (ramps): half-width 0.121 cycle, so each chevron
glows ~0.16 s with a triangular rise/fall; F3 = 1 or 2.9 (Lava) gives broad soft waves. F2 = phase offset (0 except 12, -2.55).
Ramp colours: C = e.g. (0.1,1,0.1,1.2) for the base stripes; C2 = (1,0.49,0.09, **4.4**) orange chevrons (or C2 = C.rgb with w 4.4).
Lava: C.w = 0 (no base glow), C2 = (0.97,0.34,0,4.4).

### pulse_veins (Additive; pulse_veins/2 use veins.tga/veins2.tga: R = vein mask, G = phase gradient)
PS preshader: `p = fmodh(1.66 - 4*AB)`, `q = 1.1 - 4*AB`. PS (`t0` = mip-0 read, `t` = normal read, same texture):
```
a   = 1 - 10*|(|t0.g - p| - 0.5)|
rgb = sat(t0.g*t.r*q * C.rgb)*C.w + (a >= 0 ? sat(t0.g*a*t.r * C2.rgb)*C2.w : 0)
alpha = same formula on .w      // VS computes a fog fade but the PS never uses it: no fog fade
```
Animates on the music: on each beat the veins flash to 1.1x and fade linearly to 0 in 0.275 s; a thin band (half-width 0.1)
sweeps down the G gradient at 4 units/s, i.e. a new sweep every 0.25 s, re-phased on every beat (at AB = 0 it sits at G = 0.16;
for AB > 0.415 only the lower half of each sweep shows because of fmod's sign).

### phong_emit_anim (turbine: Diffuse1, Emit1, Anim1 = turbine_anim.tga)
```
rgb = LIT(D,D.a) + E.rgb*C.rgb*C.w
b   = max(1 - 12.5*|(|A.rgb - fract(5*T)| - 0.5)|, 0)          // three phase maps, half-width 0.08
rgb += sat(A.a - 0.7) * 20 * (b.r+b.g+b.b) * (0.2,0.8,0.2)     // fixed green, independent of Color
alpha = 2 + C.w^2*E.a + sat(A.a-0.7)*20*(b.r+b.g+b.b)
```
Animates: green bands crawl over the turbine at 5*0.05 = **0.25 Hz** (one sweep per 4 s), up to (1.2,4.8,1.2) bright.
Note C.w = 25.5 on 19 turbines.

### block
VS: uv tiled by the object's scale (preshader, `s = |ModelWorld axis|`): object-space normal |n.x| > 0.1 ->
`uv*(ceil(sz/4), ceil(sy/4))`; else |n.y| > 0.1 -> `uv*(ceil(sz/4), ceil(sx/4))`; else `uv*(ceil(sy/4), ceil(sx/4))`.
PS = phong_emit: `rgb = LIT(D,D.a) + E.rgb*C.rgb*C.w`. Nothing animates.

### phong_ref_pulse (player car, Van, Cycle, Delorean) and phong_ref_pulse_gold (car_gold)
VS outputs view normal/tangent/binormal, C, C2*(1,1,1,1.5), (F1,F2,F3,ID). PS:
```
rgb  = LIT(D, D.a) + E.rgb*mix(1, C.rgb, E.a)*C.w
n    = normalize(TBN*(2N-1));  R = tex(Reflect, n.xy*(0.5,-0.5)+0.5)      // plain normal sphere map (no view vector)
rgb += sat((D.a-0.5) * 2R)
s    = band(fmodh(F3*uv.x), fmodh(F1), 1/F2) * E.a*E.r
rgb += s * C2.rgb * C2.w*1.5
alpha= 2 + E.a + sat((D.a-0.5)*2R.a)
```
Gold: spec term times (1,0.5,0.2), reflection `sat((D.a-0.5)*2.4R)`, Reflect1 = city_ref_gold.tga.
Car/Van F = (0, 0.05, 3) and nothing in the exe writes RenderMesh Float1 for the car (all +0x68 writers checked: core ball,
level-start text, narrative, HUD meter), so the "pulse" is **static**: three thin red stripes (C2 = (1,1/255,1/255,3) -> x4.5)
at u = 1/6, 1/2, 5/6 +-0.017 where E.a*E.r > 0 (~0.3% of newcaremit texels). Low visual weight.

### corecore (Campaign_Level6; Diffuse1, Diffuse2 = pcb.tga, Emit1, Anim1)
```
s   = C.w + 1.9 - 4*AB                                  // pulses on the beat (falls 4/s; can go negative if beats stop)
g   = D.a*0.2*D2(uv*4).r*(1-E.r);  D' = D.rgb + 0.2g;   rgb = L.rgb*D' + normalize(L.rgb)*L.a*3g
rgb += s*E.rgb*C.rgb
b   = max(1 - 33.3*|(|A.rgb - fmodh(1.66 - AB)| - 0.5)|, 0)
rgb += E.rgb*dot(b,(1,0.3,0.3))*C2.rgb*C2.w          // C2 = (0.77,0.29,0.11,10)
alpha = 2 + s*E.a*C.w
```

### corecorecore (Campaign_Level6) / corecorecore2 (exploding ball)
VS: object position scaled by `1 + 0.5*VTF(LightSampler, (fmodh(45.4*T), 1.6*AB)).rgb` (the vertex shader reads the
light buffer as a noise source: jitter at 2.27 Hz; hardware dependent, returns 0 without VTF on float targets).
corecorecore2 VS additionally: `P = mix(jitteredP, P, F3) + rnd1*N*F1` (rnd1 = fract(|6017*N.x|), explosion) and
`P.z += min((10*rnd2 - F2)*|10*rnd2 - F2|, 0)` (rnd2 = fract(|45467*N.z|*0.1), pieces fall); F1..F3 driven by
CoreBallExplosionLogic (re_E 6.1). PS (no diffuse texture):
```
R = world-space reflect(viewDir, n)  (cube coords (R.y, R.x, R.z) in corecorecore)
corecorecore : rgb = (1 - texCUBE(core_ref.ddsc, R).rgb)*C.rgb*C.w + 0.5*L.rgb,  alpha 0
corecorecore2: rgb = (1 - texCUBE(...).rgb)*C.rgb*C.w,                          alpha 0
```

### tvscreen (blue posters) / tvscreen_red (red posters); Emit1 = static.png (noise)
VS: view normal Nv, view position Pv, k = 1 + fmodh(0.211*ObjectID). PS:
```
n1  = E(uv + T*(1.3,-3)*k).g                      // slow noise   (0.065,-0.15)*k uv/s
o   = (normalize(Pv)+normalize(Nv)).xy;  dx = 0.045*o.x + J*n1     // J = 0.005 (red: 0.015)
pic = (D(uv).r, D(uv + (0.5dx, 0.0225 o.y)).g, D(uv + (dx, 0.045 o.y)).b)   // view-dependent chromatic split
rgb = normalize(L.rgb)*L.a + P*C.w*pic + Kn1*n1 + Kn2*(n2.r - n2.b),  n2 = E(uv + T*(17.3,-120.123))   // (0.865,-6.0) uv/s
          blue: P = 0.35, Kn1 = 0.05, Kn2 = 0.1     red: P = 0.5, Kn1 = 0.25, Kn2 = 0.3
alpha = 1 + P*C.w + noise terms
```
No diffuse lighting of the picture (only the specular highlight) and **C.rgb is not used** (only C.w).
Animates: fast scrolling static (vertical, 6 uv/s) and a slow drifting grain, plus colour fringing that shifts with view angle.

### city (backdrop city, 1/level; Diffuse1 = city.tga, Tron1 = tronics_anim.tga, width = 0.1 -> k = 10)
VS: uv, object-space position Po, Color, Color2. PS (all texture coords = `Po.xy*(0.5,-0.5)+0.5`, top-down planar):
```
b    = sat(1 - |(|A.rgb - T| - 0.5)| * 10)
glow = (C.rgb*dot((0.2,0.8,0.5),b) + C2.rgb*dot((0.8,0.2,0.5),b)) * C.w * A.a * sat(1 - 15*Po.z)   // only near the base
bld  = D.rgb*C2.rgb*C2.w
edge = uv.x<0.05 || uv.y<0.05 || uv.x>0.95 || uv.y>0.95
win  = fract(100|Po.x|)>0.05 && fract(100|Po.y|)>0.05 && fmod(Po.z,0.02) > 0.007    // window cells
rgb  = edge ? glow + bld : (win ? glow + 0.25*bld : glow)          alpha = C2.w^2*D.a*(1 | 0.25 | 0)
```
Unlit. Animates: the tron bands creep across the city base once per 20 s (T). About 59% of the area is window cells.

### tronics (ground plane; Diffuse1 = tronics_diff, Tron1 = tronics_anim, width = 0.01 -> k = 100)
VS: `uv = (Pworld.xy + 3000)*(0.0015,-0.0015)` (world planar, 667-unit tile). PS:
```
b   = sat(1 - |(|A.rgb - T| - 0.5)| * 100)
rgb = C2.rgb*C2.w*D.rgb + (C.rgb*dot((0.2,0.8,0.5),b) + C2.rgb*dot((0.8,0.2,0.5),b)) * D.a * C.w,   alpha 0
```
Unlit. Animates: thin bright pulses run along the circuit traces (D.a), one pass per 20 s per channel phase.

### fish_phong_ref (nature: 48 fish)
VS: swim/bend deformation (sin of `2*pi*ModelWorld element - 2y`, bend radius from another matrix element) - driven by the
object's matrix, not Time. PS = phong_ref with: diffuse `D.bgr`, gloss `D.r`, reflect strength `D.b - 0.3` (unclamped, can
subtract), reflection `3*C.w*(R.r*C.r, R.b*C.g, R.g*C.b)` (half-vector sphere map), Emit = black_square.

### phong_ref_cube (core_hall)
```
R    = world reflect(viewDir, normal-mapped n);  cube = texCUBE(core_ref.ddsc, R)
rgb  = L.rgb*(0.5*D.rgb + sat((D.a-0.5)*cube.rgb)) + normalize(L.rgb)*L.a*D.a + E.rgb*mix(1, C.rgb*C.w, E.a)
alpha= 2 + E.a
```
The reflection is lit by the light buffer (multiplied), not added.

### phong_ref_spec (tunnel2, stomper; Specular1, ReflectStrength1, ReflectMap1)
```
rgb  = L.rgb*D.rgb + normalize(L.rgb)*L.a*S.rgb + E.rgb*mix(1, C.rgb*C.w, E.a)
h    = normalize(normalize(n_bump) + normalize(-Pv));  R = ReflectMap(h.xy*(0.5,-0.5)+0.5)
rgb += R.rgb*RS.rgb            (no x2, no clamp)          alpha = 2 + E.a + R.a*RS.a
```

### glass (Additive; Diffuse1 = glassroad.png / tunnel_emit.dds, Diffuse2 = pcb.tga; GlassPrepass writes its normals so L exists)
```
p    = D2(uv*3.5)
rgb  = (dot(D.rgb,(1,0.5,0.2))*C.rgb*C.w + 0.3*L.rgb*p.rgb + normalize(L.rgb)*L.a*p.r) * fade
alpha= (1.3 + dot(D.rgb,(1,0.5,0.2))*C.w^2) * fade
fade = 1 - smoothstep(0,1, (viewDepth - (FogNear+FogFar)/2) / (SkyFar - (FogNear+FogFar)/2))   // same fade in additive/traffic
```

### traffic (Additive)
VS: `t = T + fmodh(0.32*ObjectID)` (per-object phase). PS:
`rgb = D(uv).r * (D(uv + (-5,0)t).g + 0.4*D(2uv + (-5,0)t).b) * C.rgb*C.w * fade`, alpha `C.w^2*D.r*m*fade`.
Animates: light streaks scroll at 5*0.05 = **0.25 uv/s** (the 2x-tiled layer 0.125 mesh-uv/s). (jet_flame, same scheme:
`t = T + fmodh(0.32*ID)`, offsets (-10,10,-17,7)*t -> 0.5 / 0.85 uv/s.)

### road
VS passes the mesh vertex colour vc. PS:
`rgb = LIT(D,D.a) + E.rgb*mix(C.rgb*C.w, C2.rgb*C2.w, vc.r)`, `alpha = 2 + E.a*mix(C.w^2, C2.w^2, vc.r)`.
vc is a 0..1 gradient on most road pieces (road_curve_up*, road_rot_*, road_turn, road_bankcurve: rgb 0..255; road_strait,
road_curve, road_turn2-4, road_bank*: alpha 0..255 which the PS ignores) - it blends Color1 into Color2 for colour transitions.
The Shaded pass ignores Normal1 (only the GBuffer pass normmap.fx uses it).

### building
`g = D.a*0.2*D2(4uv).r*(1-E.r)`; `rgb = L.rgb*(D.rgb+0.2g) + normalize(L.rgb)*L.a*3g + E.rgb*C.rgb*C.w`, alpha `2 + C.w^2*E.a`.
No building material sets Diffuse2; the fx default ResourceName `default_color.dds` is not in the game data, so the gloss/detail
term depends on the engine's fallback texture (not determined). Visually = phong_emit without spec.

### phong_emit / phong_ref / simple_emit / additive (base set)
- phong_emit: `rgb = LIT(D,D.a) + E.rgb*C.rgb*C.w`, alpha `2 + C.w^2*E.a`.
- phong_ref: `rgb = LIT(D,D.a) + E.rgb*mix(1, C.rgb*C.w, E.a) + sat((D.a-0.5)*2R)` with **R = Reflect(h.xy*(0.5,-0.5)+0.5),
  h = normalize(normalize(n_bump) + normalize(-Pv))** (half vector between normal and eye direction, view space).
- simple_emit: `rgb = C.rgb*C.w`, alpha 1.
- additive: `rgb = D.rgb*C.rgb*C.w*fade`, alpha `C.w^2*fade` (D.a unused).
All Emit1 textures checked (15 most used materials) have alpha = 1, so `mix(1, C, E.a)` = C everywhere in practice.

## 3. Vita port: global problems (gfx.c / nr_pack.py)

1. **Time base.** `gfx_time = R.t` (seconds, reset each race). The shaders' T is `fract(0.05*t_real)`. Add one global
   `gfx_T = fmodf(gfx_T + dt*0.05f, 1.0f)` (accumulate every frame, real dt) and use it where the fx use Time.
   Traffic currently scrolls 20x too fast (`uSc.w = gfx_time` -> use `gfx_T + fract(0.32*id)`); jet_flame uses
   `gfx_time*0.32` (6.4x too fast; the 0.32 multiplies ObjectID, not time) -> `gfx_T (+ per-object fract(0.32*id))`.
2. **AudioBeat.** `uBeat.x = clamp(1 - fmod(beat_t, period))` is the LightPulse factor; the beat shaders need the raw seconds
   since the beat: pass `uBeat.y = fmodf(beat_t, beat_period)` (unclamped).
3. **Mesh vertex colour is multiplied into Color1** (nr_pack ~l.563 `vc = v['c']/255*col`, incl. alpha into glow). The
   original never does this. Worst case is road: road_strait/curve/turn2-4 vertex alpha 0..255 halves/zeroes the road glow,
   road_curve_up*/rot_*/turn/bankcurve rgb 0..255 darkens it. Fix: use `col` alone for every shader except road (below).
4. **Color per material slot.** `col = cols.get(slot, cols.get(1))` gives submesh 2 the RenderMesh Color2. In the game every
   submesh gets Color = Color1 and Color2 = Color2. Use `cols[1]` for vCol and keep `cols[2]` as Color2.
5. **Glow range / packing.** vcolour clamps w to 8; the game allows 0..25.5 (turbine 25.5 on 19 objects, corecore C2.w 10).
   Also reproduce the wraps: `rgb = ((int)(x*255)) & 255`, `w = (((int)(w*10)) & 255)/10` (blueprint ramp C2.w 30 -> 4.4,
   Car C2 (1,-1,-1) -> (1,1/255,1/255)). Store glow as w/32 (or w/25.5) instead of w/8 if precision allows.
6. **Float1..3 and Color2 are never read** (`read_rendermesh` skips `<Float name="FloatN">`; Color2 only by slot). They are
   per object (ramps: 13 distinct (Color2,F) per level max, F mostly (30,0,8.25)). Minimal: add Color2 and F1..F3 to the
   material key for shaders that use them (phong_newpulse, pulse_veins, road, city, tronics, corecore, phong_ref_pulse) and
   set them as uniforms `uC2`, `uP` per material -> a few more materials/draws, no vertex format change.
7. Classification (classify()): phong_newpulse, phong_emit_anim, corecore, city, tronics -> KIND_LIT (wrong for all but block);
   pulse_veins -> plain additive; corecorecore(2) -> KIND_REF; phong_ref_spec -> KIND_REF but `Reflect1` is absent
   (it uses ReflectMap1), so it gets no reflection; phong_ref_cube's Reflect1 is a cube map (`core_ref.ddsc`).
8. No specular anywhere (L.a term). Optional; a per-vertex sun/light spec could be baked but the effect is mostly small highlights.

## 4. Per-shader port deltas and minimal GLSL

Proposed shared additions: per frame `uniform vec4 uT;` = (T, AB, fract(5T), 0); per material `uniform vec4 uP;` and
`uniform vec4 uC2;` (Color2 rgb, w). Extra textures go on unused units (Anim1/Tron1/Diffuse2 -> unit 1 or 2 when the material
has no Emit1/Reflect1), so no new samplers. Keep coordinates straight from varyings (non-dependent reads).

- **phong_newpulse** (biggest visible miss: ramps show the whole emit texture tinted by Color1, chevrons never pulse; Lava with
  C.w = 0 shows no glow). New `FS_PULSE` with VS_WORLD; CPU sets `uP = (fmodh(F1*T + F2), F3, 0, 0)` per material:
  ```glsl
  vec4 d = texture2D(uDiff, vUV); vec4 e = texture2D(uEmit, vUV);
  float k = clamp(1.0 - abs(abs(e.b - uP.x) - 0.5) * uP.y, 0.0, 1.0) * e.g;
  vec3 c = d.rgb * vLit + e.r * vCol.rgb * vCol.a + k * uC2.rgb * uC2.w;
  ```
  No extra reads. Ramp: 1.5 Hz, C2.w 4.4.
- **pulse_veins**: `FS_VEINS` (additive), CPU: `uP = (fmodh(1.66 - 4*AB), 1.1 - 4*AB)`:
  ```glsl
  vec4 t = texture2D(uDiff, vUV);
  float a = 1.0 - 10.0 * abs(abs(t.g - uP.x) - 0.5);
  vec3 c = clamp(t.g * t.r * uP.y * vCol.rgb, 0.0, 1.0) * vCol.a
         + (a >= 0.0 ? clamp(t.g * a * t.r * uC2.rgb, 0.0, 1.0) * uC2.w : vec3(0.0));   // no fog fade
  ```
- **phong_emit_anim**: FS_LIT + Anim1 on unit 2 (uRefl), one extra non-dependent read:
  ```glsl
  vec4 an = texture2D(uRefl, vUV);
  vec3 b = max(1.0 - 12.5 * abs(abs(an.rgb - uT.z) - 0.5), 0.0);
  c += clamp(an.a - 0.7, 0.0, 1.0) * 20.0 * (b.r + b.g + b.b) * vec3(0.2, 0.8, 0.2);
  ```
- **block**: bake the uv tiling in nr_pack (per vertex, object-space normal and `S` already there):
  `uv *= (ceil(sz/4), ceil(sy/4))` if |n.x|>0.1, `(ceil(sz/4), ceil(sx/4))` if |n.y|>0.1, else `(ceil(sy/4), ceil(sx/4))`
  (sx,sy,sz = object scale, n = object-space normal). Shader stays FS_LIT.
- **phong_ref_pulse (car)**: FS_REF is right (normal sphere map, `mix(1,tint,e.a)*C.w`). Missing only the static red stripes:
  `c += clamp(1.0 - abs(fract(3.0*vUV.x) - 0.5) / 0.05, 0.0, 1.0) * e.a * e.r * vec3(4.5, 0.018, 0.018);` (optional; general form
  `abs(abs(fract(F3*u) - fmodh(F1)) - 0.5)/F2`, x Color2.rgb*Color2.w*1.5).
  Gold: tint the (absent) spec and use reflection x2.4 with city_ref_gold.
- **phong_ref** (checkpoint, Crusher, Tube, stomper2, humvee): sphere coordinate should use the half vector. In VS_WORLD/VS_OBJ
  replace SPHERE_CODE's input: `vec3 h = normalize(n + normalize(uEye - w)); vec3 vn = vec3(dot(uV0.xyz,h), dot(uV1.xyz,h), dot(uV2.xyz,h));`
  (per vertex, free). Keep the plain-normal version for phong_ref_pulse (car) - give it a flag.
- **phong_ref_spec**: in nr_pack use `ReflectMap1` when `Reflect1` is missing, and bake ReflectStrength1 into the diffuse
  alpha as `d.a = 0.5 + 0.5*lum(RS)` so FS_REF's `clamp(r*(d.a-0.5)*2)` = `r*lum(RS)` (D.a is unused by this fx). Half-vector
  sphere coords as phong_ref.
- **phong_ref_cube**: needs the cube map (`core_ref.ddsc`, GL_TEXTURE_CUBE_MAP in vitaGL). Per-vertex `vR = reflect(w - uEye, n)`
  (world), FS: `c = vLit*(0.5*d.rgb + clamp((d.a-0.5)*textureCube(uCube, vR).rgb, 0.0, 1.0)) + emissive`. Cheap fallback: drop the
  reflection and use `0.5*d.rgb*vLit` (the current FS_REF samples a cube file as 2D = wrong/black).
- **tvscreen / tvscreen_red**: FS_TV has the 0.35*C.w picture term right for blue posters but red posters need 0.5; the
  `d*vLit*0.15` term does not exist in the game (screens get only a specular highlight). Add the static noise (static.png is
  already bound as uEmit) with per-vertex scrolled uvs (needs one extra varying, e.g. `varying vec4 vX;` added to VARYINGS for
  all programs): VS `vX = vec4(aUV + uT.x*vec2(1.3,-3.0)*kObj, aUV + uT.x*vec2(17.3,-120.123));`
  ```glsl
  float n1 = texture2D(uEmit, vX.xy).g; vec3 n2 = texture2D(uEmit, vX.zw).rgb;
  vec3 c = texture2D(uDiff, vUV).rgb * (uTV.x * vCol.a) + uTV.y * n1 + uTV.z * (n2.r - n2.b);   // blue (0.35,0.05,0.1), red (0.5,0.25,0.3)
  ```
  Chromatic split (optional): 2 more reads at per-vertex offset uvs (drop the J*n1 jitter, it makes the reads dependent).
- **city**: currently lit diffuse with mesh uv. Exact version needs Po as a varying (vX.xyz) and the planar uv
  `Po.xy*(0.5,-0.5)+0.5` baked as aUV (city.tga and Tron1 both use it), edge flag from the mesh uv baked per vertex (or skip).
  Cheap version: `c = d.rgb*uC2.rgb*uC2.w*0.25 + glow` (windows 59% of area at 0.25, lines 0, uv edges 1), glow from
  `b = clamp(1.0 - abs(abs(a.rgb - uT.x) - 0.5)*10.0, 0.0, 1.0)` times `clamp(1.0 - 15.0*Po.z, 0.0, 1.0)` (base only). Unlit.
- **tronics**: bake `aUV = (w.xy + 3000)*(0.0015, -0.0015)` in nr_pack (static world geometry), Tron1 on unit 1:
  ```glsl
  vec4 a = texture2D(uEmit, vUV); vec4 d = texture2D(uDiff, vUV);
  vec3 b = clamp(1.0 - abs(abs(a.rgb - uT.x) - 0.5) * 100.0, 0.0, 1.0);
  vec3 c = uC2.rgb * uC2.w * d.rgb + (vCol.rgb * dot(vec3(0.2,0.8,0.5), b) + uC2.rgb * dot(vec3(0.8,0.2,0.5), b)) * d.a * vCol.a;
  ```
  Unlit (no vLit). At 100x sharpness the band may alias on the Vita; 30..50 is a safe visual substitute.
- **glass**: FS_ADD's grey mix `dot(t.rgb,(1,0.5,0.2))*C*C.w` is right. Missing: `+ 0.3*L*pcb` lighting of the circuit pattern.
  Needs light baked for glass (nr_pack only bakes for LIT/REF/TV) and pcb (Diffuse2) at uv*3.5 from the VS (use vRef = aUV*3.5
  for the glass program): `c += 0.3 * vLit * texture2D(uEmit, vRef).rgb;` then the fade.
- **traffic**: formula correct; fix the time base (item 1). Optional per-object phase.
- **road**: in nr_pack, per vertex `m = mix(C1.rgb*C1.w, C2.rgb*C2.w, vc.r)` (vc.r = mesh vertex colour red, ignore its alpha),
  store `vCol = (m/max(m), max(m))`. FS_LIT unchanged.
- **building / phong_emit**: formulas match FS_LIT (E.a = 1 makes the `mix(1,C,e.a)` harmless). Only deltas: specular, and
  items 3-5. building's gloss/detail can stay omitted.
- **simple_emit, additive**: match (`C.rgb*C.w`; `t.rgb*C.rgb*C.w*fade`). Additive's fade starts at (FogNear+FogFar)/2 on view
  depth and ends at SkyFar; the port's `1 - vFog.y` starts at FogFar and includes height fog - small difference.
- **corecore / corecorecore / corecorecore2 / fish_phong_ref**: single level each; port as listed in section 2 if wanted.
  Cheapest faithful-looking: corecorecore(2) = unlit `(1 - cube(R))*C*C.w` (+0.5*vLit for corecorecore) with a per-vertex
  pseudo-random scale jitter `1 + 0.5*noise(2.27*t)`; corecore = building + `(C.w + 1.9 - 4*AB)*E*C` + Anim bands x C2.
