/* The story's cinematics (docs re_O; the PC's CampaignIntroLevel 0x72ac90, its cinematic helper 0x6d6050 and the
 * corespincam's SpinFocusCamera 0x6d14e0): the intro's phases and camera path, the typed terminal text (bottom left,
 * 8 lines), the story lines (top right), the red/blue screen effect's radii, the hold-to-skip prompt. The ending
 * (CampaignOutroLevel) uses the terminal and the effect too. Texts from config/cinema_config.ini (ui/cinema_config.txt). */
#ifndef NR_CINEMA_H
#define NR_CINEMA_H
#include "../src/nr_pak.h"

typedef struct {
    float eye[3], fwd[3], up[3];        /* the camera (FOV 45) */
    float bright;                       /* the scene's brightness (0 black .. 1.5 the eye's flashes) */
    float f2, f3;                       /* the screen effect: ring and sphere (radius 1.2^F) */
    float eye_glow, lightning;          /* the CORE's eye glow (FlickerOnHack), CoreArmsUpper's max lightning distance */
} cin_view;

int  cin_init(nr_pak *pak);             /* reads the texts; 0 = ok */
/* the intro: begin, then one frame at a time (1 when it is over) */
void cin_intro_begin(void);
int  cin_intro_update(float dt, cin_view *v);
/* the ending: the terminal and the effect; path_t = the camera path's time, fade = FadeTexts factor (1 = none) */
void cin_outro_begin(void);
void cin_outro_update(float dt, float path_t, float fade, cin_view *v);
void cin_draw(void);                    /* the terminal and story texts (2D, between hud_begin / hud_end) */
void cin_end(void);
/* SkipIntroLogic: any_down = a button went down this frame; the prompt is shown 0.25 s then fades over 1 s */
void cin_skip_update(float dt, int any_down);
void cin_skip_draw(void);
#endif
