/* Settings, progress, best times, achievements and lifetime stats, kept in
 * ux0:data/nitronic/save.txt (plain text). */
#ifndef NR_SAVE_H
#define NR_SAVE_H
#include <stdint.h>

#define SAVE_MAX_LEVELS 128
typedef struct { char xml[56]; float best; int unlocked, done, score; } save_level;   /* best time; score: stunt mode's best */
typedef struct {
    float master, music, sfx;          /* 0..1 */
    int show_hud, invert_y, seen_intro, vehicle;
    int nlev; save_level lev[SAVE_MAX_LEVELS];
    uint64_t ach;                      /* achievement bits (index in ui/achievements.txt) */
    int overheats, highspeed_deaths, hard_resets, wrecks, tricks, smashed;   /* smashed: streetlights + screens */
    int streetlights, screens, most_screens, barrel_rolls;     /* LifetimeData: Streetlights / Screens Broken, Most Screens Broken
                                                                * On One Level, Barrel Rolls */
    float wall_dist, ceil_dist;                                /* Wall Ride / Ceiling Ride Distance (units) */
    float play_time;                   /* racing time (seconds), as the PC's Time Played */
    int points, biggest, best_score, gold_shown;   /* lifetime trick points, biggest trick, best score on one level */
    int bind[12], swap_sticks;          /* the race controls (controls.h: button of each action), sticks swapped */
    int dyn_res;                       /* Options: Dynamic Resolution (the 3D scene smaller while under 30 fps) */
    int ghosts;                        /* Arcade: " Ghost Replays" (the PC's "Ghosts Enabled", on by default) */
    char initials[4];                  /* the name for the score tables (PlayerData "username", "AAA" at first) */
    int bloom, motion_blur, adv_controls;   /* Options: " Toggle Bloom", " Toggle Motion Blur", " Use Advanced Controls" (control_scheme 1) */
    float announcer;                   /* Options: Announcer Volume */
    unsigned vehicles;                 /* UnlockedVehicles: bit per vehicle (ui/vehicles.txt order) */
    int air_tricks;                    /* Controls: in the air without the wings the steering stick flips and rolls the car
                                        * as the trick stick does (not on the PC; on by default) */
} save_t;

void        save_defaults(save_t *s);
int         save_load(save_t *s, const char *path);   /* 0 = read */
int         save_write(const save_t *s, const char *path);
save_level *save_level_get(save_t *s, const char *xml, int create);
#endif
