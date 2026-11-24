/* How long the GPU took over each frame, and the dynamic resolution that goes by it. See gputime.c. */
#ifndef NR_GPUTIME_H
#define NR_GPUTIME_H
#include <stdint.h>

/* ---- the GPU's clock */
#define GT_FINAL_MS 3.0f                       /* the final pass (and the HUD) counted as this much GPU time a frame */
/* the speed blur (it comes on at about 1.3 s into a race, as the car gets going): its passes added 1.6 to 2.0 ms to the
 * Vita's frame in the bench (warp_speed, moto, MountainTop, Hardcore Gold and Black), about 0.8 of it in the final pass
 * - counted with the final pass's figure while on; and all of it ahead, for the frames measured before it came on (the
 * controller hears of a frame two frames late: the step missed a refresh or two at the start of most races) */
#define GT_BLUR_FINAL_MS 0.8f
#define GT_BLUR_MS 1.8f
extern float gt_final_ms;                      /* (to try another: the switch file "dynres") */
void gt_init(void (*log)(const char *fmt, ...));
void gt_mark_offscreen(void);                  /* once the final pass has begun (its first draw is in): the scenes before it are all handed over */
void gt_mark_scene3d(void);                    /* once the first screen pass has begun: the 3D scene is handed over */
extern float gt_take_3d;                       /* (the last measurement gt_take gave: the 3D scene's part of off_ms; -1 not known) */
extern int gt_take_tag;                        /* (and the tag its frame was handed over with) */
/* just before the frame is handed over (vglSwapBuffers): when its first scene went to the GPU and the size its 3D
 * scene was drawn at */
void gt_submit(uint64_t kick_us, float scale, int tag);   /* (tag: the caller's, given back with the measurement) */
void gt_confirm(void);                         /* just after: checks the frame's last scene was the one waited for */
/* the next frame's measurement not yet taken: the GPU's time (ms) over its 3D scene and glow passes, the time from
 * there to the end of its final pass (that pass's work, and with the 60 fps cap on its wait for the screen's
 * buffer), and the size its 3D scene was drawn at; 0 when none */
int gt_take(float *off_ms, float *fin_ms, float *scale);
int gt_working(void);                          /* measurements have come in the last half second */

/* ---- the dynamic resolution's controller (plain arithmetic: tools/pc/drtest.c runs it against made-up scenes) */
#define DR_MIN 0.60f                           /* the smallest 3D scene (of the screen's width and height) */
#define DR_STEP 0.05f
#define DR_HI 15.0f                             /* a frame's GPU time (ms) past which the scene shrinks */
#define DR_GOAL 14.3f                           /* ... to bring it down to this */
#define DR_UP 14.1f                             /* and it grows while the last half second's worst would stay under this */
#define DR_WIN 30
extern float dr_hi, dr_goal, dr_up, dr_min;    /* (those, to try others: the switch file "dynres") */
typedef struct {
    float scale;                               /* the 3D scene's size now: DR_MIN..1 in steps of DR_STEP */
    float fixed;                               /* the GPU time (ms) the 3D scene's size does not change (learnt) */
    float pk[DR_WIN]; int npk, ipk;            /* the last frames' GPU times, brought to this size */
    float since_up, up_wait, up_back, calm;    /* s since the last step up; no step up for up_wait s; its doubling */
    float bg[3], bs; int bn;                   /* learning: the last three times at the size before a change */
    float ag[3], as; int an, learning;         /* ... and the first three after it */
    float trig_ms, trig_s, stuck_ms; int trig, stuck; /* the time and size of the last step down, till a frame at the new size checks it;
                                                  * stuck: it did not help (no more steps down till heavier than stuck_ms) */
    int downs, ups, holds;                     /* (the log: steps since the reset; steps down not taken: they would not have helped) */
    float lost;                                /* s without a measurement (the old way takes over: see main.c) */
} dr_t;
void dr_reset(dr_t *d);
/* a frame's measurement: its GPU time and the size it was drawn at. Returns the size to draw the next frame at */
float dr_sample(dr_t *d, float ms, float scale);
void dr_time(dr_t *d, float dt);               /* once a frame: the clocks */
#endif
