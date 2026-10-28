/* The PC game's particles (graphics 0x609640, ParticleEmitter 0x5f4eb0; docs/re/re_F2.md 4): a pool
 * of camera-facing quads, added onto the scene. Each particle shows one slice of particles.ddsv
 * (here an 8 x 8 atlas of its 64 slices) tinted by a row of particle_color.tga that it walks
 * along as it ages. Used by the weather emitters (snow, ash, smoke) and the car's sparks.
 * The Vita draws fewer of them than the PC (the emitters' rates are scaled down). */
#ifndef NR_PARTICLES_H
#define NR_PARTICLES_H
#include "gfx.h"

typedef struct {
    float color_idx, color_rand, inten;             /* palette row (1-based), +-, colour multiplier */
    float tex_idx, tex_rand, tex_delta;             /* slice (1-based), +-, slices over the life */
    float size, size_rand;                          /* half size (world units) */
    float pos[3], pos_rand[3];                      /* spawn offset and +- per axis, emitter space */
    float rot, rot_rand, drot;                      /* (rotation + rnd) x pi/360; radians over the life */
    float dir[3], angle, speed, speed_rand;         /* direction (emitter space, not normalised), cone (deg) */
    float gravity, life, freq;                      /* added to vel.z a second; seconds; particles a second */
} nr_pemit;

typedef struct { float acc, prev[3]; int primed; } nr_pemit_state;

int  particles_init(nr_pak *pak);                   /* textures; 0 = ready */
void particles_clear(void);
/* the emitter at matrix M (3x4 rows, with its scale) for dt; count_scale thins it out */
extern unsigned particles_made;       /* particles made so far (the log counts the effects with it) */
void particles_emit(const nr_pemit *e, nr_pemit_state *st, const float M[3][4], float dt, float count_scale);
/* the car's sparks (SparkBitch): a burst at a scraping contact */
void particles_sparks(const float p[3], const float n[3], float slide, float count_scale);
/* the same as the PC makes them: (int)(5 x rate) sparks at a contact, rate = min(sliding speed / 3, 15); at most *budget
 * (the frame's allowance, lowered by what was made) */
void particles_spark_emit(const float p[3], const float n[3], float rate, int *budget);
void particles_update(float dt);
/* hand the quads to gfx (called from gfx's additive pass) */
void particles_draw(v3 eye, v3 fwd, v3 up);
int  particles_count(void);
#endif
