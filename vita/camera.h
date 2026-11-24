/* The race camera: the PC's ChaseCameraLogic (docs/re/re_K.md). A leash behind the car, pulled in
 * against static level collision by a ray from the car (0.07 off the wall it hits), the Normal /
 * Far / Bumper views, the speed FOV (60..75), and the death / finish cameras that stay where they
 * are and turn after their target. The PC's per-frame lerps (60 fps) are turned into rates. */
#ifndef NR_CAMERA_H
#define NR_CAMERA_H
#include "gfx.h"
#include "../src/nr_car.h"
#include "../src/nr_coll.h"

typedef struct {
    v3 pos, x, y, z;               /* the camera's frame: right, up, back (it looks along -z) */
    v3 up;                         /* the smoothed up (not normalised, as on the PC) */
    float T, W, s, lb, bb, fade, fov;   /* tumble, wings blend, speed factor, Far / Bumper blends, fade from black, FOV */
    int far_view, bumper;
    float added, bump[3];          /* the vehicle's Added Camera Leash Distance and BumperCamPos_ */
    /* the free look (the right stick): the chase camera above is worked out without it, from where it was itself (bpos,
     * bx, by, bz, tgt: what it looks at), and the view is then turned round what it looks at by lyaw, lpitch (radians,
     * eased towards where the stick points, with their rates), as far out as the walls let it (ldist, eased back out) */
    v3 bpos, bx, by, bz, tgt; int has_base;
    float lyaw, lpitch, lyaw_v, lpitch_v, ldist;
} nr_cam;

void cam_reset_to(nr_cam *c, const nr_car *car, nr_coll *coll);          /* Reset + snap behind the car */
/* one frame of the chase camera; pitch_in: the wings' pitch input, deploy: how far the wings are out
 * (0..1), no_speed: no speed FOV (the credits); tx, ty: the free look (the Advanced scheme's right stick through
 * cam_stick_shape, -1..1, right and up positive: the view turns right, the camera rises and looks down) */
void cam_chase_step(nr_cam *c, const nr_car *car, nr_coll *coll, float dt, float pitch_in, float deploy, int no_speed, float tx, float ty);
void cam_track(nr_cam *c, v3 target, float spread, float dt);           /* the death / finish camera */
/* the countdown's camera (0x712dc3): round P (R: columns right, forward, up) at the clock T (-6 .. 0) */
void cam_intro(nr_cam *c, v3 P, const float R[3][3], float T, nr_coll *coll);
void cam_next_view(nr_cam *c);                                          /* Normal -> Far -> Bumper */
/* the right stick as read (-1..1 each way, down positive) to the free look's tx, ty: a round dead zone that starts from 0
 * at its edge (no jump), a gentle curve for small moves */
void cam_stick_shape(float rx, float ry, float *tx, float *ty);
#endif
