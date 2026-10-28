/* Ghost cars (re_N_ghosts.md): the PC's ReplayManager. A ghost is a recording of every object that
 * has a ReplayRecorder (the car, and the wreck pieces when it explodes) at a fixed 0.06 s tick, kept in
 * the PC's NrGhostBin format (PC and Vita ghost files are the same). Played back: the named ghost
 * blueprints (models/ghosts.txt: CarGhost and its wings, DeadCarGhost* pieces, ...) follow the
 * records, interpolated, in the additive "shockwave" look with a colour per ghost and the runner's
 * initials above the car. */
#ifndef NR_GHOST_H
#define NR_GHOST_H
#include <stdint.h>
#include "gfx.h"
#include "../src/nr_pak.h"

#define GH_FILES 10

int  gh_init(nr_pak *pak);                 /* models/ghosts.txt and the part models; 0 = ok */
/* playback */
void gh_clear(void);                       /* ClearGhosts: every loaded replay dropped */
int  gh_load(uint8_t *data, uint32_t size, const char *initials);   /* takes data (malloc'd); its index, -1 if not a ghost */
int  gh_count(void);
void gh_rewind(void);                      /* every replay back to its start, nothing on screen */
void gh_start(void);                       /* GO: the replays run */
void gh_labels_fade(int in);               /* the initials fade in (1, at GO) or out (0, at the player's finish) */
void gh_update(float dt);                  /* each frame (dt 0 = paused): the ticks (playback, then the recording) */
/* the ghosts' parts as draw objects (additive); dims the cars near the camera as the PC (needs VP) */
int  gh_draw(gfx_obj *objs, int nobj, int max);
void gh_draw_labels(float dt);             /* the initials, between hud_begin and hud_end */
/* the recording */
void gh_rec_reset(void);                   /* ResetRecording: empty, armed */
void gh_rec_go(void);                      /* GO: recording */
void gh_rec_stop(void);
int  gh_rec_on(void);
int  gh_rec_add(const char *name);         /* an object starts being recorded (as this ghost blueprint); its id */
void gh_rec_remove(int id);
void gh_rec_wings(int id, int out);
void gh_rec_pose(int id, const float R[3][3], const float pos[3]);   /* its pose now (taken at the next tick) */
int  gh_rec_frames(void);
int  gh_rec_save(const char *path);        /* writes the NrGhostBin file; 0 = ok */
int  gh_rec_actions(void);
/* the results' replay of this run (FailedRunCamera, re_N 12): a copy of the recording as "S" blueprints */
int  gh_self_add(void);                    /* -1 if the run is too short (under 50 actions) */
void gh_self_clear(void);
int  gh_self_count(void);                   /* the copies kept (a Challenge level keeps each failed attempt) */
void gh_restart_all(void);                 /* every replay (the copy and the ghosts) from its start, running */
int  gh_self_state(void);                  /* 3 playing, 4 ended, 0 no copy */
int  gh_self_car(float W[4][4]);           /* the copy's car (FailedReplayCar): 1 and its matrix if it is there */
/* autoplay's pilot (a shipped replay the test drive follows; not drawn) */
int  gh_pilot_load(uint8_t *data, uint32_t size);
int  gh_pilot_same(const uint8_t *data, uint32_t size);
int  gh_pilot_car(float W[4][4], int *wings, int *alive);   /* 1 while the replay runs; alive: its car is there (W) */
void gh_stats(char *out, int n);           /* for the log */
/* a replay's car each 0.06 s tick (malloc'd *out; the count, -1 if not a replay): pose (qw qx qy qz, x y z), there, wings out */
typedef struct { float q[4], p[3]; uint8_t alive, wings; } gh_trackpt;
int  gh_track(const uint8_t *data, uint32_t size, gh_trackpt **out);

#endif
