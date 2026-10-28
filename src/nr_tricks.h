/* The PC game's TrickSystem2: barrel rolls, flips, frisbees and twists counted from how the car
 * turns in the air; wheelies, nose and side wheelies; wall and ceiling rides; grinds. Points,
 * the trick's heat relief and the announcer line are worked out when the car lands.
 * Ported from NitronicRush.exe (0x6dc3e0 and the functions it calls). Portable C. */
#ifndef NR_TRICKS_H
#define NR_TRICKS_H

typedef struct {
    int points; float mult;               /* the trick so far: points and the number of kinds */
    int contact[4], ncontact;             /* wheels down (FL FR RL RR) */
    float air_t, ground_t; int was_air;
    float timer;                          /* time since the trick last grew (dropped after 5 s) */
    float rot[3];                         /* turning since the last full turn, car axes (pitch roll yaw) */
    int rolls_r, rolls_l, backflips, frontflips, frisbee_l, frisbee_r, twist_f[2], twist_b[2];
    float wheelie[4], wheelie_gap[4];     /* wheelie, nose, left side, right side */
    int wall_pend, wall_on, ceil_pend, ceil_on;
    float wall_pdur, wall_pdist, wall_dur, wall_dist, wall_gap;
    float ceil_pdur, ceil_pdist, ceil_dur, ceil_dist, ceil_gap;
    float grind_dist, grind_gap;
    float total_miles;
} nr_tricks;

typedef struct {
    int contact[4];                       /* wheels touching */
    float wheel_n[4][3];                  /* their ground normals */
    float rot[3][3];                      /* car orientation, columns right forward up */
    float angvel[3];                      /* world angular velocity */
    float speed_mph;                      /* the game's speedometer value */
    float body_speed;                     /* |velocity|, game units per second */
    int body_contacts;                    /* the car's body touching something */
} nr_trick_in;

typedef struct {
    int score;                            /* points x kinds */
    int points, kinds;
    float heat_relief;                    /* fraction of the heat maximum to take off */
    const char *event;                    /* announcer line (sound event) */
    char text[256];                       /* trick lines separated by '|' */
    int barrel_roll;                      /* the announcer's Play_BarrelRoll (Do A Barrel Roll!, Barrel Rolls) */
    float wall_mi, ceil_mi;               /* the trick's wall / ceiling ride (miles: LifetimeData's distances) */
} nr_trick_award;

void nr_tricks_reset(nr_tricks *t);
/* once per frame; returns 1 and fills *aw when a trick is landed. ach_* report progress for the
 * achievements: wheelie / wall / ceiling time of the current trick */
int  nr_tricks_update(nr_tricks *t, const nr_trick_in *in, float dt, unsigned rnd, nr_trick_award *aw);
#endif
