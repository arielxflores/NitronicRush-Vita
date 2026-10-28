/* The test drive with the real physics: the car is driven (stick, pedals, jump, wings, boost, as a
 * player would) along the line of one of the PC's recorded runs. Where it can't keep up - off the line
 * for a while, or stuck - it is put back on the line ("an assist", logged), so a level's assists show
 * where the port's collision or handling differs from the PC's. For the automatic tests only. */
#ifndef NR_CHASE_H
#define NR_CHASE_H
#include <stdint.h>
#include "../src/nr_car.h"
#include "ghost.h"

typedef struct {
    gh_trackpt *pt; int n;              /* the replay's car each 0.06 s */
    float *s;                           /* distance along the line at each tick */
    uint8_t *jump;                      /* the replay jumped at this tick */
    int *tick;                          /* each point's tick in the replay (the line is the replay cleaned up) */
    int idx, last_jump;                 /* progress; the last jump used */
    float off_t, slow_t, flip_t, best_s, best_t, wing_cool, t;
    float heat;                         /* (set by the game: 0..1, the boost is left alone when hot) */
    int assists, resyncs;
    float death_p[3]; int deaths_here, death_idx, force;   /* (a crash at the same place twice: past it) */
    char why[48];
} ch_state;

int  ch_init(ch_state *s, const uint8_t *data, uint32_t size);   /* 0 = ok */
int  ch_init_route(ch_state *s, const float (*path)[3], int n);   /* (no recorded run: the level's route) */
void ch_free(ch_state *s);
void ch_restart(ch_state *s);
/* after a respawn: find the car's place on the line again */
void ch_resync(ch_state *s, const float pos[3]);
void ch_died(ch_state *s, const float pos[3]);      /* the car was wrecked here */
float ch_length(const ch_state *s);     /* the replay's time, s */
float ch_time(const ch_state *s);       /* the replay's time where the car is on the line */
/* this frame's inputs. wings: the wings are out now; *toggle_wings 1: press the wings button.
 * Returns 1 when the car should be put back on the line: *rp, *rR (columns right, forward, up), *rv */
int  ch_drive(ch_state *s, const nr_car *c, int wings, float dt, nr_car_input *in, int *toggle_wings,
              float rp[3], float rR[3][3], float rv[3]);
#endif
