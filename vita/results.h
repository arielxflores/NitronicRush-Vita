/* The results screen (MenuManager 0x6e6290, re_N 11.3): a level's score table by category (Local, Default,
 * Developer where there is one), stepped with L/R ("<< Cat" / "Cat >>"). After a race: Quit, Continue,
 * Restart and the run's row lit; from a menu (the Arcade level screen's "Results", the pause menu's "High
 * Scores"): Back only. */
#ifndef NR_RESULTS_H
#define NR_RESULTS_H
#include "front.h"

enum { RES_NONE, RES_QUIT, RES_CONTINUE, RES_RESTART, RES_BACK };

/* level and its score style; from_menu; for a finished run: its Local rank (0-based, 99 = not in the ten)
 * and its time, score and biggest trick */
void res_open(const char *level, int style, int from_menu, int rank, float time, int score, int trick);
void res_refresh(void);                 /* the table again (after the run was put into the Local table) */
int  res_update(float dt, const fe_input *in);
void res_draw(float alpha);
int  res_category(void);
#endif
