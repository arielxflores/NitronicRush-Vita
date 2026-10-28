#include "scores.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include "io.h"
#ifdef NR_PC
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#else
#include <psp2/io/stat.h>
#define MKDIR(p) sceIoMkdir(p, 0777)
#endif

static nr_pak *PAK;
static char ROOT[128];
typedef struct { char level[56]; sc_entry e; } def_row;
static def_row DEFS[64]; static int ndefs;
static def_row DEVS[16]; static int ndevs;       /* the Developer category's tables (data/default_scores/Developer) */

void sc_init(nr_pak *pak, const char *root)
{
    PAK = pak; snprintf(ROOT, sizeof ROOT, "%s", root); ndefs = 0;
    uint32_t sz; char *t = nr_pak_read(pak, "ghosts/default_scores.txt", &sz);
    for (char *ln = t ? strtok(t, "\n") : NULL; ln; ln = strtok(NULL, "\n")) {
        def_row *d = &DEFS[ndefs];
        if (ndefs < 64 && sscanf(ln, "score %55s %7s %f %d %d", d->level, d->e.initials, &d->e.time, &d->e.score, &d->e.trick) == 5) {
            snprintf(d->e.date, sizeof d->e.date, "2011-Oct-02");
            ndefs++;
        }
    }
    free(t);
    ndevs = 0;
    t = nr_pak_read(pak, "ghosts/dev_scores.txt", &sz);
    for (char *ln = t ? strtok(t, "\n") : NULL; ln; ln = strtok(NULL, "\n")) {
        def_row *d = &DEVS[ndevs];
        if (ndevs < 16 && sscanf(ln, "score %55s %7s %f %d %d", d->level, d->e.initials, &d->e.time, &d->e.score, &d->e.trick) == 5) {
            snprintf(d->e.date, sizeof d->e.date, "2011-Nov-17");
            ndevs++;
        }
    }
    free(t);
}
int sc_has(int cat, const char *level)
{
    if (cat == SC_LOCAL || cat == SC_DEFAULT) return 1;
    if (cat == SC_DEVELOPER) for (int i = 0; i < ndevs; i++) if (!strcmp(DEVS[i].level, level)) return 1;
    return 0;
}

const char *sc_category_name(int cat)
{
    static const char *N[] = { "Temporary", "Local", "Default", "Developer", "Imported", "Online" };
    return cat >= 0 && cat <= 5 ? N[cat] : "?";
}
void sc_format_rank(char *out, int n, int r)
{
    int k = r + 1;
    const char *suf = (k % 100 >= 11 && k % 100 <= 13) ? "th" : k % 10 == 1 ? "st" : k % 10 == 2 ? "nd" : k % 10 == 3 ? "rd" : "th";
    snprintf(out, n, "%d%s", k, suf);
}

static void level_dir(char *out, int n, const char *level)
{
    snprintf(out, n, "%sscores/Local/%s.xml/", ROOT, level);
}
static void make_dirs(const char *level)        /* (queued with the writes: io.c does them in order) */
{
    char p[256];
    snprintf(p, sizeof p, "%sscores", ROOT); io_mkdir(p);
    snprintf(p, sizeof p, "%sscores/Local", ROOT); io_mkdir(p);
    snprintf(p, sizeof p, "%sscores/Local/%s.xml", ROOT, level); io_mkdir(p);
}
void sc_ghost_path(char *out, int n, const char *level, int rank)
{
    char d[200]; level_dir(d, sizeof d, level);
    snprintf(out, n, "%s%s.%d.NrGhostBin", d, level, rank);
}
void sc_ghost_shift(const char *level, int rank)           /* SaveGhost 0x6f4260 */
{
    char a[256], b[256];
    make_dirs(level);
    sc_ghost_path(a, sizeof a, level, 9); io_remove(a);
    for (int i = 9; i > rank; i--) {
        sc_ghost_path(a, sizeof a, level, i - 1); sc_ghost_path(b, sizeof b, level, i);
        io_rename(a, b);
    }
}
void *sc_default_ghost(const char *level, int rank, uint32_t *size)
{
    char p[128]; snprintf(p, sizeof p, "ghosts/%s.%d.ngb", level, rank);
    return nr_pak_read(PAK, p, size);
}

/* the tables written this session, kept here: the card may not have them yet (io.c writes them in the background) */
static struct { char path[256]; sc_table t; int used; } CACHE[16]; static int ncache_next;
static int read_table(const char *path, sc_table *t)
{
    for (int i = 0; i < 16; i++) if (CACHE[i].used && !strcmp(CACHE[i].path, path)) { *t = CACHE[i].t; return 0; }
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char ln[256]; sc_entry *e = NULL;
    while (fgets(ln, sizeof ln, f)) {
        char *q;
        if ((q = strstr(ln, "<ScoreSet")) && (q = strstr(q, "style=\""))) t->style = atoi(q + 7);
        else if (strstr(ln, "<Score ") || strstr(ln, "<Score>")) { e = t->n < 11 ? &t->e[t->n++] : NULL; if (e) memset(e, 0, sizeof *e); }
        else if (e && (q = strstr(ln, "name=\"initials\""))) { q = strchr(q, '>'); if (q) { q++; char *z = strchr(q, '<'); int k = z ? (int)(z - q) : 0; if (k > 7) k = 7; memcpy(e->initials, q, k); e->initials[k] = 0; } }
        else if (e && (q = strstr(ln, "name=\"date\""))) { q = strchr(q, '>'); if (q) { q++; char *z = strchr(q, '<'); int k = z ? (int)(z - q) : 0; if (k > 15) k = 15; memcpy(e->date, q, k); e->date[k] = 0; } }
        else if (e && (q = strstr(ln, "name=\"time\"")) && (q = strstr(q, "value=\""))) e->time = (float)atof(q + 7);
        else if (e && (q = strstr(ln, "name=\"score\"")) && (q = strstr(q, "value=\""))) e->score = atoi(q + 7);
        else if (e && (q = strstr(ln, "name=\"biggest_trick\"")) && (q = strstr(q, "value=\""))) e->trick = atoi(q + 7);
        else if (strstr(ln, "</Score>")) e = NULL;
    }
    fclose(f);
    return 0;
}
typedef struct { char *b; size_t n, cap; } sbuf;
static void bp(sbuf *o, const char *fmt, ...)
{
    va_list ap;
    for (;;) {
        size_t room = o->cap - o->n;
        va_start(ap, fmt); int k = vsnprintf(o->b ? o->b + o->n : NULL, o->b ? room : 0, fmt, ap); va_end(ap);
        if (k < 0) return;
        if (o->b && (size_t)k < room) { o->n += (size_t)k; return; }
        size_t nc = (o->cap ? o->cap * 2 : 2048) + (size_t)k;
        char *nb = realloc(o->b, nc);
        if (!nb) return;
        o->b = nb; o->cap = nc;
    }
}
static int write_table(const char *path, const sc_table *t, int cat)
{
    int slot = -1;
    for (int i = 0; i < 16; i++) if (CACHE[i].used && !strcmp(CACHE[i].path, path)) slot = i;
    if (slot < 0) { slot = ncache_next; ncache_next = (ncache_next + 1) % 16; }
    snprintf(CACHE[slot].path, sizeof CACHE[slot].path, "%s", path); CACHE[slot].t = *t; CACHE[slot].used = 1;
    sbuf o = { 0 }, *f = &o;
    bp(f, "<?xml version=\"1.0\" ?>\r\n<ScoreSet version=\"1\" style=\"%d\" >\r\n", t->style);
    bp(f, "  <String name=\"level_id\" >%s.xml</String>\r\n  <Integer name=\"category\" value=\"%d\" />\r\n", t->level, cat);
    for (int i = 0; i < t->n && i < 10; i++) {
        const sc_entry *e = &t->e[i];
        bp(f, "  <Score >\r\n    <String name=\"initials\" >%s</String>\r\n    <Float name=\"time\" value=\"%f\" />\r\n", e->initials, e->time);
        bp(f, "    <Integer name=\"score\" value=\"%d\" />\r\n    <Integer name=\"biggest_trick\" value=\"%d\" />\r\n", e->score, e->trick);
        bp(f, "    <String name=\"date\" >%s</String>\r\n  </Score>\r\n", e->date);
    }
    bp(f, "</ScoreSet>\r\n");
    if (!o.b) return -1;
    io_write_take(path, o.b, o.n);
    return 0;
}

static void defaults(const char *level, int style, sc_table *t)
{
    memset(t, 0, sizeof *t);
    snprintf(t->level, sizeof t->level, "%s", level); t->style = style;
    for (int i = 0; i < ndefs && t->n < 10; i++) if (!strcmp(DEFS[i].level, level)) t->e[t->n++] = DEFS[i].e;
}

int sc_get(int cat, const char *level, int style, sc_table *t)
{
    if (cat == SC_DEFAULT) { defaults(level, style, t); return 0; }
    if (cat == SC_DEVELOPER) {
        memset(t, 0, sizeof *t); snprintf(t->level, sizeof t->level, "%s", level); t->style = style;
        for (int i = 0; i < ndevs && t->n < 10; i++) if (!strcmp(DEVS[i].level, level)) t->e[t->n++] = DEVS[i].e;
        return 0;
    }
    char d[200], p[256]; level_dir(d, sizeof d, level);
    snprintf(p, sizeof p, "%sscores.NrScoreSet", d);
    memset(t, 0, sizeof *t); snprintf(t->level, sizeof t->level, "%s", level);
    if (read_table(p, t) == 0) {
        if (t->style != style) { t->n = 0; t->style = style; }     /* ComputeRank: another style empties it */
        return 0;
    }
    /* GetScoreSet 0x6f38f0: the first time, a copy of the shipped table and its ghosts (0x6f2a30) */
    defaults(level, style, t);
    make_dirs(level);
    for (int r = 0; r < 10; r++) {
        uint32_t sz; void *g = sc_default_ghost(level, r, &sz);
        if (!g) continue;
        char gp[256]; sc_ghost_path(gp, sizeof gp, level, r);
        io_write_take(gp, g, sz);
    }
    write_table(p, t, SC_LOCAL);
    return 0;
}

int sc_rank(const sc_table *t, float time, int score, int trick)      /* Beats 0x420320 */
{
    int st = t->style, r = 0;
    for (; r < t->n; r++) {
        const sc_entry *e = &t->e[r];
        int beats;
        if ((st & 1) && e->time != time) beats = time > e->time;
        else if ((st & 2) && e->score != score) beats = e->score > score;
        else if ((st & 4) && e->trick != trick) beats = e->trick > trick;
        else beats = time > e->time;
        if (!beats) break;
    }
    return r;
}

int sc_insert_local(const char *level, int style, int rank, const char *initials, float run_time, int score, int trick)
{
    sc_table t; sc_get(SC_LOCAL, level, style, &t);
    if (rank < 0) rank = 0;
    if (rank > t.n) rank = t.n;
    if (rank >= 10) return -1;
    memmove(&t.e[rank + 1], &t.e[rank], (size_t)(t.n - rank) * sizeof t.e[0]);
    sc_entry *e = &t.e[rank]; memset(e, 0, sizeof *e);
    snprintf(e->initials, sizeof e->initials, "%s", initials);
    e->time = run_time; e->score = score; e->trick = trick;
    time_t now = time(NULL); struct tm *tm = localtime(&now);
    if (tm) strftime(e->date, sizeof e->date, "%Y-%b-%d", tm); else snprintf(e->date, sizeof e->date, "2012-Jan-01");
    if (++t.n > 10) t.n = 10;
    char d[200], p[256]; level_dir(d, sizeof d, level); snprintf(p, sizeof p, "%sscores.NrScoreSet", d);
    make_dirs(level);
    return write_table(p, &t, SC_LOCAL);
}
