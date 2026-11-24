#include "nr_coll.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void sub_build(nr_coll *c);
/* floorf(v) and ceilf(v) kept to 0..hi, without the library calls (the Vita's CPU has no rounding instruction: six calls
 * for each of a level's cell entries, up to 190000 of them, when it loads) - the same numbers for any v, NaN too (0) */
static inline int qfloor(float v, int hi)
{
    if (!(v > 0)) return 0;                              /* (floor of 0 or less: 0 or less, kept to 0) */
    if (v >= (float)hi) return hi;
    return (int)v;                                       /* (above 0: the whole part is the floor) */
}
static inline int qceil(float v, int hi)
{
    if (!(v > 0)) return 0;
    if (v > (float)(hi - 1)) return hi;                  /* (ceil(v) >= hi) */
    const int t = (int)v;
    return (float)t < v ? t + 1 : t;
}
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
    /* the cell entries' boxes (see nr_coll.h) */
    float zmax = -1e30f;
    for (uint32_t i = 0; i < c->ntri; i++) for (int k = 0; k < 3; k++) if (c->tris[i].v[k][2] > zmax) zmax = c->tris[i].v[k][2];
    c->zlo = c->ntri ? c->minz : 0; c->zq = zmax > c->zlo ? 65535.0f / (zmax - c->zlo) : 1;
    c->refbb = nref ? malloc(sizeof *c->refbb * nref) : NULL;
    if (c->refbb) {
        const float xs = 255.0f / c->cell;
        for (uint32_t cy = 0; cy < c->ny; cy++) for (uint32_t cx = 0; cx < c->nx; cx++) {
            uint32_t cell = cy * c->nx + cx; float x0 = c->minx + cx * c->cell, y0 = c->miny + cy * c->cell;
            for (uint32_t k = c->start[cell]; k < c->start[cell + 1]; k++) {
                const nr_ctri *T = &c->tris[c->idx[k] < c->ntri ? c->idx[k] : 0];
                float mn[3], mx[3];
                for (int a = 0; a < 3; a++) { mn[a] = fminf(T->v[0][a], fminf(T->v[1][a], T->v[2][a])); mx[a] = fmaxf(T->v[0][a], fmaxf(T->v[1][a], T->v[2][a])); }
                struct nr_cref *r = &c->refbb[k];
                /* (0.05 more each way: the float rounding of these and of the queries' own numbers) */
                r->x0 = (uint8_t)qfloor((mn[0] - x0) * xs - 0.05f, 255); r->x1 = (uint8_t)qceil((mx[0] - x0) * xs + 0.05f, 255);
                r->y0 = (uint8_t)qfloor((mn[1] - y0) * xs - 0.05f, 255); r->y1 = (uint8_t)qceil((mx[1] - y0) * xs + 0.05f, 255);
                r->z0 = (uint16_t)qfloor((mn[2] - c->zlo) * c->zq - 0.05f, 65535); r->z1 = (uint16_t)qceil((mx[2] - c->zlo) * c->zq + 0.05f, 65535);
            }
        }
        sub_build(c);
#ifdef NR_PC
        if (getenv("NOSUB") && c->sub) { free(c->sub); c->sub = NULL; }   /* (the test machine: every cell whole) */
#endif
    }
    return 0;
}
/* floor() for the queries' cells and parts without the library call (the Vita's CPU has no rounding instruction): fl_lo
 * is max(floor(v), 0) - for the first cell or part, clamped at 0 anyway - and fl_hi is floor(v) or -1 below 0 (the
 * last cell: below 0, none) - both the same as floorf there, for any v short of a billion */
static inline int fl_lo(float v) { return !(v > 0) ? 0 : v >= 1e9f ? 1000000000 : (int)v; }
static inline int fl_hi(float v) { return v < 0 ? -1 : v >= 1e9f ? 1000000000 : (int)v; }
/* the query box [lo, hi] in a cell's terms, for the entries' boxes: a cell entry whose box misses it is no contact.
 * The entries' bounds are whole numbers, so the test is done on whole numbers too: for a whole a and any x, a < x is
 * a < ceil(x) and a > x is a > floor(x) - the same entries pass as with the float compares, for a fraction of the work
 * on the Vita's CPU (each float compare there waits for the flags to come over from the FPU) */
typedef struct { float x0, x1, y0, y1, z0, z1; int ix0, ix1, iy0, iy1, iz0, iz1; } cref_q;
static inline int cq_lo(float x)              /* ceil(x) in -1..70000 (NaN: no miss, as the floats) */
{
    if (x != x || x <= -1.0f) return -1;
    if (x >= 70000.0f) return 70000;
    int t = (int)x; return (float)t < x ? t + 1 : t;
}
static inline int cq_hi(float x)              /* floor(x) in -1..70000 */
{
    if (x != x) return 70000;
    if (x <= -1.0f) return -1;
    if (x >= 70000.0f) return 70000;
    int t = (int)x; return (float)t > x ? t - 1 : t;
}
static inline void cref_query(const nr_coll *c, uint32_t cx, uint32_t cy, const float lo[3], const float hi[3], cref_q *q)
{
    const float xs = 255.0f / c->cell, ox = c->minx + cx * c->cell, oy = c->miny + cy * c->cell;
    q->x0 = (lo[0] - ox) * xs; q->x1 = (hi[0] - ox) * xs; q->y0 = (lo[1] - oy) * xs; q->y1 = (hi[1] - oy) * xs;
    q->z0 = (lo[2] - c->zlo) * c->zq; q->z1 = (hi[2] - c->zlo) * c->zq;
    q->ix0 = cq_lo(q->x0); q->ix1 = cq_hi(q->x1); q->iy0 = cq_lo(q->y0); q->iy1 = cq_hi(q->y1); q->iz0 = cq_lo(q->z0); q->iz1 = cq_hi(q->z1);
}
static inline int cref_miss(const struct nr_cref *r, const cref_q *q)
{
    return r->x1 < q->ix0 || r->x0 > q->ix1 || r->y1 < q->iy0 || r->y0 > q->iy1 || r->z1 < q->iz0 || r->z0 > q->iz1;
}

static int cmp_u32(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
#define SUB_MIN 96                        /* (cells with fewer entries are looked at whole; 96: the fewest entries looked at) */
#define SUB_NONE 0xffffffffu
static int sub_z(const struct nr_csub *s, float zq)
{
    int k = fl_lo((zq - s->z0) * s->zinv);
    return k < 0 ? 0 : k >= s->nz ? s->nz - 1 : k;
}
static int sub_xy(float v) { int k = fl_lo(v) >> 6; return k > 3 ? 3 : k; }   /* (0..255 in 4 parts) */
static void sub_parts(const nr_coll *c, const struct nr_csub *S, const struct nr_cref *r, int lo[3], int hi[3])
{
    (void)c;
    lo[0] = r->x0 >> 6; hi[0] = r->x1 >> 6; lo[1] = r->y0 >> 6; hi[1] = r->y1 >> 6; lo[2] = sub_z(S, r->z0); hi[2] = sub_z(S, r->z1);
}
static void sub_build(nr_coll *c)
{
    uint32_t ncell = c->nx * c->ny, nst = 0, maxn = 0;
    if (!ncell || !c->refbb) return;
    c->sub = malloc(sizeof *c->sub * ncell);
    if (!c->sub) return;
    for (uint32_t cell = 0; cell < ncell; cell++) {      /* the dense cells, their heights in parts of about 8 units */
        uint32_t s0 = c->start[cell], n = c->start[cell + 1] - s0;
        struct nr_csub *S = &c->sub[cell];
        if (n < SUB_MIN || n > 65535) { S->first = SUB_NONE; continue; }
        uint16_t z0 = 65535, z1 = 0;
        for (uint32_t k = s0; k < s0 + n; k++) { if (c->refbb[k].z0 < z0) z0 = c->refbb[k].z0; if (c->refbb[k].z1 > z1) z1 = c->refbb[k].z1; }
        float zr = (float)(z1 - z0) + 1, bin = 8.0f * c->zq;
        int nz = (int)ceilf(zr / (bin > 1 ? bin : 1)); nz = nz < 1 ? 1 : nz > 16 ? 16 : nz;
        S->nz = (uint16_t)nz; S->nbig = 0; S->z0 = z0; S->zinv = nz / zr; S->first = nst; c->nsubcell++;
        nst += 16u * nz + 2;
        if (n > maxn) maxn = n;
    }
    if (!nst) { free(c->sub); c->sub = NULL; return; }
    /* (a query's list before its repeats are dropped: an entry is in up to 8 parts) */
    uint32_t *cur = calloc(nst, 4); c->subst = calloc(nst, 4); c->ktmp = malloc(4ull * 8 * maxn + 4); c->ktmpcap = 8 * maxn;
    c->kbits = calloc((maxn + 63) / 64 + 1, 8);
    if (!cur || !c->subst || !c->ktmp || !c->kbits) goto fail;
    for (int pass = 0; pass < 2; pass++) {               /* count, then fill */
        if (pass) {
            uint32_t tot = 0;
            for (uint32_t cell = 0; cell < ncell; cell++) {
                const struct nr_csub *S = &c->sub[cell];
                if (S->first == SUB_NONE) continue;
                uint32_t np = 16u * S->nz, *st = &c->subst[S->first];
                for (uint32_t p = 0; p <= np; p++) { uint32_t v = st[p]; st[p] = tot; tot += v; }
                st[np + 1] = tot;
            }
            memcpy(cur, c->subst, 4ull * nst);
            c->subidx = malloc(2ull * tot + 2); c->nsubidx = tot;
            if (!c->subidx) goto fail;
        }
        for (uint32_t cell = 0; cell < ncell; cell++) {
            const struct nr_csub *S = &c->sub[cell];
            if (S->first == SUB_NONE) continue;
            uint32_t s0 = c->start[cell], n = c->start[cell + 1] - s0, np = 16u * S->nz, *ct = pass ? &cur[S->first] : &c->subst[S->first];
            for (uint32_t j = 0; j < n; j++) {
                int lo[3], hi[3]; sub_parts(c, S, &c->refbb[s0 + j], lo, hi);
                int parts = (hi[0] - lo[0] + 1) * (hi[1] - lo[1] + 1) * (hi[2] - lo[2] + 1);
                if (parts > 8) { if (pass) c->subidx[ct[np]++] = (uint16_t)j; else ct[np]++; continue; }
                for (int z = lo[2]; z <= hi[2]; z++) for (int y = lo[1]; y <= hi[1]; y++) for (int x = lo[0]; x <= hi[0]; x++) {
                    uint32_t p = ((uint32_t)z * 4 + y) * 4 + x;
                    if (pass) c->subidx[ct[p]++] = (uint16_t)j; else ct[p]++;
                }
            }
        }
    }
    free(cur);
    return;
fail:
    free(cur); free(c->sub); free(c->subst); free(c->subidx); free(c->ktmp); free(c->kbits);
    c->sub = NULL; c->subst = NULL; c->subidx = NULL; c->ktmp = NULL; c->ktmpcap = 0; c->kbits = NULL;
}
/* the entries of a dense cell whose boxes meet the query's (q, in the cell's terms), in the cell's order: their places
 * in the cell, in c->ktmp -> how many */
static uint32_t sub_cands(nr_coll *c, uint32_t cell, const cref_q *q)
{
    CST0;
    const struct nr_csub *S = &c->sub[cell];
    const uint32_t s0 = c->start[cell], *st = &c->subst[S->first], np = 16u * S->nz;
    const struct nr_cref *rb = &c->refbb[s0];
    int x0 = sub_xy(q->x0), x1 = sub_xy(q->x1), y0 = sub_xy(q->y0), y1 = sub_xy(q->y1), z0 = sub_z(S, q->z0), z1 = sub_z(S, q->z1);
    uint32_t n = 0, *out = c->ktmp;
    for (int z = z0; z <= z1; z++) for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        uint32_t p = ((uint32_t)z * 4 + y) * 4 + x;
        for (uint32_t i = st[p]; i < st[p + 1]; i++) { uint16_t j = c->subidx[i]; if (!cref_miss(&rb[j], q)) out[n++] = j; }
    }
    for (uint32_t i = st[np]; i < st[np + 1]; i++) { uint16_t j = c->subidx[i]; if (!cref_miss(&rb[j], q)) out[n++] = j; }
    /* the cell's order, each once (an entry over several parts is in each; the many-part ones come last) */
    if (n > 24) {                         /* (a long list: by a bit for each - the warp_speed flight's 4000 a box, where
                                           *  the sort took half the near box's gathering) */
        uint64_t *bm = c->kbits; uint32_t w0 = 0xffffffffu, w1 = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t j = out[i], w = j >> 6;
            bm[w] |= 1ull << (j & 63);
            if (w < w0) w0 = w; if (w > w1) w1 = w;
        }
        uint32_t m = 0;
        for (uint32_t w = w0; w <= w1; w++) {               /* (in order, each once, the bits cleared again) */
            uint64_t b = bm[w]; bm[w] = 0;
            while (b) { out[m++] = (w << 6) + (uint32_t)__builtin_ctzll(b); b &= b - 1; }
        }
        n = m;
    } else if (n > 1) {
        for (uint32_t i = 1; i < n; i++) { uint32_t v = out[i], k = i; while (k > 0 && out[k - 1] > v) { out[k] = out[k - 1]; k--; } out[k] = v; }
        uint32_t m = 1;
        for (uint32_t i = 1; i < n; i++) if (out[i] != out[m - 1]) out[m++] = out[i];
        n = m;
    }
    CST1(ts); CSA(sn, n);
    return n;
}


void nr_coll_free(nr_coll *c)
{
    free(c->tris); free(c->start); free(c->idx); free(c->stamp); free(c->refbb); free(c->dyn); free(c->dynbb);
    free(c->dobj); free(c->dmap); free(c->dcand); free(c->sub); free(c->subst); free(c->subidx); free(c->ktmp); free(c->kbits);
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

/* the moving triangles a query may need: the rest of the frame's reach first, if nr_objs left it for later */
#ifdef CSTAT
struct nr_cstat NRCS;
#endif
static uint32_t dyn_all(nr_coll *c)
{
    if (c->no_dyn) return 0;
    if (c->dyn_pending && c->dyn_more) { c->dyn_pending = 0; CSA(more, 1); c->dyn_more(c->dyn_user); }
    return c->ndyn;
}

/* ---- the moving objects' triangles (see nr_coll.h) */
void nr_cgrid_free(nr_cgrid *G) { free(G->start); free(G->idx); free(G->big); memset(G, 0, sizeof *G); }
static int cg_cell(const nr_cgrid *G, int a, float v)
{
    int k = fl_lo((v - G->lo[a]) * G->inv[a]);
    return k < 0 ? 0 : k >= G->g[a] ? G->g[a] - 1 : k;
}
void nr_cgrid_build(nr_cgrid *G, const float (*bb)[6], uint32_t n)
{
    memset(G, 0, sizeof *G);
    if (n < 64 || !bb) return;                          /* (a few boxes: their own boxes are quicker) */
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f }, ext[3];
    for (uint32_t t = 0; t < n; t++) for (int a = 0; a < 3; a++) { lo[a] = fminf(lo[a], bb[t][a]); hi[a] = fmaxf(hi[a], bb[t][a + 3]); }
    for (int a = 0; a < 3; a++) ext[a] = fmaxf(hi[a] - lo[a], 1e-3f);
    /* cells about as many as half the triangles, near cubes, at most 64 a side and 32768 in all */
    float h = cbrtf(ext[0] * ext[1] * ext[2] / (0.5f * (float)n));
    for (int tries = 0; tries < 8; tries++) {
        long tot = 1;
        for (int a = 0; a < 3; a++) { int g = (int)ceilf(ext[a] / h); G->g[a] = g < 1 ? 1 : g > 64 ? 64 : g; tot *= G->g[a]; }
        if (tot <= 32768) break;
        h *= 1.3f;
    }
    for (int a = 0; a < 3; a++) { G->lo[a] = lo[a]; G->inv[a] = (float)G->g[a] / ext[a]; }
    uint32_t ncell = (uint32_t)(G->g[0] * G->g[1] * G->g[2]), *cur = calloc(ncell, 4);
    G->start = malloc(4ull * (ncell + 1));
    if (!G->start || !cur) { free(cur); nr_cgrid_free(G); return; }
    for (int pass = 0; pass < 2; pass++) {               /* count, then fill (each cell's list by triangle) */
        if (pass) {
            G->start[0] = 0;
            for (uint32_t k = 0; k < ncell; k++) G->start[k + 1] = G->start[k] + cur[k];
            memcpy(cur, G->start, 4ull * ncell);
            G->idx = malloc(4ull * G->start[ncell] + 4); G->big = malloc(4ull * G->nbig + 4);
            if (!G->idx || !G->big) { free(cur); nr_cgrid_free(G); return; }
            G->nbig = 0;
        }
        for (uint32_t t = 0; t < n; t++) {
            int c0[3], c1[3], cells = 1;
            for (int a = 0; a < 3; a++) { c0[a] = cg_cell(G, a, bb[t][a]); c1[a] = cg_cell(G, a, bb[t][a + 3]); cells *= c1[a] - c0[a] + 1; }
            if (cells > 32) { if (pass) G->big[G->nbig] = t; G->nbig++; continue; }
            for (int z = c0[2]; z <= c1[2]; z++) for (int y = c0[1]; y <= c1[1]; y++) for (int x = c0[0]; x <= c1[0]; x++) {
                uint32_t cell = ((uint32_t)z * G->g[1] + y) * G->g[0] + x;
                if (pass) G->idx[cur[cell]++] = t; else cur[cell]++;
            }
        }
    }
    free(cur);
}

#ifdef DYNCMP
static void od_add(nr_coll *c, const nr_cdobj *d); static uint32_t OND;
static void dc_ray(nr_coll *c, const nr_coll_near *nb, int use_nb, const float o[3], const float d[3], float maxd, const float lo[3], const float hi[3], const char *what);
static void dc_sph(nr_coll *c, const nr_coll_near *nb, int use_nb, const float p[3], float r, int pdtest, const char *what);
#endif
void nr_coll_dyn_begin(nr_coll *c, const float at[3], float near_half, float reach_half)
{
#ifdef DYNCMP
    OND = 0;
#endif
    c->ndyn = 0; c->ndobj = 0; c->ndmap = 0; c->dyn_frame++;
    memcpy(c->dyn_at, at, 12); c->dyn_rn = near_half; c->dyn_rf = reach_half;
}
void nr_coll_dyn_add(nr_coll *c, const nr_cdobj *d, float mr)
{
    if (c->ndobj == c->dobjcap) {
        uint32_t nc = c->dobjcap ? c->dobjcap * 2 : 64;
        void *t = realloc(c->dobj, sizeof *c->dobj * nc); if (!t) return;
        c->dobj = t; c->dobjcap = nc;
    }
    if (c->ndmap + d->n > c->dmapcap) {
        uint32_t nc = c->dmapcap ? c->dmapcap : 4096;
        while (nc < c->ndmap + d->n) nc *= 2;
        void *t = realloc(c->dmap, sizeof *c->dmap * nc); if (!t) return;
        memset((struct nr_cdmap *)t + c->dmapcap, 0, sizeof *c->dmap * (nc - c->dmapcap));
        c->dmap = t; c->dmapcap = nc;
    }
    nr_cdobj *o = &c->dobj[c->ndobj++];
    *o = *d; o->map = c->ndmap; c->ndmap += d->n;
    /* its world box: the sphere's, with room for the rounding of the moved points (well over it) */
    float big = fmaxf(fabsf(d->pos[0]), fmaxf(fabsf(d->pos[1]), fabsf(d->pos[2]))) + mr;
    float m = mr + 0.01f + 4e-6f * big;
    for (int a = 0; a < 3; a++) { o->wlo[a] = d->pos[a] - m; o->whi[a] = d->pos[a] + m; }
    /* world to own space: the inverse of R * diag(scale) */
    float M[3][3];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) M[i][j] = d->R[i][j] * d->scale[j];
    double det = (double)M[0][0] * ((double)M[1][1] * M[2][2] - (double)M[1][2] * M[2][1]) - (double)M[0][1] * ((double)M[1][0] * M[2][2] - (double)M[1][2] * M[2][0])
               + (double)M[0][2] * ((double)M[1][0] * M[2][1] - (double)M[1][1] * M[2][0]);
    o->has_inv = fabs(det) > 1e-12;
    if (o->has_inv) {
        double k = 1.0 / det;
        o->inv[0][0] = (float)(((double)M[1][1] * M[2][2] - (double)M[1][2] * M[2][1]) * k); o->inv[0][1] = (float)(((double)M[0][2] * M[2][1] - (double)M[0][1] * M[2][2]) * k);
        o->inv[0][2] = (float)(((double)M[0][1] * M[1][2] - (double)M[0][2] * M[1][1]) * k); o->inv[1][0] = (float)(((double)M[1][2] * M[2][0] - (double)M[1][0] * M[2][2]) * k);
        o->inv[1][1] = (float)(((double)M[0][0] * M[2][2] - (double)M[0][2] * M[2][0]) * k); o->inv[1][2] = (float)(((double)M[0][2] * M[1][0] - (double)M[0][0] * M[1][2]) * k);
        o->inv[2][0] = (float)(((double)M[1][0] * M[2][1] - (double)M[1][1] * M[2][0]) * k); o->inv[2][1] = (float)(((double)M[0][1] * M[2][0] - (double)M[0][0] * M[2][1]) * k);
        o->inv[2][2] = (float)(((double)M[0][0] * M[1][1] - (double)M[0][1] * M[1][0]) * k);
    }
#ifdef DYNCMP
    od_add(c, o);
#endif
}

/* own space to world, as nr_objs moves its objects' points (the same sums in the same order: the same numbers) */
static void dyn_to_world(const nr_cdobj *d, const float *l, float *out)
{
    float s[3] = { l[0] * d->scale[0], l[1] * d->scale[1], l[2] * d->scale[2] };
    float w[3] = { d->R[0][0] * s[0] + d->R[0][1] * s[1] + d->R[0][2] * s[2], d->R[1][0] * s[0] + d->R[1][1] * s[1] + d->R[1][2] * s[2],
                   d->R[2][0] * s[0] + d->R[2][1] * s[1] + d->R[2][2] * s[2] };
    for (int k = 0; k < 3; k++) out[k] = d->pos[k] + w[k];
}
static float dlen3(const float *a) { return sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }


#ifdef DYNCMP
/* (the test machine: the old way - every listed triangle of the pass moved at once - beside the new, compared) */
#include <stdio.h>
static nr_ctri *OD; static float (*ODB)[6]; static uint32_t ODCAP; static unsigned long dc_n, dc_bad;
static void od_add(nr_coll *c, const nr_cdobj *d)
{
    for (uint32_t t = 0; t < d->n; t++) {
        const float *b0 = d->bb[t];
        if (b0[3] < d->lp[0] - d->rl[0] || b0[0] > d->lp[0] + d->rl[0] || b0[4] < d->lp[1] - d->rl[1] || b0[1] > d->lp[1] + d->rl[1] || b0[5] < d->lp[2] - d->rl[2] || b0[2] > d->lp[2] + d->rl[2]) continue;
        if (OND == ODCAP) { ODCAP = ODCAP ? ODCAP * 2 : 4096; OD = realloc(OD, sizeof *OD * ODCAP); ODB = realloc(ODB, sizeof *ODB * ODCAP); }
        nr_ctri *T = &OD[OND]; float *b = ODB[OND];
        for (int j = 0; j < 3; j++) dyn_to_world(d, &d->t[t][j * 3], T->v[d->flip ? 2 - j : j]);
        for (int a = 0; a < 3; a++) { b[a] = fminf(T->v[0][a], fminf(T->v[1][a], T->v[2][a])); b[a + 3] = fmaxf(T->v[0][a], fmaxf(T->v[1][a], T->v[2][a])); }
        const float *at = c->dyn_at; float reach = d->pass == 1 ? c->dyn_rn : c->dyn_rf, skip = c->dyn_rn;
        int out = 0, near = d->pass == 2;
        for (int j = 0; j < 3; j++) {
            if (b[j + 3] < at[j] - reach || b[j] > at[j] + reach) out = 1;
            if (b[j + 3] < at[j] - skip || b[j] > at[j] + skip) near = 0;
        }
        if (out || near) continue;
        float e1[3] = { T->v[1][0] - T->v[0][0], T->v[1][1] - T->v[0][1], T->v[1][2] - T->v[0][2] };
        float e2[3] = { T->v[2][0] - T->v[0][0], T->v[2][1] - T->v[0][1], T->v[2][2] - T->v[0][2] };
        float r[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        memcpy(T->n, r, 12);
        if (dlen3(T->n) < 1e-9f) continue;
        { float l = dlen3(T->n); if (l > 1e-9f) { T->n[0] /= l; T->n[1] /= l; T->n[2] /= l; } }
        T->mat = d->mat; OND++;
    }
}
#endif
#define DYN_NONE 0xffffffffu
#define DYN_UNSET 0xfffffffeu
/* triangle t of listed object d to world space, if it is in its pass's share (as nr_objs took them: its box meets the
 * pass's cube - the second pass: not the near one, the first's - and it has an area) -> its slot in dyn */
static uint32_t dyn_move(nr_coll *c, const nr_cdobj *d, uint32_t t)
{
    if (c->ndyn == c->dyncap) {
        uint32_t nc = c->dyncap ? c->dyncap * 2 : 1024;
        void *p = realloc(c->dyn, sizeof *c->dyn * nc); if (!p) return DYN_NONE;
        c->dyn = p;
        void *q = realloc(c->dynbb, sizeof *c->dynbb * nc); if (!q) return DYN_NONE;
        c->dynbb = q; c->dyncap = nc;
    }
    nr_ctri *T = &c->dyn[c->ndyn]; float *b = c->dynbb[c->ndyn];
    for (int j = 0; j < 3; j++) dyn_to_world(d, &d->t[t][j * 3], T->v[d->flip ? 2 - j : j]);
    for (int a = 0; a < 3; a++) {
        b[a] = fminf(T->v[0][a], fminf(T->v[1][a], T->v[2][a]));
        b[a + 3] = fmaxf(T->v[0][a], fmaxf(T->v[1][a], T->v[2][a]));
    }
    const float *at = c->dyn_at; float reach = d->pass == 1 ? c->dyn_rn : c->dyn_rf, skip = c->dyn_rn;
    int out = 0, near = d->pass == 2;
    for (int j = 0; j < 3; j++) {
        if (b[j + 3] < at[j] - reach || b[j] > at[j] + reach) out = 1;
        if (b[j + 3] < at[j] - skip || b[j] > at[j] + skip) near = 0;
    }
    if (out || near) return DYN_NONE;
    CSA(xf1, 1);
    float e1[3] = { T->v[1][0] - T->v[0][0], T->v[1][1] - T->v[0][1], T->v[1][2] - T->v[0][2] };
    float e2[3] = { T->v[2][0] - T->v[0][0], T->v[2][1] - T->v[0][1], T->v[2][2] - T->v[0][2] };
    float r[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
    memcpy(T->n, r, 12);
    if (dlen3(T->n) < 1e-9f) return DYN_NONE;
    { float l = dlen3(T->n); if (l > 1e-9f) { T->n[0] /= l; T->n[1] /= l; T->n[2] /= l; } }
    T->mat = d->mat;
    return c->ndyn++;
}

static void dcand_push(nr_coll *c, uint32_t v)
{
    if (c->ndcand == c->dcandcap) {
        uint32_t nc = c->dcandcap ? c->dcandcap * 2 : 256;
        void *t = realloc(c->dcand, 4ull * nc); if (!t) return;
        c->dcand = t; c->dcandcap = nc;
    }
    c->dcand[c->ndcand++] = v;
}

/* the moving triangles a query whose box is [lo, hi] may meet: their slots in dyn, in c->dcand, in the order nr_objs
 * listed them (the near pass, then the rest; by object, then triangle). The same as testing each listed triangle's
 * world box against [lo, hi] (and nb's, when given), less those that cannot touch the query: their own boxes miss
 * the query's box taken to the object's space (with room for the rounding) */
static uint32_t dyn_query(nr_coll *c, const float lo[3], const float hi[3], const nr_coll_near *nb)
{
    c->ndcand = 0;
    if (c->no_dyn || !c->ndobj) return 0;
    c->dyn_q++;
    float qc[3], qh[3], big = 0;
    for (int a = 0; a < 3; a++) { qc[a] = (lo[a] + hi[a]) * 0.5f; qh[a] = (hi[a] - lo[a]) * 0.5f; big = fmaxf(big, fabsf(qc[a]) + qh[a]); }
    for (uint32_t k = 0; k < c->ndobj; k++) {
        const nr_cdobj *d = &c->dobj[k];
#ifdef NR_PC
        static int dynref_ = -1; if (dynref_ < 0) dynref_ = getenv("DYNREF") != NULL;
        if (!dynref_)
#endif
        if (d->whi[0] < lo[0] || d->wlo[0] > hi[0] || d->whi[1] < lo[1] || d->wlo[1] > hi[1] || d->whi[2] < lo[2] || d->wlo[2] > hi[2]) continue;
        float llo[3], lhi[3];
        if (d->has_inv) {
            float m = 0.01f + 4e-6f * (big + fmaxf(fabsf(d->pos[0]), fmaxf(fabsf(d->pos[1]), fabsf(d->pos[2])))), w[3];
            for (int a = 0; a < 3; a++) w[a] = qc[a] - d->pos[a];
            for (int a = 0; a < 3; a++) {
                float cc = d->inv[a][0] * w[0] + d->inv[a][1] * w[1] + d->inv[a][2] * w[2];
                float hh = fabsf(d->inv[a][0]) * (qh[0] + m) + fabsf(d->inv[a][1]) * (qh[1] + m) + fabsf(d->inv[a][2]) * (qh[2] + m);
                hh += 1e-4f + 1e-5f * fabsf(cc);
                llo[a] = cc - hh; lhi[a] = cc + hh;
            }
        } else for (int a = 0; a < 3; a++) { llo[a] = -1e30f; lhi[a] = 1e30f; }
        uint32_t first = c->ndcand;
#define DYN_TRY(t_) do { \
            uint32_t tt_ = (t_); struct nr_cdmap *e = &c->dmap[d->map + tt_]; \
            if (e->frame != c->dyn_frame) { e->frame = c->dyn_frame; e->q = 0; e->slot = DYN_UNSET; } \
            if (e->q == c->dyn_q) break; \
            e->q = c->dyn_q; \
            const float *b = d->bb[tt_]; \
            if (b[3] < d->lp[0] - d->rl[0] || b[0] > d->lp[0] + d->rl[0] || b[4] < d->lp[1] - d->rl[1] || b[1] > d->lp[1] + d->rl[1] || b[5] < d->lp[2] - d->rl[2] || b[2] > d->lp[2] + d->rl[2]) break; \
            if (b[3] < llo[0] || b[0] > lhi[0] || b[4] < llo[1] || b[1] > lhi[1] || b[5] < llo[2] || b[2] > lhi[2]) break; \
            if (e->slot == DYN_UNSET) e->slot = dyn_move(c, d, tt_); \
            if (e->slot == DYN_NONE) break; \
            const float *wb = c->dynbb[e->slot]; \
            if (nb && (wb[3] < nb->lo[0] || wb[0] > nb->hi[0] || wb[4] < nb->lo[1] || wb[1] > nb->hi[1] || wb[5] < nb->lo[2] || wb[2] > nb->hi[2])) break; \
            if (wb[3] < lo[0] || wb[0] > hi[0] || wb[4] < lo[1] || wb[1] > hi[1] || wb[5] < lo[2] || wb[2] > hi[2]) break; \
            dcand_push(c, tt_); \
        } while (0)
#ifdef NR_PC
        static int dynref = -1; if (dynref < 0) dynref = getenv("DYNREF") ? atoi(getenv("DYNREF")) : 0;
        if (dynref) { for (int a = 0; a < 3; a++) { llo[a] = -1e30f; lhi[a] = 1e30f; } }
        if (dynref) { for (uint32_t t = 0; t < d->n; t++) DYN_TRY(t); } else
#endif
        if (d->g.g[0] && d->has_inv) {
            const nr_cgrid *G = &d->g; int c0[3], c1[3]; uint32_t ncells = 1;
            for (int a = 0; a < 3; a++) {
                float l = fmaxf(llo[a], d->lp[a] - d->rl[a]), h = fminf(lhi[a], d->lp[a] + d->rl[a]);
                if (l > h) goto next_obj;
                c0[a] = cg_cell(G, a, l); c1[a] = cg_cell(G, a, h); ncells *= (uint32_t)(c1[a] - c0[a] + 1);
            }
            if (ncells * 2 > d->n) { for (uint32_t t = 0; t < d->n; t++) DYN_TRY(t); goto sorted; }   /* (most of it: whole, in order) */
            for (int z = c0[2]; z <= c1[2]; z++) for (int y = c0[1]; y <= c1[1]; y++) for (int x = c0[0]; x <= c1[0]; x++) {
                uint32_t cell = ((uint32_t)z * G->g[1] + y) * G->g[0] + x;
                for (uint32_t i = G->start[cell]; i < G->start[cell + 1]; i++) DYN_TRY(G->idx[i]);
            }
            for (uint32_t i = 0; i < G->nbig; i++) DYN_TRY(G->big[i]);
        } else for (uint32_t t = 0; t < d->n; t++) DYN_TRY(t);
#undef DYN_TRY
        {   uint32_t n = c->ndcand - first, *v = c->dcand + first;      /* by triangle, as listed */
            if (n > 32) qsort(v, n, 4, cmp_u32);
            else for (uint32_t i = 1; i < n; i++) { uint32_t x = v[i], j = i; while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; j--; } v[j] = x; }
        }
    sorted:
        {   uint32_t n = c->ndcand - first, *v = c->dcand + first;
            for (uint32_t i = 0; i < n; i++) v[i] = c->dmap[d->map + v[i]].slot;
        }
    next_obj:;
    }
    return c->ndcand;
}

uint32_t nr_coll_dyn_list(nr_coll *c, const float lo[3], const float hi[3]) { return dyn_query(c, lo, hi, NULL); }

static void next_stamp(nr_coll *c)
{
    if (++c->stamp_id == 0) { memset(c->stamp, 0, 4 * (c->ntri + 1)); c->stamp_id = 1; }
}

/* calls fn for each triangle in the cells overlapping [x0,x1]x[y0,y1] (each once) */
#define FOR_CELLS(c, x0, y0, x1, y1, ...) do { \
    int cx0 = fl_lo(((x0) - (c)->minx) / (c)->cell), cx1 = fl_hi(((x1) - (c)->minx) / (c)->cell); \
    int cy0 = fl_lo(((y0) - (c)->miny) / (c)->cell), cy1 = fl_hi(((y1) - (c)->miny) / (c)->cell); \
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

/* the same for the triangles whose cell entry's box meets [lo, hi] (each once) */
#define FOR_CELLS_BOX(c, lo, hi, ...) do { \
    int cx0 = fl_lo(((lo)[0] - (c)->minx) / (c)->cell), cx1 = fl_hi(((hi)[0] - (c)->minx) / (c)->cell); \
    int cy0 = fl_lo(((lo)[1] - (c)->miny) / (c)->cell), cy1 = fl_hi(((hi)[1] - (c)->miny) / (c)->cell); \
    if (cx0 < 0) cx0 = 0; if (cy0 < 0) cy0 = 0; \
    if (cx1 >= (int)(c)->nx) cx1 = (int)(c)->nx - 1; if (cy1 >= (int)(c)->ny) cy1 = (int)(c)->ny - 1; \
    for (int cy = cy0; cy <= cy1; cy++) for (int cx = cx0; cx <= cx1; cx++) { \
        uint32_t cell = (uint32_t)cy * (c)->nx + cx; cref_q q_ = { 0 }; \
        if ((c)->refbb) cref_query((c), (uint32_t)cx, (uint32_t)cy, (lo), (hi), &q_); \
        const uint32_t s0_ = (c)->start[cell], *ks_ = NULL; uint32_t nk_ = (c)->start[cell + 1] - s0_; \
        if ((c)->sub && (c)->sub[cell].first != SUB_NONE) { nk_ = sub_cands((c), cell, &q_); ks_ = (c)->ktmp; } \
        for (uint32_t j_ = 0; j_ < nk_; j_++) { \
            uint32_t k = s0_ + (ks_ ? ks_[j_] : j_); \
            if (!ks_ && (c)->refbb && cref_miss(&(c)->refbb[k], &q_)) continue; \
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
    float dn = dot3(T->n, d);
    if (dn >= 0) return 0;                                /* only surfaces facing the ray */
    /* where the ray meets the triangle's plane: behind it, or past the nearest hit so far, is no hit (with room for
     * the rounding of the full test below, so the result is the same as without this) */
    float w[3]; sub3(T->v[0], o, w);
    float tp = dot3(T->n, w) / dn;
    if (tp < -1e-3f || tp > *best * 1.0001f + 1e-3f) return 0;
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
    float e[3] = { o[0] + d[0] * maxd, o[1] + d[1] * maxd, o[2] + d[2] * maxd };
    float rlo[3] = { fminf(o[0], e[0]), fminf(o[1], e[1]), fminf(o[2], e[2]) }, rhi[3] = { fmaxf(o[0], e[0]), fmaxf(o[1], e[1]), fmaxf(o[2], e[2]) };
    dyn_all(c);
    uint32_t nq = dyn_query(c, rlo, rhi, NULL);
#ifdef DYNCMP
    if (!c->no_dyn) dc_ray(c, NULL, 0, o, d, maxd, rlo, rhi, "ray");
#endif
    CSA(qf_ray, 1); CSA(cf, nq);
    for (uint32_t q = 0; q < nq; q++) {
        const nr_ctri *T = &c->dyn[c->dcand[q]];
        if (ray_tri(T, o, d, &best)) { hit = 1; memcpy(n, T->n, 12); if (mat) *mat = T->mat; }
    }
    if (!c->ntri) { if (hit) *dist = best; return hit; }
    next_stamp(c);
    /* the grid's cells the ray crosses, in the order it crosses them (not every cell of its box: a long slanting ray -
     * a laser beam - crossed hundreds), stopping once the nearest hit so far lies before the next cell. A triangle is
     * listed in every cell it overlaps, so a hit at a point is found in the cell of that point */
    float gx0 = c->minx, gy0 = c->miny, gx1 = c->minx + c->nx * c->cell, gy1 = c->miny + c->ny * c->cell;
    float t0 = 0, t1 = maxd;
    for (int ax = 0; ax < 2; ax++) {                       /* the part of the ray over the grid */
        float lo = ax ? gy0 : gx0, hi = ax ? gy1 : gx1, oo = o[ax], dd = d[ax];
        if (fabsf(dd) < 1e-12f) { if (oo < lo || oo > hi) t1 = -1; continue; }
        float ta = (lo - oo) / dd, tb = (hi - oo) / dd;
        if (ta > tb) { float x = ta; ta = tb; tb = x; }
        if (ta > t0) t0 = ta;
        if (tb < t1) t1 = tb;
    }
    /* the ray's box with room for the rounding of a hit's distance (wider: a box missed is no hit) */
    float qlo[3], qhi[3];
    for (int a = 0; a < 3; a++) { float m = 0.01f + 1e-5f * fmaxf(fabsf(rlo[a]), fabsf(rhi[a])); qlo[a] = rlo[a] - m; qhi[a] = rhi[a] + m; }
    if (t0 <= t1) {
        float px = o[0] + d[0] * t0, py = o[1] + d[1] * t0;
        int cx = fl_lo((px - c->minx) / c->cell), cy = fl_lo((py - c->miny) / c->cell);
        if (cx < 0) cx = 0; if (cy < 0) cy = 0;
        if (cx >= (int)c->nx) cx = (int)c->nx - 1; if (cy >= (int)c->ny) cy = (int)c->ny - 1;
        int sx = d[0] > 0 ? 1 : -1, sy = d[1] > 0 ? 1 : -1;
        float tmx = 1e30f, tmy = 1e30f, tdx = 1e30f, tdy = 1e30f;
        if (fabsf(d[0]) >= 1e-12f) { float bx = c->minx + (cx + (sx > 0)) * c->cell; tmx = (bx - o[0]) / d[0]; tdx = c->cell / fabsf(d[0]); }
        if (fabsf(d[1]) >= 1e-12f) { float by = c->miny + (cy + (sy > 0)) * c->cell; tmy = (by - o[1]) / d[1]; tdy = c->cell / fabsf(d[1]); }
        for (;;) {
            /* (the entries whose box misses the whole ray's: no hit wherever they are, taken in no cell) */
            uint32_t cell = (uint32_t)cy * c->nx + (uint32_t)cx; cref_q q_ = { 0 };
            if (c->refbb) cref_query(c, (uint32_t)cx, (uint32_t)cy, qlo, qhi, &q_);
            const uint32_t s0_ = c->start[cell], *ks_ = NULL; uint32_t nk_ = c->start[cell + 1] - s0_;
            if (c->sub && c->sub[cell].first != SUB_NONE) { nk_ = sub_cands(c, cell, &q_); ks_ = c->ktmp; }
            for (uint32_t j_ = 0; j_ < nk_; j_++) {
                uint32_t k = s0_ + (ks_ ? ks_[j_] : j_);
                if (!ks_ && c->refbb && cref_miss(&c->refbb[k], &q_)) continue;
                uint32_t ti = c->idx[k];
                if (c->stamp[ti] == c->stamp_id) continue;
                c->stamp[ti] = c->stamp_id;
                const nr_ctri *T = &c->tris[ti];
                if (tri_off(c, T)) continue;
                if (ray_tri(T, o, d, &best)) { hit = 1; memcpy(n, T->n, 12); if (mat) *mat = T->mat; }
            }
            float tn = tmx < tmy ? tmx : tmy;                /* where the ray leaves this cell */
            if (tn > t1 || best <= tn) break;
            if (tmx < tmy) { cx += sx; tmx += tdx; if (cx < 0 || cx >= (int)c->nx) break; }
            else { cy += sy; tmy += tdy; if (cy < 0 || cy >= (int)c->ny) break; }
        }
    }
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
#ifdef NR_PC
#include <stdio.h>
#endif
int nr_coll_sphere(nr_coll *c, const float p[3], float r, float out_push[3], float out_n[3])
{
#ifdef NR_PC
    {   /* (SPHREC=file: the queries that leave the moving triangles out, saved for tools/pc/collbench) */
        static FILE *rf; static int on = -1;
        if (on < 0) { const char *e = getenv("SPHREC"); on = e && (rf = fopen(e, "wb")) != NULL; }
        if (on && c->no_dyn) { float q[4] = { p[0], p[1], p[2], r }; fwrite(q, 4, 4, rf); }
    }
#endif
    out_push[0] = out_push[1] = out_push[2] = 0;
    out_n[0] = out_n[1] = out_n[2] = 0;
    dyn_all(c);
    const float qlo[3] = { p[0] - r, p[1] - r, p[2] - r }, qhi[3] = { p[0] + r, p[1] + r, p[2] + r };
    uint32_t nq = dyn_query(c, qlo, qhi, NULL);
#ifdef DYNCMP
    if (!c->no_dyn) dc_sph(c, NULL, 0, p, r, 0, "sphere");
#endif
    CSA(qf_sph, 1); CSA(cf, nq);
    if (!c->ntri && !nq) return 0;
    int hits = 0;
    float deepest = 0;
    for (uint32_t k = 0; k < nq; k++) {
        const nr_ctri *T = &c->dyn[c->dcand[k]];
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
    const float slo[3] = { p[0] - r, p[1] - r, p[2] - r }, shi[3] = { p[0] + r, p[1] + r, p[2] + r };
    /* (a triangle whose three corners are all more than r + m off the sphere's centre along its normal, on one side, is
     * further than r from it whatever the normal's rounding - the triangle lies between its corners' heights - so it is
     * left out before the closest point is worked out; m covers the arithmetic's rounding: the same contacts) */
    const float rm = r + 0.01f + 4e-6f * fmaxf(fabsf(p[0]), fmaxf(fabsf(p[1]), fabsf(p[2])));
    if (c->ntri) FOR_CELLS_BOX(c, slo, shi, {
        float q[3], d[3];
        const float h0 = (p[0] - T->v[0][0]) * T->n[0] + (p[1] - T->v[0][1]) * T->n[1] + (p[2] - T->v[0][2]) * T->n[2];
        if (h0 > rm || h0 < -rm) {
            const float h1 = (p[0] - T->v[1][0]) * T->n[0] + (p[1] - T->v[1][1]) * T->n[1] + (p[2] - T->v[1][2]) * T->n[2];
            const float h2 = (p[0] - T->v[2][0]) * T->n[0] + (p[1] - T->v[2][1]) * T->n[1] + (p[2] - T->v[2][2]) * T->n[2];
            if (h0 > rm ? h1 > rm && h2 > rm : h1 < -rm && h2 < -rm) continue;
        }
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
    nb->n = 0; nb->dyn_ok = 0; nb->fok = 0;
    memcpy(nb->lo, lo, 12); memcpy(nb->hi, hi, 12);
    if (!c->ntri) return;
    CST0;
    next_stamp(c);
    FOR_CELLS_BOX(c, lo, hi, {
        CSA(gv, 1);
        float b[6]; int out = 0;
        for (int a = 0; a < 3; a++) {
            b[a] = fminf(T->v[0][a], fminf(T->v[1][a], T->v[2][a])); b[a + 3] = fmaxf(T->v[0][a], fmaxf(T->v[1][a], T->v[2][a]));
            if (b[a + 3] < lo[a] || b[a] > hi[a]) out = 1;
        }
        if (out) continue;
        if (nb->n == nb->cap) {
            uint32_t nc = nb->cap ? nb->cap * 2 : 256;
            uint32_t *t = realloc(nb->tri, nc * 4); if (!t) return;
            nb->tri = t;
            void *q = realloc(nb->bb, nc * sizeof *nb->bb); if (!q) return;
            nb->bb = q; nb->cap = nc;
        }
        memcpy(nb->bb[nb->n], b, sizeof b);
        nb->tri[nb->n++] = ti; CSA(gk, 1);
    });
    CST1(tg);
}

void nr_coll_near_free(nr_coll_near *nb) { free(nb->tri); free(nb->bb); free(nb->dtri); free(nb->fk); memset(nb, 0, sizeof *nb); }

/* ---- the near box's smaller box about the car (see nr_coll.h): only for boxes past NF_MIN triangles */
#define NF_MIN 256
void nr_coll_near_focus(nr_coll_near *nb, const float pos[3], float need, float pad)
{
    if (nb->n < NF_MIN) { nb->fok = 0; return; }
    if (nb->fok) {
        int in = 1;
        for (int a = 0; a < 3; a++) if (!(pos[a] - need >= nb->flo[a] && pos[a] + need <= nb->fhi[a])) in = 0;
        if (in) return;
    }
    CST0;
    if (nb->fcap < nb->n) { free(nb->fk); nb->fcap = nb->n + 256; nb->fk = malloc(4ull * nb->fcap); if (!nb->fk) { nb->fcap = 0; nb->fok = 0; return; } }
    float lo[3], hi[3];
    for (int a = 0; a < 3; a++) { lo[a] = pos[a] - (need + pad); hi[a] = pos[a] + (need + pad); }
    uint32_t m = 0;
    for (uint32_t k = 0; k < nb->n; k++) {                         /* (the queries' own test: a box no query inside this
                                                                    *  one leaves out is never left out here - NaN too) */
        const float *b = nb->bb[k];
        if (b[3] < lo[0] || b[0] > hi[0] || b[4] < lo[1] || b[1] > hi[1] || b[5] < lo[2] || b[2] > hi[2]) continue;
        nb->fk[m++] = k;
    }
    memcpy(nb->flo, lo, 12); memcpy(nb->fhi, hi, 12); nb->fn = m; nb->fok = 1;
    CST1(tb); CSA(gcell, 1);
}
/* the triangles of the box a query in [lo, hi] looks at: NULL (all of them, in order) or the focus box's (n of them) */
static inline const uint32_t *nf_list(const nr_coll_near *nb, const float lo[3], const float hi[3], uint32_t *n)
{
    if (!nb->fok) return NULL;
    for (int a = 0; a < 3; a++) if (!(lo[a] >= nb->flo[a] && hi[a] <= nb->fhi[a])) { CSA(nn, 1); return NULL; }
    *n = nb->fn; return nb->fk;
}

#ifdef DYNCMP
void nr_coll_dyncmp_report(void);
static void dc_report(const char *what, int h1, int h2, float a1, float a2, uint32_t m1, uint32_t m2, uint32_t nnew, uint32_t nold)
{
    if (++dc_n % 20000 == 0) nr_coll_dyncmp_report();
    if (h1 == h2 && a1 == a2 && m1 == m2) return;
    if (dc_bad++ < 40) fprintf(stderr, "DYNCMP %s: new hit %d %.9g mat %08x (%u) | old hit %d %.9g mat %08x (%u)\n", what, h1, a1, m1, nnew, h2, a2, m2, nold);
}
static void dc_ray(nr_coll *c, const nr_coll_near *nb, int use_nb, const float o[3], const float d[3], float maxd, const float lo[3], const float hi[3], const char *what)
{
    float b1 = maxd, b2 = maxd; int h1 = 0, h2 = 0; uint32_t m1 = 0, m2 = 0;
    for (uint32_t q = 0; q < c->ndcand; q++) { const nr_ctri *T = &c->dyn[c->dcand[q]]; if (ray_tri(T, o, d, &b1)) { h1 = 1; m1 = T->mat ^ (uint32_t)(T->n[0] * 1e6f); } }
    uint32_t n = use_nb ? nb->dn : OND;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t i = use_nb ? nb->dtri[k] : k; const float *b = ODB[i];
        if (b[3] < lo[0] || b[0] > hi[0] || b[4] < lo[1] || b[1] > hi[1] || b[5] < lo[2] || b[2] > hi[2]) continue;
        if (ray_tri(&OD[i], o, d, &b2)) { h2 = 1; m2 = OD[i].mat ^ (uint32_t)(OD[i].n[0] * 1e6f); }
    }
    dc_report(what, h1, h2, b1, b2, m1, m2, c->ndcand, n);
}
static void dc_sph(nr_coll *c, const nr_coll_near *nb, int use_nb, const float p[3], float r, int pdtest, const char *what)
{
    float on1[3] = { 0, 0, 0 }, on2[3] = { 0, 0, 0 }, dp1 = 0, dp2 = 0; int h1 = 0, h2 = 0; uint32_t m1 = 0, m2 = 0;
    uint32_t n = use_nb ? nb->dn : OND;
    for (int way = 0; way < 2; way++) {
        uint32_t cnt = way ? n : c->ndcand;
        for (uint32_t k = 0; k < cnt; k++) {
            const nr_ctri *T;
            if (way) {
                uint32_t i = use_nb ? nb->dtri[k] : k; const float *b = ODB[i];
                if (b[3] < p[0] - r || b[0] > p[0] + r || b[4] < p[1] - r || b[1] > p[1] + r || b[5] < p[2] - r || b[2] > p[2] + r) continue;
                T = &OD[i];
            } else T = &c->dyn[c->dcand[k]];
            if (pdtest) { float pd = dot3(T->n, p) - dot3(T->n, T->v[0]); if (pd >= r || pd <= -r) continue; }
            float q[3], dd[3];
            closest_on_tri(p, T->v[0], T->v[1], T->v[2], q);
            sub3(p, q, dd);
            float l2 = dot3(dd, dd);
            if (l2 >= r * r) continue;
            float l = sqrtf(l2), nn[3];
            if (l > 1e-5f) { nn[0] = dd[0] / l; nn[1] = dd[1] / l; nn[2] = dd[2] / l; } else memcpy(nn, T->n, 12);
            if (dot3(nn, T->n) < 0) continue;
            float depth = r - l, *on = way ? on2 : on1, *dp = way ? &dp2 : &dp1;
            for (int i = 0; i < 3; i++) on[i] += nn[i] * depth;
            if (depth > *dp) { *dp = depth; if (way) m2 = T->mat; else m1 = T->mat; }
            if (way) h2++; else h1++;
        }
    }
    dc_report(what, h1, h2, on1[0] + on1[1] * 3 + on1[2] * 7 + dp1, on2[0] + on2[1] * 3 + on2[2] * 7 + dp2, m1, m2, c->ndcand, n);
}
void nr_coll_dyncmp_report(void) { fprintf(stderr, "DYNCMP total %lu queries, %lu differ\n", dc_n, dc_bad); }
#else
void nr_coll_dyncmp_report(void) {}
#endif

/* the moving triangles in the near box (by the same test as the level's own, nr_coll_gather) are the share of the car's
 * queries: they look at these few instead of every moving triangle within the frame's reach (10000 of the Evil
 * Carwash's spinning crushers: 50 queries a frame over all of them took the Vita tens of milliseconds) */
void nr_coll_near_dyn(nr_coll *c, nr_coll_near *nb)
{
    if (c->no_dyn) { nb->dn = 0; nb->dyn_ok = 0; return; }
    if (c->dyn_pending) {                          /* (the box past what was listed at once: the rest too) */
        int in = 1;
        for (int a = 0; a < 3; a++) if (nb->lo[a] < c->dyn_lo[a] || nb->hi[a] > c->dyn_hi[a]) in = 0;
        if (!in) dyn_all(c);
    }
    if (nb->dyn_ok && nb->dyn_gen == c->dyn_gen) return;
    nb->dyn_gen = c->dyn_gen; nb->dyn_ok = 1; nb->dn = 0;
    CSA(gat, 1);
#ifdef DYNCMP
    for (uint32_t k = 0; k < OND; k++) {
        const float *b = ODB[k];
        if (b[3] < nb->lo[0] || b[0] > nb->hi[0] || b[4] < nb->lo[1] || b[1] > nb->hi[1] || b[5] < nb->lo[2] || b[2] > nb->hi[2]) continue;
        if (nb->dn == nb->dcap) { nb->dcap = nb->dcap ? nb->dcap * 2 : 256; nb->dtri = realloc(nb->dtri, 4ull * nb->dcap); }
        nb->dtri[nb->dn++] = k;
    }
#endif
}

int nr_coll_ray_near(nr_coll *c, const nr_coll_near *nb, const float o[3], const float d[3], float maxd, float *dist, float n[3], uint32_t *mat)
{
    float best = maxd; int hit = 0;
    int fresh = nb->dyn_ok && nb->dyn_gen == c->dyn_gen;          /* (not: every moving triangle, as before) */
    float lo[3], hi[3];                                            /* the ray's box: the triangles' boxes first */
    for (int a = 0; a < 3; a++) { float e = o[a] + d[a] * maxd; lo[a] = fminf(o[a], e); hi[a] = fmaxf(o[a], e); }
    uint32_t nd = c->no_dyn ? 0 : dyn_query(c, lo, hi, fresh ? nb : NULL);
#ifdef DYNCMP
    if (!c->no_dyn) dc_ray(c, nb, fresh, o, d, maxd, lo, hi, fresh ? "ray_near fresh" : "ray_near");
#endif
    CSA(q_ray, 1); CST0;
    uint32_t ns = nb->n; const uint32_t *L = nf_list(nb, lo, hi, &ns);   /* (the focus box's, or all) */
    CSA(c_ray, ns + nd);
    for (uint32_t i = 0; i < ns + nd; i++) {
        const nr_ctri *T;
        if (i < ns) {
            const uint32_t k = L ? L[i] : i;
            const float *b = nb->bb[k];
            if (b[3] < lo[0] || b[0] > hi[0] || b[4] < lo[1] || b[1] > hi[1] || b[5] < lo[2] || b[2] > hi[2]) continue;
            T = &c->tris[nb->tri[k]];
            if (tri_off(c, T)) continue;
        } else T = &c->dyn[c->dcand[i - ns]];
        CSA(b_ray, 1);
        if (ray_tri(T, o, d, &best)) { hit = 1; memcpy(n, T->n, 12); if (mat) *mat = T->mat; }
    }
    if (hit) *dist = best;
    CST1(tq);
    return hit;
}

int nr_coll_sphere_near(nr_coll *c, const nr_coll_near *nb, const float p[3], float r, float out_push[3], float out_n[3], uint32_t *mat)
{
    out_push[0] = out_push[1] = out_push[2] = 0;
    out_n[0] = out_n[1] = out_n[2] = 0;
    int hits = 0; float deepest = 0;
    int fresh = nb->dyn_ok && nb->dyn_gen == c->dyn_gen;          /* (not: every moving triangle, as before) */
    const float lo[3] = { p[0] - r, p[1] - r, p[2] - r }, hi[3] = { p[0] + r, p[1] + r, p[2] + r };
    uint32_t nd = c->no_dyn ? 0 : dyn_query(c, lo, hi, fresh ? nb : NULL);
#ifdef DYNCMP
    if (!c->no_dyn) dc_sph(c, nb, fresh, p, r, 1, fresh ? "sphere_near fresh" : "sphere_near");
#endif
    CSA(q_sph, 1); CST0;
    uint32_t ns = nb->n; const uint32_t *L = nf_list(nb, lo, hi, &ns);   /* (the focus box's, or all) */
    CSA(c_sph, ns + nd);
    for (uint32_t i = 0; i < ns + nd; i++) {
        const nr_ctri *T;
        if (i < ns) {
            const uint32_t k = L ? L[i] : i;
            const float *b = nb->bb[k];                            /* (the sphere's box) */
            if (b[3] < p[0] - r || b[0] > p[0] + r || b[4] < p[1] - r || b[1] > p[1] + r || b[5] < p[2] - r || b[2] > p[2] + r) continue;
            T = &c->tris[nb->tri[k]];
            if (tri_off(c, T)) continue;
        } else T = &c->dyn[c->dcand[i - ns]];
        CSA(b_sph, 1);
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
    CST1(tq);
    return hits;
}
