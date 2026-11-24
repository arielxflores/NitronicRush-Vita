/* Particles: see particles.h. The maths follows the PC game's emitter and update (re_F2 4.2).
 *
 * Each particle is written once, when it is made, into a ring in GPU memory (its four corners: where and when it was
 * made, its velocity, gravity, life, size, rotation, texture slice and palette row); from then on the vertex shader
 * works out where it is, how old and how big (gfx.c, VS_PART), so a particle costs the CPU nothing after its birth.
 * The PC keeps up to 25000; the rings here hold 16384 short-lived ones (sparks, the wing tips' trails, the exhaust),
 * 4096 long-lived ones (the weather, thinned out here) and 512 that darken (VolcanoSmoke): 5.8 MB. A full ring
 * overwrites its oldest (a long roof slide's sparks: their last, darkest tenth).
 * Each ring's window (count slots up to head) is drawn: it shrinks past the particles that have died. */
#include "particles.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#define SPARKS_FRAME_MAX 400          /* the car's sparks in a frame (the PC: 75 a contact; a roof slide touches at several) */
enum { R_SHORT, R_LONG, R_SUB, NRINGS };
static const uint32_t RCAP[NRINGS] = { 16384, 4096, 512 };
typedef struct {
    gfx_pvert *v;                     /* RCAP x 4 corners (GPU memory on the Vita) */
    float *death;                     /* when each one dies, on the particles' clock */
    uint32_t head, count;             /* the next slot to write; how many are in the window (ending at head) */
    unsigned buf;                     /* its GL buffer (the Vita: the ring itself, no copies) */
    int blend;                        /* since the level started, a particle that blends two slices (gfx: else one read) */
    uint32_t made, made_peak;         /* written since the last draw; the most in one frame lately (see particles_draw) */
} ring_t;
static ring_t RG[NRINGS];
static double pnow;                   /* the particles' clock: seconds of game time (not while paused) */
static GLuint atlas, paltex;          /* particles.ddsv's 64 slices, 8 x 8; particle_color.tga (row = colour index, column = age) */
static int ready;
static uint32_t seed = 12345;
float particles_short_screens;                       /* (the log: the last frame's estimate of the short ones' cover) */
static float view_ppu;                              /* (the camera's pixels per unit at distance 1: particles_view) */
/* every 16th slot of the short ring, kept in plain memory (the ring is the GPU's): where and when it was made, its
 * velocity and gravity, half size - to tell how much of the screen the short ones cover (particles_draw) */
/* every SE[r]th slot of a ring, sampled when made: position, birth, velocity, gravity, half size (for the cover) */
static const uint32_t SE[NRINGS] = { 16, 4, 1 };
static float SAMP_S[16384 / 16][9], SAMP_L[4096 / 4][9], SAMP_D[512][9];
static float (*const SAMPR[NRINGS])[9] = { SAMP_S, SAMP_L, SAMP_D };

static float rnd(float x)             /* ((rand() % 2048) - 1024) / 1024 * x */
{
    seed = seed * 1103515245u + 12345u;
    return ((int)((seed >> 16) % 2048) - 1024) / 1024.0f * x;
}

/* a ring's memory as it starts: the corners never change, (-1,-1) (1,-1) (1,1) (-1,1); every slot dead until written */
static void ring_fill(int r)
{
    for (uint32_t i = 0; i < RCAP[r] * 4; i++) {
        static const int8_t C[4][2] = { { -127, -127 }, { 127, -127 }, { 127, 127 }, { -127, 127 } };
        RG[r].v[i].c[0] = C[i & 3][0]; RG[r].v[i].c[1] = C[i & 3][1]; RG[r].v[i].c[2] = RG[r].v[i].c[3] = 0;
        RG[r].v[i].l[0] = 1.0f;                           /* (dead until written: born at -1e9, see below) */
        RG[r].v[i].p[3] = -1e9f;
    }
}
int particles_init(nr_pak *pak)
{
    atlas = gfx_texture(pak, "particles_atlas");
    uint32_t sz; uint8_t *b = nr_pak_read(pak, "textures/particle_palette.nrt", &sz);
    nr_texdata t;
    paltex = 0;
    if (b && !nr_tex_parse(b, sz, &t) && t.fmt == 8 && t.w == 64 && t.h == 64) paltex = gfx_texture_rgba(t.data, 64, 64, 1);
    free(b);
    ready = 0;
    if (!atlas || !paltex) return -1;
    for (int r = 0; r < NRINGS; r++) {
        if (RG[r].v) continue;
        RG[r].v = gfx_pring_alloc(RCAP[r] * 4, &RG[r].buf);
        RG[r].death = calloc(RCAP[r], sizeof(float));
        if (!RG[r].v || !RG[r].death) return -2;
        ring_fill(r);
    }
    ready = 1;
    particles_clear();
    return 0;
}
unsigned particles_made;
void particles_clear(void)
{
    /* the window empty, the clock back to 0 (the old slots are outside the window: never drawn again until rewritten) */
    for (int r = 0; r < NRINGS; r++) { RG[r].count = 0; RG[r].blend = 0; }
    /* every ring from its first slot again at each level. Carried on from level to level (as before), the Vita's GPU
     * took 100-500 ms over a few thousand particles from about the fourth level of a session on - Old_RedLevel after
     * MountainTop, Level 3 and warp_speed; warp_speed after Levels 1, 3 and 6 (dev50) - drawing what an earlier level
     * had left in those slots; the same particles from a ring made then took 1.4 ms, and from the first slot again at
     * each level (part_dbg 128, dev41) the four levels played with none. (part_dbg 128: carried on, as before, to try
     * it again; 256: the short-lived ones' from slot 11098, where they stood when Old_RedLevel began that way) */
    if (!(gfx_part_dbg & 128)) for (int r = 0; r < NRINGS; r++) RG[r].head = 0;
    if ((gfx_part_dbg & 8192) && ready) for (int r = 0; r < NRINGS; r++) {   /* (and new memory for each: see gfx.c) */
        int e = gfx_pring_renew(&RG[r].v, &RG[r].buf, RCAP[r] * 4);
        if (e == 0) { ring_fill(r); RG[r].head = 0; }
        else if (e == -2) ready = 0;
    }
    if (gfx_part_dbg & 256) RG[R_SHORT].head = 11098;
    pnow = 0;
}
#ifdef NR_PC
int particles_alive(int ring)       /* (the test machine: the ones in a ring's window still alive, and its window) */
{
    ring_t *R = &RG[ring]; int n = 0;
    for (uint32_t k = 0; k < R->count; k++) { uint32_t s = (R->head + RCAP[ring] - 1 - k) % RCAP[ring]; if (R->death[s] > (float)pnow) n++; }
    return n * 100000 + (int)R->count;
}
unsigned particles_head(int ring) { return RG[ring].head; }
#endif
/* (the log: the alive particles of a ring by distance from the eye: count and screens' worth of pixels; the Vita logs
 * it at its benches and its slowest frames) */
/* (the hitch hunt: a ring's window as particles_draw takes it - where it lies, how it is split - and the memory the GPU
 * reads there: each slot's four corners alike but for the corner itself, and what kind of particle each is) */
void particles_check(int ring, void (*out)(const char *))
{
    ring_t *R = &RG[ring]; const uint32_t cap = RCAP[ring];
    if (!R->count) return;
    uint32_t n = R->count;
    {   uint32_t guard = 2 * R->made_peak; if (guard < cap / 32) guard = cap / 32; if (guard > cap / 2) guard = cap / 2;
        if (n > cap - guard) n = cap - guard; }
    uint32_t first = R->head >= n ? R->head - n : R->head + cap - n, a = first + n <= cap ? n : cap - first;
    int mixed = 0, dead = 0, unborn = 0, nonfin = 0, far = 0; float bmin = 1e30f, bmax = -1e30f, smax = 0, vmax = 0, pmax = 0;
    uint32_t mixed_at = 0;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t sl = (first + k) % cap;
        const gfx_pvert *v = R->v + (size_t)sl * 4;
        for (int c = 1; c < 4; c++) if (memcmp(v[c].p, v[0].p, 64)) { if (!mixed++) mixed_at = sl; break; }
        const float *f = v->p;
        for (int i = 0; i < 16; i++) if (!isfinite(f[i])) { nonfin++; break; }
        float age = (float)(pnow - v->p[3]) * v->l[0];
        if (age < 0) unborn++; else if (age >= 1) dead++;
        if (v->p[3] < bmin) bmin = v->p[3]; if (v->p[3] > bmax) bmax = v->p[3];
        if (fabsf(v->l[1]) > smax) smax = fabsf(v->l[1]);
        for (int i = 0; i < 3; i++) { if (fabsf(v->v[i]) > vmax) vmax = fabsf(v->v[i]); if (fabsf(v->p[i]) > pmax) pmax = fabsf(v->p[i]); }
        if (pmax > 1e5f) far++;
    }
    char l[320];
    snprintf(l, sizeof l, "PRING %d: head %u, %u in the window (%u drawn: slots %u-%u%s), made lately %u a frame | corners not alike %d (first at slot %u), not finite %d, dead %d, unborn %d | born %.2f-%.2f s (now %.2f), size up to %.2f, speed up to %.1f, where up to %.0f",
             ring, R->head, R->count, n, first, (first + n - 1) % cap, a < n ? ", in two draws" : "", R->made_peak, mixed, mixed_at, nonfin, dead, unborn, bmin, bmax, (float)pnow, smax, vmax, pmax);
    out(l);
    (void)far;
    if (particles_dump && ring == 0) {   /* (the window's slots as the CPU sees them, for tools on the PC) */
        static int ndump; char path[160]; snprintf(path, sizeof path, "%s%d.tga", particles_dump, ndump++);
        FILE *f = fopen(path, "wb");
        if (f) {
            uint32_t hd[8] = { 0x474e5250u, (uint32_t)ring, cap, R->head, R->count, n, first, (uint32_t)sizeof(gfx_pvert) };
            fwrite(hd, 4, 8, f); double now = pnow; fwrite(&now, 8, 1, f);
            for (uint32_t k = 0; k < n; k++) fwrite(R->v + (size_t)((first + k) % cap) * 4, sizeof(gfx_pvert), 4, f);
            fclose(f); snprintf(l, sizeof l, "PRING 0: the window's %u slots saved (%s)", n, path); out(l);
        }
    }
}
const char *particles_dump;
void particles_stat(int ring, const float eye[3], float ppu, void (*out)(const char *))
{
    static const float B[] = { 0.5f, 1, 2, 4, 8, 16, 1e9f }; float px[7] = { 0 }; int cnt[7] = { 0 };
    ring_t *R = &RG[ring];
    for (uint32_t k = 0; k < R->count; k++) {
        uint32_t sl = (R->head + RCAP[ring] - 1 - k) % RCAP[ring];
        if (R->death[sl] <= (float)pnow) continue;
        const gfx_pvert *v = R->v + (size_t)sl * 4;
        float t = (float)(pnow - v->p[3]), p[3];
        for (int i = 0; i < 3; i++) p[i] = v->p[i] + v->v[i] * t;
        p[2] += 0.5f * v->v[3] * t * t;
        float d = sqrtf((p[0] - eye[0]) * (p[0] - eye[0]) + (p[1] - eye[1]) * (p[1] - eye[1]) + (p[2] - eye[2]) * (p[2] - eye[2]));
        float h = fabsf(v->l[1]) * ppu / (d > 0.1f ? d : 0.1f), a = 4 * h * h / (960.0f * 544.0f);
        int b = 0; while (d > B[b]) b++;
        px[b] += a > 1 ? 1 : a; cnt[b]++;
#ifdef NR_PC
        if (ring == 2 && getenv("PSTAT2")) { char l[160]; snprintf(l, sizeof l, "PSTAT2 puff d %.1f half %.1f px age %.2f inten %.2f", d, h, t * v->l[0], v->t[3]); out(l); }
#endif
    }
    char line[300]; int n = snprintf(line, sizeof line, "PSTAT ring %d by distance (count/screens):", ring);
    for (int b = 0; b < 7; b++) n += snprintf(line + n, sizeof line - n, " <%g %d/%.2f", B[b], cnt[b], px[b]);
    if (ring == R_SHORT) snprintf(line + n, sizeof line - n, " | estimate %.2f", particles_short_screens);
    out(line);
}
int particles_count(void)
{
    int n = 0;
    for (int r = 0; r < NRINGS; r++) n += (int)RG[r].count;
    return n;
}

static void spawn(const nr_pemit *e, const float pos[3], const float vel[3], float tsub, float size_scale)
{
    if (!ready) return;
    float inten = e->inten;
    int r = inten < 0 ? R_SUB : e->life > 1.0f ? R_LONG : R_SHORT;
    ring_t *R = &RG[r];
    if (R->count >= RCAP[r]) R->count--;                    /* full: the oldest goes (its slot is the one written now) */
    uint32_t slot = R->head;
    R->head = slot + 1 == RCAP[r] ? 0 : slot + 1; R->count++; R->made++;
    particles_made++;
    float inv = 1.0f / (e->life > 0.01f ? e->life : 0.01f);
    /* as the PC's Emit (0x5f4eb0): made tsub into the frame's past (so already tsub old and vel x tsub further on);
     * the shader puts it at pos + vel t (+ gravity) at age t = now - birth */
    gfx_pvert q;
    for (int k = 0; k < 3; k++) q.p[k] = pos[k];
    q.p[3] = (float)(pnow - tsub);
    q.v[0] = vel[0]; q.v[1] = vel[1]; q.v[2] = vel[2]; q.v[3] = e->gravity;
    q.l[0] = inv;
    q.l[1] = (e->size + rnd(e->size_rand)) * size_scale;   /* half size; negative is a flipped quad, as on the PC */
    q.l[2] = (e->rot + rnd(e->rot_rand)) * (3.14159265f / 360.0f);     /* pi/360, as the PC */
    q.l[3] = e->drot * inv;                                 /* radians a second */
    q.t[0] = (e->tex_idx + rnd(e->tex_rand) - 0.5f) / 64.0f;
    q.t[1] = e->tex_delta * inv / 64.0f;                    /* slices a second (in 64ths) */
    q.t[2] = (e->color_idx + rnd(e->color_rand) - 0.5f) / 64.0f;
    q.t[3] = inten < 0 ? -inten : inten;                    /* (the darkening ring is drawn subtracting) */
    /* one slice all its life: no change of slice, a whole Texture_Index (the shader's slice is then exactly
     * whole, its blend weight 0) */
    if (e->tex_delta != 0 || e->tex_rand != 0 || e->tex_idx != floorf(e->tex_idx)) R->blend = 1;
#ifdef NR_PC
    if (getenv("PCHECK")) {   /* (the test machine: a particle made with numbers the Vita's GPU could choke on) */
        static int said; const float *f = q.p; int bad = 0; float big = 0;
        for (int i = 0; i < 16; i++) { if (!isfinite(f[i])) bad = 1; if (i != 3 && fabsf(f[i]) > big) big = fabsf(f[i]); }
        if ((bad || big > 1e5f) && said++ < 40)
            fprintf(stderr, "PCHECK ring %d at %.3f: p %g %g %g  birth %g  v %g %g %g g %g  l %g %g %g %g  t %g %g %g %g\n", r, (double)pnow,
                    q.p[0], q.p[1], q.p[2], q.p[3], q.v[0], q.v[1], q.v[2], q.v[3], q.l[0], q.l[1], q.l[2], q.l[3], q.t[0], q.t[1], q.t[2], q.t[3]);
    }
#endif
    gfx_pvert *v = R->v + (size_t)slot * 4;
    for (int c = 0; c < 4; c++) { memcpy(v[c].p, q.p, 16); memcpy(v[c].v, q.v, 16); memcpy(v[c].l, q.l, 16); memcpy(v[c].t, q.t, 16); }
    if (slot % SE[r] == 0) { float *sm = SAMPR[r][slot / SE[r]]; memcpy(sm, q.p, 16); memcpy(sm + 4, q.v, 16); sm[8] = fabsf(q.l[1]); }
    R->death[slot] = q.p[3] + e->life;
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
            float t = (float)k / n0, l[3] = { rnd(e->pos_rand[0]), rnd(e->pos_rand[1]), rnd(e->pos_rand[2]) }, pos[3], vel[3];
            float sp = e->speed + rnd(e->speed_rand), j[3] = { rnd(1), rnd(1), rnd(1) };
            for (int r = 0; r < 3; r++) {
                pos[r] = posW[r] * (1 - t) + st->prev[r] * t + M[r][0] * l[0] + M[r][1] * l[1] + M[r][2] * l[2];
                vel[r] = (dirW[r] + sa * j[r]) * sp;
            }
            spawn(e, pos, vel, k * dt / n0, size_scale);
        }
    }
    if (st->acc > 50) st->acc = 50;        /* (a backlog: don't let it pile up) */
    memcpy(st->prev, posW, 12);
}

void particles_sparks(const float p[3], const float n[3], float slide, float count_scale)
{
    static const nr_pemit SPARK = { 1, 0, 0.6f, 55, 0, 0, 0.05f, 0.1f, { 0, 0, 0 }, { 0.25f, 0.25f, 0.25f }, 0, 0, 0,
                                    { 0, 0, 1 }, 14, 1, 0.1f, -5, 0.75f, 0 };
    float x = slide / 3.0f; if (x > 15) x = 15;      /* CarLogic: the scraping speed / 3, up to 15 */
    int cnt = (int)(5 * x * count_scale);
    float sa = sinf(14 * 3.14159265f / 180.0f);
    for (int k = 0; k < cnt; k++) {
        float pos[3], vel[3], sp = 1 + rnd(0.1f);
        for (int r = 0; r < 3; r++) { pos[r] = p[r] + rnd(0.25f); vel[r] = (n[r] + sa * rnd(1)) * sp; }
        spawn(&SPARK, pos, vel, k * 0.015f / (cnt > 0 ? cnt : 1), 1);
    }
}

/* the PC's way (CarLogic::HandleCollisions 0x709290): the car's one SparkBitch emitter, moved to a contact and run for
 * 0.015 s at 5 x rate a second */
int particles_spark_budget(void) { return SPARKS_FRAME_MAX; }
void particles_spark_emit(const float p[3], const float n[3], float rate, int *budget)
{
    static const nr_pemit SPARK = { 1, 0, 0.6f, 55, 0, 0, 0.05f, 0.1f, { 0, 0, 0 }, { 0.25f, 0.25f, 0.25f }, 0, 0, 0,
                                    { 0, 0, 1 }, 14, 1, 0.1f, -5, 0.75f, 0 };
    /* CarLogic::HandleCollisions (0x709582): the emitter's accumulator is SET to 5 x rate and it is run once for
     * 0.015 s with a frequency of 0, so each contact makes (int)(5 x rate) sparks at once, up to 75 */
    int cnt = (int)(rate * 5.0f);
    if (cnt > *budget) cnt = *budget;
    if (cnt <= 0) return;
    *budget -= cnt;
    float sa = sinf(14 * 3.14159265f / 180.0f);
    while (cnt-- > 0) {
        float pos[3], vel[3], sp = 1 + rnd(0.1f);
        for (int r = 0; r < 3; r++) { pos[r] = p[r] + rnd(0.25f); vel[r] = (n[r] + sa * rnd(1)) * sp; }
        spawn(&SPARK, pos, vel, 0, 1);
    }
}

/* the clock (the PC's simulate step comes first in its frame: call this before the frame's emitters) */
void particles_update(float dt)
{
    pnow += dt;
    for (int r = 0; r < NRINGS; r++) {
        ring_t *R = &RG[r];
        while (R->count) {
            uint32_t first = R->head >= R->count ? R->head - R->count : R->head + RCAP[r] - R->count;
            if (R->death[first] > (float)pnow) break;
            R->count--;
        }
    }
}

/* the short ones (the car's sparks, its trails and exhaust) as they would be drawn now: how many screens' worth of
 * pixels (from the sampled slots), each shrinking to nothing from NEAR1 in front of the camera to NEAR0 (a spark
 * flown through filled the screen alone). A car scraping a wall by the camera made 15000 sparks, 14 screens' worth in
 * one frame (the Vita draws about one in the time): past SHORT_SCREENS they are drawn smaller, as much brighter as
 * smaller (up to 4 x), so they give about the same light */
#define NEAR0 0.2f
#define NEAR1 1.0f
#define SHORT_SCREENS 2.0f
void particles_view(const float eye[3], float ppu) { (void)eye; view_ppu = ppu; }
/* the darkening ones (smoke): inside MountainTop's volcano 77 puffs covered 9.6 screens and the Vita took 14 ms to
 * draw them (Nov 10, 28.3 ms a frame). Shrunk as the sparks, the smoke went thin (the darkening clamps at black);
 * instead past SUB_SCREENS only every m-th slot is drawn (the same ones while m holds), each m times as dark: about
 * the same darkness at a fraction of the cost */
#define SUB_SCREENS 2.0f
static int sub_every = 1;
static float ring_cover(const ring_t *R, int ring, v3 eye, v3 fwd, float near0, float near1)
{
    if (view_ppu <= 0 || !R->count) return 0;
    const uint32_t se = SE[ring], cap = RCAP[ring];
    float a = 0, k = 4 * view_ppu * view_ppu / (960.0f * 544.0f);
    uint32_t first = R->head >= R->count ? R->head - R->count : R->head + cap - R->count;
    uint32_t s0 = (first + se - 1) / se * se;
    for (uint32_t i = 0; i < R->count; i += se) {
        uint32_t sl = (s0 + i) % cap;
        if (R->death[sl] <= (float)pnow) continue;
        const float *sm = SAMPR[ring][sl / se];
        float t = (float)pnow - sm[3];
        float px = sm[0] + sm[4] * t - eye.x, py = sm[1] + sm[5] * t - eye.y, pz = sm[2] + sm[6] * t + 0.5f * sm[7] * t * t - eye.z;
        float z = px * fwd.x + py * fwd.y + pz * fwd.z;
        if (z <= near0 || z <= 0.05f) continue;           /* (behind the camera, or shrunk to nothing) */
        float nk = z >= near1 ? 1 : (z - near0) / (near1 - near0), h = sm[8] * nk / z, c = k * h * h;
        a += c < 1 ? c : 1;
    }
    return a * se;
}
int particles_skip;
void particles_draw(v3 eye, v3 fwd, v3 up)
{
    if (!ready) return;
    v3 r = norm(cross(fwd, up)), u = cross(r, fwd);
    for (int k = 0; k < NRINGS; k++) {
        ring_t *R = &RG[k];
        uint32_t n = R->count, cap = RCAP[k];
        /* The Vita's GPU reads the ring a frame or two after this call, while the next frames' particles are being
         * written into it: a nearly full ring's oldest slots could be new particles by then (drawn with this frame's
         * clock: before their birth). So the oldest are left out when the ring is that full: room for two frames'
         * worth of new ones, going by the most made in a frame lately */
        if (R->made > R->made_peak) R->made_peak = R->made; else R->made_peak -= (R->made_peak - R->made + 31) / 32;
        R->made = 0;
        {   uint32_t guard = 2 * R->made_peak; if (guard < cap / 32) guard = cap / 32; if (guard > cap / 2) guard = cap / 2;
            if (n > cap - guard) n = cap - guard; }
        if (!n || (particles_skip >> k & 1)) continue;
        int every = 1;
        if (k == R_SHORT) {
            float a = ring_cover(R, k, eye, fwd, NEAR0, NEAR1), sc = a > SHORT_SCREENS ? sqrtf(SHORT_SCREENS / a) : 1;
            particles_short_screens = a;
            gfx_particles_scale(NEAR0, NEAR1, sc, fminf(4.0f, 1.0f / (sc * sc)));
        } else if (k == R_SUB) {
            float a = ring_cover(R, k, eye, fwd, -1e9f, 0);
            int want = a > SUB_SCREENS ? (int)ceilf(a / SUB_SCREENS) : 1;
            if (want > 8) want = 8;
            /* (a step down only when well under: no flipping between two) */
            if (want > sub_every || a * 1.25f < SUB_SCREENS * sub_every - SUB_SCREENS) sub_every = want;
            every = sub_every;
            gfx_particles_scale(-1e9f, 0, 1, (float)every);
        } else gfx_particles_scale(-1e9f, 0, 1, 1);
        uint32_t first = R->head >= n ? R->head - n : R->head + cap - n;
        uint32_t a = first + n <= cap ? n : cap - first;
        if ((gfx_part_dbg & (16384 | 32768)) && k == R_SHORT) {
            /* (the hitch hunt: the window copied to video memory - 16384 - or to the C library's heap and drawn from there,
             * vitaGL copying it into its own data pool at the draw - 32768) */
            static gfx_pvert *cv, *ch; static unsigned cvb;
            const int vram = (gfx_part_dbg & 16384) != 0;
            if (vram && !cv) cv = gfx_pring_alloc_vram(RCAP[k] * 4, &cvb);
            if (!vram && !ch) ch = malloc((size_t)RCAP[k] * 4 * sizeof(gfx_pvert));
            gfx_pvert *cp = vram ? cv : ch;
            if (cp) {
                for (uint32_t i = 0; i < n; i++) memcpy(cp + (size_t)i * 4, R->v + (size_t)((first + i) % cap) * 4, 4 * sizeof(gfx_pvert));
                gfx_particles(cp, vram ? &cvb : NULL, 0, n, atlas, paltex, (float)pnow, r, u, 0, !R->blend, every);
                continue;
            }
        }
        if ((gfx_part_dbg & 4096) && k == R_SHORT) {      /* (the hitch hunt: the window copied to a ring of its own, at its start) */
            static gfx_pvert *cp; static unsigned cpb;
            if (!cp) cp = gfx_pring_alloc(RCAP[k] * 4, &cpb);
            if (cp) {
                for (uint32_t i = 0; i < n; i++) memcpy(cp + (size_t)i * 4, R->v + (size_t)((first + i) % cap) * 4, 4 * sizeof(gfx_pvert));
                gfx_particles(cp, &cpb, 0, n, atlas, paltex, (float)pnow, r, u, 0, !R->blend, every);
                continue;
            }
        }
        if (!(gfx_part_dbg & 2048) || a == n) gfx_particles(R->v, &R->buf, first, a, atlas, paltex, (float)pnow, r, u, k == R_SUB, !R->blend, every);
        if (a < n && !(gfx_part_dbg & 1024)) gfx_particles(R->v, &R->buf, 0, n - a, atlas, paltex, (float)pnow, r, u, k == R_SUB, !R->blend, every);
    }
    gfx_particles_scale(-1e9f, 0, 1, 1);
}
