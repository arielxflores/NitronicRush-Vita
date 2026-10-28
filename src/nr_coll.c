#include "nr_coll.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int nr_coll_parse(const uint8_t *b, uint32_t size, nr_coll *c)
{
    memset(c, 0, sizeof *c);
    if (size < 28 || memcmp(b, "NRC1", 4)) return -1;
    uint32_t h[3]; float f[3];
    memcpy(h, b + 4, 12); memcpy(f, b + 16, 12);
    c->ntri = h[0]; c->nx = h[1]; c->ny = h[2]; c->minx = f[0]; c->miny = f[1]; c->cell = f[2];
    uint64_t o = 28, ncell = (uint64_t)c->nx * c->ny;
    if (o + 52ull * c->ntri + 4 * (ncell + 1) > size) return -2;
    c->tris = malloc(52ull * c->ntri + 1); memcpy(c->tris, b + o, 52ull * c->ntri); o += 52ull * c->ntri;
    c->start = malloc(4 * (ncell + 1)); memcpy(c->start, b + o, 4 * (ncell + 1)); o += 4 * (ncell + 1);
    uint32_t nref = c->start[ncell];
    if (o + 4ull * nref > size) { nr_coll_free(c); return -3; }
    c->idx = malloc(4ull * nref + 4); memcpy(c->idx, b + o, 4ull * nref);
    c->stamp = calloc(c->ntri + 1, 4);
    c->minz = 1e30f;
    for (uint32_t i = 0; i < c->ntri; i++) for (int k = 0; k < 3; k++) if (c->tris[i].v[k][2] < c->minz) c->minz = c->tris[i].v[k][2];
    if (c->ntri == 0) c->minz = -1000;
    return 0;
}

void nr_coll_free(nr_coll *c)
{
    free(c->tris); free(c->start); free(c->idx); free(c->stamp); free(c->dyn);
    memset(c, 0, sizeof *c);
}

static int tri_off(const nr_coll *c, const nr_ctri *T)
{
    uint32_t id = NR_TRI_OBJ(T->mat);
    return c->off && !(T->mat & NR_TRI_DYN) && id && id < c->noff && c->off[id];
}
void nr_coll_point_vel(const nr_coll *c, uint32_t mat, const float p[3], float v[3])
{
    v[0] = v[1] = v[2] = 0;
    if (!(mat & NR_TRI_DYN) || !c->bodies) return;
    const nr_coll_body *b = &c->bodies[NR_TRI_OBJ(mat)];
    float r[3] = { p[0] - b->c[0], p[1] - b->c[1], p[2] - b->c[2] };
    v[0] = b->v[0] + b->w[1] * r[2] - b->w[2] * r[1];
    v[1] = b->v[1] + b->w[2] * r[0] - b->w[0] * r[2];
    v[2] = b->v[2] + b->w[0] * r[1] - b->w[1] * r[0];
}

static void next_stamp(nr_coll *c)
{
    if (++c->stamp_id == 0) { memset(c->stamp, 0, 4 * (c->ntri + 1)); c->stamp_id = 1; }
}

/* calls fn for each triangle in the cells overlapping [x0,x1]x[y0,y1] (each once) */
#define FOR_CELLS(c, x0, y0, x1, y1, ...) do { \
    int cx0 = (int)floorf(((x0) - (c)->minx) / (c)->cell), cx1 = (int)floorf(((x1) - (c)->minx) / (c)->cell); \
    int cy0 = (int)floorf(((y0) - (c)->miny) / (c)->cell), cy1 = (int)floorf(((y1) - (c)->miny) / (c)->cell); \
    if (cx0 < 0) cx0 = 0; if (cy0 < 0) cy0 = 0; \
    if (cx1 >= (int)(c)->nx) cx1 = (int)(c)->nx - 1; if (cy1 >= (int)(c)->ny) cy1 = (int)(c)->ny - 1; \
    for (int cy = cy0; cy <= cy1; cy++) for (int cx = cx0; cx <= cx1; cx++) { \
        uint32_t cell = (uint32_t)cy * (c)->nx + cx; \
        for (uint32_t k = (c)->start[cell]; k < (c)->start[cell + 1]; k++) { \
            uint32_t ti = (c)->idx[k]; \
            if ((c)->stamp[ti] == (c)->stamp_id) continue; \
            (c)->stamp[ti] = (c)->stamp_id; \
            const nr_ctri *T = &(c)->tris[ti]; \
            if (tri_off(c, T)) continue; \
            __VA_ARGS__ \
        } \
    } } while (0)

static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void sub3(const float *a, const float *b, float *o) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
static void cross3(const float *a, const float *b, float *o)
{
    o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}

static int ray_tri(const nr_ctri *T, const float o[3], const float d[3], float *best)
{
    if (dot3(T->n, d) >= 0) return 0;                     /* only surfaces facing the ray */
    float e1[3], e2[3], pv[3], tv[3], qv[3];
    sub3(T->v[1], T->v[0], e1); sub3(T->v[2], T->v[0], e2);
    cross3(d, e2, pv);
    float det = dot3(e1, pv);
    if (fabsf(det) < 1e-9f) return 0;
    float inv = 1.0f / det;
    sub3(o, T->v[0], tv);
    float u = dot3(tv, pv) * inv;
    if (u < 0 || u > 1) return 0;
    cross3(tv, e1, qv);
    float v = dot3(d, qv) * inv;
    if (v < 0 || u + v > 1) return 0;
    float t = dot3(e2, qv) * inv;
    if (t >= 0 && t < *best) { *best = t; return 1; }
    return 0;
}

int nr_coll_ray(nr_coll *c, const float o[3], const float d[3], float maxd, float *dist, float n[3], uint32_t *mat)
{
    float best = maxd; int hit = 0;
    for (uint32_t k = 0; k < c->ndyn; k++)
        if (ray_tri(&c->dyn[k], o, d, &best)) { hit = 1; memcpy(n, c->dyn[k].n, 12); if (mat) *mat = c->dyn[k].mat; }
    if (!c->ntri) { if (hit) *dist = best; return hit; }
    float e[3] = { o[0] + d[0] * maxd, o[1] + d[1] * maxd, o[2] + d[2] * maxd };
    next_stamp(c);
    FOR_CELLS(c, fminf(o[0], e[0]), fminf(o[1], e[1]), fmaxf(o[0], e[0]), fmaxf(o[1], e[1]), {
        if (dot3(T->n, d) >= 0) continue;                 /* only surfaces facing the ray */
        float e1[3], e2[3], pv[3], tv[3], qv[3];
        sub3(T->v[1], T->v[0], e1); sub3(T->v[2], T->v[0], e2);
        cross3(d, e2, pv);
        float det = dot3(e1, pv);
        if (fabsf(det) < 1e-9f) continue;
        float inv = 1.0f / det;
        sub3(o, T->v[0], tv);
        float u = dot3(tv, pv) * inv;
        if (u < 0 || u > 1) continue;
        cross3(tv, e1, qv);
        float v = dot3(d, qv) * inv;
        if (v < 0 || u + v > 1) continue;
        float t = dot3(e2, qv) * inv;
        if (t >= 0 && t < best) { best = t; hit = 1; memcpy(n, T->n, 12); if (mat) *mat = T->mat; }
    });
    if (hit) *dist = best;
    return hit;
}

/* closest point on triangle (Ericson, Real-Time Collision Detection 5.1.5) */
static void closest_on_tri(const float p[3], const float a[3], const float b[3], const float cc[3], float out[3])
{
    float ab[3], ac[3], ap[3], bp[3], cp[3];
    sub3(b, a, ab); sub3(cc, a, ac); sub3(p, a, ap);
    float d1 = dot3(ab, ap), d2 = dot3(ac, ap);
    if (d1 <= 0 && d2 <= 0) { memcpy(out, a, 12); return; }
    sub3(p, b, bp);
    float d3 = dot3(ab, bp), d4 = dot3(ac, bp);
    if (d3 >= 0 && d4 <= d3) { memcpy(out, b, 12); return; }
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) { float v = d1 / (d1 - d3); for (int i = 0; i < 3; i++) out[i] = a[i] + v * ab[i]; return; }
    sub3(p, cc, cp);
    float d5 = dot3(ab, cp), d6 = dot3(ac, cp);
    if (d6 >= 0 && d5 <= d6) { memcpy(out, cc, 12); return; }
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) { float w = d2 / (d2 - d6); for (int i = 0; i < 3; i++) out[i] = a[i] + w * ac[i]; return; }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        for (int i = 0; i < 3; i++) out[i] = b[i] + w * (cc[i] - b[i]); return;
    }
    float den = 1.0f / (va + vb + vc), v = vb * den, w = vc * den;
    for (int i = 0; i < 3; i++) out[i] = a[i] + ab[i] * v + ac[i] * w;
}

uint32_t nr_coll_last_mat;
int nr_coll_sphere(nr_coll *c, const float p[3], float r, float out_push[3], float out_n[3])
{
    out_push[0] = out_push[1] = out_push[2] = 0;
    out_n[0] = out_n[1] = out_n[2] = 0;
    if (!c->ntri && !c->ndyn) return 0;
    int hits = 0;
    float deepest = 0;
    for (uint32_t k = 0; k < c->ndyn; k++) {
        const nr_ctri *T = &c->dyn[k];
        float q[3], d[3];
        closest_on_tri(p, T->v[0], T->v[1], T->v[2], q);
        sub3(p, q, d);
        float l2 = dot3(d, d);
        if (l2 >= r * r) continue;
        float l = sqrtf(l2), nn[3];
        if (l > 1e-5f) { nn[0] = d[0] / l; nn[1] = d[1] / l; nn[2] = d[2] / l; }
        else memcpy(nn, T->n, 12);
        if (dot3(nn, T->n) < 0) continue;
        float depth = r - l;
        for (int i = 0; i < 3; i++) out_n[i] += nn[i] * depth;
        if (depth > deepest) { deepest = depth; nr_coll_last_mat = T->mat; }
        hits++;
    }
    if (c->ntri) next_stamp(c);
    if (c->ntri) FOR_CELLS(c, p[0] - r, p[1] - r, p[0] + r, p[1] + r, {
        float q[3], d[3];
        closest_on_tri(p, T->v[0], T->v[1], T->v[2], q);
        sub3(p, q, d);
        float l2 = dot3(d, d);
        if (l2 >= r * r) continue;
        float l = sqrtf(l2), nn[3];
        if (l > 1e-5f) { nn[0] = d[0] / l; nn[1] = d[1] / l; nn[2] = d[2] / l; }
        else memcpy(nn, T->n, 12);
        if (dot3(nn, T->n) < 0) continue;                 /* behind the surface: ignore (one-sided) */
        float depth = r - l;
        for (int i = 0; i < 3; i++) out_n[i] += nn[i] * depth;
        if (depth > deepest) { deepest = depth; nr_coll_last_mat = T->mat; }
        hits++;
    });
    if (hits) {
        float l = sqrtf(dot3(out_n, out_n));
        if (l > 1e-6f) for (int i = 0; i < 3; i++) out_n[i] /= l;
        for (int i = 0; i < 3; i++) out_push[i] = out_n[i] * deepest;
    }
    return hits;
}

void nr_coll_gather(nr_coll *c, const float lo[3], const float hi[3], nr_coll_near *nb)
{
    nb->n = 0;
    memcpy(nb->lo, lo, 12); memcpy(nb->hi, hi, 12);
    if (!c->ntri) return;
    next_stamp(c);
    FOR_CELLS(c, lo[0], lo[1], hi[0], hi[1], {
        float mn, mx; int out = 0;
        for (int a = 0; a < 3 && !out; a++) {
            mn = fminf(T->v[0][a], fminf(T->v[1][a], T->v[2][a])); mx = fmaxf(T->v[0][a], fmaxf(T->v[1][a], T->v[2][a]));
            if (mx < lo[a] || mn > hi[a]) out = 1;
        }
        if (out) continue;
        if (nb->n == nb->cap) {
            uint32_t nc = nb->cap ? nb->cap * 2 : 256;
            uint32_t *t = realloc(nb->tri, nc * 4); if (!t) return;
            nb->tri = t; nb->cap = nc;
        }
        nb->tri[nb->n++] = ti;
    });
}

void nr_coll_near_free(nr_coll_near *nb) { free(nb->tri); memset(nb, 0, sizeof *nb); }

int nr_coll_ray_near(const nr_coll *c, const nr_coll_near *nb, const float o[3], const float d[3], float maxd, float *dist, float n[3], uint32_t *mat)
{
    float best = maxd; int hit = 0;
    for (uint32_t k = 0; k < nb->n + c->ndyn; k++) {
        const nr_ctri *T = k < nb->n ? &c->tris[nb->tri[k]] : &c->dyn[k - nb->n];
        if (k < nb->n && tri_off(c, T)) continue;
        if (ray_tri(T, o, d, &best)) { hit = 1; memcpy(n, T->n, 12); if (mat) *mat = T->mat; }
    }
    if (hit) *dist = best;
    return hit;
}

int nr_coll_sphere_near(const nr_coll *c, const nr_coll_near *nb, const float p[3], float r, float out_push[3], float out_n[3], uint32_t *mat)
{
    out_push[0] = out_push[1] = out_push[2] = 0;
    out_n[0] = out_n[1] = out_n[2] = 0;
    int hits = 0; float deepest = 0;
    for (uint32_t k = 0; k < nb->n + c->ndyn; k++) {
        const nr_ctri *T = k < nb->n ? &c->tris[nb->tri[k]] : &c->dyn[k - nb->n];
        if (k < nb->n && tri_off(c, T)) continue;
        float pd = dot3(T->n, p) - dot3(T->n, T->v[0]);   /* distance to the triangle's plane */
        if (pd >= r || pd <= -r) continue;
        float q[3], d[3];
        closest_on_tri(p, T->v[0], T->v[1], T->v[2], q);
        sub3(p, q, d);
        float l2 = dot3(d, d);
        if (l2 >= r * r) continue;
        float l = sqrtf(l2), nn[3];
        if (l > 1e-5f) { nn[0] = d[0] / l; nn[1] = d[1] / l; nn[2] = d[2] / l; }
        else memcpy(nn, T->n, 12);
        if (dot3(nn, T->n) < 0) continue;
        float depth = r - l;
        for (int i = 0; i < 3; i++) out_n[i] += nn[i] * depth;
        if (depth > deepest) { deepest = depth; if (mat) *mat = T->mat; }
        hits++;
    }
    if (hits) {
        float l = sqrtf(dot3(out_n, out_n));
        if (l > 1e-6f) for (int i = 0; i < 3; i++) out_n[i] /= l;
        for (int i = 0; i < 3; i++) out_push[i] = out_n[i] * deepest;
    }
    return hits;
}
