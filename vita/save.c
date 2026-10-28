#include "save.h"
#include "controls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "io.h"

void save_defaults(save_t *s)
{
    memset(s, 0, sizeof *s);
    s->master = 1.0f; s->music = 0.85f; s->sfx = 1.0f;    /* the game's default_userdata.xml */
    s->show_hud = 1;
    s->dyn_res = 1;
    s->ghosts = 1;
    s->bloom = s->motion_blur = 1; s->announcer = 1.0f;
    s->air_tricks = 1;
    snprintf(s->initials, sizeof s->initials, "AAA");
    for (int i = 0; i < ACT_N; i++) s->bind[i] = ACT_DEFAULT[i];
}

save_level *save_level_get(save_t *s, const char *xml, int create)
{
    for (int i = 0; i < s->nlev; i++) if (!strcmp(s->lev[i].xml, xml)) return &s->lev[i];
    if (!create || s->nlev >= SAVE_MAX_LEVELS) return NULL;
    save_level *l = &s->lev[s->nlev++];
    memset(l, 0, sizeof *l);
    snprintf(l->xml, sizeof l->xml, "%s", xml);
    return l;
}

int save_load(save_t *s, const char *path)
{
    save_defaults(s);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char ln[256]; int bindv = 1;
    while (fgets(ln, sizeof ln, f)) {
        char k[32], name[64]; float v; int a, b; unsigned long long u;
        if (sscanf(ln, "opt %31s %f", k, &v) == 2) {
            if (!strcmp(k, "master")) s->master = v; else if (!strcmp(k, "music")) s->music = v;
            else if (!strcmp(k, "sfx")) s->sfx = v; else if (!strcmp(k, "hud")) s->show_hud = (int)v;
            else if (!strcmp(k, "invert_y")) s->invert_y = (int)v; else if (!strcmp(k, "seen_intro")) s->seen_intro = (int)v;
            else if (!strcmp(k, "vehicle")) s->vehicle = (int)v;
            else if (!strcmp(k, "swap_sticks")) s->swap_sticks = (int)v;
            else if (!strcmp(k, "dyn_res")) s->dyn_res = (int)v;
            else if (!strcmp(k, "ghosts")) s->ghosts = (int)v;
            else if (!strcmp(k, "bloom")) s->bloom = (int)v; else if (!strcmp(k, "motion_blur")) s->motion_blur = (int)v;
            else if (!strcmp(k, "adv_controls")) s->adv_controls = (int)v; else if (!strcmp(k, "announcer")) s->announcer = v;
            else if (!strcmp(k, "vehicles")) s->vehicles = (unsigned)v;
            else if (!strcmp(k, "air_tricks")) s->air_tricks = (int)v;
        } else if (sscanf(ln, "bind %d %d", &a, &b) == 2) {
            if (a >= 0 && a < ACT_N && b >= 0 && b < BTN_N) s->bind[a] = b;
        } else if (sscanf(ln, "bindv %d", &a) == 1) {
            bindv = a;
        } else if (sscanf(ln, "lev %63s %f %d %d", name, &v, &a, &b) == 4) {
            save_level *l = save_level_get(s, name, 1);
            int sc = 0; sscanf(ln, "lev %*s %*f %*d %*d %d", &sc);
            if (l) { l->best = v; l->unlocked = a; l->done = b; l->score = sc; }
        } else if (sscanf(ln, "name %31s", k) == 1) {
            int ok = strlen(k) == 3;
            for (int i = 0; i < 3 && ok; i++) ok = (k[i] >= 'A' && k[i] <= 'Z') || k[i] == '_';
            if (ok) snprintf(s->initials, sizeof s->initials, "%s", k);
        } else if (sscanf(ln, "ach %llx", &u) == 1) s->ach = u;
        else if (sscanf(ln, "stat %31s %f", k, &v) == 2) {
            if (!strcmp(k, "overheats")) s->overheats = (int)v; else if (!strcmp(k, "highspeed_deaths")) s->highspeed_deaths = (int)v;
            else if (!strcmp(k, "hard_resets")) s->hard_resets = (int)v; else if (!strcmp(k, "wrecks")) s->wrecks = (int)v;
            else if (!strcmp(k, "smashed")) s->smashed = (int)v;
            else if (!strcmp(k, "streetlights")) s->streetlights = (int)v; else if (!strcmp(k, "screens")) s->screens = (int)v;
            else if (!strcmp(k, "most_screens")) s->most_screens = (int)v; else if (!strcmp(k, "barrel_rolls")) s->barrel_rolls = (int)v;
            else if (!strcmp(k, "wall_dist")) s->wall_dist = v; else if (!strcmp(k, "ceil_dist")) s->ceil_dist = v;
            else if (!strcmp(k, "tricks")) s->tricks = (int)v; else if (!strcmp(k, "play_time")) s->play_time = v;
            else if (!strcmp(k, "points")) s->points = (int)v; else if (!strcmp(k, "biggest")) s->biggest = (int)v;
            else if (!strcmp(k, "best_score")) s->best_score = (int)v; else if (!strcmp(k, "gold_shown")) s->gold_shown = (int)v;
        }
    }
    fclose(f);
    /* the buttons from before the new defaults: still on the old ones (Cross boost, Square jump, Circle wings) -> the new */
    if (bindv < BIND_VERSION && s->bind[ACT_BOOST] == ACT_PADPOS[ACT_BOOST] && s->bind[ACT_JUMP] == ACT_PADPOS[ACT_JUMP] &&
        s->bind[ACT_WINGS] == ACT_PADPOS[ACT_WINGS]) {
        s->bind[ACT_BOOST] = ACT_DEFAULT[ACT_BOOST]; s->bind[ACT_JUMP] = ACT_DEFAULT[ACT_JUMP]; s->bind[ACT_WINGS] = ACT_DEFAULT[ACT_WINGS];
    }
    if (!s->streetlights && !s->screens && s->smashed) s->streetlights = s->smashed;   /* (saves from before the two were apart) */
    return 0;
}

/* the text built in memory, written by io.c's thread (no wait for the memory card mid-game) */
typedef struct { char *b; size_t n, cap; } sbuf;
static void bp(sbuf *o, const char *fmt, ...)
{
    va_list ap;
    for (;;) {
        size_t room = o->cap - o->n;
        va_start(ap, fmt); int k = vsnprintf(o->b ? o->b + o->n : NULL, o->b ? room : 0, fmt, ap); va_end(ap);
        if (k < 0) return;
        if (o->b && (size_t)k < room) { o->n += (size_t)k; return; }
        size_t nc = (o->cap ? o->cap * 2 : 4096) + (size_t)k;
        char *nb = realloc(o->b, nc);
        if (!nb) return;
        o->b = nb; o->cap = nc;
    }
}
int save_write(const save_t *s, const char *path)
{
    sbuf o = { 0 }, *f = &o;
    bp(f, "opt master %.3f\nopt music %.3f\nopt sfx %.3f\nopt hud %d\nopt invert_y %d\nopt seen_intro %d\nopt vehicle %d\n",
            s->master, s->music, s->sfx, s->show_hud, s->invert_y, s->seen_intro, s->vehicle);
    bp(f, "opt swap_sticks %d\nopt dyn_res %d\nopt ghosts %d\nname %s\n", s->swap_sticks, s->dyn_res, s->ghosts, s->initials[0] ? s->initials : "AAA");
    for (int i = 0; i < ACT_N; i++) bp(f, "bind %d %d\n", i, s->bind[i]);
    bp(f, "bindv %d\n", BIND_VERSION);
    for (int i = 0; i < s->nlev; i++) bp(f, "lev %s %.3f %d %d %d\n", s->lev[i].xml, s->lev[i].best, s->lev[i].unlocked, s->lev[i].done, s->lev[i].score);
    bp(f, "ach %llx\n", (unsigned long long)s->ach);
    bp(f, "stat overheats %d\nstat highspeed_deaths %d\nstat hard_resets %d\nstat wrecks %d\nstat tricks %d\nstat play_time %.1f\nstat smashed %d\n",
            s->overheats, s->highspeed_deaths, s->hard_resets, s->wrecks, s->tricks, s->play_time, s->smashed);
    bp(f, "stat points %d\nstat biggest %d\nstat best_score %d\nstat gold_shown %d\n", s->points, s->biggest, s->best_score, s->gold_shown);
    bp(f, "stat streetlights %d\nstat screens %d\nstat most_screens %d\nstat barrel_rolls %d\nstat wall_dist %.4f\nstat ceil_dist %.4f\n",
            s->streetlights, s->screens, s->most_screens, s->barrel_rolls, s->wall_dist, s->ceil_dist);
    bp(f, "opt bloom %d\nopt motion_blur %d\nopt adv_controls %d\nopt announcer %.3f\nopt vehicles %u\n", s->bloom, s->motion_blur, s->adv_controls, s->announcer, s->vehicles);
    bp(f, "opt air_tricks %d\n", s->air_tricks);
    if (!o.b) return -1;
    io_write_take(path, o.b, o.n);
    return 0;
}
