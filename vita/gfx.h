/* 3D drawing with vitaGL, the way the PC game draws:
 *  - materials shaded like its shaders (phong_emit, building, road, tvscreen, phong_ref, skybox,
 *    additive): diffuse x light + emissive x colour, the car's sphere-map reflection;
 *  - light = the level's sun (straight down, with ambient) + its point/spot/tube lights, which
 *    are baked per vertex for the level and picked per object for moving things;
 *  - fog and the fade into the sky (FogTweaks);
 *  - the scene goes to a texture, then bloom (blurred bright parts, screenfinal2's curve),
 *    the radial speed blur and edge tint are put on top when drawing it to the screen. */
#ifndef NR_GFX_H
#define NR_GFX_H
#ifdef NR_PC
#include "pc_gl.h"
#else
#include <vitaGL.h>
#endif
#include <math.h>
#include "../src/nr_pak.h"
#include "../src/nr_data.h"

typedef struct { float x, y, z; } v3;
static inline v3 v3make(float x, float y, float z) { v3 r = { x, y, z }; return r; }
static inline v3 add(v3 a, v3 b) { return v3make(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline v3 sub(v3 a, v3 b) { return v3make(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline v3 mul(v3 a, float s) { return v3make(a.x * s, a.y * s, a.z * s); }
static inline float dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline v3 cross(v3 a, v3 b) { return v3make(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline float len(v3 a) { return sqrtf(dot(a, a)); }
static inline v3 norm(v3 a) { float l = len(a); return l > 1e-6f ? mul(a, 1 / l) : v3make(0, 1, 0); }
static inline v3 lerp3(v3 a, v3 b, float t) { return add(a, mul(sub(b, a), t)); }

typedef struct { GLuint vbo, ibo; uint32_t ni; uint16_t mat, kind; float bmin[3], bmax[3]; } gchunk;

/* one of the level's static lights (from the .nrg), used to light moving objects */
typedef struct { int type; float pos[3], col[3], amb, near_, far_, axis[3], half, cos_in, cos_out; } gfx_light;

typedef struct {
    char name[64];
    nr_leveldata d;
    GLuint *tex;
    gchunk *ch; uint32_t nch;
    float *pathlen;           /* cumulative length along the route */
    float bmin[3], bmax[3];
    gfx_light *lights; int nlights;
} level_rt;

/* something drawn with a model matrix (rows; model space -> world). glow scales its emissive
 * colour (0 is taken as 1); tint (if a > 0) replaces the emissive colour */
typedef struct { const level_rt *model; float m[4][4]; float glow; float tint[4]; int use_col; float col[4]; float phase;
                 float intro_p, portal[4]; int no_cull; } gfx_obj;
/* intro_p, portal: a warp-in stand-in (NR_FX_INTRO): IntroCarLogic's progress (0 all at the portal .. 1 whole) and the
 * portal in the model's space (direction, distance); no_cull: drawn even when its box is out of the view */
/* use_col: col (RenderMesh Color1: rgb, glow) replaces the model's own, as level objects' logic sets it;
 * phase: the object's own offset for animated materials (traffic, screens), 0..1 */
/* camera-facing glow quad (additive) */
typedef struct { v3 pos; float size; float r, g, b; unsigned tex; float angle; } gfx_sprite;
/* per-frame screen effects: blur 0..1 is the speed blur (radial, as at top speed on PC),
 * edge colour fades the screen edges (a = strength), flash adds a white-blue flash,
 * bloom scales the glow (0 is taken as 1) */
typedef struct { float blur, edge[4], flash, bloom, gain; } gfx_post;   /* gain: the scene's brightness - 1 (the intro's flashes) */
/* the story cinematics' screen effect (intro_redtoblue, re_O 7): the scene in its true colours inside a sphere of
 * radius r3 round e, red and blue swapped outside it, a hot ring at r2 (the radii 1.2^Float, set each frame) */
typedef struct { int on; float e[3], r2, r3; } gfx_redblue_t;
extern gfx_redblue_t gfx_redblue;
int  gfx_redblue_init(void);                             /* its programs (built once, when a cinematic first loads) */

typedef struct { int no_mips, rgba, no_fog, no_cutout, no_cull, no_tex_cache, discard, no_transfer_wait, cg, no_post, no_bloom, no_wide; } gfx_switches;
extern gfx_switches GSW;
extern int gfx_draws, gfx_tris, gfx_dbg;
extern int gfx_objs_drawn, gfx_objs_culled;             /* the last frame's objects: drawn, outside the view */
extern float gfx_prof[8], gfx_prof_sky;              /* [7]: of [0], binding and clearing the scene target; gfx_prof_sky: of [0], the sky */                                /* the last frame's building, ms: sky+culling, level, objects, glow level, glow objects, sprites+particles, screen passes */
void gfx_prewarm(void);                                  /* every 3D program drawn once per blend state (at load: no first-use hitch later) */
/* called now and then while a level loads (the loading screen's animation); may draw */
extern void (*gfx_load_tick)(void);
extern float VP[4][4];
extern float gfx_time;                                    /* seconds, for animated materials */
extern float gfx_T, gfx_ab;                               /* the PC shaders' Time (0..1 sawtooth, 0.05 a second) and AudioBeat (s since the beat) */
extern float gfx_beat;                                    /* LightPulse: 1 on the music's beat, falling to 0 over 1 s */
extern float gfx_bloom_tune[4], gfx_bloom_thr;
extern float gfx_scene_scale;                            /* the 3D scene drawn at this fraction of the screen (0.5..1), scaled up by the final pass */

void   gfx_set_log(void (*fn)(const char *fmt, ...));
int    gfx_init(void);                                    /* 0 = ok */
int    gfx_load_nrl(level_rt *lv, nr_pak *pak, const char *pakname, const char *label);
void   gfx_free_level(level_rt *lv);
void   gfx_set_lights(level_rt *lv, const gfx_light *l, int n);
GLuint gfx_texture(nr_pak *pak, const char *name);        /* textures/<name>.nrt, cached */
/* the level, objects and sprites into the scene texture, then the screen effects onto the
 * screen (the HUD is drawn after this); fov is vertical, in degrees */
void   gfx_draw_frame(const level_rt *lv, v3 eye, v3 fwd, v3 up, float fov_deg,
                      const gfx_obj *objs, int nobj, const gfx_sprite *spr, int nspr, const gfx_post *post);
float  gfx_texture_mb(void);
/* camera-facing quads (4 vertices each, level vertex format) onto the scene, added or (subtract)
 * taken away; for the particles, from gfx_hook_additive, which gfx_draw_frame calls in its glow pass */
void   gfx_quads(const void *verts, int nquads, GLuint tex, int subtract);
extern void (*gfx_hook_additive)(v3 eye, v3 fwd, v3 up);
/* after the solid objects, before anything glows: things that darken what is under them (the tyre tracks) */
extern void (*gfx_hook_decal)(v3 eye, v3 fwd, v3 up);
/* the tyre tracks (tire_track.fx): quads in the level vertex format, uv = (across 0..1, 0.5 x along x length / 4),
 * colour r = the ends' fade coordinate, g = the track's fade, normal x = its glow (0..127), y = its rainbow (0..127);
 * pass 0: the dark tread (from gfx_hook_decal), 1: the glow (from gfx_hook_additive) */
void   gfx_tracks(const void *verts, int nquads, GLuint tread, GLuint rainbow, int pass);
#endif
