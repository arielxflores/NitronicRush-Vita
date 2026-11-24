/* Level collision (levels/<name>.nrc): world-space triangles in a 2D grid, for the car. */
#ifndef NR_COLL_H
#define NR_COLL_H
#include <stdint.h>

enum { NR_CMAT_DEFAULT = 0, NR_CMAT_ROAD = 1, NR_CMAT_BUILDING = 2, NR_CMAT_STREETLIGHT = 3, NR_CMAT_OBSTICLE = 4,
       NR_CMAT_METAL = 5, NR_CMAT_LAVA = 6, NR_CMAT_SPIKE = 7 };

/* a triangle's material word: bits 0-3 the collision material (CollisionMaterials.xml), bits 4-7
 * the game's "Material ID" (1 kills the car, 2 spike barrel, 3 streetlight, 4 screen), bits 8-30
 * the level object it belongs to (its id in the .nrg; for moving objects, their index), bit 31
 * set for moving objects' triangles */
#define NR_TRI_DYN 0x80000000u
#define NR_TRI_CMAT(m) ((m) & 15u)
#define NR_TRI_MATID(m) (((m) >> 4) & 15u)
#define NR_TRI_OBJ(m) (((m) >> 8) & 0x7fffffu)

typedef struct { float v[3][3]; float n[3]; uint32_t mat; } nr_ctri;   /* 52 bytes, as in the file */
typedef struct { float c[3], v[3], w[3]; } nr_coll_body;            /* a moving collider: centre, velocity, spin */

/* a moving model's triangles by cell of a grid over its own box: a query looks only in the cells its box meets in the
 * model's space (g[0] == 0: no grid, a small model looked at whole); triangles over many cells are in big */
typedef struct { float lo[3], inv[3]; int g[3]; uint32_t *start, *idx, nbig, *big; } nr_cgrid;
void nr_cgrid_build(nr_cgrid *G, const float (*bb)[6], uint32_t n);
void nr_cgrid_free(nr_cgrid *G);

/* a moving object a query may meet this frame: its model (own space), its transform when listed, and the cube of the
 * pass that listed it in its own space (lp, rl: nr_objs's test of the triangles' own boxes) */
typedef struct nr_cdobj {
    const float (*t)[9]; const float (*bb)[6]; nr_cgrid g; uint32_t n;
    uint32_t mat; int flip, pass;          /* pass 1: within the near cube; 2: the rest of the reach */
    float R[3][3], pos[3], scale[3];
    float lp[3], rl[3];
    float wlo[3], whi[3];                  /* its world box (round its sphere, radius mr) */
    float inv[3][3]; int has_inv;          /* world to own space */
    uint32_t map;                          /* its triangles' entries in dmap */
} nr_cdobj;

typedef struct {
    uint32_t ntri, nx, ny;
    float minx, miny, cell;
    nr_ctri *tris;
    uint32_t *start;      /* nx*ny+1 */
    uint32_t *idx;
    float minz;           /* lowest point of the level (for "fell off" checks) */
    uint32_t *stamp; uint32_t stamp_id;   /* to test each triangle once per query */
    /* moving objects near the car: nr_objs lists them each frame (nr_cdobj: the ones the car's own physics can reach at
     * once, the rest of the frame's reach only when a query that can go further wants them: dyn_more(dyn_user)); a
     * query moves to world space only the triangles it may meet, found by the grid of each one's model, and keeps them
     * for the frame's other queries (the Evil Carwash's crushers: 10000 triangles round the car, a few dozen met) */
    nr_ctri *dyn; uint32_t ndyn, dyncap;  /* moved so far this frame */
    float (*dynbb)[6];                    /* each one's box: min xyz, max xyz */
    struct nr_cdobj *dobj; uint32_t ndobj, dobjcap;
    struct nr_cdmap { uint32_t frame, slot, q; } *dmap; uint32_t ndmap, dmapcap;   /* per listed object's triangle */
    uint32_t dyn_frame, dyn_q;            /* (stamps: the frame's listing, the query) */
    uint32_t *dcand, ndcand, dcandcap;    /* a query's moving triangles (slots in dyn) */
    float dyn_at[3], dyn_rn, dyn_rf;      /* the cubes: the near one (half size dyn_rn), the whole reach (dyn_rf) */
    uint32_t dyn_gen;                     /* counts the changes (nr_coll_near keeps its share up to date) */
    float dyn_lo[3], dyn_hi[3]; int dyn_pending;
    void (*dyn_more)(void *u); void *dyn_user;
    int no_dyn;                           /* > 0: the queries leave the moving triangles out */
    const nr_coll_body *bodies;           /* their motion, by NR_TRI_OBJ of a moving triangle */
    const uint8_t *off; uint32_t noff;    /* level objects whose collision is off (broken streetlights), by id */
    /* each cell entry's box, next to the others of its cell (read in order: no jumping about memory): the triangle's
     * x and y within the cell (0..255 = 0..cell, clipped to it) and z (0..65535 over the level's heights), rounded
     * outwards; the queries skip the entries whose box misses theirs before looking at the triangle */
    struct nr_cref { uint8_t x0, x1, y0, y1; uint16_t z0, z1; } *refbb;
    float zlo, zq;                        /* z = zlo + q / zq */
    /* a cell with many entries (a building's thousands of triangles over its 32 x 32 units, all its heights) also has
     * them by part of the cell: 4 x 4 across and up to 16 by height, each part listing its entries' places in the cell
     * (in the cell's order); those over many parts in a list of their own. A small query looks only in the parts it
     * meets, and takes what it finds in the cell's order: the same triangles in the same order as the whole cell */
    struct nr_csub { uint32_t first; uint16_t nz, nbig; float z0, zinv; } *sub;   /* by cell; first: in subst (~0: none) */
    uint32_t *subst; uint16_t *subidx;    /* each part's first in subidx (16 nz + 2 a cell: the parts, the many-part list, end) */
    uint32_t nsubcell, nsubidx;           /* (how many: dense cells, part entries) */
    uint32_t *ktmp, ktmpcap;              /* a cell's entries a query takes */
    uint64_t *kbits;                      /* (a bit for each entry of the biggest dense cell, all clear between queries:
                                           *  a long list of them put in the cell's order) */
} nr_coll;

int  nr_coll_parse(const uint8_t *blob, uint32_t size, nr_coll *c);   /* copies what it needs */
void nr_coll_free(nr_coll *c);

/* ray from o along unit dir d, up to maxd; returns 1 and fills distance, normal, material on a hit */
int nr_coll_ray(nr_coll *c, const float o[3], const float d[3], float maxd, float *dist, float n[3], uint32_t *mat);

/* sphere: pushes out of every triangle it overlaps. returns the number of contacts; out_push is
 * the total correction to apply, out_n the average contact normal */
int nr_coll_sphere(nr_coll *c, const float p[3], float r, float out_push[3], float out_n[3]);
extern uint32_t nr_coll_last_mat;
/* velocity of the surface a contact is on (zero unless it is a moving object's) */
void nr_coll_point_vel(const nr_coll *c, uint32_t mat, const float p[3], float v[3]);   /* material of the deepest contact of the last sphere query */

/* the triangles near something that moves (the car): gathered once per frame from the grid, so
 * the many short queries of the physics steps only look at a few dozen triangles */
typedef struct {
    uint32_t n, cap; uint32_t *tri; float lo[3], hi[3];
    float (*bb)[6];                    /* each one's box (min xyz, max xyz): the queries try it first */
    uint32_t dn, dcap; uint32_t *dtri; uint32_t dyn_gen; int dyn_ok;   /* the moving triangles in the box (of rebuild dyn_gen) */
    /* a box with many triangles (warp_speed's 4000 by the flying car, all looked at by each of its 40 queries a frame):
     * the ones whose boxes meet a smaller box about the car (nr_coll_near_focus) - a query inside that box looks at those
     * alone, in the same order as the whole list (the same sums: the same physics, bit for bit), any other at them all */
    int fok; float flo[3], fhi[3]; uint32_t *fk, fn, fcap;
} nr_coll_near;
void nr_coll_gather(nr_coll *c, const float lo[3], const float hi[3], nr_coll_near *nb);
void nr_coll_near_free(nr_coll_near *nb);
/* the car about to make its queries at pos, each within need of it: the triangles of the box meeting pos +- (need + pad)
 * (kept while pos +- need stays inside: pad, what the car may move before it is made again) */
void nr_coll_near_focus(nr_coll_near *nb, const float pos[3], float need, float pad);
/* the moving triangles' share of the box, again when nr_objs has listed them anew (call before the queries of a step) */
void nr_coll_near_dyn(nr_coll *c, nr_coll_near *nb);
/* nr_objs: a new frame's listing (the cubes round at), then each moving object of a pass, by object */
void nr_coll_dyn_begin(nr_coll *c, const float at[3], float near_half, float reach_half);
void nr_coll_dyn_add(nr_coll *c, const nr_cdobj *d, float mr);
/* the moving triangles listed so far this frame whose box meets [lo, hi] (moved now if need be): their slots in dyn, in
 * c->dcand (until the next query) */
uint32_t nr_coll_dyn_list(nr_coll *c, const float lo[3], const float hi[3]);
int  nr_coll_ray_near(nr_coll *c, const nr_coll_near *nb, const float o[3], const float d[3], float maxd, float *dist, float n[3], uint32_t *mat);
/* like nr_coll_sphere; also gives the material of the deepest contact */
int  nr_coll_sphere_near(nr_coll *c, const nr_coll_near *nb, const float p[3], float r, float out_push[3], float out_n[3], uint32_t *mat);
#ifdef CSTAT
extern struct nr_cstat { unsigned q_sph, q_ray, c_sph, c_ray, b_sph, b_ray, xf1, add1, xf2, add2, more, nn, nd, qf_ray, qf_sph, cf, gat, gv, gk, gcell, tg, tq, tb, ts, sn; } NRCS;
#include <time.h>
static inline unsigned cs_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (unsigned)(t.tv_sec * 1000000000ull + t.tv_nsec); }
#define CST0 unsigned cst0_ = cs_ns()
#define CST1(f) (NRCS.f += cs_ns() - cst0_)
#define CSA(f, v) (NRCS.f += (v))
#else
#define CSA(f, v) ((void)0)
#define CST0 ((void)0)
#define CST1(f) ((void)0)
#endif
#endif
