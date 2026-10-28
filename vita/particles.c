/* Particles: see particles.h. The maths follows the PC game's emitter and update (re_F2 4.2). */
#include "particles.h"
#include <stdlib.h>
#include <string.h>

#define PMAX 4000                    /* the PC keeps up to 25000 */
#define SPARK_NP_MAX 1800            /* sparks are only made while fewer particles than this are alive (each costs ~1-2 us to draw) */
typedef struct { float pos[3], vel[3], age, inv_life, life, tex, dtex, rot, drot, size, colv, inten, grav, cs, sn; } part_t;   /* cs, sn: of rot (kept while drot is 0) */
static part_t P[PMAX]; static int NP;
static GLuint atlas;                 /* particles.ddsv's 64 slices, 8 x 8 */
static uint8_t pal[64 * 64 * 4];     /* particle_color.tga: row = colour index, column = age */
static int have_pal;
static uint32_t seed = 12345;

static float rnd(float x)             /* ((rand() % 2048) - 1024) / 1024 * x */
{
    seed = seed * 1103515245u + 12345u;
    return ((int)((seed >> 16) % 2048) - 1024) / 1024.0f * x;
}

int particles_init(nr_pak *pak)
{
    atlas = gfx_texture(pak, "particles_atlas");
    uint32_t sz; uint8_t *b = nr_pak_read(pak, "textures/particle_palette.nrt", &sz);
    nr_texdata t;
    have_pal = 0;
    if (b && !nr_tex_parse(b, sz, &t) && t.fmt == 8 && t.w == 64 && t.h == 64) { memcpy(pal, t.data, sizeof pal); have_pal = 1; }
    free(b);
    NP = 0;
    return atlas && have_pal ? 0 : -1;
}
unsigned particles_made;
void particles_clear(void) { NP = 0; }
int particles_count(void) { return NP; }

static void spawn(const nr_pemit *e, const float pos[3], const float vel[3], float tsub, float size_scale)
{
    if (NP >= PMAX) return;
    particles_made++;
    part_t *p = &P[NP++];
    float inv = 1.0f / (e->life > 0.01f ? e->life : 0.01f);
    p->tex = (e->tex_idx + rnd(e->tex_rand) - 0.5f) / 64.0f;
    p->colv = (e->color_idx + rnd(e->color_rand) - 0.5f) / 64.0f;
    p->inten = e->inten;
    p->size = (e->size + rnd(e->size_rand)) * size_scale;
    p->rot = (e->rot + rnd(e->rot_rand)) * (3.14159265f / 360.0f);     /* pi/360, as the PC */
    p->cs = cosf(p->rot); p->sn = sinf(p->rot);
    memcpy(p->vel, vel, 12);
    p->grav = e->gravity; p->life = e->life; p->inv_life = inv;
    p->dtex = e->tex_delta * inv / 64.0f; p->drot = e->drot * inv;
    for (int k = 0; k < 3; k++) p->pos[k] = pos[k] + vel[k] * tsub;     /* the part of the frame it already lived */
    p->age = inv * tsub; p->life -= tsub;
}

void particles_emit(const nr_pemit *e, nr_pemit_state *st, const float M[3][4], float dt, float count_scale)
{
    float posW[3], dirW[3];
    for (int r = 0; r < 3; r++) {
        posW[r] = M[r][0] * e->pos[0] + M[r][1] * e->pos[1] + M[r][2] * e->pos[2] + M[r][3];
        dirW[r] = M[r][0] * e->dir[0] + M[r][1] * e->dir[1] + M[r][2] * e->dir[2];
    }
    if (!st->primed) { memcpy(st->prev, posW, 12); st->primed = 1; }
    float sa = sinf(e->angle * 3.14159265f / 180.0f);
    st->acc += e->freq * dt * count_scale;
    int n0 = (int)st->acc;
    if (n0 > 0) {
        /* thinned out: bigger, so the screen fills about as much */
        float size_scale = count_scale < 1 ? 1.0f + 0.35f * (1.0f - count_scale) : 1.0f;
        for (int k = 0; k < n0; k++) {
            st->acc -= 1;
            if (NP >= PMAX) break;
            float t = (float)k / n0, l[3] = { rnd(e->pos_rand[0]), rnd(e->pos_rand[1]), rnd(e->pos_rand[2]) }, pos[3], vel[3];
            float sp = e->speed + rnd(e->speed_rand), j[3] = { rnd(1), rnd(1), rnd(1) };
            for (int r = 0; r < 3; r++) {
                pos[r] = posW[r] * (1 - t) + st->prev[r] * t + M[r][0] * l[0] + M[r][1] * l[1] + M[r][2] * l[2];
                vel[r] = (dirW[r] + sa * j[r]) * sp;
            }
            spawn(e, pos, vel, k * dt / n0, size_scale);
        }
    }
    if (st->acc > 50) st->acc = 50;        /* pool full: the PC lets it build up; don't */
    memcpy(st->prev, posW, 12);
}

void particles_sparks(const float p[3], const float n[3], float slide, float count_scale)
{
    static const nr_pemit SPARK = { 1, 0, 0.6f, 55, 0, 0, 0.05f, 0.1f, { 0, 0, 0 }, { 0.25f, 0.25f, 0.25f }, 0, 0, 0,
                                    { 0, 0, 1 }, 14, 1, 0.1f, -5, 0.75f, 0 };
    float x = slide / 3.0f; if (x > 15) x = 15;      /* CarLogic: the scraping speed / 3, up to 15 */
    int cnt = (int)(5 * x * count_scale);
    float sa = sinf(14 * 3.14159265f / 180.0f);
    for (int k = 0; k < cnt && NP < PMAX; k++) {
        float pos[3], vel[3], sp = 1 + rnd(0.1f);
        for (int r = 0; r < 3; r++) { pos[r] = p[r] + rnd(0.25f); vel[r] = (n[r] + sa * rnd(1)) * sp; }
        spawn(&SPARK, pos, vel, k * 0.015f / (cnt > 0 ? cnt : 1), 1);
    }
}

/* the PC's way (CarLogic::HandleCollisions 0x709290): the car's one SparkBitch emitter, moved to a contact and run for
 * 0.015 s at 5 x rate a second; *acc carries the fractions from contact to contact and frame to frame */
void particles_spark_emit(const float p[3], const float n[3], float rate, int *budget)
{
    static const nr_pemit SPARK = { 1, 0, 0.6f, 55, 0, 0, 0.05f, 0.1f, { 0, 0, 0 }, { 0.25f, 0.25f, 0.25f }, 0, 0, 0,
                                    { 0, 0, 1 }, 14, 1, 0.1f, -5, 0.75f, 0 };
    /* CarLogic::HandleCollisions (0x709582): the emitter's accumulator is SET to 5 x rate and it is run once for
     * 0.015 s with a frequency of 0, so each contact makes (int)(5 x rate) sparks at once, up to 75 */
    int cnt = (int)(rate * 5.0f);
    if (cnt > *budget) cnt = *budget;
    if (NP + cnt > SPARK_NP_MAX) cnt = SPARK_NP_MAX - NP;        /* (the Vita's pool: room left for the other particles) */
    if (cnt <= 0) return;
    *budget -= cnt;
    float sa = sinf(14 * 3.14159265f / 180.0f);
    while (cnt-- > 0 && NP < PMAX) {
        float pos[3], vel[3], sp = 1 + rnd(0.1f);
        for (int r = 0; r < 3; r++) { pos[r] = p[r] + rnd(0.25f); vel[r] = (n[r] + sa * rnd(1)) * sp; }
        spawn(&SPARK, pos, vel, 0, 1);
    }
}

void particles_update(float dt)
{
    for (int i = 0; i < NP; i++) {
        part_t *p = &P[i];
        p->age += p->inv_life * dt; p->rot += p->drot * dt; p->life -= dt; p->tex += p->dtex * dt;
        if (p->life <= 0) { P[i--] = P[--NP]; continue; }
        p->vel[2] += p->grav * dt;
        for (int k = 0; k < 3; k++) p->pos[k] += p->vel[k] * dt;     /* no damping, no collision */
    }
}

/* the quads: 4 vertices each in the levels' vertex format */
static uint8_t VB[2][PMAX * 4 * NR_VERTEX_SIZE];
void particles_draw(v3 eye, v3 fwd, v3 up)
{
    if (!NP || !atlas || !have_pal) return;
    v3 r = norm(cross(fwd, up)), u = cross(r, fwd);
    int nq[2] = { 0, 0 };
    for (int i = 0; i < NP; i++) {
        const part_t *p = &P[i];
        int ax = (int)(p->age * 64); if (ax < 0) ax = 0; if (ax > 63) ax = 63;
        int ay = (int)(p->colv * 64); if (ay < 0) ay = 0; if (ay > 63) ay = 63;
        const uint8_t *c = pal + (ay * 64 + ax) * 4;
        float h = p->size * (c[3] / 255.0f);
        if (fabsf(h) < 1e-4f) continue;
        float col[3] = { c[0] / 255.0f * p->inten, c[1] / 255.0f * p->inten, c[2] / 255.0f * p->inten };
        int neg = p->inten < 0;                       /* VolcanoSmoke darkens */
        float mx = fmaxf(fabsf(col[0]), fmaxf(fabsf(col[1]), fabsf(col[2])));
        if (mx < 1.0f / 255) continue;
        v3 q = v3make(p->pos[0], p->pos[1], p->pos[2]);
        if (dot(sub(q, eye), fwd) < -fabsf(h)) continue;   /* behind the camera */
        float cs = p->cs, sn = p->sn;
        if (p->drot != 0) { cs = cosf(p->rot); sn = sinf(p->rot); }
        v3 ax_ = add(mul(r, cs * h), mul(u, -sn * h)), ay_ = add(mul(r, sn * h), mul(u, cs * h));
        int sl = (int)(p->tex * 64); if (sl < 0) sl = 0; if (sl > 63) sl = 63;
        float u0 = (sl % 8) / 8.0f + 0.5f / 512, v0 = (sl / 8) / 8.0f + 0.5f / 512, u1 = u0 + 1.0f / 8 - 1.0f / 512, v1 = v0 + 1.0f / 8 - 1.0f / 512;
        v3 corner[4] = { sub(sub(q, ax_), ay_), sub(add(q, ax_), ay_), add(add(q, ax_), ay_), add(sub(q, ax_), ay_) };
        float uv[4][2] = { { u0, v1 }, { u1, v1 }, { u1, v0 }, { u0, v0 } };
        uint8_t cr = (uint8_t)fminf(255, fabsf(col[0]) / mx * 255), cg = (uint8_t)fminf(255, fabsf(col[1]) / mx * 255), cb = (uint8_t)fminf(255, fabsf(col[2]) / mx * 255);
        uint8_t ca = (uint8_t)fminf(255, mx / 8 * 255 + 0.5f);
        uint8_t *v = VB[neg] + nq[neg] * 4 * NR_VERTEX_SIZE;
        for (int k = 0; k < 4; k++, v += NR_VERTEX_SIZE) {
            float f[5] = { corner[k].x, corner[k].y, corner[k].z, uv[k][0], uv[k][1] };
            memcpy(v, f, 20);
            v[20] = v[21] = 0; v[22] = 127; v[23] = 0;
            v[24] = cr; v[25] = cg; v[26] = cb; v[27] = ca;
            v[28] = v[29] = v[30] = v[31] = 0;
        }
        nq[neg]++;
    }
    gfx_quads(VB[0], nq[0], atlas, 0);
    gfx_quads(VB[1], nq[1], atlas, 1);
}
