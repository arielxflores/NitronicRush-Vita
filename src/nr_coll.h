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

typedef struct {
    uint32_t ntri, nx, ny;
    float minx, miny, cell;
    nr_ctri *tris;
    uint32_t *start;      /* nx*ny+1 */
    uint32_t *idx;
    float minz;           /* lowest point of the level (for "fell off" checks) */
    uint32_t *stamp; uint32_t stamp_id;   /* to test each triangle once per query */
    /* moving objects near the car (world space, rebuilt every frame by nr_objs) */
    nr_ctri *dyn; uint32_t ndyn, dyncap;
    const nr_coll_body *bodies;           /* their motion, by NR_TRI_OBJ of a moving triangle */
    const uint8_t *off; uint32_t noff;    /* level objects whose collision is off (broken streetlights), by id */
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
typedef struct { uint32_t n, cap; uint32_t *tri; float lo[3], hi[3]; } nr_coll_near;
void nr_coll_gather(nr_coll *c, const float lo[3], const float hi[3], nr_coll_near *nb);
void nr_coll_near_free(nr_coll_near *nb);
int  nr_coll_ray_near(const nr_coll *c, const nr_coll_near *nb, const float o[3], const float d[3], float maxd, float *dist, float n[3], uint32_t *mat);
/* like nr_coll_sphere; also gives the material of the deepest contact */
int  nr_coll_sphere_near(const nr_coll *c, const nr_coll_near *nb, const float p[3], float r, float out_push[3], float out_n[3], uint32_t *mat);
#endif
