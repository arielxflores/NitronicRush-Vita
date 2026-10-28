# Nitronic Rush RE, group C: lights and glowing objects

All addresses are VAs in NitronicRush.exe. `C` = component `this`, `C+4` = owner GameObject.
Component ids for `GetComponent(id)` (0x66e730), from the factory registration table (0x735000..):
**Light = 0xb, RenderMesh = 0x47**, BodyRigid = 0x10, Transform = 0 (returns GO+0x6c),
SimpleStreetLightPulseOnOffLogic 0x29, SimpleStreetLightFlickerLogic 0x2a, BlockerLogic 0x2b,
ScalePulseLogic 0x41, DistanceColorChange 0x7a, LightPulse 0x7f, ColorPaletteComponent 0x9c,
StreetlightBreaker 0x6a. The full table is in `$HOME/nrw/compids.txt`.

Global mode `[0x7ff754]+8`: the logics treat **0 = playing**, **2 = editor/dev-pause**
(it matches the Play/Pause/DevPause attributes, so 1 = pause). Flicker logic runs only in 0.
LightPulse and PulseOnOff run in any mode except 2. ScalePulse uses a real-time dt in mode 2.

## 0. Shared data layouts

### Light component (id 0xb, vtable 0x7b0d84, ctor 0x5f6830, serialize 0x5f5950)
| off | field | notes |
|---|---|---|
| +0x3c..+0x48 | `Color` Vector4 | rgb, **w = intensity** (+0x48). LightPulse and flicker write +0x48 |
| +0x4c | Specular Strength | |
| +0x50 | Ambient Strength | |
| +0x54 | Attenuation Near | |
| +0x58 | Attenuation Far | |
| +0x60/+0x64 | Quad Width / Quad Height | only when Type==4 |
| +0x74 | Type | 0 = point, 3 = tube, 4 = quad, 1 = spot(?), sun |
| +0x98 | dirty byte | set to 1 by slot 9 (initialize) |
Slot 11 (0x5f70e0) links the light into the renderer light list (`[0x7ff6c4]`, 0x72dfa0).

### RenderMesh component (id 0x47, vtable 0x7b1074, ctor 0x5f3210, size 0x90, serialize 0x5f3900)
| off | field | encoding |
|---|---|---|
| +0x55 | visible/enabled byte (inferred) | BlockerLogic sets 0 to hide, 1 to show |
| +0x5c | override flags | bit0 Color1, bit1 Color2, bit2..4 Float1..3 |
| +0x60 | **Color1** packed D3DCOLOR | +0x60 = B*255, +0x61 = G*255, +0x62 = R*255, **+0x63 = (int)(w*10)** |
| +0x64 | **Color2** | same layout (+0x67 = w*10) |
| +0x68/+0x6c/+0x70 | Float1/2/3 | |
Loading: `byte = (int)(f*255)` for rgb and `(int)(w*10)` for alpha (both truncate). The XML
`Color1` comes from the legacy StaticMesh material `Color` param, then the RenderMesh `Color1`
overrides it. Glow therefore has a range of 0..25.5 in steps of 0.1.
The logics read the glow back as `byte*0.1f` (0x7b150c) and write it as `(int)(g*10.0f)` (0x7b158c).
Renderer (0x60c2fe..): the packed colour is unpacked to the float4 `(R/255, G/255, B/255, A*0.1)` and
set on the effect's Color1 handle ([+0x90]) and Color2 handle ([+0x94]). The instanced VS does the same
(constant 25.5 on the D3DCOLOR alpha).
**What w does in the shaders**: `building` (and similar) PS: `emissive = EmitTex.rgb * Color.rgb * Color.w`
is added to the lit colour, and `oC0.a = EmitTex.a*Color.w*…` feeds bloom. `additive` PS:
`rgb = tex.rgb*Color.rgb*Color.w`, `a = Color.w^2`, times the fade. In both, w is the
glow/emissive multiplier and drives bloom.

### How Light values reach rendering (answer: dynamic, per pixel, they light the surroundings)
The renderer uses deferred **light pre-pass** shading. Every active Light component is in the gfx light
list and drawn as a light volume (`lightsphere.msh` / `lightcone.msh` / `lighttube.msh`) with effects
`light_point/spot/sun/tube/quad.fxo` into `LightBuffer`. Material shaders sample it per pixel
(`LightSampler`, the screen-space light buffer). Per light, `LightData` = 0x30 bytes copied from Light+0x3c
(0x60c146): `color(rgb, w=intensity)`, `spec_amb_near_far`, then the type-specific vectors.
Decoded light_point PS:
```
d   = |Lpos - P|;  L = (Lpos-P)/d
att = clamp(near*(far-d) / (d*(far-near)), 0, 1.5)
diff= (NdotL>0 ? NdotL : 0) + ambient
spec= (NdotL>0 && RdotV>0) ? pow(RdotV,30) : 0           // R = reflect(view,N)
out.rgb = diff * att * Color.rgb * Color.w   (accumulated)
out.a   = spec * att * SpecularStrength
```
So **pulsing Light.Color.w changes the lighting on nearby road and building geometry** (per pixel,
within Attenuation Far). The object's own glow is separate: RenderMesh Color1.w (alpha byte).
The pulse logics drive both.

### Music-beat clock (used by LightPulse)
AudioLogic (vtable 0x79bf28, singleton `[0x7ff6ec]`, .\AudioLogic.cpp). Every level-music event
(Play_TurboTunnel, Play_EnterTheMatrix, Play_Gladiator, …; switch at 0x6dc9xx) is posted with callback
flags 0xf01 and callback 0x6dc6c0:
`if (type == AK_MusicSyncBeat(0x100) && g_audio) g_audio->beatFlag(+0x25) = 1;` (bars, 0x200, are ignored).
AudioLogic update (0x6dd210): `if (beatFlag) { beatFlag = 0; timeSinceBeat = 0; } else timeSinceBeat += dt;`
which is **`[0x7ff6ec]+0x28` = seconds since the last music beat**. It is reset to 0 at level init (0x6dceed).
The same value is fed to shaders as `AudioBeat` (0x60ac5c; corecore*, pulse_veins.fxo).
Tempo hints (heuristic parse of the playlist AkMeterInfo in Main.bnk; values of 120 may be Wwise defaults):
TurboTunnel 115, EnterTheMatrix 132, Gladiator 150, PurpleEighties 135, Nitroception 130,
GreenRmx 105, ItsChristmas 85, StuntMode_1 132, Level1Playlist2/3 128, Core8bit/CoreHeart/BlueWubstep/
Nature/NewPurple/CoreFinal/CreditsPlaylist 128, FirstPlayable/NeonSkyway/BetaRed/Finale/PinkClockwork 120.

---------------------------------------------------------------------------------------------------
## 1. LightPulse (id 0x7f, vtable 0x797114, size 0x40)
Used very widely: tunnellighttube_pulse, streetlight_pulse, arrow2pulse, lightpointpulse (Light only),
spotlight_pulse, ramp_pulse, widebloom (RenderMesh only). There are about 1450 instances.
Fields:
| off | name | type | default |
|---|---|---|---|
| +0x3c | Pulse Intensity | Float | serialized (1.0 in about 1200 instances, 1.5 in streetlight_pulse/widebloom, also 1.1, 1.15, 1.25) |
| +0x34 | baseLightIntensity | runtime | |
| +0x38 | baseGlow | runtime | |
```c
void LightPulse::Initialize(int)            // slot 9, 0x68ceb0
{ Light* L = GetComp(0xb); RenderMesh* R = GetComp(0x47);
  if (L) baseLight = L->color.w;                     // +0x48
  if (R) baseGlow  = R->color1A_byte * 0.1f; }       // +0x63
void LightPulse::VUpdate(float dt)          // slot 15, 0x68cf00
{ if (g_mode == 2) return;
  Light* L = GetComp(0xb); RenderMesh* R = GetComp(0x47);
  float k = clamp(1.0f - g_audio->timeSinceBeat, 0.0f, 1.0f);   // linear 1-second decay after each beat
  float f = PulseIntensity * k;
  if (L) L->color.w = baseLight * f;
  if (R) R->color1A_byte = (uint8)(int)(baseGlow * f * 10.0f); }   // rgb untouched
// slot 16 (0x68ce70): restore L->color.w = baseLight; glow byte = (int)(baseGlow*10)
// slot 17 (0x68ce20): baseLight = L->color.w / PulseIntensity; baseGlow = byte/PulseIntensity*0.1
//   (editor/reset hooks; caller not identified, not needed at runtime)
```
The waveform is a sawtooth synced to the music. On each beat the brightness jumps to `PulseIntensity × authored`
and then falls linearly to 0 over 1 s. At 128 BPM (beat every 0.469 s) the factor runs from 1.0 down to about 0.53
(× PulseIntensity). There is no per-object phase and no randomness: every pulsing light in the level flashes
together on the beat. No sounds.
**Port:** keep one global `timeSinceBeat` (reset on each music beat; without Wwise, take beats from the track
BPM and start offset), then apply the two writes above. Driving Light intensity must change the dynamic light
contribution, not only the emissive.

## 2. SimpleStreetLightFlickerLogic (id 0x2a, vtable 0x795b94, size 0x60)
Used on `streetlight` and `streetlight_pulse` (Tutorial, L1, L3, L4, L5). Serialize 0x6cabf0.
| off | name | type | default (bp) |
|---|---|---|---|
| +0x44 | Amount Of Time The Light Is Normally On For | Float | 2.0 |
| +0x48 | Amount Of Time The Light Is Flickering For | Float | 10.0 |
| +0x50 | Flicker Mod Number | Integer | 10 |
| +0x34 base (Light.w), +0x38 baseGlow, +0x3c state (0 = on, 1 = flickering), +0x40 sub (0 = ramp up, 1 = ramp down), +0x4c timer, +0x54 cur, +0x58 ramp, +0x5c rampDur |
```c
void Init(int)                // slot 9, 0x6cae90 (no null checks)
{ baseGlow = R->c1A*0.1f; base = cur = L->color.w; timer = 0; state = sub = 0; ramp = rampDur = 0.1f; }
void VUpdate(float dt)        // slot 15, 0x6caf00
{ if (g_mode == 2) { /* editor: recapture base/baseGlow/cur, state=sub=0, timer=0, ramp=rampDur=0.1 */ return; }
  if (g_mode != 0) return;
  timer += dt;
  float r=R->c1R/255.f, g=..., b=...;               // read back, rewritten unchanged (no drift)
  if (state == 0) {                                  // "normally on"
     L->color.w = base; R->c1A = (int)(baseGlow*10);  R->rgb = rgb*255;
     if (timer > NormallyOnTime) { state = 1; sub = 0; timer = 0; ramp = rampDur = 0.1f; }
     return; }
  if (state == 1) {
    if (sub == 1) {                                  // fade out over rampDur (0.1 s)
       ramp -= dt; cur = clamp(ramp/rampDur*base, 0, base);
       if (ramp <= 0) { sub = 0; ramp = 0; } }
    else if (sub == 0) {                             // fade in 3x faster (0.033 s)
       ramp += dt*3.0f; cur = clamp(ramp/rampDur*base, 0, base);
       if (ramp >= rampDur) {
          if ((unsigned)rand() % (2*FlickerMod) == 0) {        // CRT rand(); p ~ 1/20 per frame
             if (timer > FlickeringTime) { state = 0; timer = 0; }
             ramp = rampDur; sub = 1;                         // start next dip
          } } }                                       // else stay fully on and retry next frame
    L->color.w = cur;
    R->c1A = (int)(cur/base * baseGlow * 10.0f);  R->rgb = rgb*255; } }
// slot 16 0x6cada0: restore L.w = base, c1A = baseGlow*10.
// slot 17 0x6cad30: recapture like Init but ramp = rampDur = 0.03f (0x7b16a0).
```
The pattern is random: 2 s steady, then a 10 s flicker phase. In the flicker phase the light stays full on for a
geometric number of frames (each frame has a 1/(2·Mod) = 1/20 chance to end; mean 20 frames, about 0.33 s at
60 fps), then dims linearly to 0 in 0.1 s and comes back in 0.033 s. The flicker phase can only end at a dip
decision once 10 s have passed; the dip that started there still plays. The flicker is frame-rate dependent.
No sounds. Both Light intensity and glow alpha are scaled by the same factor.
**Note on streetlight_pulse:** it has both Flicker and LightPulse. Logic components are probably updated from one
list in activation order (0x667cd0 appends to the logic list `[0x7ff6b4]`; blueprint order puts LightPulse after Flicker). LightPulse
writes last and uses its own base, so the flicker is probably overridden on that blueprint.
**Port:** run per frame at a 60 Hz-equivalent probability (or `p = 1-(19/20)^(dt*60)`), using any RNG.

## 3. SimpleStreetLightPulseOnOffLogic (id 0x29, vtable 0x795b3c, size 0x3c), unused
No serialized fields (serialize 0x6ca880 = `ret 8`), and it appears in **no level or blueprint**.
`Init (0x6cac50): t(+0x38) = xorshift128 random float` (0x429540: u32 * 2^-15, so a random phase in [0, 131072)).
`VUpdate (0x6cb210)`: mode 2 → `base(+0x34) = L->color.w; t = 0`. Otherwise
`L->color.w = (sin(t) + 1.25f) * 0.5f * base; t += dt;` (period 2π s, range 0.125..1.125 × base).
Light only. `base` is set only in editor mode (uninitialised in pure play). Slot 16: `t = 0; L.w = 0.625*base`.
**Port:** can be skipped.

## 4. ScalePulseLogic (id 0x41, vtable 0x79617c, size 0x50)
Used on `antenna_light` (additive flare quad, material antenna_light/flare.png), L1 ×7, L2 ×13, L4 ×1, Arena.
Level instances have empty overrides, so the blueprint values apply.
| off | name | type | bp default |
|---|---|---|---|
| +0x34 | t_val_ | Float | 1.0 |
| +0x38 | min_scale_ | Float | 5.0 |
| +0x3c | max_scale_ | Float | 10.0 |
| +0x40 | min_brightness_ | Float | 1.0 |
| +0x44 | max_brightness_ | Float | 2.0 |
| +0x48 | t_inc_ | Boolean | 0 |
| +0x4c | speed_ | Float | 6.0 |
Initialize (slot 9) = no-op. Slot 7 = AntTweakBar registration (" step = 0.05 ").
```c
void VUpdate(float dt)        // 0x6c09f0
{ GameObject* cam = current render camera (gfx[0x7ff6c4]: list +0x557804, idx/gen +0x5577fc/+0x557800);
  if (!cam) return;
  if (g_mode == 2) { draw debug circles r=min_scale_, max_scale_ facing camera; dt = realFrameTime([0x7ff774]+0x30); }
  float step = (0.5f - fabsf(t - 0.5f) + 0.05f) * speed_ * dt;   // fast in the middle, slow at the ends
  if (t_inc_) { t += step; if (t > 1) { t = 1; t_inc_ = 0; } }
  else        { t -= step; if (t < 0) { t = 0; t_inc_ = 1; } }
  float u = 1 - t;
  if (R) R->c1A = (int)((min_brightness_*u + max_brightness_*t) * 10.0f);
  float s = min_scale_*u + max_scale_*t;
  Transform* T = owner+0x6c;  T->scale(+0x84,+0x88,+0x8c) = (s,s,s);
  T->orientation(+0x90, 9 floats) = cam->orientation(GO+0xfc);  T->dirty(+0x148) = 1;   // billboard
}
```
It is a smooth triangle-like ping-pong: 0→0.5 takes ln(11)/speed = 0.40 s, so a full 0→1→0 cycle takes about 1.6 s
at speed 6. Each object keeps its own t, but all start at t = 1, so the copies stay in sync. Scale 5..10 and
glow 1..2 rise and fall together. The billboard copies the camera's rotation each frame. No sounds.

## 5. BlockerLogic (id 0x2b, vtable 0x795a8c, size 0x60): holographic road blockers
Used on `blocker_holo1..9` (L2 9, L3 14, L4 38, L5 144). Holo: additive mesh, RenderMesh Color1 (1,0.3,0.1,4.5)
(holo8: w 2.0), Phantom "Play_Blockers", OneShotRadiusAudioLogic "Play_Woosh" r=25 (separate components).
| off | name | type | bp values (holo8 / holo4 / holo3 / holo2) |
|---|---|---|---|
| +0x40 | LightUp Distance | Float | 110 / 200 / 200 / 250 |
| +0x44 | Activate Distance | Float | 50 / 150 / 150 / 175 |
| +0x48 | Block Up Speed | Float | 3 |
| +0x4c | Block Height | Float | 1 / 3 / 3 / 5 |
| +0x58 | Brightness | Float | 1.0 / 2.5 / 2.5 / 2.5 |
Runtime: +0x34..+0x3c spawned-block handle (3 dwords), +0x50 light (0..1), +0x54 rise (0..1),
+0x5c upSoundPlayed, +0x5d downSoundPlayed.
```c
void Init(bool reload)       // 0x6f9d10
{ upSnd = downSnd = 0; if (!reload) { handle = 0; light = 0; rise = 0; } }

void VUpdate(float dt)       // 0x6f9da0
{ RenderMesh* R;
  if (dt == 0) { R->c1A = (int)(Brightness*10); R->visible = 1; return; }  // paused/editor: full bright
  GameObject* car = FindObjectByName(*(char**)0x7ff6d4);   // player car
  if (!car) return;
  vec3 P = ownerWorldPos, A = ownerOrient.col2 (m[2],m[5],m[8]);   // local Z axis = "up" of blocker
  float d2 = |P - carPos|^2;
  // --- 1) holo light-up / fade (time-based, 1 s) ---
  if (d2 < LightUp^2) { if (light < 1) { light += dt; R->c1A = (int)(Brightness*light*10); R->visible = 1; } }
  else if (light > 0) { light -= dt; R->c1A = (int)(Brightness*light*10); R->visible = 1; }
  else R->visible = 0;                                   // invisible when far and faded
  // --- 2) physical block ---
  if (d2 < Activate^2) {
     downSnd = 0;
     if (!upSnd) { PostEvent(Play_BlockerUp /*0x9efadacf*/, owner); upSnd = 1; }
     if (handle == 0) {                                 // first frame: spawn the solid block
        name = "blockerX"; name[7] = ownerName[12];     // "blocker_holo4" -> "blocker4"
        GO* B = ObjectFactory([0x7fe804])->Create(name); handle = B->handle;
        B->pos = P - A*2.5f*BlockHeight;  B->orient = ownerOrient;
        B->scale = (ownerScale.x, ownerScale.y, BlockHeight);  B->Activate(0);  return; }
     rise = min(rise + BlockUpSpeed*dt, 1);
     GO* B = Resolve(handle); if (!B) { handle = 0; return; }
     vec3 target = P - A*(2.5f - 5*rise)*BlockHeight;
     BodyRigid* body = B->GetComp(0x10);                // kinematic
     body->vel(+0xe0,+0xec) = A*BlockHeight*BlockUpSpeed*5;
     if (rise == 1) { zero vel/angvel/forces (0x4010b0); B->pos = target; body->pos(+0xf8,+0x104) = target; }
     light = 1 - rise*0.9f;  R->c1A = (int)(Brightness*light*10);  // holo dims to 10% as the block rises
     return; }
  if (handle == 0) return;
  upSnd = 0;
  if (rise > 0) {
     if (!downSnd) { PostEvent(Play_BlockerDown /*0x5ebf93f4*/, owner); downSnd = 1; }
     rise -= dt;                                        // lowering ignores BlockUpSpeed
     GO* B = Resolve(handle); if (!B) { handle = 0; return; }
     B->body->vel = A * (-BlockHeight) * 5;
     light = 1 - rise*0.9f;  R->c1A = (int)(Brightness*light*10);  R->visible = 1;
  } else { DestroyObject(handle) (0x67b200); handle = 0; } }
```
Geometry (assuming blockerN.msh spans ±2.5 on local Z): the block (blueprint `blockerN`: kinematic BodyRigid, ColliderTriMesh `blockerN.mdc`, material
Obsticle, mesh 5 units tall along local Z scaled by H) starts with its top at the holo's origin. It moves up
by 5·H at 5·H·speed u/s (so in 1/speed s = 0.33 s) until its bottom is at the origin and its top at +5H. It
**does block and collide** (a separate solid object). It is spawned on entering Activate Distance, lowered on
leaving it (1 s), then destroyed. The hologram is only visible within LightUp Distance (fades in over 1 s at
`Brightness`) and drops to 10% brightness when the block is fully up.
Sounds: `Play_BlockerUp` (once per entry into the activate radius), `Play_BlockerDown` (once when lowering
starts). The holo object also has a `Play_Blockers` phantom and a `Play_Woosh` one-shot (other components).

## 6. DistanceColorChange (id 0x7a, vtable 0x796f84, size 0x5c)
Used only on `widebloom` (Campaign_Level5 ×1): additive quad, RenderMesh Color1 (1,1,1,2), with LightPulse 1.5.
| off | name | type | bp |
|---|---|---|---|
| +0x34 | color_near_ | Vector4 | (0,0,0,2) |
| +0x44 | color_far_ | Vector4 | (1,1,1,2) |
| +0x54 | dist_near_ | Float | 2000 |
| +0x58 | dist_far_ | Float | 5000 |
Init = no-op.
```c
void VUpdate(float)           // 0x6b9190
{ GameObject* cam = current render camera; if (!cam) return;
  float d = |cam->pos(+0xa8) - owner->pos(+0xa8)|;
  vec4 c = d > far ? color_far : (d < near ? color_near
         : lerp: t=(far-d)/(far-near); color_near*t + color_far*(1-t));
  R->c1R=(int)(c.x*255); c1G=(int)(c.y*255); c1B=(int)(c.z*255); c1A=(int)(c.w*10); }
```
The bloom is invisible (black additive) when the camera is within 2000 units and fully white beyond 5000.
LightPulse on the same object (probably updated after it, since it comes later in the blueprint) then overwrites the alpha
with `baseGlow(2.0)*1.5*k`.

## Other notes
- Every glow write keeps rgb and only changes the alpha byte, except DistanceColorChange (writes rgb too) and
  ColorPaletteComponent (0x6888xx, writes full RGBA of Color1/Color2 at load, not analysed here).
- The streetlights also have StreetlightBreaker (id 0x6a; another group) and OneShotRadiusAudioLogic Play_Woosh_Checkpoint.
- Helper scripts: `$HOME/nrw/findstr.py` (string → refs), `compids.py` (component id table), `lin.py`
  (robust linear disassembly that skips invalid bytes), `lp.py <Comp>` (instances per level/blueprint).
