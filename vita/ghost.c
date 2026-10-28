/* Ghost cars: the PC's ReplayManager, ReplayLoadedFile, ReplayPlayer, ReplayRecorder and PlayerName
 * (re_N_ghosts.md, addresses there). */
#include "io.h"
#include "ghost.h"
#include "hud.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TICK 0.06f                  /* ReplayManager +0x08 (Init 0x6feda0) */
#define VERSION 500001u
#define MAX_DEFS 128
#define MAX_PARTS 6
#define MAX_OBJS 512                /* live ghost objects (cars, wings are parts, wreck pieces) */
#define MAX_IDS 256                 /* object ids a file may use */
#define REC_MAX_ACTIONS 0x20000     /* RM +0x14 / +0x18 */
#define REC_MAX_FRAMES 0x2000

/* ------------------------------------------------------------------ blueprints (models/ghosts.txt) */
typedef struct { int parent, model; float pos[3], ori[9], scl[3], t0, t1, dpos[3], drot, axis[3]; int state; char mname[40]; } gh_part;
typedef struct { char name[48]; int nparts, label, failed; gh_part p[MAX_PARTS]; } gh_def;   /* failed: FailedReplayCar (the results camera follows it) */
static gh_def DEF[MAX_DEFS]; static int ndef;
static level_rt MODELS[160]; static char MODEL_NAME[160][48]; static int nmodel;
static nr_pak *PAK;

static int def_find(const char *name)
{
    for (int i = 0; i < ndef; i++) if (!strcmp(DEF[i].name, name)) return i;
    return -1;
}
static int model_get(const char *name);
static void def_models(int di)
{
    if (di < 0) return;
    for (int i = 0; i < DEF[di].nparts; i++) if (DEF[di].p[i].model == -2) DEF[di].p[i].model = model_get(DEF[di].p[i].mname);
}
static int model_get(const char *name)
{
    for (int i = 0; i < nmodel; i++) if (!strcmp(MODEL_NAME[i], name)) return MODELS[i].nch ? i : -1;
    if (nmodel >= 160) return -1;
    char p[80]; snprintf(p, sizeof p, "models/%s.nrl", name);
    snprintf(MODEL_NAME[nmodel], 48, "%s", name);
    if (!gfx_load_nrl(&MODELS[nmodel], PAK, p, name)) memset(&MODELS[nmodel], 0, sizeof MODELS[nmodel]);
    nmodel++;
    return MODELS[nmodel - 1].nch ? nmodel - 1 : -1;
}

int gh_init(nr_pak *pak)
{
    PAK = pak; ndef = 0;
    uint32_t sz; char *t = nr_pak_read(pak, "models/ghosts.txt", &sz);
    if (!t) return -1;
    gh_def *d = NULL;
    for (char *ln = strtok(t, "\n"); ln; ln = strtok(NULL, "\n")) {
        char nm[48]; int np, lab, fl = 0;
        if (sscanf(ln, "ghost %47s %d %d %d", nm, &np, &lab, &fl) >= 3 && ndef < MAX_DEFS) {
            d = &DEF[ndef++]; memset(d, 0, sizeof *d);
            snprintf(d->name, sizeof d->name, "%s", nm); d->label = lab; d->failed = fl;
        } else if (d && !strncmp(ln, "part ", 5) && d->nparts < MAX_PARTS) {
            float f[26]; int idx, par; char model[48];
            if (sscanf(ln, "part %d %d %47s %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f",
                       &idx, &par, model, &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &f[7], &f[8], &f[9], &f[10], &f[11],
                       &f[12], &f[13], &f[14], &f[15], &f[16], &f[17], &f[18], &f[19], &f[20], &f[21], &f[22], &f[23], &f[24]) != 28) continue;
            gh_part *p = &d->p[d->nparts++];
            p->parent = par;
            memcpy(p->pos, f, 12); memcpy(p->ori, f + 3, 36); memcpy(p->scl, f + 12, 12);
            p->t0 = f[15]; p->t1 = f[16]; memcpy(p->dpos, f + 17, 12); p->drot = f[20]; memcpy(p->axis, f + 21, 12);
            p->state = (int)f[24];
            p->model = strcmp(model, "-") ? -2 : -1;      /* -2: loaded when a replay that uses it is (def_models) */
            snprintf(p->mname, sizeof p->mname, "%s", model);
        }
    }
    free(t);
    return ndef ? 0 : -1;
}

/* ------------------------------------------------------------------ playback */
typedef struct { float q[4], p[3]; } rec_t;                 /* qw qx qy qz, x y z */
typedef struct {
    int alive, file, def, first, label;
    rec_t prev, cur;
    float W[4][4];                                         /* interpolated root matrix */
    float anim_t; int anim_fwd;                            /* the root's AnimBasic (State 2: 0..0.6, the wings) */
    float off, hold;                                       /* PlayerName: declutter offset, hold timer */
    float cx, cy, cw, count;                               /* clip-space x, y, w of the car; overlap count */
} gh_obj;
static gh_obj O[MAX_OBJS];

typedef struct {
    uint8_t *data; uint32_t size;
    uint32_t nA, nB, nC, nD, nN;
    const uint8_t *A, *B, *C, *D;                          /* (read with memcpy: D may be unaligned) */
    int16_t namedef[64];                                   /* names -> blueprint (-1 unknown) */
    int state, frame, ai, ci, di;                          /* 2 rewound, 3 playing, 4 finished */
    int16_t obj[MAX_IDS]; uint16_t freeid[MAX_IDS]; int nfree, nids;
    float col[4]; char initials[8];
    int self;                                              /* a copy of this run (the results' replay): no palette, label or dimming */
    int pilot;                                             /* autoplay drives the car along this replay: not drawn */
    int failed_obj;                                        /* its newest FailedReplayCar object (-1 none) */
    int car_obj;                                           /* its newest car object (-1 none) */
} gh_file;
#define MAX_FILES (GH_FILES + 24)              /* the ghosts, the self copies (20 at most), the pilot */
static gh_file F[MAX_FILES]; static int nfile, nself, npilot;
static float acc, label_fade; static int fade_in;

/* the colours by load order (0x6fd470): gold, silver, bronze, then seven others; the glow byte is a x 10 */
static const float PAL[10][4] = {
    { 0.8f, 0.619f, 0.01f, 0.5f }, { 0.5f, 0.5f, 0.5f, 0.5f }, { 0.6f, 0.2f, 0.0f, 0.5f }, { 0.8f, 0.1f, 0.1f, 0.25f },
    { 0.45f, 0.45f, 0.1f, 0.25f }, { 0.3f, 0.6f, 0.1f, 0.25f }, { 0.1f, 0.8f, 0.1f, 0.25f }, { 0.2f, 0.6f, 1.0f, 0.25f },
    { 0.1f, 0.1f, 0.8f, 0.25f }, { 0.1f, 0.35f, 0.55f, 0.25f } };

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint32_t A_at(const gh_file *f, uint32_t i) { return rd32(f->A + 4 * i); }
static uint32_t B_at(const gh_file *f, uint32_t i) { return rd32(f->B + 4 * i); }
static uint16_t C_at(const gh_file *f, uint32_t i) { uint16_t v; memcpy(&v, f->C + 2 * i, 2); return v; }

static void obj_kill(int o) { if (o >= 0 && o < MAX_OBJS) O[o].alive = 0; }

int gh_count(void) { return nfile - nself - npilot; }

void gh_clear(void)
{
    for (int i = 0; i < MAX_OBJS; i++) O[i].alive = 0;
    for (int i = 0; i < nfile; i++) free(F[i].data);
    memset(F, 0, sizeof F); nfile = nself = npilot = 0; acc = 0;
}

static void file_rewind(gh_file *f)                       /* LF_Rewind 0x683230: frame cursor from 1 */
{
    for (int i = 0; i < f->nids; i++) obj_kill(f->obj[i]);
    for (int i = 0; i < MAX_IDS; i++) f->obj[i] = -1;
    f->nids = f->nfree = 0;
    f->frame = 1; f->ai = f->ci = f->di = 0; f->state = 2; f->failed_obj = f->car_obj = -1;
}

int gh_load(uint8_t *d, uint32_t sz, const char *initials)    /* RM_LoadGhostFile 0x6fea50 + LF_ReadBody 0x682b80 */
{
    if (!d) return -1;
    if (nfile >= MAX_FILES || sz < 24 || rd32(d) != VERSION) { free(d); return -1; }
    gh_file *f = &F[nfile]; memset(f, 0, sizeof *f);
    uint32_t o = 4;
#define NEED(n) do { if ((uint64_t)o + (n) > sz) { free(d); return -1; } } while (0)
    NEED(4); f->nA = rd32(d + o); o += 4; NEED((uint64_t)f->nA * 4); f->A = d + o; o += f->nA * 4;
    NEED(4); f->nB = rd32(d + o); o += 4; NEED((uint64_t)f->nB * 4); f->B = d + o; o += f->nB * 4;
    NEED(4); f->nC = rd32(d + o); o += 4; NEED((uint64_t)f->nC * 2); f->C = d + o; o += f->nC * 2;
    NEED(4); f->nD = rd32(d + o); o += 4; NEED((uint64_t)f->nD * 28); f->D = d + o; o += f->nD * 28;
    NEED(4); f->nN = rd32(d + o); o += 4;
    for (uint32_t i = 0; i < f->nN; i++) {
        NEED(4); uint32_t len = rd32(d + o); o += 4; NEED(len);
        char nm[64]; uint32_t k = len < 63 ? len : 63; memcpy(nm, d + o, k); nm[k] = 0; o += len;
        if (i < 64) { f->namedef[i] = (int16_t)def_find(nm); def_models(f->namedef[i]); }
    }
#undef NEED
    /* the port's checks (the PC trusts the file): cumulative counts within the actions, names in range */
    uint32_t prevb = 0, nsp = 0, nxf = 0;
    for (uint32_t i = 0; i < f->nB; i++) { uint32_t b = B_at(f, i); if (b < prevb || b > f->nA) { free(d); return -1; } prevb = b; }
    for (uint32_t i = 0; i < f->nA; i++) { uint32_t t = A_at(f, i) & 0xffff; nsp += t == 0; nxf += t == 2; }
    if (nsp > f->nC + 1 || nxf > f->nD || f->nN > 64) { free(d); return -1; }
    for (uint32_t i = 0; i < f->nC; i++) if (C_at(f, i) >= f->nN) { free(d); return -1; }
    f->data = d; f->size = sz;
    { int pi = nfile - nself - npilot; memcpy(f->col, PAL[pi < 10 ? pi : 9], sizeof f->col); }   /* by the ghosts' load order */
    f->failed_obj = f->car_obj = -1;
    snprintf(f->initials, sizeof f->initials, "%s", initials && initials[0] ? initials : "???");
    for (int i = 0; i < MAX_IDS; i++) f->obj[i] = -1;
    file_rewind(f);
    return nfile++;
}

void gh_rewind(void) { for (int i = 0; i < nfile; i++) file_rewind(&F[i]); acc = 0; }
void gh_start(void) { for (int i = 0; i < nfile; i++) if (F[i].state == 2 && !F[i].self) F[i].state = 3; }   /* StartGhosts: not the self copies (the results camera starts those) */
void gh_labels_fade(int in) { fade_in = in; if (in) label_fade = label_fade < 0 ? 0 : label_fade; }

static void q2m(const float q[4], float R[3][3])
{
    float w = q[0], x = q[1], y = q[2], z = q[3];
    R[0][0] = 1 - 2 * (y * y + z * z); R[0][1] = 2 * (x * y - w * z); R[0][2] = 2 * (x * z + w * y);
    R[1][0] = 2 * (x * y + w * z); R[1][1] = 1 - 2 * (x * x + z * z); R[1][2] = 2 * (y * z - w * x);
    R[2][0] = 2 * (x * z - w * y); R[2][1] = 2 * (y * z + w * x); R[2][2] = 1 - 2 * (x * x + y * y);
}
static void m2q(const float R[3][3], float q[4])
{
    float tr = R[0][0] + R[1][1] + R[2][2];
    if (tr > 0) {
        float s = sqrtf(tr + 1) * 2;
        q[0] = 0.25f * s; q[1] = (R[2][1] - R[1][2]) / s; q[2] = (R[0][2] - R[2][0]) / s; q[3] = (R[1][0] - R[0][1]) / s;
    } else if (R[0][0] > R[1][1] && R[0][0] > R[2][2]) {
        float s = sqrtf(1 + R[0][0] - R[1][1] - R[2][2]) * 2;
        q[0] = (R[2][1] - R[1][2]) / s; q[1] = 0.25f * s; q[2] = (R[0][1] + R[1][0]) / s; q[3] = (R[0][2] + R[2][0]) / s;
    } else if (R[1][1] > R[2][2]) {
        float s = sqrtf(1 + R[1][1] - R[0][0] - R[2][2]) * 2;
        q[0] = (R[0][2] - R[2][0]) / s; q[1] = (R[0][1] + R[1][0]) / s; q[2] = 0.25f * s; q[3] = (R[1][2] + R[2][1]) / s;
    } else {
        float s = sqrtf(1 + R[2][2] - R[0][0] - R[1][1]) * 2;
        q[0] = (R[1][0] - R[0][1]) / s; q[1] = (R[0][2] + R[2][0]) / s; q[2] = (R[1][2] + R[2][1]) / s; q[3] = 0.25f * s;
    }
    float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; i++) q[i] /= l > 1e-9f ? l : 1;
}

static void spawn(gh_file *f, int fi, uint16_t id)          /* LF_Spawn 0x683450 */
{
    int nm = f->ci < (int)f->nC ? C_at(f, f->ci) : -1;
    f->ci++;
    int o = -1;
    for (int i = 0; i < MAX_OBJS; i++) if (!O[i].alive) { o = i; break; }
    if (o >= 0) {
        gh_obj *g = &O[o]; memset(g, 0, sizeof *g);
        g->alive = 1; g->file = fi; g->first = 1;
        g->def = nm >= 0 && nm < 64 ? f->namedef[nm] : -1;
        g->label = g->def >= 0 && DEF[g->def].label && !f->self && !f->pilot;
        if (g->def >= 0 && DEF[g->def].failed) f->failed_obj = o;      /* FailedReplayCar.Init 0x6a4bb0 */
        if (g->def >= 0 && (DEF[g->def].label || DEF[g->def].failed)) f->car_obj = o;
        g->prev.q[0] = g->cur.q[0] = 1;
    }
    /* the same LIFO id rule as the recorder ("Object ID Mismatch" otherwise) */
    int slot = f->nfree ? f->freeid[--f->nfree] : f->nids++;
    if (slot < MAX_IDS) f->obj[slot] = (int16_t)o; else obj_kill(o);
    (void)id;
}
static void destroy(gh_file *f, uint16_t id)                /* LF_Destroy 0x683190 */
{
    if (id < MAX_IDS) { obj_kill(f->obj[id]); f->obj[id] = -1; }
    if (f->nfree < MAX_IDS) f->freeid[f->nfree++] = id;
}
static void transform(gh_file *f, uint16_t id)              /* LF_Transform 0x682860 */
{
    if (f->di >= (int)f->nD) return;
    rec_t r; memcpy(&r, f->D + 28 * (size_t)f->di, 28);
    f->di++;
    int o = id < MAX_IDS ? f->obj[id] : -1;
    if (o < 0 || !O[o].alive) return;
    O[o].prev = O[o].cur; O[o].cur = r;
    if (O[o].first) { O[o].first = 0; O[o].prev = r; }
}
static void file_tick(gh_file *f, int fi)                   /* LF_Tick 0x6837b0 */
{
    if (f->state != 3) return;
    if (f->frame >= (int)f->nB || f->ai >= (int)f->nA) { f->state = 4; return; }
    uint32_t end = B_at(f, f->frame);
    for (; f->ai < (int)end; f->ai++) {
        uint32_t a = A_at(f, f->ai); uint16_t type = a & 0xffff, id = a >> 16;
        switch (type) {
        case 0: spawn(f, fi, id); break;
        case 1: destroy(f, id); break;
        case 2: transform(f, id); break;
        case 3: case 4: { int o = id < MAX_IDS ? f->obj[id] : -1; if (o >= 0 && O[o].alive) O[o].anim_fwd = type == 3; break; }
        }
    }
    f->frame++;
}

/* ------------------------------------------------------------------ the recording */
typedef struct { int id, on; float q[4], p[3]; } recobj;
static struct {
    int state;                                  /* 1 armed, 2 recording, 3 stopped */
    uint32_t *A; uint32_t nA, capA;
    uint32_t *B; uint32_t nB, capB;
    uint16_t *C; uint32_t nC, capC;
    float *D; uint32_t nD, capD;                /* 7 floats each */
    char names[24][48]; int nnames;
    int next_id; uint16_t freeid[256]; int nfree;
    recobj obj[32]; int nobj;                   /* the live recorders, in the order they joined */
} REC;

static int grow(void **p, uint32_t *cap, uint32_t need, size_t el)
{
    if (need <= *cap) return 1;
    uint32_t c = *cap ? *cap * 2 : 1024; while (c < need) c *= 2;
    void *n = realloc(*p, c * el); if (!n) return 0;
    *p = n; *cap = c; return 1;
}
static void push_action(uint32_t a)
{
    if (REC.nA >= REC_MAX_ACTIONS + 64) return;
    if (grow((void **)&REC.A, &REC.capA, REC.nA + 1, 4)) REC.A[REC.nA++] = a;
}

void gh_rec_reset(void)                                     /* ResetRecording 0x6fda10 */
{
    REC.nA = REC.nB = REC.nC = REC.nD = 0; REC.nnames = 0;
    REC.next_id = 0; REC.nfree = 0; REC.nobj = 0; REC.state = 1;
}
void gh_rec_go(void) { if (REC.state == 1) REC.state = 2; }
void gh_rec_stop(void) { if (REC.state == 2) REC.state = 3; }
int  gh_rec_on(void) { return REC.state == 2; }
int  gh_rec_frames(void) { return (int)REC.nB; }

int gh_rec_add(const char *name)                            /* RM_Register 0x6fdc90 */
{
    if (REC.state == 3 || REC.nobj >= 32 || !name || !name[0]) return -1;
    int id = REC.nfree ? REC.freeid[--REC.nfree] : REC.next_id++;
    int ni = -1;
    for (int i = 0; i < REC.nnames; i++) if (!strcmp(REC.names[i], name)) { ni = i; break; }
    if (ni < 0 && REC.nnames < 24) { ni = REC.nnames++; snprintf(REC.names[ni], 48, "%s", name); }
    if (ni < 0) ni = 0;
    if (grow((void **)&REC.C, &REC.capC, REC.nC + 1, 2)) REC.C[REC.nC++] = (uint16_t)ni;
    push_action(0 | (uint32_t)id << 16);
    recobj *r = &REC.obj[REC.nobj++]; memset(r, 0, sizeof *r);
    r->id = id; r->on = 0; r->q[0] = 1;
    return id;
}
void gh_rec_remove(int id)                                  /* RM_Unregister 0x6fdc20 */
{
    if (id < 0) return;
    for (int i = 0; i < REC.nobj; i++) if (REC.obj[i].id == id) {
        memmove(&REC.obj[i], &REC.obj[i + 1], (REC.nobj - i - 1) * sizeof REC.obj[0]); REC.nobj--;
        if (REC.state != 3) { push_action(1 | (uint32_t)id << 16); if (REC.nfree < 256) REC.freeid[REC.nfree++] = (uint16_t)id; }
        return;
    }
}
void gh_rec_wings(int id, int out) { if (id >= 0 && REC.state != 3) push_action((out ? 3u : 4u) | (uint32_t)id << 16); }
void gh_rec_pose(int id, const float R[3][3], const float pos[3])
{
    for (int i = 0; i < REC.nobj; i++) if (REC.obj[i].id == id) {
        float q[4]; m2q(R, q);
        /* the same hemisphere as before (a body's quaternion moves smoothly; the player's nlerp has no check) */
        if (REC.obj[i].on && q[0] * REC.obj[i].q[0] + q[1] * REC.obj[i].q[1] + q[2] * REC.obj[i].q[2] + q[3] * REC.obj[i].q[3] < 0)
            for (int k = 0; k < 4; k++) q[k] = -q[k];
        memcpy(REC.obj[i].q, q, 16); memcpy(REC.obj[i].p, pos, 12); REC.obj[i].on = 1;
        return;
    }
}
static void rec_frame(void)                                 /* RecordFrame 0x6fd790 */
{
    for (int i = 0; i < REC.nobj; i++) {
        recobj *r = &REC.obj[i];
        if (!grow((void **)&REC.D, &REC.capD, (REC.nD + 1) * 7, 4)) return;
        float *d = REC.D + 7 * REC.nD++;
        memcpy(d, r->q, 16); memcpy(d + 4, r->p, 12);
        push_action(2 | (uint32_t)r->id << 16);
    }
    if (grow((void **)&REC.B, &REC.capB, REC.nB + 1, 4)) REC.B[REC.nB++] = REC.nA;
    if (REC.nA >= REC_MAX_ACTIONS || REC.nB > REC_MAX_FRAMES) REC.state = 3;
}

/* the recording as an NrGhostBin file in memory (RM_SaveFile 0x6fcd00's layout); suffix: added to every
 * blueprint name (CopyRecordingAsSelf 0x682a20 adds "S") */
static uint8_t *rec_bytes(uint32_t *size, const char *suffix)
{
    size_t n = 4 + 4 + 4 * (size_t)REC.nA + 4 + 4 * (size_t)REC.nB + 4 + 2 * (size_t)REC.nC + 4 + 28 * (size_t)REC.nD + 4;
    for (int i = 0; i < REC.nnames; i++) n += 4 + strlen(REC.names[i]) + strlen(suffix) + 1;
    uint8_t *b = malloc(n), *w = b;
    if (!b) return NULL;
#define PUT(p, k) do { memcpy(w, (p), (k)); w += (k); } while (0)
    uint32_t v = VERSION; PUT(&v, 4);
    PUT(&REC.nA, 4); if (REC.nA) PUT(REC.A, 4 * (size_t)REC.nA);
    PUT(&REC.nB, 4); if (REC.nB) PUT(REC.B, 4 * (size_t)REC.nB);
    PUT(&REC.nC, 4); if (REC.nC) PUT(REC.C, 2 * (size_t)REC.nC);
    PUT(&REC.nD, 4); if (REC.nD) PUT(REC.D, 28 * (size_t)REC.nD);
    uint32_t nn = (uint32_t)REC.nnames; PUT(&nn, 4);
    for (int i = 0; i < REC.nnames; i++) {
        uint32_t len = (uint32_t)(strlen(REC.names[i]) + strlen(suffix) + 1);
        PUT(&len, 4); PUT(REC.names[i], strlen(REC.names[i])); PUT(suffix, strlen(suffix) + 1);
    }
#undef PUT
    *size = (uint32_t)(w - b);
    return b;
}
int gh_rec_save(const char *path)                    /* (written by io.c's thread: no wait at the finish line) */
{
    uint32_t n; uint8_t *b = rec_bytes(&n, "");
    if (!b) return -1;
    io_write_take(path, b, n);
    return 0;
}
int gh_rec_actions(void) { return (int)REC.nA; }

/* ---- the results' replay of this run (12: CopyRecordingAsSelf, FailedRunCamera) */
int gh_self_add(void)                                       /* setup 0x6fe860: only a recording of 50 actions or more */
{
    if (REC.nA < 50) return -1;
    if (nself >= 20 && nfile > 0 && F[nfile - 1].self) {     /* 20 kept: the last slot is overwritten */
        gh_file *f = &F[nfile - 1];
        for (int i = 0; i < f->nids; i++) obj_kill(f->obj[i]);
        free(f->data); memset(f, 0, sizeof *f);
        nfile--; nself--;
    }
    if (nfile >= MAX_FILES) return -1;
    uint32_t n; uint8_t *b = rec_bytes(&n, "S");
    int i = gh_load(b, n, "");
    if (i < 0) return -1;
    F[i].self = 1; nself++;
    for (int k = 0; k < nfile; k++) if (F[k].self) file_rewind(&F[k]);   /* every copy back to its start, not playing */
    return i;
}
int gh_self_count(void) { return nself; }
void gh_self_clear(void)                                    /* the level's exit (vslot 23): the copies go */
{
    while (nself > 0 && nfile > 0 && F[nfile - 1].self) {
        gh_file *f = &F[nfile - 1];
        for (int i = 0; i < f->nids; i++) obj_kill(f->obj[i]);
        free(f->data); memset(f, 0, sizeof *f);
        nfile--; nself--;
    }
}
void gh_restart_all(void)                                   /* RewindFailed + StartFailed, RewindGhosts + StartGhosts */
{
    for (int i = 0; i < nfile; i++) { file_rewind(&F[i]); F[i].state = 3; }
    acc = 0;
}
int gh_self_state(void)                                     /* the newest copy: 3 playing, 4 ended, 0 none */
{
    for (int i = nfile - 1; i >= 0; i--) if (F[i].self) return F[i].state;
    return 0;
}
/* ---- autoplay's pilot: the car driven along a shipped replay (the test drive looks like a real run) */
int gh_pilot_load(uint8_t *data, uint32_t size)
{
    int i = gh_load(data, size, "");
    if (i < 0) return -1;
    F[i].pilot = 1; npilot++;
    return i;
}
int gh_pilot_same(const uint8_t *data, uint32_t size)        /* is this replay the pilot's? */
{
    for (int i = 0; i < nfile; i++) if (F[i].pilot && F[i].size == size && !memcmp(F[i].data, data, size)) return 1;
    return 0;
}
int gh_pilot_car(float W[4][4], int *wings, int *alive)
{
    for (int i = 0; i < nfile; i++) if (F[i].pilot) {
        int o = F[i].car_obj;
        *alive = o >= 0 && O[o].alive && !O[o].first && O[o].file == i && F[i].state == 3;
        *wings = *alive && O[o].anim_fwd;
        if (*alive) memcpy(W, O[o].W, sizeof O[o].W);
        return F[i].state == 3 ? 1 : 0;
    }
    return 0;
}
/* a replay's car, tick by tick (for the test drive that steers along it with the real physics): the file is
 * walked as LF_Tick walks it, following the newest object of a car blueprint (one with the runner's
 * label, or the results' FailedReplayCar) */
int gh_track(const uint8_t *d, uint32_t sz, gh_trackpt **out)
{
    *out = NULL;
    if (!d || sz < 24 || rd32(d) != VERSION) return -1;
    uint32_t o = 4, nA, nB, nC, nD, nN;
    const uint8_t *A, *B, *C, *D;
#define NEED(n) do { if ((uint64_t)o + (n) > sz) return -1; } while (0)
    NEED(4); nA = rd32(d + o); o += 4; NEED((uint64_t)nA * 4); A = d + o; o += nA * 4;
    NEED(4); nB = rd32(d + o); o += 4; NEED((uint64_t)nB * 4); B = d + o; o += nB * 4;
    NEED(4); nC = rd32(d + o); o += 4; NEED((uint64_t)nC * 2); C = d + o; o += nC * 2;
    NEED(4); nD = rd32(d + o); o += 4; NEED((uint64_t)nD * 28); D = d + o; o += nD * 28;
    NEED(4); nN = rd32(d + o); o += 4;
    uint8_t iscar[64] = { 0 };
    for (uint32_t i = 0; i < nN; i++) {
        NEED(4); uint32_t len = rd32(d + o); o += 4; NEED(len);
        char nm[64]; uint32_t k = len < 63 ? len : 63; memcpy(nm, d + o, k); nm[k] = 0; o += len;
        int di = def_find(nm);
        if (i < 64) iscar[i] = di >= 0 && (DEF[di].label || DEF[di].failed);
    }
#undef NEED
    if (nB < 2) return -1;
    gh_trackpt *t = calloc(nB, sizeof *t);
    if (!t) return -1;
    /* the ids: name of each live id, its pose, and the LIFO free list (as spawn() hands them out) */
    int16_t idname[MAX_IDS]; uint8_t idlive[MAX_IDS], idposed[MAX_IDS], idwing[MAX_IDS]; rec_t idpose[MAX_IDS];
    uint16_t freeid[MAX_IDS]; int nfree = 0, nids = 0, car = -1;
    memset(idlive, 0, sizeof idlive); memset(idposed, 0, sizeof idposed); memset(idwing, 0, sizeof idwing);
    uint32_t ai = 0, ci = 0, di = 0;
    int n = 0;
    for (uint32_t fr = 1; fr < nB; fr++) {
        uint32_t end = rd32(B + 4 * fr);
        if (end > nA) end = nA;
        for (; ai < end; ai++) {
            uint32_t a = rd32(A + 4 * ai); uint16_t type = a & 0xffff, id = a >> 16;
            if (type == 0) {                                  /* spawn: the id the recorder gave it */
                uint16_t nm; int nmi = -1;
                if (ci < nC) { memcpy(&nm, C + 2 * ci, 2); nmi = nm; }
                ci++;
                int slot = nfree ? freeid[--nfree] : nids++;
                if (slot < MAX_IDS) {
                    idname[slot] = (int16_t)nmi; idlive[slot] = 1; idposed[slot] = 0; idwing[slot] = 0;
                    if (nmi >= 0 && nmi < 64 && iscar[nmi]) car = slot;
                }
            } else if (type == 1) {
                if (id < MAX_IDS) { idlive[id] = 0; if (car == id) car = -1; }
                if (nfree < MAX_IDS) freeid[nfree++] = id;
            } else if (type == 2) {
                if (di < nD) {
                    if (id < MAX_IDS && idlive[id]) { memcpy(&idpose[id], D + 28 * (size_t)di, 28); idposed[id] = 1; }
                    di++;
                }
            } else if (type == 3 || type == 4) {
                if (id < MAX_IDS) idwing[id] = type == 3;
            }
        }
        gh_trackpt *q = &t[n++];
        if (car >= 0 && idlive[car] && idposed[car]) {
            q->alive = 1; q->wings = idwing[car];
            memcpy(q->q, idpose[car].q, 16); memcpy(q->p, idpose[car].p, 12);
        }
    }
    *out = t;
    return n;
}

int gh_self_car(float W[4][4])                              /* failedCars[focus]: alive and placed? */
{
    for (int i = nfile - 1; i >= 0; i--) if (F[i].self) {
        int o = F[i].failed_obj;
        if (o < 0 || !O[o].alive || O[o].first || O[o].file != i) return 0;
        memcpy(W, O[o].W, sizeof O[o].W);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ each frame */
static void interpolate(gh_obj *g, float a)                 /* RP_Interpolate 0x6a6030: lerp + nlerp */
{
    float q[4], l = 0;
    for (int i = 0; i < 4; i++) { q[i] = g->prev.q[i] * (1 - a) + g->cur.q[i] * a; l += q[i] * q[i]; }
    l = sqrtf(l); for (int i = 0; i < 4; i++) q[i] /= l > 1e-9f ? l : 1;
    float R[3][3]; q2m(q, R);
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) g->W[r][c] = R[r][c];
        g->W[r][3] = g->prev.p[r] * (1 - a) + g->cur.p[r] * a;
    }
    g->W[3][0] = g->W[3][1] = g->W[3][2] = 0; g->W[3][3] = 1;
}

void gh_update(float dt)                                    /* RM_Update 0x6ff320 */
{
    if (dt <= 0) return;                                    /* paused: everything holds */
    label_fade = fade_in ? fminf(label_fade + dt, 1) : fmaxf(label_fade - dt, 0);
    if (dt > 0.1f) dt = 0.1f;
    /* the cars' AnimBasic (State 2, 0..0.6 s at frame time): the wings open and fold */
    for (int i = 0; i < MAX_OBJS; i++) if (O[i].alive && O[i].def >= 0 && DEF[O[i].def].p[0].state == 2) {
        gh_part *p = &DEF[O[i].def].p[0];
        if (O[i].anim_fwd) O[i].anim_t = fminf(O[i].anim_t + dt, p->t1);
        else O[i].anim_t = fmaxf(O[i].anim_t - dt, p->t0);
    }
    acc += dt;
    while (acc >= TICK) {
        for (int i = 0; i < nfile; i++) file_tick(&F[i], i);
        if (REC.state == 2) rec_frame();
        acc -= TICK;
    }
    float a = acc / TICK;
    for (int i = 0; i < MAX_OBJS; i++) if (O[i].alive && F[O[i].file].state == 3) interpolate(&O[i], a);
}

static void m4mul(float r[4][4], const float a[4][4], const float b[4][4])
{
    float t[4][4];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { float s = 0; for (int k = 0; k < 4; k++) s += a[i][k] * b[k][j]; t[i][j] = s; }
    memcpy(r, t, sizeof t);
}
static void axis_rot(float R[9], const float ax[3], float an)
{
    float l = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    float x = l > 1e-6f ? ax[0] / l : 0, y = l > 1e-6f ? ax[1] / l : 0, z = l > 1e-6f ? ax[2] / l : 1;
    float c = cosf(an), s = sinf(an), C = 1 - c;
    float r[9] = { c + x * x * C, x * y * C - z * s, x * z * C + y * s, y * x * C + z * s, c + y * y * C, y * z * C - x * s, z * x * C - y * s, z * y * C + x * s, c + z * z * C };
    memcpy(R, r, sizeof r);
}

/* DimGhosts 0x6fd260: a car fades out within 7 units of the camera, and overlapping cars share
 * their brightness (additive ghosts would add up) */
static void dim_ghosts(void)
{
    for (int i = 0; i < MAX_OBJS; i++) {
        gh_obj *g = &O[i];
        if (!g->alive || !g->label) continue;
        float p[4] = { g->W[0][3], g->W[1][3], g->W[2][3], 1 };
        g->cx = VP[0][0] * p[0] + VP[0][1] * p[1] + VP[0][2] * p[2] + VP[0][3];
        g->cy = VP[1][0] * p[0] + VP[1][1] * p[1] + VP[1][2] * p[2] + VP[1][3];
        g->cw = VP[3][0] * p[0] + VP[3][1] * p[1] + VP[3][2] * p[2] + VP[3][3];
        g->count = 1;
    }
    for (int i = 0; i < MAX_OBJS; i++) {
        gh_obj *a = &O[i];
        if (!a->alive || !a->label || a->cw < 1 || a->cw > 100 || F[a->file].state < 3) continue;
        for (int j = i + 1; j < MAX_OBJS; j++) {
            gh_obj *b = &O[j];
            if (!b->alive || !b->label || b->cw < 1 || b->cw > 100 || F[b->file].state < 3) continue;
            float dx = b->cx / b->cw - a->cx / a->cw, dy = b->cy / b->cw - a->cy / a->cw;
            float R = 2 / a->cw + 2 / b->cw, f = (R * R - (dx * dx + dy * dy)) / (R * R);
            f = f < 0 ? 0 : f > 1 ? 1 : f;
            a->count += f; b->count += f;
        }
    }
}

int gh_draw(gfx_obj *objs, int nobj, int max)
{
    dim_ghosts();
    for (int i = 0; i < MAX_OBJS && nobj < max; i++) {
        gh_obj *g = &O[i];
        if (!g->alive || g->def < 0 || F[g->file].state < 3 || g->first || F[g->file].pilot) continue;
        const gh_def *d = &DEF[g->def];
        const float *col = F[g->file].col;
        /* the colour bytes (RenderMesh +0x60..+0x62), the glow byte a x 10 -> x 0.1 */
        float cb[3] = { (int)(col[0] * 255) / 255.0f, (int)(col[1] * 255) / 255.0f, (int)(col[2] * 255) / 255.0f };
        float glow = (int)(col[3] * 10) * 0.1f;
        float k = 1;
        if (g->label) { k = (g->cw - 1) / 6; k = k < 0 ? 0 : k > 1 ? 1 : k; k /= g->count > 1 ? g->count : 1; }
        float PW[MAX_PARTS][4][4];
        for (int pi = 0; pi < d->nparts; pi++) {
            const gh_part *p = &d->p[pi];
            if (pi == 0) memcpy(PW[0], g->W, sizeof g->W);
            else {
                /* State 3: the wings follow the car's timer */
                float f = p->t1 > p->t0 ? (g->anim_t - p->t0) / (p->t1 - p->t0) : 0;
                f = f < 0 ? 0 : f > 1 ? 1 : f;
                float pos[3] = { p->pos[0] + p->dpos[0] * f, p->pos[1] + p->dpos[1] * f, p->pos[2] + p->dpos[2] * f };
                float Ra[9], R[9]; axis_rot(Ra, p->axis, p->drot * f);
                for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
                    float s = 0; for (int q = 0; q < 3; q++) s += p->ori[r * 3 + q] * Ra[q * 3 + c];
                    R[r * 3 + c] = s;
                }
                float L[4][4];
                for (int r = 0; r < 3; r++) { for (int c = 0; c < 3; c++) L[r][c] = R[r * 3 + c] * p->scl[c]; L[r][3] = pos[r]; }
                L[3][0] = L[3][1] = L[3][2] = 0; L[3][3] = 1;
                m4mul(PW[pi], PW[p->parent >= 0 && p->parent < pi ? p->parent : 0], L);
            }
            if (p->model < 0 || nobj >= max) continue;
            gfx_obj *o = &objs[nobj++]; memset(o, 0, sizeof *o);
            o->model = &MODELS[p->model]; memcpy(o->m, PW[pi], sizeof o->m);
            if (F[g->file].self) continue;                 /* (the self copies keep their blueprint's look: car_dead, Color1) */
            o->use_col = 1;
            float kk = pi == 0 ? k : 1;                    /* only the body is dimmed, not its wings */
            o->col[0] = cb[0] * kk; o->col[1] = cb[1] * kk; o->col[2] = cb[2] * kk; o->col[3] = glow;
        }
    }
    return nobj;
}

/* PlayerName (0x6fe510, 0x6fde70): the initials above each ghost car, faded and sized by distance,
 * pushed apart when they overlap. The PC sizes them in window pixels (the same at any resolution);
 * here scaled by 544/720 so they take the screen share they have on a 720-line PC. */
void gh_draw_labels(float dt)
{
    const float W = 960, H = 544, KS = 544.0f / 720.0f;
    const float NearOp = 0.8f, FarOp = 0.7f, NearS = 4000, FarS = 5000, MinS = 100, MaxS = 500, Near = 10, Far = 150, MaxD = 300, MinD = 1;
    typedef struct { int o; float x, y, s, al; } lab;
    lab L[64]; int n = 0;
    if (dt > 0.1f) dt = 0.1f;
    for (int i = 0; i < MAX_OBJS && n < 64; i++) {
        gh_obj *g = &O[i];
        if (!g->alive || !g->label || F[g->file].state < 3 || g->first) continue;
        float p[3] = { g->W[0][3], g->W[1][3], g->W[2][3] };
        float cx = VP[0][0] * p[0] + VP[0][1] * p[1] + VP[0][2] * p[2] + VP[0][3];
        float cy = VP[1][0] * p[0] + VP[1][1] * p[1] + VP[1][2] * p[2] + VP[1][3];
        float w = VP[3][0] * p[0] + VP[3][1] * p[1] + VP[3][2] * p[2] + VP[3][3];
        if (w < 0.05f) continue;
        float t = (w - Near) / (Far - Near), al;
        if (t < 0) al = fmaxf(0, (w - MinD) / (Near - MinD) * NearOp);
        else if (t > 1) al = fmaxf(0, (w - MaxD) / (Far - MaxD) * FarOp);
        else al = NearOp + (FarOp - NearOp) * t;
        float s = (NearS + (FarS - NearS) * t) / w; s = s < MinS ? MinS : s > MaxS ? MaxS : s;
        s *= KS;
        lab *l = &L[n++]; l->o = i; l->s = s; l->al = al;
        l->x = W / 2 + W / 2 * cx / w; l->y = H / 2 + H / 2 * (cy + 1) / w + g->off;   /* y up */
    }
    /* pass 2: overlapping labels push apart (the upper one rises) */
    for (int a = 0; a < n; a++) for (int b = 0; b < n; b++) {
        if (a == b || L[a].al < 0.1f || L[b].al < 0.2f) continue;
        float S = L[a].s + L[b].s;
        if (fabsf(L[a].x - L[b].x) < 0.08f * S && fabsf(L[a].y - L[b].y) < 0.06f * S) {
            lab *up = L[a].y >= L[b].y ? &L[a] : &L[b], *lo = up == &L[a] ? &L[b] : &L[a];
            if (up == &L[b]) continue;                     /* each pair once */
            O[up->o].hold = 0.1f;
            if (fabsf(L[a].y - L[b].y) < 0.057f * S) {
                float d = S * dt * 0.1f;
                O[up->o].off += d; up->y += d;
                if (O[lo->o].off > 0) { O[lo->o].off -= d; lo->y -= d; }
            }
        }
    }
    for (int i = 0; i < n; i++) {
        lab *l = &L[i]; gh_obj *g = &O[l->o];
        if (l->al < 0.1f) continue;
        if (g->hold > 0) g->hold -= dt;
        else if (g->off > 0) g->off = fmaxf(0, g->off - fmaxf(MinS * KS * 3.5f, l->s) * dt * 0.2f);
        float a = l->al * label_fade;
        if (a <= 0.004f) continue;
        const float *c = F[g->file].col;
        unsigned r = (unsigned)(fminf(1, c[0] + 0.2f) * 255), gg = (unsigned)(fminf(1, c[1] + 0.2f) * 255), b = (unsigned)(fminf(1, c[2] + 0.2f) * 255);
        unsigned rgba = r << 24 | gg << 16 | b << 8 | (unsigned)(fminf(1, a) * 255);
        hud_text(HUD_FONT_MAIN, l->x - 0.12f * l->s, H - (l->y + 0.06f * l->s), 0.140625f * l->s, rgba, HUD_LEFT, F[g->file].initials);
    }
}

void gh_stats(char *out, int n)
{
    int live = 0; for (int i = 0; i < MAX_OBJS; i++) live += O[i].alive;
    int pos = snprintf(out, n, "ghosts %d (objects %d)", nfile, live);
    for (int i = 0; i < nfile && pos < n; i++) pos += snprintf(out + pos, n - pos, " | %s state %d frame %d/%u", F[i].initials, F[i].state, F[i].frame, F[i].nB);
    for (int i = 0; i < MAX_OBJS && pos < n; i++) if (O[i].alive) {
        pos += snprintf(out + pos, n - pos, " | obj %d def %d at %.1f %.1f %.1f k %.2f", i, O[i].def, O[i].W[0][3], O[i].W[1][3], O[i].W[2][3], O[i].cw);
        break;
    }
    if (pos < n) snprintf(out + pos, n - pos, " | recording %d: %u frames, %u actions", REC.state, REC.nB, REC.nA);
}
