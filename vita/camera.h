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
} nr_cam;

void cam_reset_to(nr_cam *c, const nr_car *car, nr_coll *coll);          /* Reset + snap behind the car */
/* one frame of the chase camera; pitch_in: the wings' pitch input, deploy: how far the wings are out
 * (0..1), no_speed: no speed FOV (the credits); tx, ty: CamTurnX / CamTurnY (the Advanced scheme's right stick,
 * -1..1, up positive) */
void cam_chase_step(nr_cam *c, const nr_car *car, nr_coll *coll, float dt, float pitch_in, float deploy, int no_speed, float tx, float ty);
void cam_track(nr_cam *c, v3 target, float spread, float dt);           /* the death / finish camera */
/* the countdown's camera (0x712dc3): round P (R: columns right, forward, up) at the clock T (-6 .. 0) */
void cam_intro(nr_cam *c, v3 P, const float R[3][3], float T, nr_coll *coll);
void cam_next_view(nr_cam *c);                                          /* Normal -> Far -> Bumper */
#endif
