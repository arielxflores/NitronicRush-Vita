# re_M - why the L6 black "tunnellighttube_pulse" slab looks fine on PC (and why it hides the car in the port)

Object: Campaign_Level6.xml line 4763, tube.msh scaled (194.6, 1210.4, 1) at (-1.91, 1341.56, -7.60), Color1 = 0, Light 0.
World box: x -18.77..14.94, y 131.2..2551.9, z -7.70..-7.50. Hall floor (hall_tall, local z -28) at z = -8.74.

## 1. The PC draws it (opaque black). Nothing hides it.
- No colour test anywhere. Pass draw 0x72d1f0 skips an item only if RenderMesh +0x54 == 0 (frustum cull) or
  +0x55 == 0 (logic "visible") (0x72d4a5 / 0x72d4af). Item build 0x5f2af0 and per-object setup 0x60c000 have no early-outs.
- +0x54: every frame 0x613b46 sets RM+0x54 = 1 (and Light+0x98 = 1) for all CulledStatic objects, then
  0x5ecd90 walks a BVH with the 6 frustum planes (0x5ec9c0, AABB p/n-vertex test 0x5ec660) and 0x5ecca0 sets the
  byte to 0 for culled leaves. The leaf AABB (0x5f7a10) = the mesh's stored AABB (mesh+0x6c) transformed by the full
  scaled transform (GO+0xf0 scale x GO+0xfc orientation); node bounds = union of members (SAH build 0x5ece10).
  So large scale is handled correctly. Leaf byte pointer = RM+0x54 / Light+0x98 (0x5ed9f4..0x5eda26).
- LightPulse (0x68cf00) only rescales Light.w and RM+0x63 (glow byte): 0 stays 0. Light never touches RenderMesh.
- RenderMesh VLoad 0x5f3900 loads Color1 = 0 normally (bytes (int)(x*255), (int)(w*10), flag bit0).
- Pipeline (0x72cc30): GBufferBoth, GlassPrepass, ..., Light, [Z clear 0x612a7b], Depth, Shaded, PostShaded, Additive.
  emit = norm.fx / depth.fx / simple_emit.fx; all three: ZENABLE 1, ZWRITE 1, no blend state (opaque).
  simple_emit PS: rgb = C.rgb*C.w (= 0), a = 1. => opaque black, writes depth.

## 2. No collision.
StaticMesh (vt 0x7b0ab4) is inert: VLoad 0x5f80d0 = `ret 8`, register/unregister = `ret`, init 0x5f8120 only sets
defaults. CulledStatic (vt 0x7b0b2c) only links into the cull list (0x5f7960) and the BVH. Only Collider* components
(0x11-0x1a) collide; this object has none.

## 3. Why it is harmless on PC: it is the black backdrop under a kill-laser floor
Same layer: `killblock_laser` at (-1.67, 1306.42, -105.96), scale (33.76, 10000, 100): block.msh (+-1), material
GroundLaser (Additive pass; ONE/ONE, ZWRITE 0), ColliderOrientedBox Size (1,1,1) "Obsticle", Material ID 1 (instant death).
**ColliderOrientedBox Size is a HALF extent**: init 0x636470 `h(+0xb8) = GO.scale * Size`; AABB 0x6353c0
`= c +- |R| h` (0x671ac0 = fabs(R)); support fn 0x634fd0 returns (+-h.x, +-h.y, +-h.z).
=> kill box x -35.4..32.1, y -8694..11306, z -205.96..**-5.96**; the red laser surface (visual block top) is at -5.96 too.
The hall floor (-8.74) and the slab (-7.70..-7.50) are both inside/below it. The player is launched by ramp_pulse
(y 39..158, top z 4.4) and flies over the laser to the core (y 2573, z 65); touching the laser kills. The slab only gives
the additive red laser a black background (instead of the lit hall floor). The car can never be at its height alive.

## Port bug (the real cause)
`tools/nr_objs.py box_tris`: `h = size * 0.5`, and `src/nr_objs.c` ~l.123-126 builds corners at +-0.5*Size.
Both treat Size as a FULL extent: the port's killblock_laser is +-50 tall -> top at -55.96, below the floor, so the car
survives on the floor at -8.74, under the black slab. Fix: corners at +-Size (times scale), i.e. drop the 0.5.
(The port's own HV_HALF for the humvee box already assumes half extents.) Affects every placed ColliderOrientedBox:
building2 24, SlidingBlock 22, building1 21, killblock_laser 14, killblock 12, GunHumvee 11, block 7 (+ spawned gibs).
Check: re_A killblock_laser bp: scale 3, block.msh +-1 -> visual +-3 = collider +-3 only with the half-extent rule.

## Rule for the converter
- Do NOT hide RenderMesh instances by colour/material. The PC has no such rule: Color1 = 0 emit meshes are drawn as
  opaque black depth-writing geometry (other examples: 81 black tubes in CRUIS'N EGYPT, 9 in L6 ceilings; Gladiator
  tubes with w = 0). The only PC visibility switches are RM+0x55 (logic), RM+0x54 (frustum cull) and the Play flag.
- Collision comes only from Collider* components; StaticMesh / CulledStatic / RenderMesh / Light never collide.
- ColliderOrientedBox: half extents = Size * scale (not Size/2).
