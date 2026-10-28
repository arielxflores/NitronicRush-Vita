/* The front end: splash screens, title, main menu, Story/Arcade level selection,
 * achievements, controls, options and credits, over the game's 3D menu scene. */
#ifndef NR_FRONT_H
#define NR_FRONT_H
#include "gfx.h"
#include "save.h"

typedef struct {
    int up, down, left, right, accept, back, start, any;     /* pressed this frame (with repeat) */
    int l, r, alt;                                           /* shoulder buttons, Triangle pressed */
    int btn;                                                 /* a button pressed this frame (controls.h BTN_*), -1 none */
    /* the front touch screen, in the 960x544 screen's points */
    int tap; float tx, ty;                                   /* a tap this frame (lifted without dragging): where it began */
    int touch; float hx, hy;                                 /* a finger is down now: where */
    int swipe_x, swipe_y;                                    /* the finger moved 48 points: -1 left / up, +1 right / down */
} fe_input;

typedef struct { char xml[56]; char name[96]; int music, unlocked, sd, wings, score; char img[56]; int installed; } fe_level;
typedef struct { char file[48]; char name[64]; char desc[256]; int n; fe_level lv[24]; } fe_set;
extern fe_set FE_SETS[12];
extern int FE_NSETS;

typedef struct { char key[24]; char name[32]; int unlocked; char desc[96], locked_desc[96], img[32], img_locked[32]; } fe_vehicle;
extern fe_vehicle FE_VEH[8];
extern int FE_NVEH;
int  vehicle_unlocked(int i);

typedef struct { char name[48]; char desc[128]; int hidden; } fe_ach;   /* hidden: the 18 the PC never awards nor lists */
extern fe_ach FE_ACH[64];
extern int FE_NACH;

enum { FE_SPLASH, FE_TITLE, FE_MAIN, FE_SETS_SCREEN, FE_LEVELS, FE_ACHIEVEMENTS, FE_CONTROLS, FE_OPTIONS, FE_CREDITS, FE_QUIT,
       FE_TUTPROMPT, FE_VEHICLES, FE_GHOSTSEL, FE_LEVELRESULTS };
enum { FE_STAY = 0, FE_PLAY, FE_EXIT, FE_DONE };     /* FE_DONE: an overlay (front_overlay) was closed */
#define FE_LEVEL_INTRO (-2)                          /* FE_PLAY's level: the story's intro (CampaignIntroLevel) */

int   front_init(nr_pak *pak, save_t *save, const char *save_path);
void  front_enter(int screen);
int   front_story(void);                     /* the levels came from "Story" (not Arcade) */
/* returns FE_PLAY with set and level filled when a level was chosen, FE_EXIT to quit */
int   front_update(float dt, const fe_input *in, int *set, int *level);
void  front_draw(void);                      /* 2D, between hud_begin/hud_end */
void  front_camera(float dt, v3 *eye, v3 *fwd, float *fade);
int   front_screen(void);
/* a menu screen over the paused race (Achievements, Controls, Options): Back returns FE_DONE */
void  front_overlay(int screen);
int   front_ghost_cat(void);                 /* the ghosts chosen in "Select Ghosts to Challenge" (scores.h SC_*) */
/* the PC's confirmation boxes ("Restart Current Level?", "Quit?" ...): a title and up to three buttons */
typedef struct { char title[48]; const char *item[3]; int n, sel, open; float t; } ui_dlg;
void  ui_dlg_open(ui_dlg *d, const char *title, int n, const char *a, const char *b, const char *c);
int   ui_dlg_update(ui_dlg *d, const fe_input *in);  /* the button chosen, -1 still open, -2 backed out */
void  ui_dlg_draw(const ui_dlg *d);
void  vehicle_unlock(const char *key);       /* UnlockedVehicles + the "Vehicle Unlocked" popup (once) */
void  front_set_ghost_cat(int cat);
extern int FE_LEVEL_SCREENS;                 /* screens broken in this level attempt (Smash TV's progress) */
void  front_set_dummies(const float car[3], const float fly[3]);
/* shared widgets (pause and results screens use them too) */
void  ui_button(float cx, float cy, float w, float h, const char *label, int selected, float alpha);
int   ui_hit(const fe_input *in, float cx, float cy, float w, float h);   /* a tap inside the box centred there */
int   ui_menu(const char **items, int n, int *sel, const fe_input *in, float cx, float y0, float w, float h, float gap);
void  ui_slider(float x, float y, float w, float v, int selected);
void  ui_toggle(float x, float y, int on, int selected);
void  ui_popup_update(float dt);
void  ui_popup_draw(void);
void  ui_popup(int kind, const char *text);   /* 0 achievement, 1 level unlocked */
/* achievements by name; shows the popup and saves */
void  ach_unlock(const char *name);
int   ach_count(void);
int   ach_visible(void);                     /* how many the menu lists */
/* music for a level set's "music" value (the game's own table) */
const char *music_event(int music);
const char *music_layer_name(int music);
void  front_apply_volumes(void);
void  front_menu_level(void);           /* the menu loading after a level: its sounds (Stop_SFX, Stop_Announcer, "Nitronic Rush!") */
#endif
