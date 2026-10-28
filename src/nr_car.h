/* The player's car, as the PC game drives it: a BodyRigid on four raycast wheels (ControllerCar),
 * wings (ControllerWings) and the CarLogic that turns the pad into throttle, brake, steering, jumps,
 * boost and flight. The numbers and the order of operations come from NitronicRush.exe; the
 * tuning values from the vehicle's blueprint (models/<vehicle>.cfg).
 * Portable C: runs on the Vita and in the PC test build. */
#ifndef NR_CAR_H
#define NR_CAR_H
#include "nr_coll.h"

#define NR_CAR_DT 0.005f          /* PhysicsManager: fixed 200 Hz steps */
#define NR_CAR_HULL_MAX 8

typedef struct {
    /* ControllerCar */
    int nwheels;
    float wheel_pos[4][3], wheel_steer[4], wheel_brake[4], wheel_brake_max[4];
    float fric_fs[4], fric_fk[4], fric_ss[4], fric_sk[4];      /* front/side, static/kinetic */
    int wheel_drive[4];
    float radius, spring_min, spring_max, spring_rest, spring_k, max_spring_force, damp_comp, damp_relax;
    float fwd_force, rev_force, side_roll, front_roll;
    float drive_rest, roll_rest, drive_fric, roll_fric;          /* body collision while driving / in the air */
    /* BodyRigid (+ the inertia ColliderConvexMesh works out from the hull's box) */
    float drag[3], rot_factor, inv_mass, com[3], inv_inertia[3];
    float hull_min[3], hull_max[3];
    int nhull; float hull[NR_CAR_HULL_MAX][4];                 /* spheres standing in for the hull */
    /* CarLogic, ControllerWings */
    float jump_impulse, brake_time, steer_red_start, steer_red_factor, boost_force;
    float wing_pitch, wing_roll, wing_yaw;
    float gravity;
} nr_car_params;

typedef struct {
    float pos[3];
    float rot[3][3];          /* columns: right, forward, up (world) */
    float q[4];               /* the same orientation as a quaternion (x y z w) */
    float vel[3], angvel[3];
    int grounded;             /* wheels touching the ground */
    float wheel_len[4], wheel_force[4], wheel_spin[4], wheel_slip[4];
    int wheel_contact[4], wheel_skid[4];
    float wheel_n[4][3];      /* ground normal under each wheel (last contact) */
    float wheel_cp[4][3];     /* where each wheel touches it (last contact; the tyre tracks) */
    int body_touch;           /* the body touched the level during this frame's steps */
    /* what CarLogic hands the wheels */
    float steer, throttle, brake;
    int mode_fwd, mode_rev; float rev_timer;
    float boost_ramp; int boosting;
    float deploy; float wing_q[4]; int wings_were_on;
    float lean_roll, lean_pitch, lean_yaw;
    float cam_right[3], cam_up[3];  /* the chase camera, set by the game every frame (the wings use it) */
    float air_time, last_hit;
    int jumped;               /* set by nr_car_frame when a jump impulse was applied */
    int last_hull; uint32_t last_mat; float last_n[3];   /* last body contact, for sounds and diagnostics */
    /* the body's contacts during this frame's physics steps (CarLogic's CollisionInfoCollector):
     * normal (out of the surface, into the car), point, closing speed along the normal, sliding
     * speed, the surface's material word (nr_coll.h) */
    struct { float n[3], p[3], d, slide; uint32_t mat; } contact[16];
    int ncontact;
} nr_car;

typedef struct {
    float steer;              /* left stick x, > 0 = left (CarTurn) */
    float throttle;           /* gas 0..1 (CarGas) */
    float brake;              /* brake, then reverse, 0..1 (CarBrake) */
    float pitch;              /* left stick y, > 0 = nose up (CarPitch) */
    float roll_x, roll_y;     /* right stick: barrel rolls (CarRollX / CarRollY) */
    int jump;                 /* pressed this frame */
    int boost;                /* held */
    int wings;                /* the wings are out (the game decides when they may be) */
} nr_car_input;

void  nr_car_default_params(nr_car_params *p);
int   nr_car_load_params(nr_car_params *p, const char *cfg);          /* models/<vehicle>.cfg text */
void  nr_car_set_hull(nr_car_params *p, const float mn[3], const float mx[3]);   /* the collision hull's box */
void  nr_car_reset(nr_car *c, const float pos[3], const float fwd[3]);
/* at pos with the orientation R (columns right, forward, up), standing still (CarLogic's respawn) */
void  nr_car_reset_rot(nr_car *c, const float pos[3], const float R[3][3]);
/* once per frame, before the physics steps: CarLogic (steering, gas/brake/reverse, leaning,
 * jumping, wings, boost) */
void  nr_car_frame(nr_car *c, const nr_car_params *p, nr_coll *w, const nr_car_input *in, float dt);
/* one physics step (dt = NR_CAR_DT): wheels, wings, integration, body collision */
void  nr_car_step(nr_car *c, const nr_car_params *p, nr_coll *w, float dt);
float nr_car_speed(const nr_car *c);
float nr_car_forward_speed(const nr_car *c);
/* the game's speedometer (0x62e260): forward speed on the wheels, else the speed, in its MPH */
float nr_car_mph(const nr_car *c);

/* simple driver that follows the level's route (for automatic tests) */
typedef struct { uint32_t idx; float stuck; int respawns; float best; int boosting; } nr_ai;
void nr_ai_drive(nr_ai *ai, const nr_car *c, const float (*path)[3], const float *plen, uint32_t n, nr_car_input *out);
/* the same, plus jumping over walls ahead (needs the level collision) */
void nr_ai_drive_world(nr_ai *ai, const nr_car *c, nr_coll *w, const float (*path)[3], const float *plen, uint32_t n, nr_car_input *out);
/* puts the car back on the route when it fell off the level, flipped over or got stuck.
 * returns 1 when it did; *why says what happened */
int nr_car_watchdog(nr_car *c, nr_ai *ai, const float (*path)[3], const float *plen, uint32_t n,
                    float minz, float dt, float *bad_time, const char **why, int strict);
/* put the car on the route at the point nearest to it (or at route index 'at' if >= 0) */
void nr_car_place_on_route(nr_car *c, nr_ai *ai, const float (*path)[3], const float *plen, uint32_t n, int at);
/* index of the route point nearest to p, searching forward from 'from' */
uint32_t nr_route_nearest(const float (*path)[3], uint32_t n, const float p[3], uint32_t from, uint32_t window);
#endif
