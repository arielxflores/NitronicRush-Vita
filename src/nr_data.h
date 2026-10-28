/* Converted level (.nrl) and texture (.nrt) formats, as written by converter/nr_pack.py. */
#ifndef NR_DATA_H
#define NR_DATA_H
#include <stdint.h>

enum { NR_BLEND_OPAQUE = 0, NR_BLEND_CUTOUT = 1, NR_BLEND_ADD = 2 };
/* material flags */
enum { NR_MAT_SKY = 1, NR_MAT_EMIT = 2, NR_MAT_NO_DIFFUSE = 4, NR_MAT_GLOSS_EMITMASK = 8, NR_MAT_PULSE = 16, NR_MAT_EMIT_COLOUR = 32,
       NR_MAT_SPHERE_N = 64, NR_MAT_REFSPEC = 128 };
/* NR_MAT_EMIT_COLOUR: simple_emit.fx, the surface is its Color1 x intensity (light tubes)
 * NR_MAT_SPHERE_N: phong_ref_pulse (the car): the reflection's sphere map by the plain normal
 *   (other phong_ref shaders use the half vector between the normal and the eye)
 * NR_MAT_REFSPEC: phong_ref_spec: reflection x ReflectStrength1 (p[3] = its mean), not x diffuse alpha */
/* fx: the PC shader's own animation (NRL3), with c2 = Color2 (rgb, glow) and p = Float1..3 */
enum { NR_FX_NONE = 0, NR_FX_PULSE,      /* phong_newpulse (ramps, lava): chevrons running along Emit1.b */
       NR_FX_VEINS,                      /* pulse_veins (additive): flash and sweep on the music's beat */
       NR_FX_ANIM,                       /* phong_emit_anim (turbines): green bands, Anim1 = 'anim' */
       NR_FX_TRONICS,                    /* tronics (ground plane): pulses along the traces, Tron1 = 'anim' */
       NR_FX_CITY,                       /* city (backdrop): tron bands at the base, lit windows */
       NR_FX_TV,                         /* tvscreen / tvscreen_red: static noise (p = picture, noise 1, noise 2) */
       NR_FX_TRAFFIC,                    /* traffic.fx / jet_flame.fx time base */
       NR_FX_INTRO };                    /* phong_ref_intro (the car's warp-in / warp-out stand-in): detail = TriangleEdges, c2 = glow */
/* shading kinds: which of the PC game's shaders the material uses */
enum { NR_SHADE_LIT = 0, NR_SHADE_TV = 1, NR_SHADE_SKY = 2, NR_SHADE_ADD = 3, NR_SHADE_REF = 4, NR_SHADE_UNLIT = 5 };
/* chunk kinds */
enum { NR_KIND_SKY = 0, NR_KIND_WORLD = 1, NR_KIND_BACKDROP = 2 };
/* f32 pos[3], f32 uv[2], s8 normal[4], u8 colour[4] (rgb tint, a = glow strength / 8; NRL3: / 32),
 * u8 light[4] (the level's lights baked per vertex, rgb / 4) */
#define NR_VERTEX_SIZE 32

/* variant: additive look (0 plain, 1 glass.fx, 2 traffic.fx, 3 jet_flame.fx) */
enum { NR_ADD_PLAIN = 0, NR_ADD_GLASS, NR_ADD_TRAFFIC, NR_ADD_FLAME, NR_ADD_GROUND };   /* GROUND: GroundLaser.fx (the laser floors) */
typedef struct {
    int16_t diffuse, emit, detail, reflect; uint8_t blend, kind, flags, variant; float gloss;   /* NRL2: these 16 bytes */
    int16_t anim; uint8_t fx, pad; float c2[4], p[4];                                            /* NRL3: + 36 */
} nr_mat;
/* the level's lighting (NRL2 header): the sun (straight down) with its ambient and specular,
 * fog and the sky colour far geometry fades into */
typedef struct {
    float sun[3], amb, spec, sun_dir[3];
    float fog[4], fog_near, fog_far, sky_far, sky_avg[3], sky_tint[4], pad[2];
} nr_lighting;
typedef struct {
    uint16_t mat, kind; uint32_t nv, ni; float bmin[3], bmax[3];
    const uint8_t *verts; const uint16_t *idx;     /* point into the level blob */
} nr_chunk;
typedef struct {
    uint32_t ntex, nmat, nchunk, npath;
    int version;                                   /* 2 or 3 */
    float glow_scale;                              /* vertex colour alpha x this = glow (8 in NRL2, 32 in NRL3) */
    float start[3];
    nr_lighting light;
    char (*texnames)[64];
    nr_mat *mats;
    nr_chunk *chunks;
    float (*path)[3];
} nr_leveldata;

/* parses a .nrl blob; the result points into blob (keep it alive) except mats/chunks/path,
 * which are allocated. returns 0 on success */
int  nr_level_parse(const uint8_t *blob, uint32_t size, nr_leveldata *out);
void nr_level_data_free(nr_leveldata *l);

typedef struct { uint16_t w, h; uint8_t fmt, nmips; uint16_t flags; const uint8_t *data; } nr_texdata;
int      nr_tex_parse(const uint8_t *blob, uint32_t size, nr_texdata *out);
uint32_t nr_tex_mip_size(const nr_texdata *t, int level, int *w, int *h);
/* decode one mip level of a DXT1/DXT5 texture to RGBA8 (fallback path) */
void     nr_dxt_decode(const nr_texdata *t, const uint8_t *mip, int w, int h, uint8_t *rgba);
#endif
