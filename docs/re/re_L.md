# RE group L: credits screen text (CreditsLevel2) - Nitronic Rush PC

Class CreditsLevel2 (vtable 0x7971e4). Used by BOTH the main-menu "Credits" item (0x6ea84e) and the post-story outro
(GoToCredits 0x724740). The legacy class "CreditsLevel" (0x738d23 registration, vt slot 0x734530) is never launched.
Slots: 17 Enter 0x710930, 19 PostBaseLevelEnter 0x710900 (log + Unlock(30)), 22 PostUpdate 0x710840, 26 OnFinished 0x70f860.
Fields: +0x190 vector<Entry> (Entry = 3 std::string: whole, name, roles; 0x54 bytes), +0x1a8 3 text-object handles (12 B each),
+0x1cc t, +0x1d0 idx, +0x1d4 T1, +0x1d8 T2, +0x1dc level xml, +0x1f8 distanceMode, +0x1fc StartTunnel pos, +0x208 EndTunnel pos,
+0x214 D, +0x218 fadeMode (0 none, 1 in, 2 hold, 3 out).

## 1. Loading (Enter 0x710930, LoadCredits 0x710480, AddEntry 0x70f880, split 0x669a50)
```c
t=0; T1=2.5; T2=3.0; idx=0; fadeMode=1; musicIndex(+0x174)=4 /*CreditsPlaylist*/; Settings["Currently In Credits"]="1";
ifstream f("data/credits.txt");  getline(f, levelXml /*+0x1dc*/, 128);   // line 1 = "Levels/credits_level.xml": metadata, NOT shown
while (f.good()) { getline(f, buf, 128);                                  // text mode: CRLF -> LF
  vector<string> p = split(buf, "$");          // tokenizer, EMPTY TOKENS DROPPED; "" -> 0 tokens -> no entry (final EOF read)
  if (p.size()==1) entries.push({p[0], "", ""});          // whole line ("|" = blank, "Ben Ellinger|  Game Instructor||...")
  else if (p.size()>=2) entries.push({"", p[0], p[1]}); } // name / roles (extra '$' parts ignored)
LoadLevel(levelXml); 0x7155b0(); +0x148=0 (no HUD timer); +0x150=0;
distanceMode=1;  S=Find("StartTunnel")->pos; E=Find("EndTunnel")->pos;   // if either missing: distanceMode=0
k = 1/(2*T1+T2); T1*=k; T2*=k;   /* = 0.3125, 0.375 */   D = |S-E|;        // credits_level: D = 4680.0 (straight along +y)
for (i<3) slot[i] = Spawn("HudTextCredits");  SetTexts(entries[0]);
```
credits.txt -> N = 31 entries: 0,1 blank; 2 Kyle; 3 Andy; 4 Chris; 5-7 blank; 8 Andrew; 9 Jason; 10 blank; 11 Laura; 12 Eddie;
13 blank; 14 Ariel; 15 Nathan; 16-18 blank; 19 Jordan; 20 M.J.; 21,22 blank; 23 Ben/Claude (whole); 24 Rachel/Chris (whole);
25 Disco Tank; 26,27 Special Thanks; 28 DigiPen (c) (whole); 29 Wwise (whole); 30 "||Thanks For Playing!" (whole).
SetTexts 0x710020(e): slot0.Text=e.whole, slot1.Text=e.name, slot2.Text=e.roles; each Text::Relayout (0x607e70); DrawMode=2.

## 2. Per frame (PostUpdate 0x710840, runs every frame)
```c
if (aux(+0x50,double) > 4.0) ChangeLevel("MainMenuLevel");
if (state == 2 /*racing*/) CreditsUpdate(dt);     // 0x710180 - texts frozen in every other state
car = Find("Car")->CarLogic(0x20); car.respawnPos.y = max(car.respawnPos.y, E.y - |Car.pos - E|);   // 0x70f930
```
CreditsUpdate 0x710180 (distance mode = the shipped case; ONE entry visible at a time, all 3 slots share one alpha):
```c
if (idx >= N) return;
float p = clamp(1 - |Car.pos - E| / D, 0, 1);  float f = N * p;  int ni = (int)f;     // trunc
if (ni != idx) { idx = ni; SetTexts(entries[idx]); }
float u = (f - idx) * (2*T1+T2);               // = frac(f), 0..1 within the entry (~151 world units per entry)
if (u < T1)          { fadeMode=1; t=u; }         // alpha = sin(u/0.3125)            0 -> sin(1)=0.8415
else if (u < T1+T2)  { fadeMode=2; t=u; }         // alpha NOT written: keeps last fade-in value (~0.84)
else if (u < 1)      { fadeMode=3; t=u-(T1+T2); } // alpha = sin((T1-t)/T1) = sin((1-u)/0.3125)  0.8415 -> 0
else                 { t=0; }                     // float edge only
ApplySlots();  // 0x70fa10
```
Time mode (only if a tunnel is missing): mode0 -> SetTexts(entries[idx]), mode 1; mode1 t+=dt, >2.5 -> mode2,t=0 (alpha
sin(t/2.5)); mode2 >3.0 -> mode3,t=0 (alpha kept); mode3 >2.5 -> idx++, mode0, t=0 (alpha sin((2.5-t)/2.5)); mode0 alpha=0.
ApplySlots 0x70fa10, for i in 0..2 (W,H = screen px [0x7ff6c4]+0x557a2c/+0x557a30):
```c
txt.Position = (0,0,0); txt.color.a = alpha(fadeMode);  txt.Scale = (0.3*W, 0.3*W);
obj.pos = (0.05*W, i<2 ? 0.85*H : 0.75*H); obj.dirty = 1;     // y-up pixel space (see 3)
```
No slide/scale animation: positions are constant, only alpha fades. Colour rgb stays white.

## 3. Text layout / rendering (Text comp 0x33, vt 0x7b00ac; layout 0x607e70; draw 0x610750; font parse 0x60cbd7)
HudTextCredits blueprint: font hemihead.fnt (BMFont text, 512x512, lineHeight 72, base 57, chars 32..138 contiguous),
colour (1,1,1,1), LetterGap 1.0, WordGap 0.05, LineGap 1.0, no wrap. DrawMode 2 = screen-space HUD list (renderer+0x43c, render
cmd 26 0x6133b5; cmd 24 0x61314c sets the HUD ortho); mode 1 = list +0x424 (cmd 18), mode 0 = not drawn. (re_F2's "mode 2 = centred" is wrong: centring is
runtime flag text+0xcd (x -= 0.5*maxX*sx), right-align +0xcc; both 0 here -> LEFT aligned.)
Glyph metrics are normalised by scaleW/H (512); kerning ignored. Layout in normalised units, pen (cx,cy)=(0,0):
```c
for c in text: if (c=='\n'||c=='|') {cx=0; cy+=72/512*LineGap;} else if (c<32||c>138) continue;  // no advance
  else if (c==' ') cx += 0.05;  /* WordGap, not the font's space */   else { g=glyph[c-32];
  quad x: cx+g.xoff/512 .. +g.w/512;  y: -(cy+g.yoff/512) .. -(cy+(g.yoff+g.h)/512);  uv (x,y)/512 .. (x+w,y+h)/512;
  cx += g.xadv/512 * LetterGap; }     ('%A' etc. = button glyphs 0x7f..0x8a; none in credits)
```
Draw: screenPos = obj.pos + Scale * v (+Position); HUD proj (0x61314c): ndc = (p+0.5)*2/(W,H) - 1, i.e. pixels, ORIGIN
BOTTOM-LEFT, Y UP (D3D9 half-pixel). Shader text.fx: out = tex * Color; texkill if tex.a*Color.a < 0.01. Texture
Textures/Hemihead.tga: RGB = white fill, A = fill + 2 px outline -> white text with black outline (blend assumed SrcA/InvSrcA).
Top-down equivalent, S = 0.3*W, k = S/512 (texel->px), line top Y0 = 0.15*H (whole/name) or 0.25*H (roles), X0 = 0.05*W:
  glyph left = X0 + k*(penX + xoffset), top = Y0 + k*(line*72 + yoffset), size k*(w,h); penX += xadvance; space += 25.6 texels.
960x544: S=288, k=0.5625, X0=48, Y0=81.6 / 136, line advance 40.5 px, space 14.4 px. (1280x720: S=384, k=0.75, 54 px lines.)

## 4. End of the credits
EndTunnel2 has a LevelSwitch component (type 0x39, vt 0x7a22b4, update 0x6a8040): l = R^T(car-pos)/scale (scale 2);
once: if (lx^2+(lz-2)^2 < 81 && |ly| < 19 && lz > 0) Broadcast LevelFinishedMessage(0x13, success=1) -> BaseLevel::Finish
0x714d60 (aux=0, clock=0, state 3/5, car finish pose at EndTunnel, finish camera, vslot 26) -> 0x70f860 state=5.
-> fires ~38 world units before EndTunnel: p~0.9919, idx 30 ("Thanks For Playing!"), u~0.75 -> its alpha freezes at
~sin(3.2*0.25)~0.72 (fade never completes; text stays up). p never reaches 1, so idx>=N / "all shown" never happens.
State 5 (BaseLevel update 0x713fe7): aux += dt; screen brightness [0x557a38] = clamp(3.5 - aux, 0, 1) -> full for 2.5 s,
fade to black 2.5..3.5 s; at aux > 4.0 s PostUpdate switches to "MainMenuLevel". (brightness is a post shader param;
text.fx does not use it - whether HUD text is dimmed is unverified.) Race HUD is hidden while "Currently In Credits" is set
(0x712040(0) at 0x7123c8). Also note: the level itself has 3D billboards credits_<name> (board5x7.msh, materials credits_*).
