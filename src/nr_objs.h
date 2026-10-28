/* Level objects that move, react or animate: the PC game's logic components, ported from
 * NitronicRush.exe (docs/re/re_A..E.md). converter/nr_objs.py writes them into the level's .nrg as
 * "obj" lines (placed objects) and "spawn" lines (blueprints the logic creates at run time).
 *
 *   saws, crushers, fans (StaticSawLogic), sliding saws (BackAndForthSawLogic), drones, the blimp
 *   and its searchlights, the train and its track, fake spotlights, laser gates (LaserLogic),
 *   sweeping and spinning lasers, holographic blockers and their solid blocks, air mines, shard
 *   buildings, spike barrels, breakable streetlights (flicker, beat pulse) and screens, antenna
 *   flares (ScalePulseLogic), DistanceColorChange, the core's lightning and ball, the core's
 *   braking field, trigger zones (achievements, the story's end).
 *
 * Portable C (no GL): the game draws the objects with their models and plays their sounds through
 * the host callbacks. World is Z-up, as in the PC game. */
#ifndef NR_OBJS_H
#define NR_OBJS_H
#include <stdint.h>
#include "nr_coll.h"
#include "nr_car.h"

enum {
    OK_NONE, OK_SAW, OK_BAFSAW, OK_SAWPATH, OK_DRONE, OK_BLIMP, OK_SEARCHLIGHT, OK_TRAIN, OK_TRAINCAR, OK_TRACK,
    OK_SPOT, OK_LASERARCH, OK_BEAM, OK_SWEEP, OK_SPINLASER, OK_SINGLELASER, OK_LASERDOT, OK_BLOCKER, OK_BLOCK,
    OK_MINEPARENT, OK_MINE, OK_SHARDBLD, OK_SHARD, OK_SHARDTARGET, OK_FX, OK_BARRELSPAWNER, OK_BARREL, OK_DEBRIS,
    OK_STREETLIGHT, OK_SCREEN, OK_ANTENNA, OK_DISTCOLOR, OK_COREBALL, OK_SPARK, OK_FORCEFIELD, OK_TRIGGER, OK_RECV,
    /* the bonus levels' movers (docs/re/re_F1.md) and the cooling, finish line (re_F2.md) */
    OK_SLIDER, OK_SWINGER, OK_STOMPER, OK_FOOT, OK_MORTAR, OK_MORTARMINE, OK_DROPSHIP, OK_DROPBLOCK, OK_SHIP,
    OK_COOLPAD, OK_CARCOOLER, OK_FINISH,
    OK_EMITTER,                              /* ParticleEmitter (the game runs the particles), CameraFollower */
    OK_FISHZONE, OK_FISH,                    /* FishZone, Flocking (the Observatory's fish) */
    OK_GARAGE, OK_HUMVEE, OK_HVPART, OK_PHASER, /* the CORE drones (docs/re/re_J.md) */
    OK_COREEXPL, OK_EYE,                     /* the story's end: CoreExploder, the core's eye (re_E) */
    OK_CHECKPOINT                            /* CheckpointLogic's gate: its colour as it is passed */
};

#define NR_OBJ_KIDS 16
typedef struct {
    uint8_t kind, alive, visible, dynamic;   /* dynamic: a free body (barrels, debris), never kills the car */
    uint8_t cmat, matid, sndloop, pad;
    int16_t model, cmodel, proto, parent;
    int id;                                  /* id in the .nrg (tags its static collision), 0 when spawned */
    float pos[3], R[3][3], scale[3];         /* R: columns = the object's axes in the world */
    float vel[3], angvel[3];                 /* for contacts with the car (kinematic motion) */
    float col[4], base[4];                   /* RenderMesh Color1 now / as authored: rgb, glow */
    float pulse, death_speed, radius;        /* LightPulse intensity (0: none), CustomDeathSpeed, bounds */
    float bmin[3], bmax[3];                  /* the model's box, own space */
    float t, f[24]; int s[8];                /* logic state */
    int16_t kid[NR_OBJ_KIDS]; int nkid;      /* objects it spawned and moves */
    float lpos[3], lR[3][3], lscale[3];      /* own transform relative to the parent */
    const char *line;                        /* its .nrg line (parameters) */
    char snd[40]; int snd_slot;              /* looping sound (the game plays it while the car is near) */
} nr_obj;

typedef struct { char bp[40]; const char *line; } nr_proto;
typedef struct { char name[48]; uint32_t n; float (*t)[9]; float r; float (*bb)[6]; } nr_cmodel;   /* bb: each triangle's box, own space */

typedef struct {
    void *user;
    /* a model (models/<name>.nrl) -> handle (-1: none), with its box */
    int (*model)(void *user, const char *name, float bmin[3], float bmax[3]);
    /* a file from the packs (malloc'd) */
    uint8_t *(*read)(void *user, const char *path, uint32_t *size);
    /* a one-shot 3D sound */
    void (*sound)(void *user, const char *event, const float pos[3]);
    /* an RTPC on an object's looping sound */
    void (*rtpc)(void *user, int obj, const char *name, float value);
    /* game events: "achievement" (arg = PC achievement id), "EndStory", "streetlight", "screen" */
    void (*event)(void *user, const char *what, int arg);
} nr_objs_host;

typedef struct {
    nr_obj *o; int n, cap;
    nr_proto *proto; int nproto;
    nr_cmodel *cm; int ncm;
    struct { char name[48]; int h; float bmin[3], bmax[3]; } *mdl; int nmdl;
    nr_coll *coll;
    uint8_t *off; uint32_t noff;             /* static collision switched off, by .nrg id */
    nr_coll_body *bodies;                    /* moving colliders' motion, by object index */
    nr_objs_host host;
    char *text; int textlen;
    uint32_t rng;
    float time;
    int screens_broken, lights_broken;      /* this run */
    int reuse;
    /* the trigger system's results for the game (TriggerRespawnLocation, TriggerTutorialText) */
    char vehicle[32];                       /* the vehicle's name (TriggerBranchOnVehicle) */
    int advanced;                          /* the Advanced control scheme (TriggerControlScheme's Advanced texts) */
    int respawn_set; float respawn_pos[3], respawn_R[3][3];
    const char *tutorial;                   /* the TriggerTutorialText line that fired (read with nr_objs_value) */
    int depth;
    int fish_zone, fish_zone_changed;        /* FishZone's manager: the zone nearest the car */
    /* the CORE drones: a car each (src/nr_car.c with the GunHumvee blueprint's handling) */
    struct nr_humvee *hv; int nhv;
    nr_car_params hvp; int hvp_loaded;
    int outro;                               /* the ending's camera is running (CoreExploder works only then) */
} nr_objs;

typedef struct {
    /* in */
    float car_pos[3], car_vel[3], car_rot[3][3], car_half[3]; int car_active;
    float cam_pos[3], cam_rot[3][3];         /* columns: right, up, back (the PC's right-handed camera) */
    float beat;                              /* LightPulse: clamp(1 - seconds since the music's last beat, 0, 1) */
    /* out */
    float heat;                              /* heat for the car (lasers) */
    float car_dv[3];                         /* explosions' push on the car */
    float car_vscale;                        /* the core's braking field: the car's velocity x this */
    float shake;
    float cool;                              /* CooldownPadLogic: heat off per second (PC units, 0.5 x speed) */
    int cool_glow;                           /* the car's green cooling glow (pads, CarCooler) */
    int car_cooler;                          /* CarCooler in the level: boost heat x -6 */
    int finished;                            /* the car crossed a finish line (EnumLogic type 6) */
    float car_push[3];                       /* a CORE drone pushed the car out of itself */
} nr_objs_frame;

typedef struct {
    int death;                               /* 2: a deadly surface (Material ID 1), 3: crashed */
    int restore_vel;                         /* drove through a streetlight: back to last frame's velocity */
    int fold_wings, scraping, crash_sound;   /* crash_sound: 1 small, 2 medium */
    float shake;
    float spark[3], spark_n[3], spark_rate;  /* the strongest scrape */
    float dv[3];                             /* a mine's blast on the car */
    float death_p[3], death_n[3];
} nr_objs_hit;

int  nr_objs_load(nr_objs *W, const char *nrg, nr_coll *coll, const nr_objs_host *host);
void nr_objs_free(nr_objs *W);
void nr_objs_restart(nr_objs *W);          /* every object back as the level starts */
void nr_objs_update(nr_objs *W, nr_objs_frame *f, float dt);
/* moving objects' triangles near 'at' into coll->dyn (before the car's physics steps) */
void nr_objs_collision(nr_objs *W, const float at[3], float reach);
/* CarLogic's collision rules (0x709290) over the car's contacts of this frame */
void nr_objs_car_contacts(nr_objs *W, nr_car *c, int wings_out, nr_objs_hit *h);
void nr_objs_matrix(const nr_obj *o, float m[4][4]);
int  nr_objs_count(const nr_objs *W, int kind);
/* the trigger system: send a message to every receiver whose Incoming is ev */
void nr_objs_broadcast(nr_objs *W, const char *ev);
void nr_objs_level_finished(nr_objs *W);   /* LevelFinishedMessage: TriggerAtFinish */
void nr_objs_checkpoint_reset(nr_objs *W); /* PlayerResetAtCheckpointMessage: the drop ships */
void nr_objs_checkpoint_passed(nr_objs *W, const float pos[3], const float R[3][3]);   /* the gate's ghosts */
/* the ending (CampaignOutroLevel, re_E 7): start the core's destruction; the SpinFocusCamera's path
 * at 'time' seconds and where it looks (focus is kept between calls) */
void nr_objs_outro_start(nr_objs *W);
void nr_objs_outro_camera(nr_objs *W, float time, float dt, float eye[3], float focus[3]);
int  nr_objs_find(nr_objs *W, const char *name);   /* an object by its name (-1: none) */
/* a value from an obj line (key = "Component.Field"), '_' read back as spaces when spaces is set */
const char *nr_objs_value(const char *line, const char *key, char *buf, int n, int spaces);
#endif
