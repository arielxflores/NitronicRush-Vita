/* Level objects: see nr_objs.h. The numbers come from the PC game's code (docs/re/). */
#include "nr_objs.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI_F 3.14159265f
struct nr_humvee { nr_car car; float acc; int obj, used; };   /* a CORE drone's car */
static void humvee_free(nr_objs *W, int i);

/* ------------------------------------------------------------------ small maths */
static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float len3(const float *a) { return sqrtf(dot3(a, a)); }
static void cross3(const float *a, const float *b, float *o)
{
    float r[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
    memcpy(o, r, 12);
}
static void norm3(float *a) { float l = len3(a); if (l > 1e-9f) { a[0] /= l; a[1] /= l; a[2] /= l; } }
static void set3(float *o, float x, float y, float z) { o[0] = x; o[1] = y; o[2] = z; }
static void madd3(float *o, const float *a, float s) { o[0] += a[0] * s; o[1] += a[1] * s; o[2] += a[2] * s; }
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static void colv(const float R[3][3], int k, float *o) { o[0] = R[0][k]; o[1] = R[1][k]; o[2] = R[2][k]; }
static void set_cols(float R[3][3], const float *a, const float *b, const float *c)
{
    for (int i = 0; i < 3; i++) { R[i][0] = a[i]; R[i][1] = b[i]; R[i][2] = c[i]; }
}
static void mat_mul(const float A[3][3], const float B[3][3], float O[3][3])
{
    float r[3][3];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r[i][j] = A[i][0] * B[0][j] + A[i][1] * B[1][j] + A[i][2] * B[2][j];
    memcpy(O, r, sizeof r);
}
static void mat_vec(const float A[3][3], const float *v, float *o)
{
    float r[3] = { A[0][0] * v[0] + A[0][1] * v[1] + A[0][2] * v[2], A[1][0] * v[0] + A[1][1] * v[1] + A[1][2] * v[2],
                   A[2][0] * v[0] + A[2][1] * v[1] + A[2][2] * v[2] };
    memcpy(o, r, 12);
}
static void identity(float R[3][3]) { memset(R, 0, 36); R[0][0] = R[1][1] = R[2][2] = 1; }
/* R = rotation about unit axis a by angle t, times R */
static void rotate_by(float R[3][3], const float *w, float dt)
{
    float ang = len3(w) * dt;
    if (ang < 1e-7f) return;
    float a[3] = { w[0], w[1], w[2] }; norm3(a);
    float c = cosf(ang), s = sinf(ang), k = 1 - c;
    float Q[3][3] = { { c + a[0] * a[0] * k, a[0] * a[1] * k - a[2] * s, a[0] * a[2] * k + a[1] * s },
                      { a[1] * a[0] * k + a[2] * s, c + a[1] * a[1] * k, a[1] * a[2] * k - a[0] * s },
                      { a[2] * a[0] * k - a[1] * s, a[2] * a[1] * k + a[0] * s, c + a[2] * a[2] * k } };
    mat_mul(Q, R, R);
    /* keep it orthonormal (and its handedness) */
    float x[3], y[3], z[3], det;
    colv(R, 0, x); colv(R, 1, y); colv(R, 2, z);
    float cz[3]; cross3(x, y, cz); det = dot3(cz, z) < 0 ? -1.0f : 1.0f;
    norm3(y); madd3(z, y, -dot3(z, y)); norm3(z); cross3(y, z, x); x[0] *= det; x[1] *= det; x[2] *= det;
    set_cols(R, x, y, z);
}
static void rot_axis(float R[3][3], int axis, float ang)     /* rotation about a coordinate axis */
{
    float c = cosf(ang), s = sinf(ang);
    identity(R);
    int i = (axis + 1) % 3, j = (axis + 2) % 3;
    R[i][i] = c; R[i][j] = -s; R[j][i] = s; R[j][j] = c;
}

static uint32_t rnd(nr_objs *W) { uint32_t x = W->rng ? W->rng : 0x9e3779b9u; x ^= x << 13; x ^= x >> 17; x ^= x << 5; W->rng = x; return x; }
static float frand(nr_objs *W) { return (rnd(W) >> 8) * (1.0f / 16777216.0f); }
static float frange(nr_objs *W, float a, float b) { return a + (b - a) * frand(W); }

/* ------------------------------------------------------------------ .nrg line values */
static const char *kv(const char *line, const char *key)
{
    size_t kl = strlen(key);
    for (const char *p = line; p && (p = strstr(p, key)); p++)
        if ((p == line || p[-1] == ' ') && p[kl] == '=') return p + kl + 1;
    return NULL;
}
static float kvf(const char *line, const char *key, float d) { const char *v = kv(line, key); return v ? strtof(v, NULL) : d; }
static int kvv(const char *line, const char *key, float *out, int n)
{
    const char *v = kv(line, key); int k = 0;
    while (v && k < n) {
        char *e; float x = strtof(v, &e);
        if (e == v) break;
        out[k++] = x; v = e;
        if (*v == ',') v++; else break;
    }
    return k;
}
static int kvs(const char *line, const char *key, char *out, int n)
{
    const char *v = kv(line, key); int k = 0;
    if (!v) { out[0] = 0; return 0; }
    while (v[k] && v[k] != ' ' && k < n - 1) { out[k] = v[k]; k++; }
    out[k] = 0;
    return k;
}
static int has(const char *line, const char *prefix) { char b[64]; snprintf(b, sizeof b, " %s", prefix); return strstr(line, b) != NULL; }

/* ------------------------------------------------------------------ models and collision shapes */
static int model_of(nr_objs *W, const char *name, float bmin[3], float bmax[3])
{
    set3(bmin, -1, -1, -1); set3(bmax, 1, 1, 1);
    if (!name[0] || !strcmp(name, "-")) return -1;
    for (int i = 0; i < W->nmdl; i++) if (!strcmp(W->mdl[i].name, name)) { memcpy(bmin, W->mdl[i].bmin, 12); memcpy(bmax, W->mdl[i].bmax, 12); return W->mdl[i].h; }
    void *t = realloc(W->mdl, sizeof *W->mdl * (W->nmdl + 1)); if (!t) return -1;
    W->mdl = t;
    int h = W->host.model ? W->host.model(W->host.user, name, bmin, bmax) : -1;
    snprintf(W->mdl[W->nmdl].name, sizeof W->mdl[W->nmdl].name, "%s", name);
    W->mdl[W->nmdl].h = h; memcpy(W->mdl[W->nmdl].bmin, bmin, 12); memcpy(W->mdl[W->nmdl].bmax, bmax, 12);
    W->nmdl++;
    return h;
}

static int cmodel_of(nr_objs *W, const char *name)
{
    if (!name[0] || !strcmp(name, "-")) return -1;
    for (int i = 0; i < W->ncm; i++) if (!strcmp(W->cm[i].name, name)) return i;
    nr_cmodel m; memset(&m, 0, sizeof m);
    snprintf(m.name, sizeof m.name, "%s", name);
    if (!strncmp(name, "box:", 4)) {                       /* ColliderOrientedBox: Size (x scale) is the HALF extent (0x636470) */
        float s[3] = { 1, 1, 1 }; sscanf(name + 4, "%f,%f,%f", &s[0], &s[1], &s[2]);
        float c[8][3];
        for (int i = 0; i < 8; i++) set3(c[i], (i & 4 ? 1.0f : -1.0f) * s[0], (i & 2 ? 1.0f : -1.0f) * s[1], (i & 1 ? 1.0f : -1.0f) * s[2]);
        static const int q[6][4] = { { 0, 1, 3, 2 }, { 4, 6, 7, 5 }, { 0, 4, 5, 1 }, { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 5, 7, 3 } };
        m.n = 12; m.t = malloc(sizeof *m.t * 12);
        for (int f = 0; f < 6; f++) {
            const int *k = q[f];
            int t0[3] = { k[0], k[1], k[2] }, t1[3] = { k[0], k[2], k[3] };
            for (int j = 0; j < 3; j++) { memcpy(&m.t[f * 2][j * 3], c[t0[j]], 12); memcpy(&m.t[f * 2 + 1][j * 3], c[t1[j]], 12); }
        }
    } else if (!strncmp(name, "cyl:", 4)) {               /* ColliderCylinder: radius, half height along Z */
        float r = 1, hh = 1; sscanf(name + 4, "%f,%f", &r, &hh);
        int N = 12; m.n = N * 4; m.t = malloc(sizeof *m.t * m.n);
        for (int i = 0; i < N; i++) {
            float a0 = 2 * PI_F * i / N, a1 = 2 * PI_F * (i + 1) / N;
            float A[3] = { r * cosf(a0), r * sinf(a0), -hh }, B[3] = { r * cosf(a1), r * sinf(a1), -hh };
            float C[3] = { B[0], B[1], hh }, D[3] = { A[0], A[1], hh }, T[3] = { 0, 0, hh }, Bo[3] = { 0, 0, -hh };
            float *t = m.t[i * 4];
            memcpy(t, A, 12); memcpy(t + 3, B, 12); memcpy(t + 6, C, 12);
            t = m.t[i * 4 + 1]; memcpy(t, A, 12); memcpy(t + 3, C, 12); memcpy(t + 6, D, 12);
            t = m.t[i * 4 + 2]; memcpy(t, D, 12); memcpy(t + 3, C, 12); memcpy(t + 6, T, 12);
            t = m.t[i * 4 + 3]; memcpy(t, A, 12); memcpy(t + 3, Bo, 12); memcpy(t + 6, B, 12);
        }
    } else {
        char p[96]; snprintf(p, sizeof p, "models/%s.ncm", name);
        uint32_t sz = 0; uint8_t *b = W->host.read ? W->host.read(W->host.user, p, &sz) : NULL;
        if (b && sz >= 8 && !memcmp(b, "NCM1", 4)) {
            uint32_t n; memcpy(&n, b + 4, 4);
            if (8 + 36ull * n <= sz) { m.n = n; m.t = malloc(36ull * n + 1); memcpy(m.t, b + 8, 36ull * n); }
        }
        free(b);
    }
    for (uint32_t i = 0; i < m.n; i++) for (int j = 0; j < 3; j++) { float l = len3(&m.t[i][j * 3]); if (l > m.r) m.r = l; }
    m.bb = m.n ? malloc(sizeof *m.bb * m.n) : NULL;
    for (uint32_t i = 0; i < m.n && m.bb; i++) for (int k = 0; k < 3; k++) {
        float a = m.t[i][k], b = m.t[i][3 + k], c = m.t[i][6 + k];
        m.bb[i][k] = fminf(a, fminf(b, c)); m.bb[i][3 + k] = fmaxf(a, fmaxf(b, c));
    }
    void *t = realloc(W->cm, sizeof *W->cm * (W->ncm + 1)); if (!t) { free(m.t); return -1; }
    W->cm = t; W->cm[W->ncm] = m;
    return W->ncm++;
}

/* ------------------------------------------------------------------ objects */
static void obj_bounds(nr_obj *o)
{
    float e[3];
    for (int i = 0; i < 3; i++) e[i] = fmaxf(fabsf(o->bmin[i]), fabsf(o->bmax[i])) * fabsf(o->scale[i]);
    o->radius = len3(e) + 0.5f;
}

/* a new object from an obj / spawn line (its transform as written there) */
static int obj_new(nr_objs *W, int kind, const char *line)
{
    if (W->n >= W->cap) {
        int nc = W->cap ? W->cap * 2 : 512;
        void *t = realloc(W->o, sizeof *W->o * nc); if (!t) return -1;
        W->o = t; W->cap = nc;
        t = realloc(W->bodies, sizeof *W->bodies * nc); if (!t) return -1;
        W->bodies = t;
        if (W->coll) W->coll->bodies = W->bodies;
    }
    int idx = -1;      /* reuse the slots of sparks, debris and flashes (nothing refers to them) */
    if (kind == OK_SPARK || kind == OK_DEBRIS || kind == OK_FX)
        for (int i = W->reuse; i < W->n; i++) {
            int k = W->o[i].kind;
            if (!W->o[i].alive && (k == OK_SPARK || k == OK_DEBRIS || k == OK_FX)) { idx = i; W->reuse = i + 1; break; }
        }
    if (idx < 0) W->reuse = 0;
    if (idx < 0) idx = W->n++;
    nr_obj *o = &W->o[idx];
    memset(o, 0, sizeof *o);
    o->kind = kind; o->alive = 1; o->visible = 1; o->parent = -1; o->proto = -1; o->snd_slot = -1; o->line = line;
    char b[64];
    kvs(line, "m", b, sizeof b); o->model = model_of(W, b, o->bmin, o->bmax);
    kvs(line, "cm", b, sizeof b); o->cmodel = cmodel_of(W, b);
    o->cmat = (uint8_t)kvf(line, "cmat", 0); o->matid = (uint8_t)kvf(line, "mid", 0); o->death_speed = kvf(line, "ds", 0);
    float r[9] = { 0 };
    if (kvv(line, "p", o->pos, 3) < 3) set3(o->pos, 0, 0, 0);
    if (kvv(line, "s", o->scale, 3) < 3) set3(o->scale, 1, 1, 1);
    kvv(line, "r", r, 9);
    for (int i = 0; i < 9; i++) o->R[i / 3][i % 3] = r[i];
    if (fabsf(r[0]) + fabsf(r[4]) + fabsf(r[8]) + fabsf(r[1]) + fabsf(r[3]) < 1e-6f) identity(o->R);
    float c[4] = { 1, 1, 1, 1 }; kvv(line, "c", c, 4);
    memcpy(o->col, c, 16); memcpy(o->base, c, 16);
    o->pulse = kvf(line, "pulse", 0);
    kvs(line, "snd", b, sizeof b);
    char *colon = strrchr(b, ':');
    if (colon) { o->sndloop = colon[1] == '1'; *colon = 0; snprintf(o->snd, sizeof o->snd, "%s", b); }
    memcpy(o->lpos, o->pos, 12); memcpy(o->lR, o->R, 36); memcpy(o->lscale, o->scale, 12);
    obj_bounds(o);
    return idx;
}

static int find_proto(nr_objs *W, const char *bp)
{
    for (int i = 0; i < W->nproto; i++) if (!strcmp(W->proto[i].bp, bp)) return i;
    return -1;
}
/* spawn a blueprint the logic needs (transform: the blueprint's own, as local values) */
static int spawn(nr_objs *W, const char *bp, int kind)
{
    int p = find_proto(W, bp);
    if (p < 0) return -1;
    int i = obj_new(W, kind, W->proto[p].line);
    if (i >= 0) W->o[i].proto = p;
    return i;
}
static void kill_obj(nr_objs *W, int i)
{
    if (i < 0 || i >= W->n) return;
    nr_obj *o = &W->o[i];
    o->alive = 0; o->visible = 0;
    for (int k = 0; k < o->nkid; k++) kill_obj(W, o->kid[k]);
    o->nkid = 0;
}
static void add_kid(nr_obj *o, int k) { if (k >= 0 && o->nkid < NR_OBJ_KIDS) o->kid[o->nkid++] = (int16_t)k; }

/* world transform of a child from its parent and its local transform */
static void place_child(nr_objs *W, int ci)
{
    nr_obj *c = &W->o[ci], *p = &W->o[c->parent];
    float lp[3] = { c->lpos[0] * p->scale[0], c->lpos[1] * p->scale[1], c->lpos[2] * p->scale[2] }, w[3];
    mat_vec(p->R, lp, w);
    for (int k = 0; k < 3; k++) c->pos[k] = p->pos[k] + w[k];
    mat_mul(p->R, c->lR, c->R);
    for (int k = 0; k < 3; k++) c->scale[k] = c->lscale[k] * p->scale[k];
}

static void to_world(const nr_obj *o, const float *l, float *out)
{
    float s[3] = { l[0] * o->scale[0], l[1] * o->scale[1], l[2] * o->scale[2] }, w[3];
    mat_vec(o->R, s, w);
    for (int k = 0; k < 3; k++) out[k] = o->pos[k] + w[k];
}

void nr_objs_matrix(const nr_obj *o, float m[4][4])
{
    for (int r = 0; r < 3; r++) {
        for (int k = 0; k < 3; k++) m[r][k] = o->R[r][k] * o->scale[k];
        m[r][3] = o->pos[r];
    }
    m[3][0] = m[3][1] = m[3][2] = 0; m[3][3] = 1;
}

int nr_objs_count(const nr_objs *W, int kind)
{
    int n = 0;
    for (int i = 0; i < W->n; i++) if (W->o[i].alive && W->o[i].kind == kind) n++;
    return n;
}

static void play(nr_objs *W, const char *ev, const float *pos) { if (W->host.sound && ev && ev[0]) W->host.sound(W->host.user, ev, pos); }

/* the car as a box: does the segment o + d*[0,1] pass through it? (slab test in the car's space) */
static int seg_hits_car(const nr_objs_frame *f, const float *o, const float *d, float *tout)
{
    if (!f->car_active) return 0;
    float lo[3], ld[3], rel[3] = { o[0] - f->car_pos[0], o[1] - f->car_pos[1], o[2] - f->car_pos[2] };
    for (int k = 0; k < 3; k++) {
        lo[k] = f->car_rot[0][k] * rel[0] + f->car_rot[1][k] * rel[1] + f->car_rot[2][k] * rel[2];
        ld[k] = f->car_rot[0][k] * d[0] + f->car_rot[1][k] * d[1] + f->car_rot[2][k] * d[2];
    }
    float t0 = 0, t1 = 1;
    for (int k = 0; k < 3; k++) {
        float h = f->car_half[k];
        if (fabsf(ld[k]) < 1e-9f) { if (lo[k] < -h || lo[k] > h) return 0; continue; }
        float a = (-h - lo[k]) / ld[k], b = (h - lo[k]) / ld[k];
        if (a > b) { float x = a; a = b; b = x; }
        if (a > t0) t0 = a;
        if (b < t1) t1 = b;
        if (t0 > t1) return 0;
    }
    if (tout) *tout = t0;
    return 1;
}

/* TV_RadialImpulse: delta-v away from the centre, c0 + c1/d + c2/d^2, capped */
static void radial_push(const float *c, float R, const float *k, float mx, const float *at, float *dv)
{
    float d[3] = { at[0] - c[0], at[1] - c[1], at[2] - c[2] };
    float d2 = dot3(d, d);
    if (d2 <= 0 || d2 >= R * R) return;
    float inv = 1 / sqrtf(d2), m = k[0] + k[1] * inv + k[2] * inv * inv;
    if (fabsf(m) > mx) m = m < 0 ? -mx : mx;
    madd3(dv, d, m * inv);
}

/* ------------------------------------------------------------------ BezierPath (the train) */
typedef struct { int n; float (*P)[3], (*H)[3]; int nt, ct; float (*tab)[2]; float (*smp)[3]; float L; } bez_t;
static void bez_eval(const bez_t *b, int seg, float t, float *o)
{
    if (seg >= b->n - 1) { seg = b->n - 2; t = 1; }
    if (seg < 0) { memcpy(o, b->P[0], 12); return; }
    const float *p0 = b->P[seg], *c1 = b->H[2 * seg], *c2 = b->H[2 * seg + 1], *p3 = b->P[seg + 1];
    float u = 1 - t, a = u * u * u, bb = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
    for (int k = 0; k < 3; k++) o[k] = a * p0[k] + bb * c1[k] + c * c2[k] + d * p3[k];
}
static void bez_push(bez_t *b, float param, float dist, const float *pt)
{
    if (b->nt == b->ct) {
        b->ct = b->ct ? b->ct * 2 : 256;
        b->tab = realloc(b->tab, sizeof *b->tab * b->ct); b->smp = realloc(b->smp, sizeof *b->smp * b->ct);
    }
    b->tab[b->nt][0] = param; b->tab[b->nt][1] = dist; memcpy(b->smp[b->nt], pt, 12); b->nt++;
}
static void bez_add(bez_t *b, int seg, float t, const float *pt, int depth)
{
    float lt = b->tab[b->nt - 1][0] - seg, mid[3], *last = b->smp[b->nt - 1];
    bez_eval(b, seg, (lt + t) * 0.5f, mid);
    float avg[3] = { (last[0] + pt[0]) * 0.5f, (last[1] + pt[1]) * 0.5f, (last[2] + pt[2]) * 0.5f }, d[3] = { mid[0] - avg[0], mid[1] - avg[1], mid[2] - avg[2] };
    if (dot3(d, d) < 0.001f || depth > 14) {
        float e[3] = { pt[0] - last[0], pt[1] - last[1], pt[2] - last[2] };
        b->L += len3(e); bez_push(b, seg + t, b->L, pt);
    } else {
        bez_add(b, seg, (lt + t) * 0.5f, mid, depth + 1);
        bez_add(b, seg, t, pt, depth + 1);
    }
}
static void bez_build(bez_t *b, const float (*pts)[3], int n)
{
    memset(b, 0, sizeof *b);
    if (n < 2) return;
    b->n = n; b->P = malloc(12 * n); b->H = malloc(12 * 2 * (n - 1));
    memcpy(b->P, pts, 12 * n);
    for (int s = 0; s < n - 1; s++) { memcpy(b->H[2 * s], b->P[s], 12); memcpy(b->H[2 * s + 1], b->P[s + 1], 12); }
    for (int k = 1; k < n - 1; k++) {                 /* smooth(k): handles 1/4 of the neighbours' difference */
        float d[3] = { b->P[k + 1][0] - b->P[k - 1][0], b->P[k + 1][1] - b->P[k - 1][1], b->P[k + 1][2] - b->P[k - 1][2] };
        for (int j = 0; j < 3; j++) {
            if (dot3(d, d) < 1e-4f) { b->H[2 * k - 1][j] = b->H[2 * k][j] = b->P[k][j]; continue; }
            b->H[2 * k - 1][j] = b->P[k][j] - 0.25f * d[j]; b->H[2 * k][j] = b->P[k][j] + 0.25f * d[j];
        }
    }
    bez_push(b, 0, 0, b->P[0]);
    for (int s = 0; s < n - 1; s++) {
        float m[3]; bez_eval(b, s, 0.5f, m);
        bez_add(b, s, 0.5f, m, 0);
        bez_add(b, s, 1.0f, b->P[s + 1], 0);
    }
}
static void bez_free(bez_t *b) { free(b->P); free(b->H); free(b->tab); free(b->smp); memset(b, 0, sizeof *b); }
static void bez_at(const bez_t *b, float dist, float *o)
{
    if (b->n < 1) { set3(o, 0, 0, 0); return; }
    if (b->n < 2 || dist <= 0) { memcpy(o, b->P[0], 12); return; }
    if (dist >= b->L) { memcpy(o, b->smp[b->nt - 1], 12); return; }
    int lo = 0, hi = b->nt - 1;
    while (hi - lo > 1) { int m = (lo + hi) / 2; if (b->tab[m][1] <= dist) lo = m; else hi = m; }
    float d0 = b->tab[lo][1], d1 = b->tab[hi][1], k = d1 > d0 ? (dist - d0) / (d1 - d0) : 0;
    float param = b->tab[lo][0] + (b->tab[hi][0] - b->tab[lo][0]) * k;
    int seg = (int)param;
    bez_eval(b, seg, param - seg, o);
}
#define MAX_TRAINS 8
static bez_t TRAINP[MAX_TRAINS]; static int NTRAINP;

/* two path points -> a car or track piece between them (local +Y towards the back) */
static void place_between(nr_obj *o, const float *A, const float *B)
{
    for (int k = 0; k < 3; k++) o->pos[k] = (A[k] + B[k]) * 0.5f;
    float f[3] = { B[0] - A[0], B[1] - A[1], B[2] - A[2] }; norm3(f);
    float r[3] = { f[1], -f[0], 0 }; norm3(r);
    float u[3]; cross3(r, f, u);
    set_cols(o->R, r, f, u);
}
static float wrapf(float d, float L) { if (L <= 0) return 0; while (d > L) d -= L; while (d < 0) d += L; return d; }

/* ------------------------------------------------------------------ setting objects up */
static const char *ZONES[4] = { "TriggerSphereZone", "TriggerCylinderZone", "TriggerZoneAABox", "TriggerZoneOrientedBox" };
static void set_glow(nr_obj *o, float g) { o->col[3] = g; }

static void init_obj(nr_objs *W, int i)
{
    nr_obj *o = &W->o[i];
    const char *L = o->line;
    switch (o->kind) {
    case OK_SAW: {
        int ax = (int)kvf(L, "StaticSawLogic.RotAxis", 0); if (ax < 0) ax = 2; if (ax > 2) ax = 0;
        float a[3]; colv(o->R, ax, a);
        float rs = kvf(L, "StaticSawLogic.RotSpeed", 0);
        if (!has(L, "StaticSawLogic.") && (int)kvf(L, "EnumLogic.Type", 0) == 2) { colv(o->R, 1, a); rs = 1; }   /* EnumLogic type 2: 1 rad/s about local Y */
        set3(o->angvel, a[0] * rs, a[1] * rs, a[2] * rs);
        o->s[4] = has(L, "CopyPositionLogic.") ? 1 + ((int)kvf(L, "CopyPositionLogic.Axis_Flags", 0) & 7) : 0;   /* follows the camera */
        if (strstr(L, " bp=core_eye ")) o->col[3] = o->base[3] = 0.1f;   /* FlickerOnHack: glow 0.1 outside the story's intro */
        if (has(L, "CoreLightningLogic.")) {
            o->s[1] = 1;
            o->f[1] = kvf(L, "CoreLightningLogic.max_lightning_distance_", 350); o->f[2] = kvf(L, "CoreLightningLogic.lightnings_per_sec_", 60);
            o->f[3] = kvf(L, "CoreLightningLogic.min_life_", 0.2f); o->f[4] = kvf(L, "CoreLightningLogic.max_life_", 0.5f);
            o->f[5] = kvf(L, "CoreLightningLogic.spark_width_", 0.9f); o->f[6] = kvf(L, "CoreLightningLogic.spark_brightness_", 20);
            o->f[7] = 0; o->s[2] = -1;
        }
        break;
    }
    case OK_BAFSAW: {
        o->f[0] = kvf(L, "BackAndForthSawLogic.RotSpeed", -100); o->f[1] = kvf(L, "BackAndForthSawLogic.MoveExtents", 100);
        o->f[2] = kvf(L, "BackAndForthSawLogic.MoveSpeedMultiplier", 1);
        int ax = (int)kvf(L, "BackAndForthSawLogic.RotAxis", 0); if (ax < 0 || ax > 2) ax = 0; o->s[0] = ax;
        memcpy(&o->f[3], o->pos, 12); colv(o->R, 1, &o->f[6]);
        o->t = 0;
        int p = spawn(W, "saw_path", OK_SAWPATH); o = &W->o[i];
        if (p >= 0) {
            nr_obj *q = &W->o[p];
            memcpy(q->pos, o->pos, 12); memcpy(q->R, o->R, 36);
            set3(q->scale, o->scale[0] * 0.125f, o->scale[1] * 1.25f + o->f[1], 0.01f);
            obj_bounds(q); add_kid(o, p);
        }
        break;
    }
    case OK_DRONE:
        o->f[0] = kvf(L, "DroneLogic.speed", 2); o->f[1] = kvf(L, "DroneLogic.radius", 20); memcpy(&o->f[2], o->pos, 12);
        break;
    case OK_BLIMP: {
        o->f[0] = kvf(L, "BlimpLogic.speed_", 120); o->f[1] = kvf(L, "BlimpLogic.rot_speed_", 0.65f); o->f[2] = kvf(L, "BlimpLogic.target_dist_", 500);
        for (int k = 0; k < 4; k++) {
            char nm[16]; snprintf(nm, sizeof nm, "searchlight%d", k + 1);
            int c = spawn(W, nm, OK_SEARCHLIGHT); o = &W->o[i];
            if (c < 0) continue;
            nr_obj *q = &W->o[c];
            q->parent = (int16_t)i; q->f[0] = 50 * PI_F / 180; q->f[1] = 0.15f; q->f[2] = 0.5f;   /* max angle, interpolation, rest */
            float d[3] = { -q->lR[0][2], -q->lR[1][2], -q->lR[2][2] };
            memcpy(&q->f[3], d, 12); memcpy(&q->f[6], d, 12);       /* from, to */
            q->t = -frand(W) * 0.1f; q->s[0] = 0;
            add_kid(o, c); place_child(W, c); obj_bounds(&W->o[c]);
        }
        break;
    }
    case OK_TRAIN: {
        if (NTRAINP >= MAX_TRAINS) break;
        float pts[256][3]; int n = 0;
        const char *v = kv(L, "path");
        while (v && n < 256) {
            if (sscanf(v, "%f,%f,%f", &pts[n][0], &pts[n][1], &pts[n][2]) != 3) break;
            n++;
            const char *s = strchr(v, ';'), *sp = strchr(v, ' ');
            if (!s || (sp && sp < s)) break;
            v = s + 1;
        }
        if (n == 0) { memcpy(pts[0], o->pos, 12); n = 1; }
        if (n < 2) { pts[1][0] = pts[0][0]; pts[1][1] = pts[0][1] + 10; pts[1][2] = pts[0][2]; n = 2; }
        o->s[0] = NTRAINP; bez_build(&TRAINP[NTRAINP++], (const float (*)[3])pts, n);
        memcpy(o->pos, pts[n - 1], 12);
        float s = o->scale[0];
        o->f[0] = kvf(L, "TrainLogic.Speed", 10); o->f[1] = kvf(L, "TrainLogic.Start_Distance", 1);
        o->f[2] = kvf(L, "TrainLogic.Engine_Length", 7); o->f[3] = kvf(L, "TrainLogic.MidCar_Length", 3.5f);
        o->f[4] = kvf(L, "TrainLogic.Caboose_Length", 4); o->f[5] = kvf(L, "TrainLogic.Track_Lead", 20);
        float tc[4] = { 0.9f, 1, 0.6f, 3 }, kc[4] = { 1, 0.3f, 0.1f, 0.5f };
        kvv(L, "TrainLogic.Train_Color", tc, 4); kvv(L, "TrainLogic.Track_Color", kc, 4);
        memcpy(&o->f[6], tc, 16); memcpy(&o->f[10], kc, 16);
        int mid = (int)kvf(L, "TrainLogic.Mid_Car_Count", 4), npc = (int)kvf(L, "TrainLogic.Track_Piece_Count", 25);
        o->f[14] = o->f[1];                       /* dist */
        /* the cars and the track pieces are many: they live in a run of objects after the train,
         * s[2] = first car, s[3] = cars, s[4] = first piece, s[5] = pieces, s[6] = head piece */
        int first = -1, cnt = 0;
        for (int k = 0; k < mid + 2; k++) {
            int c = spawn(W, k == 0 ? "choochoo" : k == mid + 1 ? "choocaboose" : "choocart", OK_TRAINCAR); o = &W->o[i];
            if (c < 0) continue;
            if (first < 0) first = c;
            W->o[c].parent = (int16_t)i; set3(W->o[c].scale, s, s, s); memcpy(W->o[c].col, tc, 16); obj_bounds(&W->o[c]); cnt++;
        }
        o->s[2] = first; o->s[3] = cnt;
        first = -1; cnt = 0;
        for (int k = 0; k < npc; k++) {
            int c = spawn(W, "Railroad", OK_TRACK); o = &W->o[i];
            if (c < 0) continue;
            if (first < 0) first = c;
            W->o[c].parent = (int16_t)i; set3(W->o[c].scale, s, s, s); memcpy(W->o[c].col, kc, 16); obj_bounds(&W->o[c]); cnt++;
        }
        o->s[4] = first; o->s[5] = cnt; o->s[6] = 0;
        o->visible = 0;
        /* lay the track ahead of the engine */
        bez_t *b = &TRAINP[o->s[0]];
        float head = wrapf(o->f[14] + o->f[5] * s, b->L);
        head -= fmodf(head, 2.0f);
        float B[3], A[3], d = head; bez_at(b, d, B);
        for (int k = 0; k < o->s[5]; k++) {
            memcpy(A, B, 12); d = wrapf(d - 2.0f * s, b->L); bez_at(b, d, B);
            place_between(&W->o[o->s[4] + k], A, B);
        }
        o->f[15] = head;
        break;
    }
    case OK_SPOT:
        o->s[0] = kvf(L, "FakeSpotLightLogic.Start_Counterclockwise", 1) != 0;
        o->f[0] = kvf(L, "FakeSpotLightLogic.Rotation_Speed", 3); o->f[1] = kvf(L, "FakeSpotLightLogic.Swap_Threshold", 0.75f);
        break;
    case OK_LASERARCH: case OK_SWEEP: {
        int N = 5;
        if (o->kind == OK_SWEEP) {
            N = (int)kvf(L, "SweepLaserLogic.Number_Of_Lasers", 8); if (N > NR_OBJ_KIDS) N = NR_OBJ_KIDS;
            o->f[3] = kvf(L, "SweepLaserLogic.Laser_Speed", 10); o->f[4] = kvf(L, "SweepLaserLogic.Laser_Rotation_Speed", 0.5f);
            o->s[1] = 1;   /* sweep direction */
        }
        o->s[0] = 0; o->t = 0; o->f[1] = 1.0f; o->f[2] = 0;       /* state, timer, duration, sweep offset */
        for (int k = 0; k < N; k++) {
            int c = spawn(W, "laser", OK_BEAM); o = &W->o[i];
            if (c < 0) continue;
            W->o[c].parent = (int16_t)i; W->o[c].visible = 0; add_kid(o, c);
        }
        break;
    }
    case OK_SPINLASER: {
        float a[3]; colv(o->R, 0, a);
        float rs = kvf(L, "StaticSawLogic.RotSpeed", -1);
        int ax = (int)kvf(L, "StaticSawLogic.RotAxis", 0); if (ax >= 0 && ax <= 2) colv(o->R, ax, a);
        set3(o->angvel, a[0] * rs, a[1] * rs, a[2] * rs);
        for (int k = 0; k < 15; k++) {
            int c = spawn(W, "SingleLaser", OK_SINGLELASER); o = &W->o[i];
            if (c < 0) continue;
            nr_obj *q = &W->o[c];
            float X[3][3]; rot_axis(X, 0, k * 0.418867f);
            mat_mul(X, q->lR, q->lR);
            float c1[3]; colv(q->lR, 1, c1);
            set3(q->lpos, 3 * c1[0], 3 * c1[1], 3 * c1[2]);
            q->parent = (int16_t)i; q->f[0] = kvf(q->line, "SingleLaserLogic.Max_Distance", 125);
            q->s[0] = spawn(W, "LaserDot", OK_LASERDOT); o = &W->o[i];
            add_kid(o, c);
        }
        break;
    }
    case OK_BLOCKER:
        o->f[2] = kvf(L, "BlockerLogic.LightUp_Distance", 250); o->f[3] = kvf(L, "BlockerLogic.Activate_Distance", 175);
        o->f[4] = kvf(L, "BlockerLogic.Block_Up_Speed", 3); o->f[5] = kvf(L, "BlockerLogic.Block_Height", 5);
        o->f[6] = kvf(L, "BlockerLogic.Brightness", 2.5f);
        o->f[0] = 0; o->f[1] = 0; o->s[0] = -1; o->s[1] = o->s[2] = 0;
        o->visible = 0;
        break;
    case OK_MINEPARENT: {
        o->visible = 0;
        int c = spawn(W, "AirMine", OK_MINE); o = &W->o[i];
        if (c < 0) break;
        nr_obj *q = &W->o[c];
        memcpy(q->pos, o->pos, 12); memcpy(q->R, o->R, 36); memcpy(q->scale, o->scale, 12); obj_bounds(q);
        q->f[0] = frand(W) * 1000.0f;
        q->f[1] = kvf(q->line, "AirMineLogic.Rotation_Amount", 3); q->f[2] = kvf(q->line, "AirMineLogic.Floating_Distance", 0.2f);
        q->f[3] = kvf(q->line, "AirMineLogic.Floating_Speed", 0.5f); q->f[4] = kvf(q->line, "AirMineLogic.Attacking_Distance", 50);
        q->f[5] = kvf(q->line, "AirMineLogic.Attacking_Speed", 0);
        break;
    }
    case OK_SHARDBLD: {
        float ic[4] = { 1, 0.45f, 0.13f, 0.8f }, ac[4] = { 1, 0.15f, 0.13f, 1.9f };
        kvv(L, "ShardBuildingLogic.Idle_Color", ic, 4); kvv(L, "ShardBuildingLogic.Angry_Color", ac, 4);
        memcpy(&o->f[1], ic, 16); memcpy(&o->f[5], ac, 16);
        o->f[9] = kvf(L, "ShardBuildingLogic.Shard_Speed", 170); o->f[10] = kvf(L, "ShardBuildingLogic.Shard_Rotation_Speed", 3);
        o->f[11] = kvf(L, "ShardBuildingLogic.Trigger_Distance", 500);
        set3(&o->f[12], 0, 0, 0); kvv(L, "ShardBuildingLogic.Trigger_Area_Offset", &o->f[12], 3);
        o->f[15] = kvf(L, "ShardBuildingLogic.Distance_Predict_Coef", 1); o->f[16] = kvf(L, "ShardBuildingLogic.Target_Spread", 10);
        o->s[1] = kvf(L, "ShardBuildingLogic.Uses_Limited_Targeting_Area", 0) != 0;
        set3(&o->f[17], 0, 0, 0); kvv(L, "ShardBuildingLogic.Limited_Targeting_Area_Center", &o->f[17], 3);
        o->f[20] = kvf(L, "ShardBuildingLogic.Limited_Targeting_Area_Radius", 250);
        memcpy(o->col, ic, 16);
        float M[3][3]; identity(M);
        int tp = find_proto(W, "Shard");
        float v[3] = { 0, 0.6f, 7 };
        if (tp >= 0) { float r[9] = { 0 }; if (kvv(W->proto[tp].line, "r", r, 9) == 9) for (int k = 0; k < 9; k++) M[k / 3][k % 3] = r[k]; kvv(W->proto[tp].line, "p", v, 3); }
        if (fabsf(M[0][0]) + fabsf(M[1][1]) + fabsf(M[2][2]) < 1e-6f) identity(M);
        float Rz[3][3]; rot_axis(Rz, 2, PI_F / 3);
        for (int k = 0; k < 7; k++) {
            int c = spawn(W, k < 6 ? "Shard" : "ShardCore", OK_SHARD); o = &W->o[i];
            if (c < 0) continue;
            nr_obj *q = &W->o[c];
            memcpy(q->scale, o->scale, 12);
            if (k < 6) {
                mat_mul(o->R, M, q->R);
                to_world(o, v, q->pos);
                mat_mul(Rz, M, M); mat_vec(Rz, v, v);
            } else {
                memcpy(q->R, o->R, 36);
                float cp[3] = { 0, 0, 5 }; to_world(o, cp, q->pos);
            }
            q->s[0] = 0; q->s[1] = -1; q->f[12] = kvf(q->line, "ShardLogic.Spin_Speed", 15);
            memcpy(q->col, ic, 16);
            obj_bounds(q); add_kid(o, c);
        }
        o->s[0] = 1; o->t = 0; o->s[2] = 0;
        break;
    }
    case OK_BARRELSPAWNER: {
        float td = kvf(L, "BarrelSpawnerLogic.trigger_dist_", 200);
        o->f[1] = td * td; o->f[2] = kvf(L, "BarrelSpawnerLogic.delay_between_spawns_", 2);
        o->f[3] = kvf(L, "BarrelSpawnerLogic.delay_before_first_spawn_", 1); o->s[1] = (int)kvf(L, "BarrelSpawnerLogic.max_barrels_", 10);
        o->f[4] = kvf(L, "BarrelSpawnerLogic.barrel_rot_vel_", 12); o->f[0] = 0;
        o->visible = 0;           /* the spawner itself is in the baked level */
        break;
    }
    case OK_STREETLIGHT:
        o->s[3] = has(L, "SimpleStreetLightFlickerLogic.");
        o->f[4] = kvf(L, "SimpleStreetLightFlickerLogic.Amount_Of_Time_The_Light_Is_Normally_On_For", 2);
        o->f[5] = kvf(L, "SimpleStreetLightFlickerLogic.Amount_Of_Time_The_Light_Is_Flickering_For", 10);
        o->s[2] = (int)kvf(L, "SimpleStreetLightFlickerLogic.Flicker_Mod_Number", 10);
        o->f[0] = 0; o->s[0] = 0; o->s[1] = 0; o->f[1] = o->f[2] = 0.1f; o->f[3] = 1;
        break;
    case OK_ANTENNA:
        o->f[0] = kvf(L, "ScalePulseLogic.t_val_", 1); o->f[1] = kvf(L, "ScalePulseLogic.min_scale_", 5);
        o->f[2] = kvf(L, "ScalePulseLogic.max_scale_", 10); o->f[3] = kvf(L, "ScalePulseLogic.min_brightness_", 1);
        o->f[4] = kvf(L, "ScalePulseLogic.max_brightness_", 2); o->s[0] = kvf(L, "ScalePulseLogic.t_inc_", 0) != 0;
        o->f[5] = kvf(L, "ScalePulseLogic.speed_", 6);
        break;
    case OK_CHECKPOINT: {      /* CheckpointLogic (0x727090 / 0x726fe0): the gate shows Initial Color until passed */
        float c0[4] = { 0.1f, 0.5f, 0.95f, 2.25f }, c1[4] = { 0.5f, 1, 0.5f, 6 }, c2[4] = { 0.2f, 1, 0.2f, 3 };
        kvv(L, "CheckpointLogic.Initial_Color", c0, 4); kvv(L, "CheckpointLogic.Activated_Start_Color", c1, 4);
        kvv(L, "CheckpointLogic.Activated_End_Color", c2, 4);
        memcpy(o->col, c0, 16); memcpy(o->base, c0, 16); memcpy(&o->f[0], c1, 16); memcpy(&o->f[4], c2, 16);
        o->s[0] = 0; o->f[8] = 0;
        break;
    }
    case OK_DISTCOLOR: {
        float n4[4] = { 0, 0, 0, 2 }, f4[4] = { 1, 1, 1, 2 };
        kvv(L, "DistanceColorChange.color_near_", n4, 4); kvv(L, "DistanceColorChange.color_far_", f4, 4);
        memcpy(&o->f[0], n4, 16); memcpy(&o->f[4], f4, 16);
        o->f[8] = kvf(L, "DistanceColorChange.dist_near_", 2000); o->f[9] = kvf(L, "DistanceColorChange.dist_far_", 5000);
        break;
    }
    case OK_COREBALL:
        o->f[1] = kvf(L, "CoreBallLogic.max_lightning_distance_", 25); o->f[2] = kvf(L, "CoreBallLogic.lightnings_per_sec_", 20);
        o->f[3] = kvf(L, "CoreBallLogic.min_life_", 0.8f); o->f[4] = kvf(L, "CoreBallLogic.max_life_", 2);
        o->f[5] = kvf(L, "CoreBallLogic.spark_width_", 2); o->f[6] = kvf(L, "CoreBallLogic.spark_brightness_", 25); o->f[7] = 0;
        break;
    case OK_FORCEFIELD: {
        o->visible = 0;
        o->f[0] = kvf(L, "TV_RadialForce.Radius", 30);
        float c[3] = { 0, 0, 0 }; kvv(L, "TV_RadialForce.Force_Coeffients", c, 3); memcpy(&o->f[1], c, 12);
        break;
    }
    case OK_TRIGGER: {         /* TriggerSphereZone / CylinderZone / ZoneAABox / ZoneOrientedBox (re_F2 1) */
        o->visible = 0;
        o->s[0] = 0;
        for (int z = 0; z < 4; z++) { char k[48]; snprintf(k, sizeof k, "%s.", ZONES[z]); if (has(L, k)) { o->s[0] = z; break; } }
        char k[64]; const char *Z = ZONES[o->s[0]];
        snprintf(k, sizeof k, "%s.Radius", Z); o->f[0] = kvf(L, k, 10);
        snprintf(k, sizeof k, "%s.Height", Z); o->f[1] = kvf(L, k, 30);       /* a half height, about world Z */
        float he[3] = { 20, 20, 20 }; snprintf(k, sizeof k, "%s.Half_Extents", Z); kvv(L, k, he, 3); memcpy(&o->f[2], he, 12);
        snprintf(k, sizeof k, "%s.Recheck_Delay", Z); o->f[5] = kvf(L, k, 0.1f);
        snprintf(k, sizeof k, "%s.OnEntryOnly", Z); o->s[2] = (int)kvf(L, k, 1);
        o->s[1] = 0; o->t = 0;
        break;
    }
    case OK_RECV:
        o->visible = 0; o->s[5] = 0;
        break;
    case OK_SLIDER: {          /* SliderLogic (re_F1 2): up and down its local Z at a constant speed */
        o->f[0] = kvf(L, "SliderLogic.Speed", 1); o->f[1] = kvf(L, "SliderLogic.Max_Distance", 2); o->f[2] = kvf(L, "SliderLogic.Initial_Offset", 0);
        memcpy(&o->f[3], o->pos, 12);
        float ax[3], off = (1 - fabsf(1 - o->f[2])) * o->f[1]; colv(o->R, 2, ax);
        for (int k = 0; k < 3; k++) o->pos[k] = o->f[3 + k] + ax[k] * off;
        o->s[0] = o->f[2] >= 1 ? 2 : 1;
        for (int k = 0; k < 3; k++) o->vel[k] = ax[k] * o->f[0] * (o->s[0] == 1 ? 1.0f : -1.0f);
        break;
    }
    case OK_SWINGER: {         /* SwingerLogic (re_F1 3): a pendulum about its own origin */
        float io = kvf(L, "SwingerLogic.Initial_Offset", 0), sp = kvf(L, "SwingerLogic.Speed", 2.1f), mc = kvf(L, "SwingerLogic.Min_Cosine", 0.75f);
        o->f[0] = sp; o->f[1] = mc;
        memcpy(o->lR, o->R, 36);                       /* q0, the rest orientation */
        float sg = 1 - 2 * fabsf(1 - io), h = 0.5f * sg * acosf(clampf(mc, -1, 1));
        float Y[3], w[3], Q[3][3]; colv(o->lR, 1, Y);
        identity(Q); set3(w, Y[0] * 2 * h, Y[1] * 2 * h, Y[2] * 2 * h); rotate_by(Q, w, 1);
        mat_mul(o->lR, Q, o->R);                       /* q0 x (cos h, sin h Y): Y taken in the local frame */
        float z0[3], zb[3], Yb[3]; colv(o->lR, 2, z0); colv(o->R, 2, zb); colv(o->R, 1, Yb);
        float k = (io >= 1 ? 1.0f : -1.0f) * sp * fmaxf(dot3(z0, zb) - mc + 0.005f, 0.0001f);
        set3(o->angvel, Yb[0] * k, Yb[1] * k, Yb[2] * k);
        break;
    }
    case OK_STOMPER:           /* StomperLogic (re_F1 4): the can is in the level; the foot is ours */
        o->visible = 0;
        o->f[0] = kvf(L, "StomperLogic.Wait_Duration", 1); o->f[1] = kvf(L, "StomperLogic.Warning_Duration", 0.75f);
        o->f[2] = kvf(L, "StomperLogic.Down_Duration", 0.3f); o->f[3] = kvf(L, "StomperLogic.Up_Duration", 0.5f);
        o->f[4] = kvf(L, "StomperLogic.Max_Distance", 1.4f);
        o->s[6] = -1; o->s[1] = 0;
        break;
    case OK_MORTAR:            /* MortarGunLogic (re_F1 1): the flame holo, one mine per approach */
        o->s[0] = 1; o->f[0] = 0; o->visible = 0;
        o->f[1] = kvf(L, "MortarGunLogic.Trigger_Distance", 200); o->f[2] = kvf(L, "MortarGunLogic.Reset_Distance", 350);
        o->f[3] = kvf(L, "MortarGunLogic.Power", 200); o->f[4] = kvf(L, "MortarGunLogic.Spin", 15); o->f[5] = kvf(L, "MortarGunLogic.Max_Alpha", 5);
        break;
    case OK_DROPSHIP: {        /* DropShipLogic (re_F1 5): a ship flies over and drops a block on the pad */
        o->visible = 0; o->f[0] = 0; o->s[0] = 0; o->s[1] = o->s[2] = -1;
        o->f[1] = kvf(L, "DropShipLogic.Trigger_Distance", 250); o->f[2] = kvf(L, "DropShipLogic.Reset_Distance", 300);
        o->f[3] = kvf(L, "DropShipLogic.Reset_Time", 5); o->f[4] = kvf(L, "DropShipLogic.Ship_Height", 15);
        o->f[5] = kvf(L, "DropShipLogic.Ship_Speed", 250); o->f[6] = kvf(L, "DropShipLogic.Ship_Delay_Time", 1.5f);
        o->f[7] = kvf(L, "DropShipLogic.Max_Alpha", 5); o->f[8] = kvf(L, "DropShipLogic.Block_Offset", 0.9f);
        float so[3] = { 0, -0.36f, 3.1f }; kvv(L, "DropShipLogic.Ship_Offset", so, 3); memcpy(&o->f[9], so, 12);
        break;
    }
    case OK_COOLPAD: {         /* CooldownPadLogic (re_F2 5.1) */
        float so[3] = { 0.35f, 1.5f, 0.2f }, po[3] = { 0, 0, 0.95f }, pc[4] = { 0.2f, 1, 0.3f, 1 };
        kvv(L, "CooldownPadLogic.Scale_Offset", so, 3); kvv(L, "CooldownPadLogic.Scaled_Pos_Offset", po, 3); kvv(L, "CooldownPadLogic.On_Pad_Color", pc, 4);
        memcpy(&o->f[0], so, 12); memcpy(&o->f[3], po, 12); memcpy(&o->f[6], pc, 16);
        break;
    }
    case OK_CARCOOLER: case OK_EMITTER: o->visible = 0; break;
    case OK_FISHZONE: o->visible = 0; o->f[0] = kvf(L, "FishZone.InnerRadius", 20); break;
    case OK_FISH: o->s[0] = 0; set3(o->vel, 0, 0, 0); break;         /* the target is picked on the first frame */
    case OK_GARAGE: o->visible = 0; o->s[0] = 0; o->f[0] = kvf(L, "GarageLogic.Spawn_Distance", 340); break;
    case OK_COREEXPL: o->s[0] = 0; o->s[1] = -1; o->t = 0; break;
    case OK_EYE: o->col[3] = o->base[3] = 0.1f; break;               /* FlickerOnHack: glow 0.1 outside the story's intro */
    case OK_FINISH: o->visible = 0; o->s[0] = 0; o->f[0] = 0; break;    /* EnumLogic type 6: the tunnel is in the level */
    case OK_HUMVEE: o->s[7] = -1; break;
    default: break;
    }
}

/* ------------------------------------------------------------------ loading */
static int kind_from(const char *k)
{
    static const struct { const char *n; int k; } T[] = {
        { "saw", OK_SAW }, { "bafsaw", OK_BAFSAW }, { "drone", OK_DRONE }, { "blimp", OK_BLIMP }, { "train", OK_TRAIN },
        { "spot", OK_SPOT }, { "laserarch", OK_LASERARCH }, { "sweep", OK_SWEEP }, { "spinlaser", OK_SPINLASER },
        { "blocker", OK_BLOCKER }, { "streetlight", OK_STREETLIGHT }, { "screen", OK_SCREEN }, { "mineparent", OK_MINEPARENT },
        { "shardbld", OK_SHARDBLD }, { "barrelspawner", OK_BARRELSPAWNER }, { "antenna", OK_ANTENNA },
        { "distcolor", OK_DISTCOLOR }, { "coreball", OK_COREBALL }, { "forcefield", OK_FORCEFIELD },
        { "trigger", OK_TRIGGER }, { "recv", OK_RECV }, { "slider", OK_SLIDER }, { "swinger", OK_SWINGER }, { "stomper", OK_STOMPER },
        { "mortar", OK_MORTAR }, { "dropship", OK_DROPSHIP }, { "coolpad", OK_COOLPAD }, { "carcooler", OK_CARCOOLER }, { "finish", OK_FINISH }, { "emitter", OK_EMITTER },
        { "fishzone", OK_FISHZONE }, { "fish", OK_FISH }, { "garage", OK_GARAGE }, { "humvee", OK_HUMVEE },
        { "coreexploder", OK_COREEXPL }, { "eye", OK_EYE }, { "checkpoint", OK_CHECKPOINT } };
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcmp(T[i].n, k)) return T[i].k;
    return OK_NONE;
}

static void create_all(nr_objs *W)
{
    NTRAINP = 0;
    W->n = 0; W->screens_broken = W->lights_broken = 0; W->fish_zone = -1; W->fish_zone_changed = 0;
    for (int k = 0; k < W->nhv; k++) W->hv[k].used = 0;
    uint32_t maxid = 0;
    for (const char *p = W->text; p < W->text + W->textlen; p = strchr(p, '\0') + 1) {
        if (!strncmp(p, "obj ", 4)) {
            char k[24]; kvs(p, "k", k, sizeof k);
            int kind = kind_from(k);
            int id = (int)kvf(p, "id", 0);
            if (kind != OK_NONE) {
                int i = obj_new(W, kind, p);
                if (i >= 0) { W->o[i].id = id; if ((uint32_t)id > maxid) maxid = id; }
            }
        }
    }
    free(W->off);
    W->noff = maxid + 1; W->off = calloc(W->noff, 1);
    if (W->coll) { W->coll->off = W->off; W->coll->noff = W->noff; W->coll->bodies = W->bodies; W->coll->ndyn = 0; }
    int n0 = W->n;
    for (int i = 0; i < n0; i++) init_obj(W, i);
}

int nr_objs_load(nr_objs *W, const char *nrg, nr_coll *coll, const nr_objs_host *host)
{
    memset(W, 0, sizeof *W);
    W->host = *host; W->coll = coll; W->rng = 0x12345678u;
    size_t n = strlen(nrg);
    W->text = malloc(n + 2); memcpy(W->text, nrg, n + 1); W->text[n + 1] = 0;
    W->textlen = (int)n + 1;
    for (char *p = W->text; *p; p++) if (*p == '\n' || *p == '\r') *p = 0;
    /* blueprints the logic spawns */
    for (const char *p = W->text; p < W->text + n; p = strchr(p, '\0') + 1) {
        if (strncmp(p, "spawn ", 6)) continue;
        void *t = realloc(W->proto, sizeof *W->proto * (W->nproto + 1)); if (!t) break;
        W->proto = t;
        kvs(p, "bp", W->proto[W->nproto].bp, sizeof W->proto[W->nproto].bp);
        W->proto[W->nproto].line = p;
        W->nproto++;
    }
    /* their models and collision now, not the first time one appears in the race (a hitch) */
    for (int i = 0; i < W->nproto; i++) {
        char b[64]; float mn[3], mx[3];
        if (kvs(W->proto[i].line, "m", b, sizeof b)) model_of(W, b, mn, mx);
        if (kvs(W->proto[i].line, "cm", b, sizeof b)) cmodel_of(W, b);
    }
    create_all(W);
    return W->n;
}

void nr_objs_restart(nr_objs *W)
{
    for (int i = 0; i < NTRAINP; i++) bez_free(&TRAINP[i]);
    create_all(W);
}

void nr_objs_free(nr_objs *W)
{
    for (int i = 0; i < NTRAINP; i++) bez_free(&TRAINP[i]);
    NTRAINP = 0;
    for (int i = 0; i < W->ncm; i++) { free(W->cm[i].t); free(W->cm[i].bb); }
    if (W->coll) { W->coll->off = NULL; W->coll->noff = 0; W->coll->bodies = NULL; W->coll->ndyn = 0; }
    free(W->o); free(W->proto); free(W->cm); free(W->mdl); free(W->off); free(W->bodies); free(W->text); free(W->hv);
    memset(W, 0, sizeof *W);
}

/* ------------------------------------------------------------------ effects */
static void fx_init(nr_obj *q)
{
    const char *L = q->line;
    q->f[1] = kvf(L, "EnumLogic.Time", 1); q->f[0] = q->f[1]; q->f[2] = kvf(L, "EnumLogic.Speed", 0);
    float s4[4], e4[4]; memcpy(s4, q->col, 16); memcpy(e4, q->col, 16); e4[3] = 0;
    kvv(L, "EnumLogic.Start_Color", s4, 4); kvv(L, "EnumLogic.End_Color", e4, 4);
    memcpy(&q->f[3], s4, 16); memcpy(&q->f[7], e4, 16);
    q->s[0] = (int)kvf(L, "EnumLogic.Type", 4);
}
static int spawn_fx(nr_objs *W, const char *bp, const float *pos, float scale)
{
    int c = spawn(W, bp, OK_FX);
    if (c < 0) return -1;
    nr_obj *q = &W->o[c];
    memcpy(q->pos, pos, 12); q->scale[0] *= scale; q->scale[1] *= scale; q->scale[2] *= scale; identity(q->R);
    fx_init(q); obj_bounds(q);
    return c;
}
static int spawn_debris(nr_objs *W, const char *bp, const float *pos, const float R[3][3], const float *scale, const float *vel, float life)
{
    int c = spawn(W, bp, OK_DEBRIS);
    if (c < 0) return -1;
    nr_obj *q = &W->o[c];
    memcpy(q->pos, pos, 12); memcpy(q->R, R, 36); memcpy(q->scale, scale, 12);
    memcpy(q->vel, vel, 12);
    set3(q->angvel, frange(W, -6, 6), frange(W, -6, 6), frange(W, -6, 6));
    q->f[0] = life; q->dynamic = 1; q->cmodel = -1;
    obj_bounds(q);
    return c;
}
static int spawn_spark(nr_objs *W, float life, float width, float bright)
{
    char nm[16]; snprintf(nm, sizeof nm, "sparkdead%d", 1 + (int)(rnd(W) % 4));
    int c = spawn(W, nm, OK_SPARK);
    if (c < 0) return -1;
    nr_obj *q = &W->o[c];
    q->f[0] = 0; q->f[1] = life; q->f[2] = bright; q->f[9] = width;
    return c;
}
static void place_spark(nr_obj *q, const nr_objs_frame *f)
{
    float *p1 = &q->f[3], *p2 = &q->f[6], d[3] = { p2[0] - p1[0], p2[1] - p1[1], p2[2] - p1[2] }, l = len3(d);
    if (l < 0.01f) { q->visible = 0; return; }
    float dir[3] = { d[0] / l, d[1] / l, d[2] / l }, tc[3] = { f->cam_pos[0] - p1[0], f->cam_pos[1] - p1[1], f->cam_pos[2] - p1[2] }, n[3], z[3];
    cross3(dir, tc, n); norm3(n); cross3(n, dir, z);
    set_cols(q->R, n, dir, z); memcpy(q->pos, p1, 12);
    set3(q->scale, q->f[9] * l, l, l);
    q->visible = 1; q->radius = l + 1;
}

/* the air mine goes off: wreck pieces, a flash, a push */
static void mine_explode(nr_objs *W, int i, const float *car_pos, float *car_dv)
{
    nr_obj *o = &W->o[i];
    if (!o->alive) return;
    float p[3]; memcpy(p, o->pos, 12);
    float s = o->scale[0], R[3][3]; memcpy(R, o->R, 36);
    static const char *G[10] = { "DeadGunHumveeBack", "DeadGunHumveeFront", "DeadGunHumveeTop", "DeadGunHumveeSideBack", "DeadGunHumveeSideBack",
                                 "DeadGunHumveeSideFront", "DeadGunHumveeSideFront", "DeadGunHumveeSideMid", "DeadGunHumveeSideMid", "DeadGunHumveeWindow" };
    static const int MIR[10] = { 0, 0, 0, 0, 1, 0, 1, 0, 1, 0 };
    float k[3] = { 10 * s, 10 * s, 40 * s }, R15 = 15 * s, mx = 100 * s;
    for (int g = 0; g < 10; g++) {
        int pi = find_proto(W, G[g]);
        if (pi < 0) continue;
        float lp[3] = { 0, 0, 0 }, r9[9] = { 0 }, lr[3][3], wr[3][3];
        kvv(W->proto[pi].line, "p", lp, 3);
        if (MIR[g]) lp[0] = -lp[0];
        identity(lr);
        if (kvv(W->proto[pi].line, "r", r9, 9) == 9 && fabsf(r9[0]) + fabsf(r9[4]) + fabsf(r9[8]) > 1e-6f) for (int j = 0; j < 9; j++) lr[j / 3][j % 3] = r9[j];
        float sl[3] = { lp[0] * s, lp[1] * s, lp[2] * s }, w[3], at[3];
        mat_vec(R, sl, w);
        for (int j = 0; j < 3; j++) at[j] = p[j] + w[j];
        mat_mul(R, lr, wr);
        float dv[3] = { 0, 0, 0 }; radial_push(p, R15, k, mx, at, dv);
        float sc[3] = { s * 0.5f, s * 0.5f, s * 0.5f };
        spawn_debris(W, G[g], at, wr, sc, dv, frange(W, 3, 4));
    }
    int fx = spawn_fx(W, "MineExplosion", p, 1); if (fx >= 0) { W->o[fx].scale[0] = W->o[fx].scale[1] = W->o[fx].scale[2] = s * 3; obj_bounds(&W->o[fx]); }
    if (car_pos && car_dv) radial_push(p, R15, k, mx, car_pos, car_dv);
    play(W, "Play_Rand_Mine_Explosion", p);
    kill_obj(W, i);
}

/* ------------------------------------------------------------------ helpers of the bonus levels' movers */
/* distance from p to the car's box (0 inside) */
static float car_box_dist(const nr_objs_frame *f, const float *p)
{
    float rel[3] = { p[0] - f->car_pos[0], p[1] - f->car_pos[1], p[2] - f->car_pos[2] }, d2 = 0;
    for (int k = 0; k < 3; k++) {
        float l = f->car_rot[0][k] * rel[0] + f->car_rot[1][k] * rel[1] + f->car_rot[2][k] * rel[2];
        float e = fabsf(l) - f->car_half[k];
        if (e > 0) d2 += e * e;
    }
    return sqrtf(d2);
}
/* the holograms' fade (MortarGun 0x6b3f60, DropShip 0x6b4700): glow = (int)(MaxAlpha x alpha x 10) / 10 */
static void holo_fade(nr_obj *o, float dt, int in, float maxa)
{
    if (in) {
        if (o->f[0] < 1) { o->f[0] += dt; o->col[3] = floorf(maxa * o->f[0] * 10) / 10; o->visible = 1; }
    } else {
        if (o->f[0] > 0) { o->f[0] = fmaxf(o->f[0] - dt, 0); o->col[3] = floorf(maxa * o->f[0] * 10) / 10; o->visible = 1; }
        else o->visible = 0;
    }
}
static void world_of(const float R[3][3], const float *O, const float *l, float *out)
{
    float w[3]; mat_vec(R, l, w);
    for (int k = 0; k < 3; k++) out[k] = O[k] + w[k];
}

/* StomperLogic::NextState (0x6cba90) */
static void stomper_next(nr_objs *W, int i, const nr_objs_frame *f)
{
    nr_obj *o = &W->o[i], *ft = &W->o[o->s[6]];
    char pat[32]; kvs(o->line, "StomperLogic.Stomp_Pattern", pat, sizeof pat);
    if (!pat[0]) snprintf(pat, sizeof pat, "01230");
    int n = (int)strlen(pat);
    o->t = 0; o->s[0] = pat[o->s[1] % n] - '0';
    float Z[3]; colv(ft->R, 2, Z);
    float D = ft->scale[2] * o->f[4], *rest = &o->f[6];
    switch (o->s[0]) {
    case 0: set3(ft->vel, 0, 0, 0); break;
    case 1: o->f[9] = 0; set3(ft->vel, 0, 0, 0); play(W, "Play_StomperAlarm", ft->pos); break;
    case 2: {
        float travel = 1, dist, nn[3], down[3] = { -Z[0], -Z[1], -Z[2] };
        if (W->coll && D > 0) {
            uint32_t saved = W->coll->ndyn; W->coll->ndyn = 0;
            if (nr_coll_ray(W->coll, rest, down, D, &dist, nn, NULL)) travel = dist / D;
            W->coll->ndyn = saved;
        }
        float seg[3] = { down[0] * D, down[1] * D, down[2] * D }, tc;
        if (seg_hits_car(f, rest, seg, &tc) && tc < travel) travel = tc;     /* the ray hits the car too */
        o->f[5] = travel;
        float v = travel * D / fmaxf(o->f[2], 1e-3f);
        set3(ft->vel, down[0] * v, down[1] * v, down[2] * v);
        break;
    }
    case 3: { float v = o->f[5] * D / fmaxf(o->f[3], 1e-3f); set3(ft->vel, Z[0] * v, Z[1] * v, Z[2] * v); break; }
    }
    o->s[1] = (o->s[1] + 1) % n;
}

/* MortarMineLogic::Explode (0x688350): 20 pieces, a flash, a push scaled by the mine's size */
static void mortar_explode(nr_objs *W, int i, nr_objs_frame *f)
{
    nr_obj *o = &W->o[i];
    float p[3], R[3][3], sc[3]; memcpy(p, o->pos, 12); memcpy(R, o->R, 36); memcpy(sc, o->scale, 12);
    float s = sc[0], k[3] = { 10 * s, 10 * s, 80 * s }, Rr = 7 * s, mx = 50 * s;
    for (int g = 1; g <= 20; g++) {
        char nm[16]; snprintf(nm, sizeof nm, "mineGib%02d", g);
        int pi = find_proto(W, nm);
        if (pi < 0) continue;
        float lp[3] = { 0, 0, 0 }, r9[9] = { 0 }, lr[3][3], wr[3][3], at[3];
        kvv(W->proto[pi].line, "p", lp, 3);
        identity(lr);
        if (kvv(W->proto[pi].line, "r", r9, 9) == 9 && fabsf(r9[0]) + fabsf(r9[4]) + fabsf(r9[8]) > 1e-6f) for (int j = 0; j < 9; j++) lr[j / 3][j % 3] = r9[j];
        float sl[3] = { lp[0] * sc[0], lp[1] * sc[1], lp[2] * sc[2] };
        world_of(R, p, sl, at);
        mat_mul(R, lr, wr);
        float dv[3] = { 0, 0, 0 }; radial_push(p, Rr, k, mx, at, dv);
        spawn_debris(W, nm, at, wr, sc, dv, frange(W, 3, 4));
    }
    int fx = spawn_fx(W, "MineExplosion2", p, 1);
    if (fx >= 0) { nr_obj *q = &W->o[fx]; for (int j = 0; j < 3; j++) q->scale[j] = sc[j] * 3; obj_bounds(q); }
    if (f->car_active) radial_push(p, Rr, k, mx, f->car_pos, f->car_dv);
    play(W, "Play_Rand_Mine_Explosion", p);
    kill_obj(W, i);
}

/* DropShipLogic::ResetDrop (0x6b4900) */
static void dropship_reset(nr_objs *W, int i)
{
    nr_obj *o = &W->o[i];
    o->s[0] = 0;
    if (o->s[1] >= 0) kill_obj(W, o->s[1]);
    if (o->s[2] >= 0) kill_obj(W, o->s[2]);
    o->s[1] = o->s[2] = -1;
}

/* ------------------------------------------------------------------ the trigger system (re_F2 1-2) */
static const char *RECEIVERS[] = { "TriggerAchievement", "TriggerEndStory", "TriggerRespawnLocation", "TriggerControlScheme",
                                   "TriggerTutorialText", "TriggerBranchOnVehicle", "TriggerDelay", "TriggerAnnouncer",
                                   "TriggerChainer", "TriggerBase", "TriggerSphereZone", "TriggerCylinderZone", "TriggerZoneAABox",
                                   "TriggerZoneOrientedBox" };
const char *nr_objs_value(const char *line, const char *key, char *buf, int n, int spaces)
{
    if (!line || n < 1) return NULL;
    if (!kvs(line, key, buf, n)) return NULL;
    if (spaces) for (char *c = buf; *c; c++) if (*c == '_') *c = ' ';
    return buf;
}
static void send_out(nr_objs *W, const char *line, const char *comp, const char *field)
{
    char k[96], out[96]; snprintf(k, sizeof k, "%s.%s", comp, field);
    if (kvs(line, k, out, sizeof out)) nr_objs_broadcast(W, out);
}
static void fire(nr_objs *W, int i, const char *comp)
{
    const char *L = W->o[i].line;
    if (!strcmp(comp, "TriggerAchievement")) {
        if (W->host.event) W->host.event(W->host.user, "achievement", (int)kvf(L, "TriggerAchievement.Achievement_ID", 0));
    } else if (!strcmp(comp, "TriggerEndStory")) {          /* corecorecore -> corecorecore2 (0x726af0) */
        for (int k = 0; k < W->n; k++) {
            nr_obj *b = &W->o[k];
            if (!b->alive || b->kind != OK_COREBALL || b->s[6]) continue;
            b->s[6] = 1; b->f[10] = 0; b->f[11] = b->col[3];
            if (b->id > 0 && (uint32_t)b->id < W->noff) W->off[b->id] = 1;   /* corecorecore2 has no collider */
        }
        if (W->host.event) W->host.event(W->host.user, "EndStory", 0);
    } else if (!strcmp(comp, "TriggerRespawnLocation")) {          /* stores the respawn point only (0x7216e0) */
        nr_obj *o = &W->o[i];
        float loc[3] = { 0, 0, 0 }; kvv(L, "TriggerRespawnLocation.Respawn_Location", loc, 3);
        int abs_ = (int)kvf(L, "TriggerRespawnLocation.Absolute_Positioning", 0);
        for (int k = 0; k < 3; k++) W->respawn_pos[k] = abs_ ? loc[k] : o->pos[k] + loc[k];
        memcpy(W->respawn_R, o->R, 36); W->respawn_set = 1;
    } else if (!strcmp(comp, "TriggerControlScheme")) {            /* a gamepad: the Basic or the Advanced scheme (0x721dd0) */
        send_out(W, L, comp, "Outgoing");
        send_out(W, L, comp, W->advanced ? "Outgoing__Advanced_Controls_" : "Outgoing__Basic_Controls_");
    } else if (!strcmp(comp, "TriggerTutorialText")) {
        send_out(W, L, comp, "Outgoing");
        W->tutorial = L;
    } else if (!strcmp(comp, "TriggerBranchOnVehicle")) {
        char v[48]; kvs(L, "TriggerBranchOnVehicle.Vehicle_Needed", v, sizeof v);
        if (!strcmp(v, W->vehicle)) send_out(W, L, comp, "Outgoing");
    } else if (!strcmp(comp, "TriggerDelay")) {
        nr_obj *o = &W->o[i];
        if (!o->s[5] || kvf(L, "TriggerDelay.flag", 0) != 0) { o->f[20] = 0; o->s[5] = 1; }
    } else send_out(W, L, comp, "Outgoing");                         /* TriggerBase::Fire: pass it on */
}
void nr_objs_broadcast(nr_objs *W, const char *ev)
{
    if (!ev || !ev[0] || !strcmp(ev, "-") || W->depth > 8) return;
    W->depth++;
    int n0 = W->n;
    for (int i = 0; i < n0; i++) {
        nr_obj *o = &W->o[i];
        if (o->kind != OK_TRIGGER && o->kind != OK_RECV) continue;
        for (unsigned r = 0; r < sizeof RECEIVERS / sizeof RECEIVERS[0]; r++) {
            char k[64], inc[96]; snprintf(k, sizeof k, "%s.Incoming", RECEIVERS[r]);
            if (kvs(o->line, k, inc, sizeof inc) && !strcmp(inc, ev)) fire(W, i, RECEIVERS[r]);
        }
    }
    W->depth--;
}
void nr_objs_level_finished(nr_objs *W)
{
    int n0 = W->n;
    for (int i = 0; i < n0; i++)
        if ((W->o[i].kind == OK_TRIGGER || W->o[i].kind == OK_RECV) && has(W->o[i].line, "TriggerAtFinish."))
            send_out(W, W->o[i].line, "TriggerAtFinish", "Outgoing");
}
/* CheckpointLogic as the car passes (0x727c10): the gate lights up (its colour fades from Activated
 * Start to End), five ghosts of it rise and fade (EnumLogic type 1) */
void nr_objs_checkpoint_passed(nr_objs *W, const float pos[3], const float R[3][3])
{
    int best = -1; float bd = 40 * 40;
    for (int i = 0; i < W->n; i++) {
        const nr_obj *o = &W->o[i];
        if (!o->alive || o->kind != OK_CHECKPOINT) continue;
        float d[3] = { o->pos[0] - pos[0], o->pos[1] - pos[1], o->pos[2] - pos[2] }, dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        if (dd < bd) { bd = dd; best = i; }
    }
    if (best >= 0) { W->o[best].s[0] = 1; W->o[best].f[8] = 0; }
    for (int i = 0; i < 5; i++) {
        int c = spawn(W, "checkpoint_ghost", OK_FX);
        if (c < 0) return;
        nr_obj *q = &W->o[c];
        float y[3]; colv(R, 1, y);
        for (int k = 0; k < 3; k++) q->pos[k] = pos[k] + y[k] * i;
        memcpy(q->R, R, 36);
        fx_init(q);
        q->s[0] = 1; q->f[1] = q->f[0] = (5 - i) * 0.3f; q->f[2] = 40.0f * i;
        obj_bounds(q);
    }
}
void nr_objs_checkpoint_reset(nr_objs *W)
{
    for (int i = 0; i < W->n; i++) {
        if (!W->o[i].alive) continue;
        if (W->o[i].kind == OK_DROPSHIP) dropship_reset(W, i);
        if (W->o[i].kind == OK_GARAGE) W->o[i].s[0] = 0;                 /* a new pair next time */
        if (W->o[i].kind == OK_HUMVEE && W->o[i].s[7] >= 0) humvee_free(W, i);   /* the drones go quietly */
        if (W->o[i].kind == OK_PHASER) kill_obj(W, i);
    }
}

/* ------------------------------------------------------------------ the Observatory's fish (re_F1 8) */
static void fish_target(nr_objs *W, nr_obj *o, const nr_objs_frame *f)
{
    int z = W->fish_zone;
    if (z >= 0 && z < W->n && W->o[z].kind == OK_FISHZONE) {
        const nr_obj *Z = &W->o[z];
        float a = fminf(Z->f[0], Z->scale[0]), b = fmaxf(Z->f[0], Z->scale[0]);
        float r = frange(W, a, b), th = frange(W, -PI_F, PI_F), h = frange(W, -Z->scale[1], Z->scale[1]);
        float c0[3], c1[3], c2[3]; colv(Z->R, 0, c0); colv(Z->R, 1, c1); colv(Z->R, 2, c2);
        for (int k = 0; k < 3; k++) o->f[k] = Z->pos[k] + c0[k] * r * cosf(th) + c2[k] * r * sinf(th) + c1[k] * h;
    } else for (int k = 0; k < 3; k++) o->f[k] = f->car_pos[k] + 3 * f->car_vel[k];
}
static void fish_update(nr_objs *W, int i, const nr_objs_frame *f, float dt)
{
    nr_obj *o = &W->o[i];
    if (!o->s[0] || W->fish_zone_changed) {
        fish_target(W, o, f);
        if (W->fish_zone_changed || !o->s[0]) { memcpy(o->pos, o->f, 12); o->s[0] = 1; return; }   /* they all move to the new zone */
    }
    float d[3] = { o->f[0] - o->pos[0], o->f[1] - o->pos[1], o->f[2] - o->pos[2] };
    if (dot3(d, d) < 36) { fish_target(W, o, f); for (int k = 0; k < 3; k++) d[k] = o->f[k] - o->pos[k]; }
    float sep[3] = { 0, 0, 0 }, coh[3] = { 0, 0, 0 }, al[3] = { 0, 0, 0 }; int nn = 0;
    for (int j = 0; j < W->n; j++) {
        nr_obj *q = &W->o[j];
        if (j == i || !q->alive || q->kind != OK_FISH) continue;
        float e[3] = { q->pos[0] - o->pos[0], q->pos[1] - o->pos[1], q->pos[2] - o->pos[2] }, e2 = dot3(e, e);
        if (e2 > 19 * 19) continue;
        if (e2 < 0.001f) { set3(e, frange(W, -1, 1), frange(W, -1, 1), frange(W, -1, 1)); e2 = dot3(e, e) + 1e-6f; }
        float el = sqrtf(e2), g = 7.7f * 7.7f / e2 - 1;
        if (g >= 0) madd3(sep, e, -(0.5f * g * 150) / el); else madd3(coh, e, -(g * 150) / el);
        madd3(al, q->vel, 1); nn++;
    }
    if (nn) for (int k = 0; k < 3; k++) al[k] = (al[k] / nn - o->vel[k]) * 5.5f;
    float dl = len3(d), seek[3];
    for (int k = 0; k < 3; k++) seek[k] = ((dl > 1e-4f ? d[k] / dl * 300 : 0) - o->vel[k]) * 0.05f;
    /* the rules in order of priority until MaxAcc (60) is used up */
    float acc[3] = { 0, 0, 0 }, left = 60;
    const float *rules[4] = { sep, coh, al, seek };
    for (int r = 0; r < 4 && left > 0; r++) {
        float l = len3(rules[r]);
        if (l <= left) { madd3(acc, rules[r], 1); left -= l; }
        else { madd3(acc, rules[r], left / l); left = 0; }
    }
    for (int k = 0; k < 3; k++) o->vel[k] = (o->vel[k] + acc[k] * dt) * 0.98f;
    madd3(o->pos, o->vel, dt);
    float sp = len3(o->vel);
    if (sp > 0.5f) {                                    /* nose along the velocity, level */
        float y[3] = { o->vel[0] / sp, o->vel[1] / sp, o->vel[2] / sp }, up[3] = { 0, 0, 1 }, x[3], z[3];
        cross3(y, up, x); if (len3(x) < 1e-3f) return;
        norm3(x); cross3(x, y, z);
        set_cols(o->R, x, y, z);
    }
}

/* ------------------------------------------------------------------ the CORE drones (re_J) */
static const float HV_HALF[3] = { 1.0635f, 2.507f, 1.1025f };           /* ColliderOrientedBox (half extents) */
static int hv_params(nr_objs *W)
{
    if (W->hvp_loaded) return W->hvp_loaded > 0;
    W->hvp_loaded = -1;
    uint32_t sz = 0; uint8_t *b = W->host.read ? W->host.read(W->host.user, "models/humvee.cfg", &sz) : NULL;
    if (!b) return 0;
    char *t = malloc(sz + 1); memcpy(t, b, sz); t[sz] = 0; free(b);
    nr_car_default_params(&W->hvp);
    int n = nr_car_load_params(&W->hvp, t);
    free(t);
    float mn[3] = { -HV_HALF[0], -HV_HALF[1], -HV_HALF[2] };
    nr_car_set_hull(&W->hvp, mn, HV_HALF);
    W->hvp_loaded = n > 0 ? 1 : -1;
    return W->hvp_loaded > 0;
}
/* a drone at pos, turned as R (columns right, forward, up) -> object index */
static int humvee_new(nr_objs *W, const float pos[3], const float R[3][3])
{
    if (!hv_params(W)) return -1;
    int slot = -1;
    for (int k = 0; k < W->nhv; k++) if (!W->hv[k].used) { slot = k; break; }
    if (slot < 0) {
        if (W->nhv >= 16) return -1;
        void *t = realloc(W->hv, sizeof *W->hv * (W->nhv + 1)); if (!t) return -1;
        W->hv = t; slot = W->nhv++;
    }
    int i = spawn(W, "GunHumvee", OK_HUMVEE);
    if (i < 0) return -1;
    struct nr_humvee *h = &W->hv[slot];
    memset(h, 0, sizeof *h); h->used = 1; h->obj = i;
    nr_car_reset_rot(&h->car, pos, R);
    nr_obj *o = &W->o[i];
    float mn[3], mx[3]; o->model = (int16_t)model_of(W, "humvee", mn, mx); memcpy(o->bmin, mn, 12); memcpy(o->bmax, mx, 12);
    memcpy(o->pos, pos, 12); memcpy(o->R, R, 36); set3(o->scale, 1, 1, 1); obj_bounds(o);
    o->s[7] = slot; o->s[0] = 0; o->t = 0; o->s[1] = o->s[2] = 1; o->s[3] = 0;
    for (int k = 0; k < 8; k++) o->f[k] = 0;
    o->nkid = 0;
    int b = spawn(W, "GunHumveeBase", OK_HVPART), g = spawn(W, "GunHumveeGun", OK_HVPART);
    o = &W->o[i];
    if (b >= 0) { add_kid(o, b); W->o[b].parent = (int16_t)i; }
    if (g >= 0) { add_kid(o, g); W->o[g].parent = (int16_t)i; W->o[g].sndloop = 0; snprintf(W->o[g].snd, sizeof W->o[g].snd, "Play_HumveePhaser"); }
    return i;
}
static void humvee_free(nr_objs *W, int i)
{
    nr_obj *o = &W->o[i];
    if (o->s[7] >= 0 && o->s[7] < W->nhv) W->hv[o->s[7]].used = 0;
    kill_obj(W, i);
}
/* world transforms of the turret: base = humvee x T(0,-0.445,0.674) RotZ(yaw); gun = base x T(0,0.168,0.247) RotX(pitch) */
static void humvee_pose(nr_objs *W, int i)
{
    nr_obj *o = &W->o[i];
    float Ry[3][3], Rp[3][3], Rb[3][3];
    rot_axis(Ry, 2, o->f[4]); rot_axis(Rp, 0, o->f[5]);
    mat_mul(o->R, Ry, Rb);
    if (o->nkid > 0) {
        nr_obj *b = &W->o[o->kid[0]];
        float lb[3] = { 0, -0.445f, 0.674f }; world_of(o->R, o->pos, lb, b->pos); memcpy(b->R, Rb, 36);
        if (o->nkid > 1) {
            nr_obj *g = &W->o[o->kid[1]];
            float lg[3] = { 0, 0.168f, 0.247f }; world_of(Rb, b->pos, lg, g->pos); mat_mul(Rb, Rp, g->R);
        }
    }
}
static void humvee_destroy(nr_objs *W, int i, nr_objs_frame *f)
{
    nr_obj *o = &W->o[i];
    if (!o->alive) return;
    struct nr_humvee *h = &W->hv[o->s[7]];
    float P[3], R[3][3], v[3], w[3]; memcpy(P, o->pos, 12); memcpy(R, o->R, 36); memcpy(v, h->car.vel, 12); memcpy(w, h->car.angvel, 12);
    int sw = spawn_fx(W, "ShardShockwave", P, 1); o = &W->o[i];
    float k[3] = { 10, 100, 0 };
    if (sw >= 0) memcpy(W->o[sw].R, R, 36);
    if (f && f->car_active) radial_push(P, 10, k, 25, f->car_pos, f->car_dv);
    static const char *G[10] = { "Back", "Front", "Top", "SideBack", "SideBack", "SideFront", "SideFront", "SideMid", "SideMid", "Window" };
    static const int MIR[10] = { 0, 0, 0, 0, 1, 0, 1, 0, 1, 0 };
    for (int g = 0; g < 10; g++) {
        char nm[40]; snprintf(nm, sizeof nm, "DeadGunHumvee%s", G[g]);
        int pi = find_proto(W, nm);
        if (pi < 0) continue;
        float lp[3] = { 0, 0, 0 }, r9[9] = { 0 }, lr[3][3], wr[3][3], at[3], sc[3] = { 1, 1, 1 };
        kvv(W->proto[pi].line, "p", lp, 3); kvv(W->proto[pi].line, "s", sc, 3);
        if (MIR[g]) lp[0] = -lp[0];
        identity(lr);
        if (kvv(W->proto[pi].line, "r", r9, 9) == 9 && fabsf(r9[0]) + fabsf(r9[4]) + fabsf(r9[8]) > 1e-6f) for (int j = 0; j < 9; j++) lr[j / 3][j % 3] = r9[j];
        world_of(R, P, lp, at); mat_mul(R, lr, wr);
        float dv[3] = { v[0] + frange(W, -5, 5), v[1] + frange(W, -5, 5), v[2] + frange(W, -5, 5) };
        radial_push(P, 10, k, 25, at, dv);
        int d = spawn_debris(W, nm, at, wr, sc, dv, frange(W, 3, 4));
        if (d >= 0) memcpy(W->o[d].angvel, w, 12);
    }
    o = &W->o[i];
    for (int p_ = 0; p_ < 2 && p_ < o->nkid; p_++) {         /* the turret's base and gun */
        nr_obj *q = &W->o[o->kid[p_]];
        const char *nm = p_ == 0 ? "DeadGunHumveeBase" : "DeadGunHumveeGun";
        float qp[3], qr[3][3], sc[3] = { 1, 1, 1 }; memcpy(qp, q->pos, 12); memcpy(qr, q->R, 36);
        int pi = find_proto(W, nm); if (pi >= 0) kvv(W->proto[pi].line, "s", sc, 3);
        float dv[3] = { v[0] + frange(W, -5, 5), v[1] + frange(W, -5, 5), v[2] + frange(W, -5, 5) };
        int d = spawn_debris(W, nm, qp, qr, sc, dv, frange(W, 3, 4));
        if (d >= 0) memcpy(W->o[d].angvel, w, 12);
        o = &W->o[i];
    }
    for (int t = 0; t < W->hvp.nwheels && t < 4; t++) {     /* the tyres keep rolling */
        float wp[3], wr[3][3], sc[3] = { 0.5f, 0.5f, 0.5f }, ax[3];
        world_of(R, P, W->hvp.wheel_pos[t], wp);
        float c0[3], c1[3], c2[3]; colv(R, 0, c0); colv(R, 1, c1); colv(R, 2, c2);
        set_cols(wr, c1, c2, c0);                            /* columns permuted as the PC does */
        int pi = find_proto(W, "DeadGunHumveeTireL"); if (pi >= 0) kvv(W->proto[pi].line, "s", sc, 3);
        int d = spawn_debris(W, "DeadGunHumveeTireL", wp, wr, sc, v, frange(W, 3, 4));
        colv(R, 0, ax);
        if (d >= 0) for (int j = 0; j < 3; j++) W->o[d].angvel[j] = w[j] + ax[j] * W->hv[W->o[i].s[7]].car.wheel_spin[t];
    }
    play(W, "Play_Rand_Humvee_Explosion", P);
    play(W, "Play_Rand_Humvee_Death_Voice", P);
    humvee_free(W, i);
}
/* HumveeLogic::update (0x6c3100) */
static void humvee_update(nr_objs *W, int i, nr_objs_frame *f, float dt)
{
    nr_obj *o = &W->o[i];
    struct nr_humvee *h = &W->hv[o->s[7]];
    nr_car *c = &h->car;
    float up_z = c->rot[2][2], F[3], U[3], X[3];
    colv(c->rot, 1, F); colv(c->rot, 2, U); colv(c->rot, 0, X);
    float d[3] = { f->car_pos[0] - c->pos[0], f->car_pos[1] - c->pos[1], f->car_pos[2] - c->pos[2] }, d2 = dot3(d, d);
    float *steerCmd = &o->f[1], *thr = &o->f[2], *brakeT = &o->f[3], *yaw = &o->f[4], *pitch = &o->f[5], *shot = &o->f[6];
    int aim = 0, shoot = 0;
    if (!o->s[3]) { o->s[3] = 1; play(W, "Play_HumveeSiren", o->pos); }
    o->t += dt;
    switch (o->s[0]) {
    case 0:
        if (o->t > 60) { humvee_destroy(W, i, f); return; }
        if (up_z < 0.5f) { o->s[0] = 2; o->t = 0; }
        else if (o->t > 3 && d2 < 625) { o->s[0] = 1; *shot = 0; o->t = 0; }
        aim = 1; break;
    case 1:
        if (o->t > 3) { o->s[0] = 0; o->t = 0; }
        aim = 1; shoot = 1; break;
    case 2:
        if (o->t > 3) { humvee_destroy(W, i, f); return; }
        *yaw += 5 * dt;                                      /* the turret spins */
        if (up_z > 0.7f) { o->s[0] = 0; o->t = 0; }
        break;
    }
    if (aim) {
        /* SteerToCar (0x6c1b10): straight at the car; reverse with opposite lock when it is behind */
        float n[3] = { d[0], d[1], d[2] }; norm3(n);
        float cr[3]; cross3(F, n, cr); float sn = dot3(cr, U);
        if (dot3(d, F) >= 0) { *steerCmd = 1.3f * sn; *thr = 1; } else { *steerCmd = -5.0f * sn; *thr = -1; }
        if (o->s[0] == 1) *thr = 0;                          /* coasts while shooting */
        /* AimTurret (0x6c1700): yaw unlimited, pitch -0.4..0.3, 1 rad/s, proportional when nearly on target */
        float L[3] = { dot3(X, d), dot3(F, d), dot3(U, d) }, fy[3] = { -sinf(*yaw), cosf(*yaw), 0 };
        float cz = fy[0] * L[1] - fy[1] * L[0];
        float step = (fabsf(cz) < 0.2f && dot3(L, fy) > 0) ? cz * dt * 5 : (cz > 0 ? dt : -dt);
        *yaw += step;
        float cy = cosf(*yaw), sy = sinf(*yaw), Lb[3] = { L[0], L[1] + 0.445f, L[2] - 0.674f };
        float B[3] = { cy * Lb[0] + sy * Lb[1], -sy * Lb[0] + cy * Lb[1], Lb[2] };
        float gy = cosf(*pitch), gz = sinf(*pitch), c2 = gy * B[2] - gz * B[1];
        if (fabsf(c2) < 0.2f && gy * B[1] + gz * B[2] > 0) *pitch = clampf(*pitch + c2 * dt * 5, -0.4f, 0.3f);
        else *pitch = clampf(*pitch + (c2 > 0 ? dt : -dt), -0.4f, 0.3f);
    }
    /* ApplySteer (0x6c1150) and ApplyThrottle (0x6c0f90) */
    float vf = dot3(c->vel, F), k = 1 / (1 + fmaxf((vf - 10) * 0.2f, 0)), target = *steerCmd * k, ms = k * dt * 5;
    float cur = c->steer;
    cur = target - cur > ms ? cur + ms : target - cur < -ms ? cur - ms : target;
    c->steer = clampf(cur, -1, 1);
    if (*thr > 0 ? o->s[1] : o->s[2]) {
        c->throttle = clampf(*thr, -1, 1); c->brake = 0;
        if (*thr > 0) o->s[2] = 0; else if (*thr < 0) o->s[1] = 0;
    } else { c->throttle = 0; c->brake = fminf(fabsf(*thr), 1); }
    if (dot3(c->vel, c->vel) < 36) { *brakeT -= dt; if (*brakeT <= 0) { *brakeT = 0; o->s[1] = o->s[2] = 1; } }
    else { *brakeT = 0.2f; if (vf > 0.1f) { o->s[1] = 1; o->s[2] = 0; } else if (vf < -0.1f) { o->s[2] = 1; o->s[1] = 0; } }
    /* the car's physics (its own wheels and body against the level) */
    c->ncontact = 0; c->body_touch = 0;
    h->acc += dt;
    if (W->coll) {
        uint32_t saved = W->coll->ndyn; W->coll->ndyn = 0;
        int steps = 0;
        while (h->acc >= NR_CAR_DT && steps < 12) { nr_car_step(c, &W->hvp, W->coll, NR_CAR_DT); h->acc -= NR_CAR_DT; steps++; }
        if (steps == 12) h->acc = 0;
        W->coll->ndyn = saved;
    }
    memcpy(o->pos, c->pos, 12); memcpy(o->R, c->rot, 36); memcpy(o->vel, c->vel, 12); memcpy(o->angvel, c->angvel, 12);
    /* the contact pass (0x6c2d70): lava, or > 30 into the level, destroys it */
    for (int q = 0; q < c->ncontact; q++)
        if (NR_TRI_MATID(c->contact[q].mat) == 1 || c->contact[q].d > 30) { humvee_destroy(W, i, f); return; }
    if (o->pos[2] <= -70) { humvee_free(W, i); return; }    /* fell off: removed quietly */
    /* the player's car against it: both dynamic (inverse mass 1), so the car is never killed;
     * rammed at more than 68 it explodes (I Know Car Fu) */
    if (f->car_active) {
        float best = 0, bn[3] = { 0, 0, 0 }; int hit = 0;
        for (int a = -1; a <= 1; a++) for (int b = -1; b <= 1; b++) {
            float pa[3], pb[3], la[3] = { 0, a * 1.45f, 0 }, lb[3] = { 0, b * fmaxf(f->car_half[1] - f->car_half[0], 0), 0 };
            world_of(c->rot, c->pos, la, pa); world_of(f->car_rot, f->car_pos, lb, pb);
            float e[3] = { pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2] }, dl = len3(e), rr = 1.1f + fmaxf(f->car_half[0], f->car_half[2]);
            if (dl < rr && rr - dl > best) { best = rr - dl; if (dl > 1e-4f) { bn[0] = e[0] / dl; bn[1] = e[1] / dl; bn[2] = e[2] / dl; } else set3(bn, 0, 0, 1); hit = 1; }
        }
        if (hit) {
            float rv[3] = { f->car_vel[0] - c->vel[0], f->car_vel[1] - c->vel[1], f->car_vel[2] - c->vel[2] }, vn = dot3(rv, bn);
            if (vn < 0) {
                if (-vn > 68) {
                    if (W->host.event) W->host.event(W->host.user, "achievement", 24);
                    humvee_destroy(W, i, f); return;
                }
                float j = -1.3f * vn * 0.5f;                     /* restitution 0.3, equal masses */
                madd3(f->car_dv, bn, j); madd3(c->vel, bn, -j);
            }
            madd3(f->car_push, bn, best * 0.5f); madd3(c->pos, bn, -best * 0.5f);
        }
    }
    /* the turret, the shots (0x6c2920: 3 a second, a 200 u/s bolt 1.5 ahead of the gun, 1 u/s recoil) */
    humvee_pose(W, i);
    o = &W->o[i];
    if (shoot) {
        *shot += 3 * dt;
        if (*shot > 1 && o->nkid > 1) {
            *shot = 0;
            nr_obj *g = &W->o[o->kid[1]];
            float G0[3], G1[3], G2[3], M[3]; colv(g->R, 0, G0); colv(g->R, 1, G1); colv(g->R, 2, G2);
            for (int q = 0; q < 3; q++) M[q] = g->pos[q] + 1.5f * G1[q];
            int p_ = spawn(W, "Phaser", OK_PHASER); o = &W->o[i];
            if (p_ >= 0) {
                nr_obj *ph = &W->o[p_];
                memcpy(ph->pos, M, 12); set_cols(ph->R, G2, G0, G1);
                ph->f[0] = kvf(ph->line, "PhaserLogic.Speed", 200); ph->f[1] = kvf(ph->line, "EnumLogic.Time", 3);
                obj_bounds(ph);
            }
            madd3(c->vel, G1, -1.0f);
        }
    }
    /* the engine's pitch, the phaser loop near the car (0x6c1450, 0x6c12a0) */
    if (W->host.rtpc) W->host.rtpc(W->host.user, i, "CarEngineRPM", clampf(2 * fabsf(vf), 0.125f, 50) * 0.02f + 0.1f);
    if (o->nkid > 1) W->o[o->kid[1]].sndloop = d2 < 2500;
}
/* PhaserLogic (0x6a6e00): straight on at its speed until it hits something */
static void phaser_update(nr_objs *W, int i, nr_objs_frame *f, float dt)
{
    nr_obj *o = &W->o[i];
    o->f[1] -= dt;
    if (o->f[1] < 0) { kill_obj(W, i); return; }
    float dir[3]; colv(o->R, 2, dir);
    float L = o->f[0] * dt, best = L, hit = 0, dist, n[3];
    if (W->coll) {
        uint32_t saved = W->coll->ndyn; W->coll->ndyn = 0;
        if (nr_coll_ray(W->coll, o->pos, dir, L, &dist, n, NULL) && dist < best) { best = dist; hit = 1; }
        W->coll->ndyn = saved;
    }
    float seg[3] = { dir[0] * L, dir[1] * L, dir[2] * L }, tc;
    if (seg_hits_car(f, o->pos, seg, &tc) && tc * L < best) { best = tc * L; hit = 1; }
    madd3(o->pos, dir, best);
    if (hit) {
        float p[3]; memcpy(p, o->pos, 12);
        spawn_fx(W, "PhaserExplosion", p, 1);
        float k[3] = { 2, 20, 0 };
        if (f->car_active) radial_push(p, 5, k, 15, f->car_pos, f->car_dv);
        kill_obj(W, i);
    }
}

/* ------------------------------------------------------------------ the story's end: CoreExploder (re_E 1) */
static int find_named(nr_objs *W, const char *name)
{
    char b[48];
    for (int i = 0; i < W->n; i++) if (W->o[i].alive && kvs(W->o[i].line, "name", b, sizeof b) && !strcmp(b, name)) return i;
    return -1;
}
int nr_objs_find(nr_objs *W, const char *name) { return find_named(W, name); }
/* SpawnAt (0x6ce780): the blueprint's own transform inside the building's, as a kind */
static int core_spawn(nr_objs *W, int bi, const char *name, int kind, const float off[3])
{
    int c = spawn(W, name, kind);
    if (c < 0) return -1;
    nr_obj *b = &W->o[bi], *q = &W->o[c];
    float l[3] = { (q->pos[0] + off[0]) * b->scale[0], (q->pos[1] + off[1]) * b->scale[1], (q->pos[2] + off[2]) * b->scale[2] }, R[3][3];
    world_of(b->R, b->pos, l, q->pos);
    mat_mul(b->R, q->R, R); memcpy(q->R, R, 36);
    for (int k = 0; k < 3; k++) q->scale[k] *= b->scale[k];
    if (kind == OK_FX) fx_init(q);
    obj_bounds(q);
    return c;
}
static void core_shake(nr_objs_frame *f, float a) { if (a > f->shake) f->shake = a; }
/* Debris (0x6ce370): a claw off an arm, flung outwards */
static void core_claw(nr_objs *W, int pi_, const char *name, int mirror)
{
    int c = spawn(W, name, OK_DEBRIS);
    if (c < 0) return;
    nr_obj *p = &W->o[pi_], *o = &W->o[c];
    float lp[3]; memcpy(lp, o->pos, 12);
    float R[3][3]; memcpy(R, o->R, 36);
    if (mirror) { float Rz[3][3], t[3][3]; lp[0] = -lp[0]; lp[1] = -lp[1]; rot_axis(Rz, 2, PI_F); mat_mul(Rz, R, t); memcpy(R, t, 36); }
    float sl[3] = { lp[0] * p->scale[0], lp[1] * p->scale[1], lp[2] * p->scale[2] }, rel[3];
    mat_vec(p->R, sl, rel);
    for (int k = 0; k < 3; k++) { o->pos[k] = p->pos[k] + rel[k]; o->scale[k] *= p->scale[k]; }
    mat_mul(p->R, R, o->R);
    set3(o->vel, -0.5f * rel[0], -0.5f * rel[1], -0.5f * rel[2]);
    set3(o->angvel, 0.05f * rel[1], -0.05f * rel[0], 0);
    o->f[0] = 60; o->dynamic = 1; o->cmodel = -1;
    obj_bounds(o);
}
void nr_objs_outro_start(nr_objs *W)
{
    for (int i = 0; i < W->n; i++) {
        nr_obj *o = &W->o[i];
        if (!o->alive || o->kind != OK_COREEXPL || o->s[0]) continue;
        o->s[0] = 1; o->t = 0;
        float z[3] = { 0, 0, 0 };
        o->s[1] = core_spawn(W, i, "CoreDeathSparker", OK_EMITTER, z);
    }
}
static void core_move_sparker(nr_objs *W, nr_obj *o)
{
    if (o->s[1] < 0 || !W->o[o->s[1]].alive) return;
    float teye = kvf(o->line, "CoreExploder.TimeEye", 4);
    float z = (o->t / teye) * 83.128f + 5.976f, y;
    if (z < 72.318f) y = -11.136f;
    else if (z > 82.641f) y = -6.741f;
    else { float k = (z - 72.318f) * 0.09687109f; y = k * -6.741f - (1 - k) * 11.136f; }
    float l[3] = { 0, y * o->scale[1], z * o->scale[2] };
    world_of(o->R, o->pos, l, W->o[o->s[1]].pos);
}
static void core_update(nr_objs *W, int i, nr_objs_frame *f, float dt)
{
    nr_obj *o = &W->o[i];
    if (!W->outro || !o->s[0]) return;
    o->t += dt;
    const char *L = o->line;
    float tr0 = kvf(L, "CoreExploder.TimeRing0", 1.6f), tr1 = kvf(L, "CoreExploder.TimeRing1", 2.0f), ts0 = kvf(L, "CoreExploder.TimeSmallRing0", 2.5f);
    float ts1 = kvf(L, "CoreExploder.TimeSmallRing1", 2.65f), ts2 = kvf(L, "CoreExploder.TimeSmallRing2", 2.8f), te = kvf(L, "CoreExploder.TimeEye", 4);
    float tb = kvf(L, "CoreExploder.TimeBuilding", 6);
    static const float RING_Z[6] = { 45.946f, 52.007f, 60.112f, 62.409f, 65.144f, 86.23f };
    static const float SHAKE[5] = { 0.02f, 0.02f, 0.01f, 0.01f, 0.01f };
    float times[5] = { tr0, tr1, ts0, ts1, ts2 };
    int st = o->s[0];
    if (st >= 1 && st <= 5 && o->t > times[st - 1]) {
        core_shake(f, SHAKE[st - 1]);
        float off[3] = { 0, 1.104f, RING_Z[st - 1] };
        core_spawn(W, i, "FireRing", OK_FX, off); o = &W->o[i];
        if (st == 4) {                                          /* DropClaws: the arms come apart */
            int up = find_named(W, "CoreArmsUpper"), lo = find_named(W, "CoreArmsLower");
            if (up >= 0) { for (int c = 0; c < 2; c++) for (int m = 0; m < 2; m++) { char n[24]; snprintf(n, sizeof n, "DeadCore_Claw%d", c); core_claw(W, up, n, m); } kill_obj(W, up); }
            if (lo >= 0) { for (int c = 2; c < 4; c++) for (int m = 0; m < 2; m++) { char n[24]; snprintf(n, sizeof n, "DeadCore_Claw%d", c); core_claw(W, lo, n, m); } kill_obj(W, lo); }
            o = &W->o[i];
        }
        o->s[0]++;
    } else if (st == 6 && o->t > te) {                           /* the eye is blown off */
        float off[3] = { 0, 1.104f, RING_Z[5] }, z0[3] = { 0, 0, 0 };
        core_spawn(W, i, "FireRing", OK_FX, off);
        int e = core_spawn(W, i, "DeadCoreEye", OK_DEBRIS, z0); o = &W->o[i];
        if (e >= 0) {
            nr_obj *q = &W->o[e]; float y[3], x[3]; colv(q->R, 1, y); colv(q->R, 0, x);
            set3(q->vel, -20 * y[0], -20 * y[1], -20 * y[2]); set3(q->angvel, 20 * x[0], 20 * x[1], 20 * x[2]);
            q->f[0] = 60; q->dynamic = 1; q->cmodel = -1;
        }
        if (o->s[1] >= 0) kill_obj(W, o->s[1]);
        float so[3] = { 0, -6.741f, 89.104f };
        int sp = core_spawn(W, i, "CoreEyeSparker", OK_EMITTER, so); o = &W->o[i];
        if (sp >= 0) set3(W->o[sp].scale, 1, 1, 1);
        o->s[1] = sp;
        int eye = find_named(W, "coreeye"); if (eye >= 0) kill_obj(W, eye);
        o->s[0] = 7;
    } else if (st == 7) {
        core_shake(f, 0.003f);
        if (o->t > tb) {                                        /* BreakBuilding: 61 pieces and the flash */
            core_shake(f, 0.15f);
            if (o->s[1] >= 0) kill_obj(W, o->s[1]);
            static const struct { const char *fmt; int n; } PIECES[] = { { "DeadCore_A_%d", 9 }, { "DeadCore_B_%d", 7 }, { "DeadCore_C_%02d", 15 },
                                                                         { "DeadCore_D_%d", 8 }, { "DeadCore_E_%02d", 19 }, { "DeadCore_F_%d", 5 } };
            float z0[3] = { 0, 0, 0 }, P[3]; memcpy(P, o->pos, 12);
            int b0 = core_spawn(W, i, "DeadCoreBase", OK_DEBRIS, z0); o = &W->o[i];
            if (b0 >= 0) { W->o[b0].f[0] = 600; W->o[b0].dynamic = 1; W->o[b0].cmodel = -1; }
            for (unsigned g = 0; g < sizeof PIECES / sizeof PIECES[0]; g++) for (int k = 0; k < PIECES[g].n; k++) {
                char nm[24]; snprintf(nm, sizeof nm, PIECES[g].fmt, k);
                int c = core_spawn(W, i, nm, OK_DEBRIS, z0); o = &W->o[i];
                if (c < 0) continue;
                nr_obj *q = &W->o[c];
                q->f[0] = 600; q->dynamic = 1; q->cmodel = -1; set3(q->vel, 0, 0, 0); set3(q->angvel, 0, 0, 0);
                float kk[3] = { 0.1f, 1, 1 }; radial_push(P, 80, kk, 1.5f, q->pos, q->vel);
            }
            core_spawn(W, i, "CoreExplosion", OK_FX, z0); o = &W->o[i];
            if (o->id > 0 && (uint32_t)o->id < W->noff) W->off[o->id] = 1;   /* its collision goes */
            kill_obj(W, i);
            return;
        }
    }
    o = &W->o[i];
    if (o->s[0] <= 6) core_move_sparker(W, o);
}
/* SpinFocusCamera's outro path (0x6d3550 / 0x6d14e0) */
void nr_objs_outro_camera(nr_objs *W, float time, float dt, float eye[3], float focus[3])
{
    static bez_t path; static int built;
    if (!built) {
        static const float P[6][3] = { { 15.158f, 565.451f, -77.154f }, { 278.586f, 430.220f, -14.752f }, { 216.916f, 348.226f, -0.945f },
                                       { 88.184f, 333.840f, 114.810f }, { -4.0f, 371.836f, 300.0f }, { -421.500f, -72.664f, 521.045f } };
        bez_build(&path, P, 6); built = 1;
    }
    float t = time / 25.0f;
    bez_at(&path, t * path.L, eye);
    float target[3] = { -0.133f, 650.921f, 337.262f };
    int sp = -1;
    for (int i = 0; i < W->n; i++) if (W->o[i].alive && W->o[i].kind == OK_EMITTER && strstr(W->o[i].line, "bp=CoreDeathSparker ")) sp = i;
    if (t > 0.3f && sp >= 0 && W->o[sp].pos[2] > -20.262f) memcpy(target, W->o[sp].pos, 12);
    float k = t < 0.315f ? 0.0001f : 0.1f, ke = 1 - powf(1 - k, dt * 60);   /* per frame on the PC (60 Hz) */
    for (int j = 0; j < 3; j++) focus[j] = focus[j] * (1 - ke) + target[j] * ke;
}

/* ------------------------------------------------------------------ per-frame logic */
static void laser_visual(nr_obj *L, int state, float t, float dur, int in_beam)
{
    float f = sinf(t * 50) + 1;
    (void)in_beam;
    switch (state) {
    case 0: L->visible = 0; break;
    case 1: { float r = dur > 0 ? t / dur : 1; L->visible = 1; set3(L->col, 0.5f * r * f, 0.5f * r * f, 0.05f * r * f); L->col[3] = 0.25f * r * f; break; }
    case 2: L->visible = 1; set3(L->col, 0.5f * f, 0.5f * f, 0.05f * f); L->col[3] = 0.25f * f; break;
    case 3: L->visible = 1; set3(L->col, 1, 0.1f, 0.1f); L->col[3] = 3; break;
    case 4: { float a = fmaxf(0, 1 - (dur > 0 ? t / dur : 1)); L->visible = a > 0; set3(L->col, a, 0.1f * a, 0.1f * a); L->col[3] = 3 * a; break; }
    }
}
/* LaserLogic's state machine (0x692f20); d2 = squared distance to the car (arches) or camera (sweep) */
static void laser_states(nr_obj *o, float d2)
{
    int *st = &o->s[0]; float *t = &o->t, *dur = &o->f[1];
    if (*st != 0 && *st != 3 && *t > *dur) {
        *t = 0;
        if (*st == 1) { *st = 2; *dur = 0.5f; } else if (*st == 2) { *st = 3; *dur = 3.0f; } else if (*st == 4) { *st = 0; *dur = 1.0f; }
        return;
    }
    if (*st == 0) { if (d2 < 40000) { *st = 1; *dur = 0.1f; } }
    else if (d2 > 40000) { *st = 4; *dur = 0.1f; }
}
static float laser_volume(int st, float t, float dur)
{
    float f = sinf(t * 50) + 1;
    switch (st) { case 1: return 0.35f * (dur > 0 ? t / dur : 1); case 2: return 0.35f * f; case 3: return 1; case 4: return fmaxf(0, 1 - t / dur); }
    return 0;
}

static void update_obj(nr_objs *W, int i, nr_objs_frame *f, float dt)
{
    nr_obj *o = &W->o[i];
    const float *cp = f->car_pos;
    float camback[3]; colv(f->cam_rot, 2, camback);
    switch (o->kind) {
    case OK_SAW:
        rotate_by(o->R, o->angvel, dt);
        if (o->s[4]) for (int k = 0; k < 3; k++) if (((o->s[4] - 1) >> k) & 1) o->pos[k] = f->cam_pos[k];   /* teleported: no velocity */
        if (o->s[1]) {           /* CoreLightningLogic: bolts between the claw tips of the two arms */
            if (o->s[2] < 0) for (int k = 0; k < W->n; k++) if (W->o[k].kind == OK_SAW && strstr(W->o[k].line, "bp=core_arms_lower ")) { o->s[2] = k; break; }
            if (o->s[2] < 0) break;
            nr_obj *lo = &W->o[o->s[2]];
            float U[4][3], D[4][3];
            for (int k = 0; k < 4; k++) {
                float sx = (k & 2) ? -1.0f : 1.0f, sy = (k & 1) ? -1.0f : 1.0f;
                float a[3] = { 19.869f * sx, 17.238f * sy, -6.229f }, b[3] = { 19.874f * sx, 18.409f * sy, 6.746f };
                to_world(o, a, U[k]); to_world(lo, b, D[k]);
            }
            o->f[7] += o->f[2] * dt;
            while (o->f[7] > 1) {
                o->f[7] -= 1;
                int ii = rnd(W) % 4, jj = rnd(W) % 4;
                float d[3] = { U[jj][0] - D[ii][0], U[jj][1] - D[ii][1], U[jj][2] - D[ii][2] };
                if (dot3(d, d) > o->f[1] * o->f[1]) continue;
                float lo_ = fminf(o->f[3], o->f[4]), hi = fmaxf(o->f[3], o->f[4]);
                int c = spawn_spark(W, lo_ + frand(W) * (hi - lo_), o->f[5], o->f[6]); o = &W->o[i];
                if (c < 0) continue;
                W->o[c].s[0] = 1; W->o[c].s[1] = jj; W->o[c].s[2] = ii; W->o[c].parent = (int16_t)i;
            }
            for (int k = 0; k < W->n; k++) {
                nr_obj *q = &W->o[k];
                if (!q->alive || q->kind != OK_SPARK || !q->s[0] || q->parent != i) continue;
                memcpy(&q->f[3], U[q->s[1]], 12); memcpy(&q->f[6], D[q->s[2]], 12);
            }
        }
        break;
    case OK_BAFSAW: {
        float s = sinf(o->f[2] * o->t), a[3]; colv(o->R, o->s[0], a);
        set3(o->angvel, a[0] * o->f[0] * s, a[1] * o->f[0] * s, a[2] * o->f[0] * s);
        for (int k = 0; k < 3; k++) {
            o->pos[k] = o->f[3 + k] + o->f[6 + k] * s * o->f[1];
            o->vel[k] = o->f[6 + k] * o->f[1] * o->f[2] * cosf(o->f[2] * o->t);
        }
        rotate_by(o->R, o->angvel, dt);
        o->t += dt;
        break;
    }
    case OK_DRONE: case OK_BLIMP: {
        float g[3];
        if (o->kind == OK_DRONE) for (int k = 0; k < 2; k++) g[k] = o->f[2 + k] - camback[k] * o->f[1];
        else for (int k = 0; k < 2; k++) g[k] = f->cam_pos[k] - o->f[2] * camback[k];
        float d[3] = { g[0] - o->pos[0], g[1] - o->pos[1], 0 }; norm3(d);
        float c0[3], c1[3]; colv(o->R, 0, c0); colv(o->R, 1, c1);
        float turn = dot3(c0, d) > 0 ? -1.0f : 1.0f, dy = dot3(c1, d);
        if (dy > 0.8f) turn = (1 - dy) * turn * 5;
        float sp = o->f[0], rs = o->kind == OK_BLIMP ? o->f[1] : 1.0f;
        set3(o->vel, c1[0] * sp, c1[1] * sp, c1[2] * sp);
        set3(o->angvel, 0, 0, turn * rs);
        madd3(o->pos, o->vel, dt);
        rotate_by(o->R, o->angvel, dt);
        for (int k = 0; k < o->nkid; k++) place_child(W, o->kid[k]);
        break;
    }
    case OK_SEARCHLIGHT: {       /* SearchlightLogic: wander inside a cone, rest, move again */
        if (o->parent < 0) break;
        o->t += dt;
        if (o->s[0] == 0 && o->t > o->f[2]) {
            float a = frand(W) * o->f[0], b = frand(W) * 2 * PI_F, l[3] = { sinf(a) * sinf(b), -sinf(a) * cosf(b), -cosf(a) }, w[3];
            float base[3][3]; memcpy(base, W->o[o->parent].lR, 36);   /* not used: cone around the rest direction */
            (void)base;
            memcpy(&o->f[3], &o->f[6], 12);
            mat_vec(o->lR, l, w); memcpy(&o->f[6], w, 12);
            o->s[0] = 1; o->t = -frand(W) * 0.2f * o->f[1];
        } else if (o->s[0] == 1 && o->t > o->f[1]) { o->s[0] = 0; o->t = -frand(W) * 0.2f * o->f[2]; }
        float k = o->s[0] == 1 ? clampf(o->t / o->f[1], 0, 1) : 1, d[3];
        for (int j = 0; j < 3; j++) d[j] = o->f[3 + j] + (o->f[6 + j] - o->f[3 + j]) * k;
        norm3(d);
        /* turn the light so its -Z (the beam) points along d, in the blimp's space */
        float z0[3] = { -o->lR[0][2], -o->lR[1][2], -o->lR[2][2] }, ax[3]; cross3(z0, d, ax);
        float s = len3(ax), c = dot3(z0, d), Rr[3][3];
        identity(Rr);
        if (s > 1e-5f) { float ang = atan2f(s, c); ax[0] /= s; ax[1] /= s; ax[2] /= s; float w[3] = { ax[0] * ang, ax[1] * ang, ax[2] * ang }; rotate_by(Rr, w, 1); }
        float saved[3][3]; memcpy(saved, o->lR, 36);
        mat_mul(Rr, saved, o->lR);
        place_child(W, i);
        memcpy(o->lR, saved, 36);
        break;
    }
    case OK_TRAIN: {
        bez_t *b = &TRAINP[o->s[0]];
        if (b->L <= 0) break;
        float s = o->scale[0], sp = o->f[0];
        o->f[14] = wrapf(o->f[14] + sp * s * dt, b->L);
        float d = o->f[14], A[3], B[3];
        bez_at(b, d, A);
        for (int k = 0; k < o->s[3]; k++) {
            float len = (k == 0 ? o->f[2] : k == o->s[3] - 1 ? o->f[4] : o->f[3]) * s;
            d = wrapf(d - len, b->L); bez_at(b, d, B);
            nr_obj *c = &W->o[o->s[2] + k];
            place_between(c, A, B);
            float c1[3]; colv(c->R, 1, c1);
            set3(c->vel, -c1[0] * s * sp, -c1[1] * s * sp, -c1[2] * s * sp);
            memcpy(A, B, 12);
        }
        /* track: recycle the tail piece to the front, one per frame; new pieces pop in large and faint */
        int N = o->s[5];
        if (N > 0) {
            float gap = o->f[14] + o->f[5] * s - o->f[15];
            if (gap > b->L / 2) gap -= b->L;
            if (gap < -b->L / 2) gap += b->L;
            if (gap > 2 * s) {
                int idx = (o->s[6] - 1 + N) % N;
                float P1[3], P2[3]; bez_at(b, wrapf(o->f[15] + 2 * s, b->L), P1); bez_at(b, o->f[15], P2);
                place_between(&W->o[o->s[4] + idx], P1, P2);
                o->f[15] = wrapf(o->f[15] + 2 * s, b->L); o->s[6] = idx; gap -= 2 * s;
            }
            float fr = clampf(gap / (2 * s), 0, 1), kc = o->f[13];
            for (int k = 0; k < N; k++) { nr_obj *p = &W->o[o->s[4] + k]; p->col[3] = kc; set3(p->scale, s, s, s); }
            for (int k = 0; k < 5 && k < N; k++) {
                nr_obj *p = &W->o[o->s[4] + (o->s[6] + k) % N];
                p->col[3] = (k + fr) * 0.2f * kc;
                float sc = (2 - (k + fr) * 0.2f) * s; set3(p->scale, sc, sc, sc);
                nr_obj *q = &W->o[o->s[4] + (o->s[6] + N - 1 - k) % N];
                q->col[3] = (k + fr) * 0.2f * kc;
            }
        }
        break;
    }
    case OK_SPOT: {
        float c1[3], c2[3]; colv(o->R, 1, c1); colv(o->R, 2, c2);
        float w = o->s[0] ? o->f[0] : -o->f[0];
        float ang = (fabsf(c2[2] - o->f[1]) + 0.1f) * w * dt, Ry[3][3];
        identity(Ry); Ry[0][0] = cosf(ang); Ry[0][2] = sinf(ang); Ry[2][0] = -sinf(ang); Ry[2][2] = cosf(ang);
        mat_mul(o->R, Ry, o->R);
        if (c2[2] < o->f[1]) o->s[0] = (c1[0] * c2[1] - c1[1] * c2[0]) >= 0;
        break;
    }
    case OK_LASERARCH: {
        float X[3], Y[3], Z[3]; colv(o->R, 0, X); colv(o->R, 1, Y); colv(o->R, 2, Z);
        float *S = o->scale;
        o->t += dt;
        float d[3] = { cp[0] - o->pos[0], cp[1] - o->pos[1], cp[2] - o->pos[2] }, d2 = f->car_active ? dot3(d, d) : 1e12f;
        /* CarInBeams: the car near the arch's plane, then the five vertical lines through its box */
        int mask = 0;
        if (f->car_active) {
            float r = len3(f->car_half), lx = dot3(d, X), ly = dot3(d, Y), lz = dot3(d, Z);
            if (ly * ly < r * r && fabsf(lx) < 0.8f * S[0] + r && fabsf(lz) < 0.8f * S[2] + r)
                for (int k = -2; k <= 2; k++) {
                    float p0[3], dir[3] = { Z[0] * 2 * S[2], Z[1] * 2 * S[2], Z[2] * 2 * S[2] };
                    for (int j = 0; j < 3; j++) p0[j] = o->pos[j] + Z[j] * (0.6f * S[2] - S[2]) + X[j] * (0.3f * S[0] * k);
                    if (seg_hits_car(f, p0, dir, NULL)) mask |= 1 << (k + 2);
                }
        }
        float mZ[3] = { -Z[0], -Z[1], -Z[2] };
        for (int k = 0; k < o->nkid; k++) {
            nr_obj *L = &W->o[o->kid[k]];
            set_cols(L->R, X, mZ, Y);
            set3(L->scale, S[0] * 1.74f, S[1] * 1.74f, S[2] * 1.74f);
            for (int j = 0; j < 3; j++) L->pos[j] = o->pos[j] + Z[j] * (S[2] * 0.87f) + X[j] * (S[0] * 0.3f * (k - 2));
            laser_visual(L, o->s[0], o->t, o->f[1], (mask >> k) & 1);
            obj_bounds(L);
            if (o->s[0] == 3 && ((mask >> k) & 1)) f->heat += 3000 * dt;
        }
        laser_states(o, d2);
        if (o->s[0] == 3 || o->s[0] == 0) o->t = 0;
        if (W->host.rtpc) W->host.rtpc(W->host.user, i, "Laser_Volume", laser_volume(o->s[0], o->t, o->f[1]));
        break;
    }
    case OK_SWEEP: {
        int N = o->nkid; if (!N) break;
        float Y0[3]; colv(o->R, 1, Y0);
        set3(o->angvel, Y0[0] * o->f[4], Y0[1] * o->f[4], Y0[2] * o->f[4]);
        rotate_by(o->R, o->angvel, dt);
        float X[3], Y[3], Z[3]; colv(o->R, 0, X); colv(o->R, 1, Y); colv(o->R, 2, Z);
        float sx = o->scale[0], sz = o->scale[2], top[3], A[3], Pp[3], Pm[3];
        for (int j = 0; j < 3; j++) {
            top[j] = o->pos[j] + 0.87f * sz * Z[j]; A[j] = top[j] - 0.6f * sx * X[j];
            Pp[j] = top[j] + X[j] * (1.8f * sx / N); Pm[j] = top[j] - X[j] * (1.8f * sx / N);
        }
        o->t += dt;
        float mZ[3] = { -Z[0], -Z[1], -Z[2] };
        for (int k = 0; k < N; k++) {
            nr_obj *L = &W->o[o->kid[k]];
            laser_visual(L, o->s[0], o->t, o->f[1], 0);
            set_cols(L->R, X, mZ, Y);
            set3(L->scale, o->scale[0] * 1.74f, o->scale[1] * 1.74f, o->scale[2] * 1.74f);
            for (int j = 0; j < 3; j++) L->pos[j] = A[j] + o->f[2] * X[j] + k * 0.1f * sx * X[j];
            obj_bounds(L);
            if (o->s[0] == 3) {
                float dir[3] = { mZ[0] * L->scale[1], mZ[1] * L->scale[1], mZ[2] * L->scale[1] };
                if (seg_hits_car(f, L->pos, dir, NULL)) f->heat += 3000 * dt;
            }
        }
        float dc[3] = { o->pos[0] - f->cam_pos[0], o->pos[1] - f->cam_pos[1], o->pos[2] - f->cam_pos[2] };
        laser_states(o, dot3(dc, dc));
        if (o->s[0] == 3 || o->s[0] == 0) o->t = 0;
        nr_obj *L0 = &W->o[o->kid[0]], *L1 = &W->o[o->kid[N - 1]];
        if (o->s[1] == 1) {
            o->f[2] += o->f[3] * dt;
            float e[3] = { L0->pos[0] - Pp[0], L0->pos[1] - Pp[1], L0->pos[2] - Pp[2] };
            if (dot3(e, e) < L0->scale[1]) o->s[1] = 0;
        } else {
            o->f[2] -= o->f[3] * dt;
            float e[3] = { L1->pos[0] - Pm[0], L1->pos[1] - Pm[1], L1->pos[2] - Pm[2] };
            if (dot3(e, e) < L1->scale[1]) o->s[1] = 1;
        }
        if (W->host.rtpc) W->host.rtpc(W->host.user, i, "Laser_Volume", laser_volume(o->s[0], o->t, o->f[1]));
        break;
    }
    case OK_SPINLASER:
        rotate_by(o->R, o->angvel, dt);
        for (int k = 0; k < o->nkid; k++) {
            int ci = o->kid[k]; nr_obj *q = &W->o[ci];
            place_child(W, ci);
            q->scale[0] = q->lscale[0]; q->scale[2] = q->lscale[2];
            float dir[3]; colv(q->R, 1, dir);
            float maxd = q->f[0], t = maxd, n[3]; uint32_t m;
            if (W->coll && nr_coll_ray(W->coll, q->pos, dir, maxd, &t, n, &m)) {}
            float seg[3] = { dir[0] * t, dir[1] * t, dir[2] * t }, tc;
            if (seg_hits_car(f, q->pos, seg, &tc)) { t *= tc; f->heat += dt; }
            q->scale[1] = t;
            obj_bounds(q);
            if (q->s[0] >= 0) {
                nr_obj *dot = &W->o[q->s[0]];
                for (int j = 0; j < 3; j++) dot->pos[j] = q->pos[j] + dir[j] * t;
                identity(dot->R); obj_bounds(dot);
            }
        }
        break;
    case OK_BLOCKER: {
        float A[3]; colv(o->R, 2, A);
        float d[3] = { o->pos[0] - cp[0], o->pos[1] - cp[1], o->pos[2] - cp[2] }, d2 = f->car_active ? dot3(d, d) : 1e12f;
        float H = o->f[5], B = o->f[6];
        if (d2 < o->f[2] * o->f[2]) { if (o->f[0] < 1) { o->f[0] += dt; } o->visible = 1; }
        else if (o->f[0] > 0) { o->f[0] -= dt; o->visible = 1; }
        else o->visible = 0;
        set_glow(o, B * clampf(o->f[0], 0, 1));
        if (d2 < o->f[3] * o->f[3]) {
            o->s[2] = 0;
            if (!o->s[1]) { play(W, "Play_BlockerUp", o->pos); o->s[1] = 1; }
            if (o->s[0] < 0) {
                char nm[24]; const char *bp = strstr(o->line, "bp=blocker_holo");
                snprintf(nm, sizeof nm, "blocker%c", bp ? bp[15] : '2');
                int c = spawn(W, nm, OK_BLOCK); o = &W->o[i];
                if (c < 0) break;
                nr_obj *q = &W->o[c];
                memcpy(q->R, o->R, 36); set3(q->scale, o->scale[0], o->scale[1], H);
                for (int j = 0; j < 3; j++) q->pos[j] = o->pos[j] - A[j] * 2.5f * H;
                obj_bounds(q); o->s[0] = c; o->f[1] = 0;
                break;
            }
            o->f[1] = fminf(o->f[1] + o->f[4] * dt, 1);
            nr_obj *q = &W->o[o->s[0]];
            for (int j = 0; j < 3; j++) {
                q->pos[j] = o->pos[j] - A[j] * (2.5f - 5 * o->f[1]) * H;
                q->vel[j] = o->f[1] < 1 ? A[j] * H * o->f[4] * 5 : 0;
            }
            o->f[0] = 1 - o->f[1] * 0.9f; set_glow(o, B * o->f[0]);
            break;
        }
        if (o->s[0] < 0) break;
        o->s[1] = 0;
        if (o->f[1] > 0) {
            if (!o->s[2]) { play(W, "Play_BlockerDown", o->pos); o->s[2] = 1; }
            o->f[1] = fmaxf(0, o->f[1] - dt);
            nr_obj *q = &W->o[o->s[0]];
            for (int j = 0; j < 3; j++) { q->pos[j] = o->pos[j] - A[j] * (2.5f - 5 * o->f[1]) * H; q->vel[j] = -A[j] * H * 5; }
            o->f[0] = 1 - o->f[1] * 0.9f; set_glow(o, B * o->f[0]); o->visible = 1;
        } else { kill_obj(W, o->s[0]); o->s[0] = -1; }
        break;
    }
    case OK_MINE: {
        float up[3]; colv(o->R, 2, up);
        float s = o->scale[0];
        float d[3] = { cp[0] - o->pos[0], cp[1] - o->pos[1], cp[2] - o->pos[2] };
        if (f->car_active && dot3(d, d) < o->f[4] * o->f[4]) {
            float l = len3(d);
            if (l > 0) set3(o->vel, d[0] / l * o->f[5], d[1] / l * o->f[5], d[2] / l * o->f[5]);
            o->f[0] += dt * 7;
            set3(o->col, (sinf(3 * o->f[0]) + 1) * 0.5f, 25 / 255.0f, 12 / 255.0f); o->col[3] = 7;
        } else {
            o->f[0] += dt;
            float fs = o->f[3], fd = o->f[2], ra = o->f[1];
            float v = fs * fd * s * cosf(fs * o->f[0]), w = fs * ra * sinf(fs * o->f[0]);
            set3(o->vel, up[0] * v, up[1] * v, up[2] * v); set3(o->angvel, up[0] * w, up[1] * w, up[2] * w);
            set3(o->col, 1, (sinf(3 * o->f[0]) + 1) * 0.5f * 0.6f, 25 / 255.0f); o->col[3] = 3;
        }
        madd3(o->pos, o->vel, dt);
        rotate_by(o->R, o->angvel, dt);
        break;
    }
    case OK_SHARDBLD: {
        if (o->s[0] == 1) {
            float tp[3] = { o->pos[0] + o->f[12], o->pos[1] + o->f[13], o->pos[2] + o->f[14] };
            float d[3] = { cp[0] - tp[0], cp[1] - tp[1], cp[2] - tp[2] };
            if (f->car_active && dot3(d, d) < o->f[11] * o->f[11]) {
                o->s[0] = 2; o->t = 0;
                for (int k = 0; k < 6 && k < o->nkid; k++) {
                    nr_obj *q = &W->o[o->kid[k]];
                    q->s[0] = 1; q->f[9] = 0; colv(q->R, 1, &q->f[6]); q->f[10] = o->f[9]; q->f[11] = o->f[10];
                }
            }
            o->s[2] = 0;
        } else if (o->s[0] == 2) {
            o->t += dt;
            float t = o->t, k = (1.41421356f * t - t * t) / (1.41421356f - 1);
            float c[4]; for (int j = 0; j < 4; j++) c[j] = o->f[5 + j] * k + o->f[1 + j] * (1 - k);
            memcpy(o->col, c, 16);
            for (int j = 0; j < o->nkid; j++) memcpy(W->o[o->kid[j]].col, c, 16);
            if (!o->s[2]) { play(W, "Play_BuildingDestruction", o->pos); o->s[2] = 1; }
            if (o->t >= 1) { o->s[0] = 3; o->t = 0; }
        } else if (o->s[0] >= 3 && o->s[0] <= 8) {
            o->t += dt;
            if (o->t <= 0.15f) break;
            o->t = 0;
            float p[3], fwd[3];
            if (o->s[1]) {
                float R = o->f[20];
                set3(p, o->pos[0] + o->f[17] + frange(W, -R / 2, R / 2), o->pos[1] + o->f[18] + frange(W, -R / 2, R / 2), o->pos[2] + o->f[19]);
                set3(fwd, 0, 1, 0);
            } else {
                float d[3] = { cp[0] - o->pos[0], cp[1] - o->pos[1], cp[2] - o->pos[2] }, k = len3(d) * o->f[15] / fmaxf(o->f[9], 1);
                for (int j = 0; j < 3; j++) p[j] = cp[j] + f->car_vel[j] * k;
                p[0] += frange(W, -o->f[16], o->f[16]); p[1] += frange(W, -o->f[16], o->f[16]);
                colv(f->car_rot, 1, fwd);
            }
            float from[3] = { p[0], p[1], p[2] + 30 }, down[3] = { 0, 0, -1 }, t, n[3]; uint32_t m;
            if (!W->coll || !nr_coll_ray(W->coll, from, down, 60, &t, n, &m)) break;     /* missed: again in 0.15 s */
            float P[3] = { from[0], from[1], from[2] - t }, side[3], y[3];
            cross3(fwd, n, side); norm3(side); cross3(n, side, y);
            int sh = o->s[0] - 3;
            if (sh >= o->nkid) { o->s[0]++; break; }
            int mk = spawn(W, "ShardTarget", OK_SHARDTARGET); o = &W->o[i];
            if (mk >= 0) { nr_obj *q = &W->o[mk]; memcpy(q->pos, P, 12); set_cols(q->R, side, y, n); fx_init(q); obj_bounds(q); }
            nr_obj *q = &W->o[o->kid[sh]];
            memcpy(&q->f[0], P, 12); memcpy(&q->f[3], n, 12); q->s[0] = 2; q->s[1] = mk;
            o->s[0]++;
        }
        break;
    }
    case OK_SHARD: {
        if (dt <= 0) break;
        float a[3]; colv(o->R, 2, a);
        if (o->s[0] == 1) {
            o->f[9] += dt;
            if (o->f[9] > 1) { set3(o->vel, 0, 0, 0); set3(o->angvel, a[0] * o->f[12], a[1] * o->f[12], a[2] * o->f[12]); }
            else {
                float v = o->scale[2] * o->f[9] * 2.5f;
                set3(o->vel, o->f[6] * v, o->f[7] * v, o->f[8] * v);
                set3(o->angvel, a[0] * o->f[12] * o->f[9], a[1] * o->f[12] * o->f[9], a[2] * o->f[12] * o->f[9]);
            }
        } else if (o->s[0] == 2) {
            float d[3] = { o->f[0] - o->pos[0], o->f[1] - o->pos[1], o->f[2] - o->pos[2] }, *n = &o->f[3], c[3];
            cross3(a, d, c);
            if (dot3(n, d) > 0 && dot3(n, a) < 0) {
                set3(o->vel, 0, 0, 0); set3(o->angvel, 0, 0, 0); o->s[0] = 3;
                play(W, "Play_Rand_ShardImpact", o->pos);
                if (o->s[1] >= 0 && W->o[o->s[1]].alive) {
                    int sw = spawn_fx(W, "ShardShockwave", o->pos, 1); o = &W->o[i];
                    if (sw >= 0) memcpy(W->o[sw].R, W->o[o->s[1]].R, 36);
                    kill_obj(W, o->s[1]);
                    if (f->car_active) { float k3[3] = { 10, 100, 0 }; radial_push(o->pos, 10, k3, 25, cp, f->car_dv); }
                }
                o->s[1] = -1;
                break;
            }
            float k = 1, c2 = dot3(c, c), d2 = dot3(d, d);
            float r = d2 > 0 ? c2 / d2 : 0;
            if (c2 > 0) norm3(c);
            if (r < 0.15f && dot3(d, a) > 0) k = r * 6.66667f;
            for (int j = 0; j < 3; j++) { o->angvel[j] = c[j] * o->f[11] * k + a[j] * o->f[12]; o->vel[j] = a[j] * o->f[10]; }
        }
        madd3(o->pos, o->vel, dt);
        rotate_by(o->R, o->angvel, dt);
        break;
    }
    case OK_SHARDTARGET: case OK_FX: {
        if (o->kind == OK_SHARDTARGET && o->s[0] != 4) { o->f[0] -= dt; if (o->f[0] < 0) kill_obj(W, i); break; }
        if (o->f[0] < 0) { kill_obj(W, i); break; }
        float s = fmaxf(o->f[0] / fmaxf(o->f[1], 1e-3f), 0);
        for (int j = 0; j < 4; j++) o->col[j] = o->f[3 + j] * s + o->f[7 + j] * (1 - s);
        if (o->s[0] == 1) { float y[3]; colv(o->R, 1, y); madd3(o->pos, y, o->f[2] * dt); }   /* type 1: rises along local Y */
        else { for (int j = 0; j < 3; j++) o->scale[j] += o->f[2] * dt; obj_bounds(o); }      /* type 4: grows */
        o->f[0] -= dt;
        break;
    }
    case OK_BARRELSPAWNER: {
        int alive = 0;
        for (int k = 0; k < W->n; k++) if (W->o[k].alive && W->o[k].kind == OK_BARREL && W->o[k].s[0] == i) alive++;
        if (f->car_active) {
            float d[3] = { o->pos[0] - cp[0], o->pos[1] - cp[1], o->pos[2] - cp[2] };
            if (dot3(d, d) < o->f[1]) {
                if (o->f[0] > 0 && alive < o->s[1]) {
                    int c = spawn(W, "spike_barrel", OK_BARREL); o = &W->o[i];
                    if (c >= 0) {
                        nr_obj *q = &W->o[c];
                        float c0[3], c1[3], c2[3], m2[3]; colv(o->R, 0, c0); colv(o->R, 1, c1); colv(o->R, 2, c2);
                        set3(m2, -c2[0], -c2[1], -c2[2]);
                        set_cols(q->R, m2, c1, c0);
                        memcpy(q->pos, o->pos, 12); memcpy(q->col, o->col, 16);
                        q->s[0] = i; q->f[5] = o->f[4]; q->f[0] = o->pos[0]; q->f[1] = o->pos[1];
                        float dir[3] = { -c1[0], -c1[1], 0 }; norm3(dir); q->f[2] = dir[0]; q->f[3] = dir[1];
                        q->f[4] = 0; q->f[7] = 0; q->f[8] = 0; q->dynamic = 1;
                        obj_bounds(q);
                    }
                    o->f[0] = -o->f[2];
                }
            } else o->f[0] = -o->f[3];
        }
        o->f[0] += dt;
        break;
    }
    case OK_BARREL: {
        if (o->pos[2] < -70) { kill_obj(W, i); break; }
        float a[3]; colv(o->R, 2, a);
        float rv = o->f[5], Rb = 2.0f;
        float rd[3] = { a[1], -a[0], 0 }; norm3(rd);
        /* pure-pursuit steering back to the spawn line (reversed after 4 s: they veer off) */
        float L = 20 * rv, q[2] = { o->pos[0] + L * rd[0], o->pos[1] + L * rd[1] };
        float t = (q[0] - o->f[0]) * o->f[2] + (q[1] - o->f[1]) * o->f[3];
        float e[2] = { o->f[0] + o->f[2] * t - o->pos[0], o->f[1] + o->f[3] * t - o->pos[1] }, el = sqrtf(e[0] * e[0] + e[1] * e[1]);
        float steer = el > 0.001f ? (e[0] * a[0] + e[1] * a[1]) / el : 0;
        if (o->f[4] > 4.0f) steer = -steer;
        o->f[8] = (o->f[8] + steer * dt * 30) * powf(0.995f, dt / 0.005f);
        if (fabsf(o->f[7]) < rv) o->f[7] = rv;
        /* gravity, the road: three spheres along the axis */
        o->vel[2] -= 25 * dt;
        float sp = len3(o->vel);
        if (sp > 1e-4f) { float ns = fmaxf(0, sp - 0.995f * dt); for (int j = 0; j < 3; j++) o->vel[j] *= ns / sp; }
        madd3(o->pos, o->vel, dt);
        int ground = 0;
        for (int k = -1; k <= 1 && W->coll; k++) {
            float c[3] = { o->pos[0] + a[0] * 1.4f * k, o->pos[1] + a[1] * 1.4f * k, o->pos[2] + a[2] * 1.4f * k }, push[3], n[3];
            int saved = (int)W->coll->ndyn; W->coll->ndyn = 0;
            int h = nr_coll_sphere(W->coll, c, Rb, push, n);
            W->coll->ndyn = (uint32_t)saved;
            if (!h) continue;
            madd3(o->pos, push, 1.0f / 3);
            float vn = dot3(o->vel, n);
            if (vn < 0) {
                if (-vn > 30) play(W, "Play_Rand_Barrel_Impact", o->pos);
                madd3(o->vel, n, -vn * 1.05f);
            }
            if (n[2] > 0.5f) ground = 1;
        }
        if (ground) {                 /* rolling: the spin carries it along its roll direction */
            float want = o->f[7] * Rb, along = dot3(o->vel, rd);
            float side[3] = { o->vel[0] - rd[0] * along, o->vel[1] - rd[1] * along, 0 };
            along += (want - along) * fminf(1, dt * 4);
            for (int j = 0; j < 2; j++) o->vel[j] = rd[j] * along + side[j] * fmaxf(0, 1 - dt * 3);
        }
        float wz[3] = { 0, 0, o->f[8] };
        rotate_by(o->R, wz, dt);
        float spin[3] = { a[0] * o->f[7], a[1] * o->f[7], a[2] * o->f[7] };
        rotate_by(o->R, spin, dt);
        set3(o->angvel, spin[0], spin[1], spin[2] + o->f[8]);
        o->f[4] += dt;
        break;
    }
    case OK_DEBRIS: {
        o->f[0] -= dt;
        if (o->f[0] <= 0) { kill_obj(W, i); break; }
        o->vel[2] -= 25 * dt;
        madd3(o->pos, o->vel, dt);
        float push[3], n[3];
        if (W->coll && o->f[0] > 0) {
            int saved = (int)W->coll->ndyn; W->coll->ndyn = 0;
            float r = 0.3f * fmaxf(o->scale[0], 1);
            if (nr_coll_sphere(W->coll, o->pos, r, push, n)) {
                madd3(o->pos, push, 1);
                float vn = dot3(o->vel, n);
                if (vn < 0) { madd3(o->vel, n, -1.35f * vn); for (int j = 0; j < 3; j++) { o->vel[j] *= 0.75f; o->angvel[j] *= 0.7f; } }
            }
            W->coll->ndyn = (uint32_t)saved;
        }
        rotate_by(o->R, o->angvel, dt);
        break;
    }
    case OK_STREETLIGHT: {
        if (!o->visible) break;
        float g = 1;
        if (o->s[3]) {           /* SimpleStreetLightFlickerLogic: 2 s steady, 10 s of dips */
            o->f[0] += dt;
            if (o->s[0] == 0) {
                if (o->f[0] > o->f[4]) { o->s[0] = 1; o->s[1] = 0; o->f[0] = 0; o->f[1] = o->f[2] = 0.1f; }
            } else if (o->s[1] == 1) {
                o->f[1] -= dt; o->f[3] = clampf(o->f[1] / o->f[2], 0, 1);
                if (o->f[1] <= 0) { o->s[1] = 0; o->f[1] = 0; }
            } else {
                o->f[1] += dt * 3; o->f[3] = clampf(o->f[1] / o->f[2], 0, 1);
                if (o->f[1] >= o->f[2]) {
                    /* the PC rolls rand() % 20 every frame (at about 60 fps) */
                    float p = 1 - powf(1 - 1.0f / (2 * (o->s[2] > 0 ? o->s[2] : 10)), dt * 60);
                    if (frand(W) < p) {
                        if (o->f[0] > o->f[5]) { o->s[0] = 0; o->f[0] = 0; }
                        o->f[1] = o->f[2]; o->s[1] = 1;
                    }
                }
            }
            g = o->s[0] == 0 ? 1 : o->f[3];
        }
        set_glow(o, o->base[3] * g);
        break;
    }
    case OK_ANTENNA: {
        float t = o->f[0], step = (0.5f - fabsf(t - 0.5f) + 0.05f) * o->f[5] * dt;
        if (o->s[0]) { t += step; if (t > 1) { t = 1; o->s[0] = 0; } }
        else { t -= step; if (t < 0) { t = 0; o->s[0] = 1; } }
        o->f[0] = t;
        float u = 1 - t, sc = o->f[1] * u + o->f[2] * t;
        set_glow(o, o->f[3] * u + o->f[4] * t);
        set3(o->scale, sc, sc, sc);
        memcpy(o->R, f->cam_rot, 36);
        obj_bounds(o);
        break;
    }
    case OK_CHECKPOINT:        /* passed (0x728070): Activated Start Color -> End Color over a second */
        if (o->s[0] && o->f[8] < 1) {
            float t = o->f[8];
            for (int j = 0; j < 4; j++) o->col[j] = o->f[j] * (1 - t) + o->f[4 + j] * t;
            o->f[8] += dt;
        }
        break;
    case OK_DISTCOLOR: {
        float d[3] = { f->cam_pos[0] - o->pos[0], f->cam_pos[1] - o->pos[1], f->cam_pos[2] - o->pos[2] }, l = len3(d);
        float *n4 = &o->f[0], *f4 = &o->f[4], c[4];
        if (l > o->f[9]) memcpy(c, f4, 16);
        else if (l < o->f[8]) memcpy(c, n4, 16);
        else { float t = (o->f[9] - l) / (o->f[9] - o->f[8]); for (int j = 0; j < 4; j++) c[j] = n4[j] * t + f4[j] * (1 - t); }
        memcpy(o->col, c, 16);
        break;
    }
    case OK_COREBALL:
        if (o->s[6]) {             /* CoreBallExplosionLogic (re_E 6.1): pulses 3.25 s, bursts in 0.275 s, falls 1 s */
            o->f[10] += dt;
            float T = o->f[10], P = 3.25f, E = 0.275f, Fl = 1.0f;
            if (T < P) { float p = T / P * 1.25f; o->col[3] = (1 - p) * o->f[11] + 2; set3(o->scale, o->lscale[0] * (1 + 0.08f * sinf(T * 30) * p), o->lscale[1] * (1 + 0.08f * sinf(T * 30) * p), o->lscale[2] * (1 + 0.08f * sinf(T * 30) * p)); }
            else if (T < P + E + Fl) {     /* (the PC shader throws the pieces out and down; here it bursts and fades) */
                float k = (T - P) / (E + Fl), g = 1 + 1.5f * fminf((T - P) / E, 1);
                set3(o->scale, o->lscale[0] * g, o->lscale[1] * g, o->lscale[2] * g);
                o->col[3] = 2 * (1 - k); o->pos[2] = o->lpos[2] - 13 * k * k * o->lscale[2] * 0.2f;
            } else { o->visible = 0; break; }
            obj_bounds(o);
        }
        o->f[7] += o->f[2] * dt;
        while (o->f[7] > 1) {
            o->f[7] -= 1;
            float a = frand(W) * 2 - 1, b = frand(W) * 2 - 1, c = frand(W) * 2 - 1;
            float D[3] = { c * o->f[1], b * o->f[1], a * o->f[1] };
            if (fabsf(D[0]) + fabsf(D[1]) + fabsf(D[2]) < 0.001f) continue;
            float u[3] = { D[0], D[1], D[2] }; norm3(u);
            float O[3] = { o->pos[0] + o->scale[0] * u[0], o->pos[1] + o->scale[1] * u[1], o->pos[2] + o->scale[2] * u[2] };
            float dl = len3(D), t, n[3]; uint32_t m;
            if (!W->coll || !nr_coll_ray(W->coll, O, u, dl, &t, n, &m)) continue;
            float lo_ = fminf(o->f[3], o->f[4]), hi = fmaxf(o->f[3], o->f[4]);
            int s = spawn_spark(W, lo_ + frand(W) * (hi - lo_), o->f[5], o->f[6]); o = &W->o[i];
            if (s < 0) continue;
            nr_obj *q = &W->o[s];
            memcpy(&q->f[3], O, 12); set3(&q->f[6], O[0] + u[0] * t, O[1] + u[1] * t, O[2] + u[2] * t);
        }
        break;
    case OK_SPARK:
        if (o->f[0] > o->f[1]) { kill_obj(W, i); break; }
        o->col[3] = (1 - o->f[0] / o->f[1]) * o->f[2];
        o->f[0] += dt;
        place_spark(o, f);
        break;
    case OK_FORCEFIELD:
        if (f->car_active) {
            float d[3] = { cp[0] - o->pos[0], cp[1] - o->pos[1], cp[2] - o->pos[2] };
            if (dot3(d, d) < o->f[0] * o->f[0]) {
                float s = len3(f->car_vel);
                if (s > 0) {
                    float dec = (o->f[1] + o->f[2] * s + o->f[3] * s * s) * dt;
                    if (dec > s) dec = s;
                    f->car_vscale *= (s - dec) / s;
                }
            }
        }
        break;
    case OK_TRIGGER: {         /* the zones test the car's origin every Recheck Delay; sizes are not scaled */
        o->t += dt;
        if (!(o->t > o->f[5])) break;
        o->t = 0;
        if (!f->car_active) break;
        float d[3] = { cp[0] - o->pos[0], cp[1] - o->pos[1], cp[2] - o->pos[2] }, *he = &o->f[2];
        int in = 0;
        switch (o->s[0]) {
        case 0: in = o->f[0] * o->f[0] > dot3(d, d); break;
        case 1: in = o->f[1] > fabsf(d[2]) && o->f[0] * o->f[0] > d[0] * d[0] + d[1] * d[1]; break;   /* world Z, half height */
        case 2: in = fabsf(d[0]) <= he[0] && fabsf(d[1]) <= he[1] && fabsf(d[2]) <= he[2]; break;
        case 3: {
            in = 1;
            for (int k = 0; k < 3; k++) { float a[3]; colv(o->R, k, a); if (fabsf(dot3(a, d)) > he[k]) in = 0; }
            break;
        }
        }
        if (in) {
            if (o->s[2] && o->s[1]) break;
            o->s[1] = 1;
            send_out(W, o->line, ZONES[o->s[0]], "Outgoing");
        } else o->s[1] = 0;
        break;
    }
    case OK_RECV:              /* TriggerDelay */
        if (o->s[5]) {
            o->f[20] += dt;
            if (o->f[20] > kvf(o->line, "TriggerDelay.Delay", 1)) { o->s[5] = 0; send_out(W, o->line, "TriggerDelay", "Outgoing"); }
        }
        break;
    case OK_SLIDER: {
        float ax[3], rel[3] = { o->pos[0] - o->f[3], o->pos[1] - o->f[4], o->pos[2] - o->f[5] }; colv(o->R, 2, ax);
        float h = dot3(rel, ax);
        if (o->s[0] == 1 && h >= o->f[1]) o->s[0] = 2;
        else if (o->s[0] == 2 && h <= 0) o->s[0] = 1;
        float v = o->f[0] * (o->s[0] == 1 ? 1.0f : -1.0f);
        set3(o->vel, ax[0] * v, ax[1] * v, ax[2] * v);
        madd3(o->pos, o->vel, dt);
        break;
    }
    case OK_SWINGER: {         /* angular acceleration -Speed sin(angle) about the swing axis */
        float z0[3], zb[3], c[3]; colv(o->lR, 2, z0); colv(o->R, 2, zb); cross3(z0, zb, c);
        madd3(o->angvel, c, -o->f[0] * dt);
        rotate_by(o->R, o->angvel, dt);
        break;
    }
    case OK_STOMPER: {
        if (o->s[6] < 0) {
            int c = spawn(W, "stomper_foot", OK_FOOT); o = &W->o[i];
            if (c < 0) { o->alive = 0; break; }
            o->s[6] = c;
            nr_obj *ft = &W->o[c];
            float Z[3]; colv(o->R, 2, Z);
            for (int k = 0; k < 3; k++) o->f[6 + k] = o->pos[k] - Z[k] * o->scale[2];     /* one scaled unit below the can */
            memcpy(ft->pos, &o->f[6], 12); memcpy(ft->R, o->R, 36); memcpy(ft->scale, o->scale, 12);
            obj_bounds(ft);
            o->f[5] = o->f[4];
            stomper_next(W, i, f); o = &W->o[i];
        }
        nr_obj *ft = &W->o[o->s[6]];
        float dur = o->s[0] == 0 ? o->f[0] : o->s[0] == 1 ? o->f[1] : o->s[0] == 2 ? o->f[2] : o->f[3];
        if (o->s[0] == 1) o->f[9] += dt;
        o->t += dt / fmaxf(dur, 1e-3f);
        if (o->t > 1) {
            if (o->s[0] == 2) { set3(ft->vel, 0, 0, 0); play(W, "Play_StomperStomp", ft->pos); }
            if (o->s[0] == 3) { memcpy(ft->pos, &o->f[6], 12); set3(ft->vel, 0, 0, 0); }
            stomper_next(W, i, f);
        }
        madd3(ft->pos, ft->vel, dt);
        break;
    }
    case OK_MORTAR: {
        float d[3] = { o->pos[0] - cp[0], o->pos[1] - cp[1], o->pos[2] - cp[2] }, d2 = dot3(d, d);
        if (d2 > o->f[2] * o->f[2]) { o->s[0] = 1; holo_fade(o, dt, 0, o->f[5]); }
        else if (!o->s[0]) holo_fade(o, dt, 0, o->f[5]);
        else {
            holo_fade(o, dt, 1, o->f[5]);
            if (d2 < o->f[1] * o->f[1]) {
                o->s[0] = 0;
                int c = spawn(W, "MortarMine", OK_MORTARMINE); o = &W->o[i];
                if (c >= 0) {
                    nr_obj *q = &W->o[c];
                    memcpy(q->pos, o->pos, 12); memcpy(q->R, o->R, 36);
                    for (int k = 0; k < 3; k++) q->scale[k] = o->scale[k] * 0.2f;
                    float z[3], x[3]; colv(o->R, 2, z); colv(o->R, 0, x);
                    set3(q->vel, z[0] * o->f[3], z[1] * o->f[3], z[2] * o->f[3]);
                    set3(q->angvel, x[0] * o->f[4], x[1] * o->f[4], x[2] * o->f[4]);
                    q->t = 0; q->dynamic = 1;
                    q->f[0] = kvf(q->line, "MortarMineLogic.Timer_Min", 0.5f); q->f[1] = kvf(q->line, "MortarMineLogic.Timer_Max", 1.5f);
                    obj_bounds(q);
                }
            }
        }
        break;
    }
    case OK_MORTARMINE: {
        o->t += dt;
        set3(o->col, 1, floorf((sinf(3 * o->t) + 1) * 0.5f * 0.6f * 255) / 255, 25.0f / 255); o->col[3] = 3;   /* blinks */
        o->vel[2] -= 25 * dt;                          /* a normal dynamic body: gravity, Drag (0.5, 0.01, 0.0025) */
        float sp = len3(o->vel);
        if (sp > 1e-4f) { float ns = fmaxf(0, sp - (0.5f + 0.01f * sp + 0.0025f * sp * sp) * dt); for (int k = 0; k < 3; k++) o->vel[k] *= ns / sp; }
        madd3(o->pos, o->vel, dt);
        rotate_by(o->R, o->angvel, dt);
        int contact = 0; float r = fmaxf(o->scale[0], 0.1f);
        if (W->coll) {
            uint32_t saved = W->coll->ndyn; W->coll->ndyn = 0;
            float push[3], n[3];
            if (nr_coll_sphere(W->coll, o->pos, r, push, n)) {
                contact = 1; madd3(o->pos, push, 1);
                float vn = dot3(o->vel, n); if (vn < 0) madd3(o->vel, n, -1.05f * vn);
            }
            W->coll->ndyn = saved;
        }
        if (f->car_active && car_box_dist(f, o->pos) < r) contact = 1;
        if ((contact && o->t > o->f[0]) || o->t > o->f[1] || o->pos[2] < -70) mortar_explode(W, i, f);
        break;
    }
    case OK_DROPSHIP: {
        float O[3], X[3], Y[3], Z[3]; memcpy(O, o->pos, 12); colv(o->R, 0, X); colv(o->R, 1, Y); colv(o->R, 2, Z);
        float d[3] = { O[0] - cp[0], O[1] - cp[1], O[2] - cp[2] }, d2 = dot3(d, d);
        nr_obj *blk = o->s[1] >= 0 ? &W->o[o->s[1]] : NULL;
        switch (o->s[0]) {
        case 0:
            holo_fade(o, dt, 0, o->f[7]);
            if (f->car_active && d2 < o->f[1] * o->f[1]) {
                float H = o->f[4], Sp = o->f[5], T = sqrtf(0.08f * H);           /* time to fall H at 25 */
                o->f[13] = Sp / fmaxf(T, 1e-3f); o->f[14] = 0.5f * Sp * T;
                float back = o->f[14] + (o->f[6] - T) * Sp;
                char nm[48];
                kvs(o->line, "DropShipLogic.Ship_Name", nm, sizeof nm); if (!nm[0]) snprintf(nm, sizeof nm, "drone_hawk2");
                int sh = spawn(W, nm, OK_SHIP); o = &W->o[i];
                if (sh >= 0) {
                    nr_obj *q = &W->o[sh];
                    float l[3] = { o->f[9], o->f[10] - back, o->f[11] + H };
                    world_of(o->R, O, l, q->pos); memcpy(q->R, o->R, 36);
                    set3(q->vel, Y[0] * Sp, Y[1] * Sp, Y[2] * Sp); obj_bounds(q);
                }
                kvs(o->line, "DropShipLogic.Block_Name", nm, sizeof nm); if (!nm[0]) snprintf(nm, sizeof nm, "mblock2");
                int bk = spawn(W, nm, OK_DROPBLOCK); o = &W->o[i];
                if (bk >= 0) {
                    nr_obj *q = &W->o[bk];
                    float l[3] = { 0, -back, H }, bpR[3][3]; memcpy(bpR, q->R, 36);
                    world_of(o->R, O, l, q->pos); mat_mul(o->R, bpR, q->R);
                    set3(q->vel, Y[0] * Sp, Y[1] * Sp, Y[2] * Sp); obj_bounds(q);
                }
                o->s[1] = bk; o->s[2] = sh; o->s[0] = 1;
            }
            break;
        case 1:
            holo_fade(o, dt, 1, o->f[7]);
            if (blk) { float e[3] = { O[0] - blk->pos[0], O[1] - blk->pos[1], O[2] - blk->pos[2] }; if (dot3(e, Y) <= o->f[14]) o->s[0] = 2; }
            else o->s[0] = 3;
            break;
        case 2:
            holo_fade(o, dt, 0, o->f[7]);
            if (blk) {
                madd3(blk->vel, Y, -o->f[13] * dt); madd3(blk->vel, Z, -25 * dt);   /* brakes and falls */
                float e[3] = { blk->pos[0] - O[0], blk->pos[1] - O[1], blk->pos[2] - O[2] }, h = dot3(e, Z) - o->f[8];
                if (h <= 0) {
                    o->s[0] = 3; set3(blk->vel, 0, 0, 0); madd3(blk->pos, Z, -h);
                    int sw = spawn_fx(W, "ShardShockwave", O, 1); o = &W->o[i];
                    if (sw >= 0) memcpy(W->o[sw].R, o->R, 36);
                    float k[3] = { 10, 100, 0 };
                    if (f->car_active) radial_push(O, 10, k, 25, cp, f->car_dv);
                }
            } else o->s[0] = 3;
            break;
        case 3: holo_fade(o, dt, 0, o->f[7]); if (d2 > o->f[2] * o->f[2]) { o->s[0] = 4; o->f[12] = 0; } break;
        case 4: o->f[12] += dt; if (o->f[12] > o->f[3]) dropship_reset(W, i); break;
        }
        o = &W->o[i];
        for (int k = 1; k <= 2; k++) if (o->s[k] >= 0 && W->o[o->s[k]].alive) madd3(W->o[o->s[k]].pos, W->o[o->s[k]].vel, dt);
        break;
    }
    case OK_COOLPAD: {
        float h[3], c[3], d[3];
        for (int k = 0; k < 3; k++) { h[k] = o->f[k] * fabsf(o->scale[k]); c[k] = o->pos[k] + o->f[3 + k] * h[k]; d[k] = cp[k] - c[k]; }
        int in = f->car_active;
        for (int k = 0; k < 3 && in; k++) { float a[3]; colv(o->R, k, a); if (fabsf(dot3(a, d)) > h[k]) in = 0; }
        if (!in) { memcpy(o->col, o->base, 16); break; }
        memcpy(o->col, &o->f[6], 16);                  /* the pad turns green */
        f->cool += 0.5f * len3(f->car_vel); f->cool_glow = 1;
        break;
    }
    case OK_CARCOOLER: f->car_cooler = 1; f->cool_glow = 1; break;
    case OK_FISH: fish_update(W, i, f, dt); break;
    case OK_GARAGE:            /* GarageLogic (re_F1 7): two drones out of the door when the car comes within 340 */
        if (!o->s[0] && f->car_active) {
            float e[3] = { cp[0] - o->pos[0], cp[1] - o->pos[1], cp[2] - o->pos[2] };
            if (dot3(e, e) < o->f[0] * o->f[0]) {
                float X[3], Y[3], Z[3], R[3][3], P[3], mx[3], my[3]; colv(o->R, 0, X); colv(o->R, 1, Y); colv(o->R, 2, Z);
                set3(mx, -X[0], -X[1], -X[2]); set3(my, -Y[0], -Y[1], -Y[2]); set_cols(R, mx, my, Z);
                float sx = o->scale[0], sz = o->scale[2];
                for (int side = -1; side <= 1; side += 2) {
                    for (int k = 0; k < 3; k++) P[k] = o->pos[k] + X[k] * sx * 2 * side + Z[k] * sz * 0.25f;
                    humvee_new(W, P, R); o = &W->o[i];
                }
                o->s[0] = 2;
            }
        }
        break;
    case OK_HUMVEE:
        if (o->s[7] < 0 || o->s[7] >= W->nhv || W->hv[o->s[7]].obj != i) {   /* a placed one: becomes a drone */
            float P[3], R[3][3]; memcpy(P, o->pos, 12); memcpy(R, o->R, 36);
            kill_obj(W, i); humvee_new(W, P, R);
            break;
        }
        humvee_update(W, i, f, dt);
        break;
    case OK_PHASER: phaser_update(W, i, f, dt); break;
    case OK_COREEXPL: core_update(W, i, f, dt); break;
    case OK_EYE: o->col[3] = 0.1f; break;
    case OK_FINISH: {          /* the car's origin crosses the tunnel's local XZ plane within the window */
        if (o->s[0] || !f->car_active) break;
        float d[3] = { cp[0] - o->pos[0], cp[1] - o->pos[1], cp[2] - o->pos[2] }, l[3];
        for (int k = 0; k < 3; k++) { float a[3]; colv(o->R, k, a); l[k] = dot3(a, d); }
        if (fabsf(l[1]) > 20) { o->f[0] = l[1]; break; }
        if (l[1] * o->f[0] > 0) { o->f[0] = l[1]; break; }
        o->f[0] = l[1];
        if (fabsf(l[0] / (o->scale[0] != 0 ? o->scale[0] : 1)) > 8.6f) break;
        float z = l[2] / (o->scale[2] != 0 ? o->scale[2] : 1);
        if (z > 17.6f || z < 1.2f) break;
        o->s[0] = 1; f->finished = 1;
        break;
    }
    default: break;
    }
}

void nr_objs_update(nr_objs *W, nr_objs_frame *f, float dt)
{
    f->heat = 0; f->car_dv[0] = f->car_dv[1] = f->car_dv[2] = 0; f->car_vscale = 1; f->shake = 0;
    f->cool = 0; f->cool_glow = 0; f->car_cooler = 0; f->finished = 0;
    f->car_push[0] = f->car_push[1] = f->car_push[2] = 0;
    {   /* FishZone's manager: the zone whose origin is nearest the car */
        int prev = W->fish_zone, best = -1; float bd = 1e30f;
        for (int i = 0; i < W->n; i++) if (W->o[i].kind == OK_FISHZONE) {
            float e[3] = { W->o[i].pos[0] - f->car_pos[0], W->o[i].pos[1] - f->car_pos[1], W->o[i].pos[2] - f->car_pos[2] };
            if (dot3(e, e) < bd) { bd = dot3(e, e); best = i; }
        }
        W->fish_zone = best; W->fish_zone_changed = best != prev && prev >= 0;
    }
    if (dt > 0.1f) dt = 0.1f;
    W->time += dt;
    int n0 = W->n;                    /* objects spawned now start next frame */
    for (int i = 0; i < n0; i++) {
        nr_obj *o = &W->o[i];
        if (!o->alive) continue;
        update_obj(W, i, f, dt);
        o = &W->o[i];
        if (o->pulse > 0 && o->alive)   /* LightPulse: the glow flashes on the music's beat */
            o->col[3] = o->base[3] * o->pulse * f->beat;
    }
}

/* ------------------------------------------------------------------ the car against moving things */
static int moves(int k)
{
    return k == OK_SAW || k == OK_BAFSAW || k == OK_SWEEP || k == OK_SPINLASER || k == OK_BLOCK || k == OK_MINE ||
           k == OK_SHARD || k == OK_TRAINCAR || k == OK_BLIMP || k == OK_BARREL || k == OK_SLIDER || k == OK_SWINGER ||
           k == OK_FOOT || k == OK_DROPBLOCK;
}

void nr_objs_collision(nr_objs *W, const float at[3], float reach)
{
    nr_coll *c = W->coll;
    if (!c) return;
    c->ndyn = 0; c->bodies = W->bodies;
    for (int i = 0; i < W->n; i++) {
        nr_obj *o = &W->o[i];
        if (!o->alive || o->cmodel < 0 || !moves(o->kind)) continue;
        const nr_cmodel *m = &W->cm[o->cmodel];
        float sc = fmaxf(fabsf(o->scale[0]), fmaxf(fabsf(o->scale[1]), fabsf(o->scale[2])));
        float d[3] = { o->pos[0] - at[0], o->pos[1] - at[1], o->pos[2] - at[2] }, rr = m->r * sc + reach;
        if (dot3(d, d) > rr * rr) continue;
        memcpy(W->bodies[i].c, o->pos, 12); memcpy(W->bodies[i].v, o->vel, 12); memcpy(W->bodies[i].w, o->angvel, 12);
        float det = o->scale[0] * o->scale[1] * o->scale[2];
        uint32_t mat = (o->cmat & 15u) | ((o->matid & 15u) << 4) | ((uint32_t)i << 8) | NR_TRI_DYN;
        /* the reach in the object's own space: skip triangles there before moving them (big meshes
         * such as the Nitro Tunnel's tube) */
        float lp[3], rl = reach / fmaxf(fminf(fabsf(o->scale[0]), fminf(fabsf(o->scale[1]), fabsf(o->scale[2]))), 1e-3f);
        for (int k = 0; k < 3; k++) {
            float a[3]; colv(o->R, k, a);
            lp[k] = (a[0] * (-d[0]) + a[1] * (-d[1]) + a[2] * (-d[2])) / (fabsf(o->scale[k]) > 1e-6f ? o->scale[k] : 1);
        }
        for (uint32_t t = 0; t < m->n; t++) {
            if (m->bb) {
                const float *b = m->bb[t];
                if (b[3] < lp[0] - rl || b[0] > lp[0] + rl || b[4] < lp[1] - rl || b[1] > lp[1] + rl || b[5] < lp[2] - rl || b[2] > lp[2] + rl) continue;
            }
            if (c->ndyn == c->dyncap) {
                uint32_t nc = c->dyncap ? c->dyncap * 2 : 1024;
                void *p = realloc(c->dyn, sizeof *c->dyn * nc); if (!p) return;
                c->dyn = p; c->dyncap = nc;
            }
            nr_ctri *T = &c->dyn[c->ndyn];
            for (int j = 0; j < 3; j++) to_world(o, &m->t[t][j * 3], T->v[det < 0 ? 2 - j : j]);
            float e1[3] = { T->v[1][0] - T->v[0][0], T->v[1][1] - T->v[0][1], T->v[1][2] - T->v[0][2] };
            float e2[3] = { T->v[2][0] - T->v[0][0], T->v[2][1] - T->v[0][1], T->v[2][2] - T->v[0][2] };
            int out = 0;          /* only what the car can reach this frame */
            for (int j = 0; j < 3 && !out; j++) {
                float lo = fminf(T->v[0][j], fminf(T->v[1][j], T->v[2][j])), hi = fmaxf(T->v[0][j], fmaxf(T->v[1][j], T->v[2][j]));
                if (hi < at[j] - reach || lo > at[j] + reach) out = 1;
            }
            if (out) continue;
            cross3(e1, e2, T->n);
            if (len3(T->n) < 1e-9f) continue;
            norm3(T->n);
            T->mat = mat;
            c->ndyn++;
        }
    }
}

static nr_obj *obj_of(nr_objs *W, uint32_t mat)
{
    uint32_t id = NR_TRI_OBJ(mat);
    if (mat & NR_TRI_DYN) return id < (uint32_t)W->n ? &W->o[id] : NULL;
    if (!id) return NULL;
    for (int i = 0; i < W->n; i++) if (W->o[i].id == (int)id) return &W->o[i];
    return NULL;
}

static void break_streetlight(nr_objs *W, nr_obj *o, const float *car_vel)
{
    if (!o->visible) return;
    o->visible = 0;
    if (o->id > 0 && (uint32_t)o->id < W->noff) W->off[o->id] = 1;
    play(W, "Play_SmashPole", o->pos);
    float pos[3], R[3][3], sc[3];
    memcpy(pos, o->pos, 12); memcpy(R, o->R, 36); memcpy(sc, o->scale, 12);
    float col[4]; memcpy(col, o->col, 16);
    for (int k = 1; k <= 4; k++) {
        char nm[24]; snprintf(nm, sizeof nm, "streetlight_dead%d", k);
        int pi = find_proto(W, nm);
        if (pi < 0) continue;
        float lp[3] = { 0, 0, 0 }, r9[9] = { 0 }, lr[3][3], wr[3][3], at[3], ls[3] = { 1, 1, 1 };
        kvv(W->proto[pi].line, "p", lp, 3); kvv(W->proto[pi].line, "s", ls, 3);
        identity(lr);
        if (kvv(W->proto[pi].line, "r", r9, 9) == 9 && fabsf(r9[0]) + fabsf(r9[4]) + fabsf(r9[8]) > 1e-6f) for (int j = 0; j < 9; j++) lr[j / 3][j % 3] = r9[j];
        float sl[3] = { lp[0] * sc[0], lp[1] * sc[1], lp[2] * sc[2] }, w[3];
        mat_vec(R, sl, w);
        for (int j = 0; j < 3; j++) at[j] = pos[j] + w[j];
        mat_mul(R, lr, wr);
        float s3[3] = { sc[0] * ls[0], sc[1] * ls[1], sc[2] * ls[2] };
        /* the car's push (0.3 x its velocity on pieces of inverse mass 100): kept to the car's speed */
        float k1 = frange(W, 0.6f, 1.2f), v[3] = { car_vel[0] * k1, car_vel[1] * k1, car_vel[2] * k1 + frange(W, 2, 8) };
        int d = spawn_debris(W, nm, at, wr, s3, v, frange(W, 13, 14));
        if (d >= 0) memcpy(W->o[d].col, col, 16);
    }
    W->lights_broken++;
    if (W->host.event) W->host.event(W->host.user, "streetlight", W->lights_broken);
}

static void break_screen(nr_objs *W, nr_obj *o)
{
    if (o->s[7]) return;
    o->s[7] = 1;
    int pi = find_proto(W, "poster_dead");
    if (pi >= 0) {
        char b[64]; kvs(W->proto[pi].line, "m", b, sizeof b);
        float mn[3], mx[3]; int m = model_of(W, b, mn, mx);
        if (m >= 0) o->model = (int16_t)m;
    }
    play(W, "Play_ScreenGlassBreak", o->pos);
    W->screens_broken++;
    if (W->host.event) W->host.event(W->host.user, "screen", W->screens_broken);
}

void nr_objs_car_contacts(nr_objs *W, nr_car *c, int wings_out, nr_objs_hit *h)
{
    memset(h, 0, sizeof *h);
    float up[3] = { c->rot[0][2], c->rot[1][2], c->rot[2][2] };
    for (int k = 0; k < c->ncontact; k++) {
        uint32_t mat = c->contact[k].mat;
        nr_obj *o = obj_of(W, mat);
        int okind = o ? o->kind : -1, oidx = o ? (int)(o - W->o) : -1, odyn = o ? o->dynamic : 0;
        float ods = o ? o->death_speed : 0;
        int matid = (int)NR_TRI_MATID(mat);
        float d = c->contact[k].d, *n = c->contact[k].n;
        if (matid == 1) {                                  /* killblock, lava: instant */
            h->death = 2; memcpy(h->death_p, c->contact[k].p, 12); memcpy(h->death_n, n, 12);
            return;
        }
        float rate = fminf(c->contact[k].slide / 3.0f, 15.0f);
        if (rate > h->spark_rate) { h->spark_rate = rate; memcpy(h->spark, c->contact[k].p, 12); memcpy(h->spark_n, n, 12); }
        if (rate >= 2.0f) h->scraping = 1;
        float sk = fminf(0.05f * d / 60.0f, 0.10f);
        if (sk > h->shake) h->shake = sk;
        if (okind == OK_MINE) mine_explode(W, oidx, c->pos, h->dv);
        if (matid == 4 && d > 15 && okind == OK_SCREEN) break_screen(W, &W->o[oidx]);
        if (matid == 3 && d > 15) {
            if (okind == OK_STREETLIGHT) break_streetlight(W, &W->o[oidx], c->vel);
            h->restore_vel = 1;
        } else {
            float thr;
            int dyn_body = odyn;
            if (wings_out) thr = 20;
            else if (ods > 0) thr = ods;
            else if (matid == 2) thr = 50;
            else if (dyn_body) thr = 1e9f;
            else thr = 60;
            if (d > thr && dot3(n, up) < 0.7f) {
                h->death = 3; memcpy(h->death_p, c->contact[k].p, 12); memcpy(h->death_n, n, 12);
                return;
            }
        }
        if (!h->crash_sound) { if (d > 30) h->crash_sound = 2; else if (d > 15) h->crash_sound = 1; }
    }
    if (c->ncontact > 0) h->fold_wings = 1;
}
