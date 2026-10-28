/* The local score tables (the PC's ScoreManager, re_I 3 and re_N 4.4/11): per level the ten best runs
 * (initials, time, score, biggest trick, date), seeded from the game's shipped table the first time,
 * kept as the PC keeps them - scores/Local/<level>.xml/scores.NrScoreSet next to the runs' ghosts
 * <level>.<rank>.NrGhostBin - so PC files can be copied over. */
#ifndef NR_SCORES_H
#define NR_SCORES_H
#include "../src/nr_pak.h"

typedef struct { char initials[8]; float time; int score, trick; char date[16]; } sc_entry;
typedef struct { char level[56]; int style, n; sc_entry e[12]; } sc_table;
enum { SC_LOCAL = 1, SC_DEFAULT = 2, SC_DEVELOPER = 3 };

void sc_init(nr_pak *pak, const char *root);         /* root: the folder for scores/ (ends in '/') */
/* the table of a category; Local is created from the shipped one (and its ghosts copied) when missing */
int  sc_get(int cat, const char *level, int style, sc_table *t);
int  sc_has(int cat, const char *level);              /* a category with a table for the level (Default always; Developer: Calcite, Ruby) */
/* where a run with this time/score/trick lands in the table (ComputeRank 0x6f3d80) */
int  sc_rank(const sc_table *t, float time, int score, int trick);
/* the run into the Local table at rank (kept to ten) and the table saved (0x6f4ae0) */
int  sc_insert_local(const char *level, int style, int rank, const char *initials, float time, int score, int trick);
/* the ghost files of the Local table: path of rank r; make room at rank r (files r..8 one down, 9 deleted) */
void sc_ghost_path(char *out, int n, const char *level, int rank);
void sc_ghost_shift(const char *level, int rank);
/* the ghost file of the shipped table's rank r, from the packs (malloc'd), or NULL */
void *sc_default_ghost(const char *level, int rank, uint32_t *size);
const char *sc_category_name(int cat);
void sc_format_rank(char *out, int n, int rank);    /* 0 -> "1st" ... (0x6f2870) */

#endif
