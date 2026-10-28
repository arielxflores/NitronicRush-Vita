/* Front end: the original game's menu flow, art, strings and announcer, on the Vita. */
#include "front.h"
#include "hud.h"
#include "snd.h"
#include "controls.h"
#include "scores.h"
#include "results.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

fe_set FE_SETS[12];
int FE_NSETS;
fe_ach FE_ACH[64];
int FE_NACH;
fe_vehicle FE_VEH[8];
int FE_NVEH;
int FE_LEVEL_SCREENS;

static nr_pak *g_pak;
static save_t *S;
static const char *g_save_path;
static int scr = FE_SPLASH;
static float scr_t;                  /* time on this screen */
static int sel_main, sel_set, sel_level, sel_opt, sel_quit;
static int sel_ctl, ctl_capture; static float ctl_capture_t;   /* Controls: the row, waiting for a button */
static int levels_set;               /* set shown on the level screen */
static int levels_story;             /* the run was started from "Story" */
static int overlay;                  /* a screen over the paused race: Back returns FE_DONE */
static int ghost_cat = SC_LOCAL;     /* "Select Ghosts to Challenge" */
static int lv_row, lv_col;           /* the Arcade level screen's focus: row 0 < Previous / Go / Next >, row 1 the rest */
static int veh_sel, veh_btn;         /* Vehicles: the one shown, the focus (0 < Previous, 1 Next >, 2 Back) */
static int tut_sel;                  /* "Play Tutorial?" */
static int ach_sel, ach_btn;         /* Achievements: the row, the focus (0 the list, 1 Reset Achievements, 2 Back) */
static ui_dlg dlg;                   /* the screen's confirmation box (Reset ALL Achievements?, Select Ghosts) */
static int dlg_cats[3];              /* Select Ghosts: the category of each button */
int front_story(void) { return levels_story; }
int front_ghost_cat(void) { return ghost_cat; }
void front_set_ghost_cat(int cat) { ghost_cat = cat; }
static float splash_t; static int splash_i;
static char *credits; static int credits_lines; static float credits_y;
static float cam_pans[8][7]; static int npans; static float pan_fade = 2.0f;
static float dummy_car[3] = { -96.6f, -295.4f, -10.6f }, dummy_fly[3] = { 18.96f, 15.48f, 13.74f };
static float pan_t; static int pan_i;

/* ------------------------------------------------------------------ music (the game's table) */
static const char *MUSIC[28][2] = {
    { NULL, NULL }, { "Play_MainMenuPlaylist", "Green" }, { "Play_MainMenuPlaylist", "Green" }, { "Play_Level1Playlist", "Red" },
    { "Play_CreditsPlaylist", "Credits" }, { "Play_Level1Playlist2", "Blue" }, { "Play_Level1Playlist3", "Pink" },
    { "Play_StuntMode_1", "Stunt_1" }, { "Play_GreenRMX", "GreenRMX" }, { "Play_NewPurple", "NewPurple" }, { "Play_CoreFinal", "CoreFinal" },
    { "Play_First_Playable", "FirstPlayable" }, { "Play_Neon_Skyway", "NeonSkyway" }, { "Play_Beta_Red", "BetaRed" },
    { "Play_IntroCutscene", NULL }, { "Play_Finale", NULL }, { "Play_Core8Bit", "Core8Bit" }, { "Play_CoreHeart", "CoreHeart" },
    { "Play_BlueWubstep", "BlueWubstep" }, { "Play_PinkClockwork", "PinkClockwork" }, { "Play_CreditsOldPlaylist", NULL },
    { "Play_PurpleEighties", "PurpleEighties" }, { "Play_ItsChristmas", "ItsChristmas" }, { "Play_Gladiator", "Gladiator" },
    { "Play_Nature", "Nature" }, { "Play_Nitroception", "Nitroception" }, { "Play_TurboTunnel", "TurboTunnel" },
    { "Play_EnterTheMatrix", "EnterTheMatrix" } };
const char *music_event(int m) { return (m > 0 && m < 28) ? MUSIC[m][0] : NULL; }
const char *music_layer_name(int m) { return (m > 0 && m < 28) ? MUSIC[m][1] : NULL; }

static int menu_music_on;
static void menu_music(void)
{
    if (menu_music_on) return;
    snd_post("Play_MainMenuPlaylist", 0);
    menu_music_on = 1;
}
/* MainMenuLevel coming in after a level (0x733c00: Stop_SFX, Stop_Announcer; its Init 0x733010: "Nitronic Rush!") */
void front_menu_level(void)
{
    snd_post("Stop_SFX", 0); snd_post("Stop_Announcer", 0);
    snd_post("Play_NitronicRush", 0);
}
void front_apply_volumes(void)
{
    snd_set_param("Master_Volume", S->master < 0.001f ? 0.001f : S->master);
    snd_set_param("Music_Volume", S->music < 0.001f ? 0.001f : S->music);
    snd_set_param("SFX_Volume", S->sfx < 0.001f ? 0.001f : S->sfx);
    snd_set_param("Announcer_Volume", S->announcer < 0.001f ? 0.001f : S->announcer);
    snd_set_param("Ambient_Volume", S->sfx < 0.001f ? 0.001f : S->sfx);
}

/* ------------------------------------------------------------------ data */
static void load_sets(void)
{
    uint32_t sz; char *t = nr_pak_read(g_pak, "ui/levelsets.txt", &sz);
    FE_NSETS = 0;
    for (char *ln = t ? strtok(t, "\n") : NULL; ln; ln = strtok(NULL, "\n")) {
        if (!strncmp(ln, "set ", 4) && FE_NSETS < 12) {
            fe_set *s = &FE_SETS[FE_NSETS++]; memset(s, 0, sizeof *s);
            char *bar = strchr(ln + 4, '|');
            if (bar) { *bar = 0; snprintf(s->name, sizeof s->name, "%s", bar + 1); }
            snprintf(s->file, sizeof s->file, "%s", ln + 4);
        } else if (!strncmp(ln, "desc ", 5) && FE_NSETS) snprintf(FE_SETS[FE_NSETS - 1].desc, sizeof FE_SETS[0].desc, "%s", ln + 5);
        else if (!strncmp(ln, "level ", 6) && FE_NSETS) {
            fe_set *s = &FE_SETS[FE_NSETS - 1];
            if (s->n >= 24) continue;
            fe_level *l = &s->lv[s->n];
            char *f[8]; int nf = 0; char *p = ln + 6;
            while (nf < 8) { f[nf++] = p; char *b = strchr(p, '|'); if (!b) break; *b = 0; p = b + 1; }
            if (nf < 8) continue;
            snprintf(l->xml, sizeof l->xml, "%s", f[0]); snprintf(l->name, sizeof l->name, "%s", f[1]);
            l->music = atoi(f[2]); l->unlocked = atoi(f[3]); l->sd = atoi(f[4]); l->wings = atoi(f[5]); l->score = atoi(f[6]);
            snprintf(l->img, sizeof l->img, "%s", f[7]);
            char pn[96]; snprintf(pn, sizeof pn, "levels/%s.nrl", l->xml);
            l->installed = nr_pak_find(g_pak, pn, NULL, NULL, NULL) >= 0;
            s->n++;
        }
    }
    free(t);
}
static void load_achievements(void)
{
    uint32_t sz; char *t = nr_pak_read(g_pak, "ui/achievements.txt", &sz);
    FE_NACH = 0;
    for (char *ln = t ? strtok(t, "\n") : NULL; ln && FE_NACH < 64; ln = strtok(NULL, "\n")) {
        char *bar = strchr(ln, '|');
        fe_ach *a = &FE_ACH[FE_NACH++];
        if (bar) *bar = 0;
        snprintf(a->name, sizeof a->name, "%s", ln);
        snprintf(a->desc, sizeof a->desc, "%s", bar && bar[1] ? bar + 1 : "");
        a->hidden = !(bar && bar[1]);             /* no description: no code awards it on the PC, and its menu skips it */
    }
    free(t);
}
static void load_vehicles(void)
{
    uint32_t sz; char *t = nr_pak_read(g_pak, "ui/vehicles.txt", &sz);
    FE_NVEH = 0;
    for (char *ln = t ? strtok(t, "\n") : NULL; ln && FE_NVEH < 8; ln = strtok(NULL, "\n")) {
        char *f[7]; int nf = 0; char *p = ln;
        while (nf < 7) { f[nf++] = p; char *b = strchr(p, '|'); if (!b) break; *b = 0; p = b + 1; }
        if (nf < 7) continue;
        fe_vehicle *v = &FE_VEH[FE_NVEH++];
        snprintf(v->key, sizeof v->key, "%s", f[0]); snprintf(v->name, sizeof v->name, "%s", f[1]); v->unlocked = atoi(f[2]);
        snprintf(v->desc, sizeof v->desc, "%s", f[3]); snprintf(v->locked_desc, sizeof v->locked_desc, "%s", f[4]);
        snprintf(v->img, sizeof v->img, "%s", f[5]); snprintf(v->img_locked, sizeof v->img_locked, "%s", f[6]);
    }
    free(t);
}
/* UnlockedVehicles (PlayerData): the ones unlocked from the start, and the bits saved as they were won */
int vehicle_unlocked(int i)
{
    if (i < 0 || i >= FE_NVEH) return 0;
    return FE_VEH[i].unlocked || (S->vehicles >> i & 1);
}
static int vehicle_index(const char *key)
{
    for (int k = 0; k < FE_NVEH; k++) if (!strcmp(FE_VEH[k].key, key)) return k;
    return -1;
}
/* the Avenger as the ending starts (0x725500), the Goldenceptor with the last achievement: "Vehicle Unlocked" */
void vehicle_unlock(const char *key)
{
    int i = vehicle_index(key);
    if (i < 0 || vehicle_unlocked(i)) return;
    S->vehicles |= 1u << i;
    ui_popup(2, FE_VEH[i].name);
    save_write(S, g_save_path);
}
static int story_set(void)
{
    for (int s = 0; s < FE_NSETS; s++) if (strstr(FE_SETS[s].file, "Campaign")) return s;
    return 0;
}

/* (the scrolling text is only used when the credits level isn't in the data) data/credits.txt: the credits level's
 * file on the first line, then "Name$Role|Role" entries and "|" spacers, which become lines of their own here */
static void load_credits(void)
{
    uint32_t sz; credits = nr_pak_read(g_pak, "ui/credits.txt", &sz);
    credits_lines = 0;
    if (!credits) return;
    char *nl = strchr(credits, '\n');
    if (nl && nl - credits > 4 && !strncmp(nl - 4, ".xml", 4)) memmove(credits, nl + 1, strlen(nl + 1) + 1);
    else if (nl && nl - credits > 5 && !strncmp(nl - 5, ".xml\r", 5)) memmove(credits, nl + 1, strlen(nl + 1) + 1);
    for (char *p = credits; *p; p++) {
        if (*p == '\r') *p = ' ';
        if (*p == '\t') *p = ' ';
        if (*p == '$' || *p == '|') *p = '\n';
        if (*p == '\n') credits_lines++;
    }
}
static void load_menu_cam(void)
{
    uint32_t sz; char *t = nr_pak_read(g_pak, "ui/menu.txt", &sz);
    npans = 0;
    for (char *ln = t ? strtok(t, "\n") : NULL; ln; ln = strtok(NULL, "\n")) {
        float *p = cam_pans[npans];
        if (npans < 8 && sscanf(ln, "pan %f %f %f %f %f %f %f", &p[0], &p[1], &p[2], &p[3], &p[4], &p[5], &p[6]) == 7) npans++;
        sscanf(ln, "fade %f", &pan_fade);
    }
    free(t);
}
void front_set_dummies(const float car[3], const float fly[3])
{
    if (car) memcpy(dummy_car, car, 12);
    if (fly) memcpy(dummy_fly, fly, 12);
}

int front_init(nr_pak *pak, save_t *save, const char *save_path)
{
    g_pak = pak; S = save; g_save_path = save_path;
    load_sets(); load_achievements(); load_credits(); load_menu_cam(); load_vehicles();
    /* (saves from before the unlocks were kept: the Story finished - the ending was reached - and every achievement) */
    {
        int st = story_set(), all = FE_NSETS > 0 && FE_SETS[st].n > 1;
        for (int k = 1; all && k < FE_SETS[st].n; k++) { save_level *l = save_level_get(S, FE_SETS[st].lv[k].xml, 0); if (!l || !l->done) all = 0; }
        int a = vehicle_index("avenger"), g = vehicle_index("goldenceptor");
        if (all && a >= 0) S->vehicles |= 1u << a;
        if (S->gold_shown && g >= 0) S->vehicles |= 1u << g;
    }
    if (S->vehicle < 0 || S->vehicle >= FE_NVEH || !vehicle_unlocked(S->vehicle)) S->vehicle = 0;
    /* unlocked flags from the level sets seed the save */
    for (int s = 0; s < FE_NSETS; s++) for (int i = 0; i < FE_SETS[s].n; i++) {
        save_level *l = save_level_get(S, FE_SETS[s].lv[i].xml, 1);
        if (l && FE_SETS[s].lv[i].unlocked) l->unlocked = 1;
    }
    front_apply_volumes();
    return FE_NSETS ? 0 : -1;
}

/* ------------------------------------------------------------------ popups */
/* AchievementDisplay (0x6fa6a0): queued, one at a time for 3.3 s, with Play_ViperGTS_Rev as each shows */
#define POP_TIME 3.3f
static struct { int kind; char text[64]; float t; } pop[8]; static int npop;
void ui_popup(int kind, const char *text)
{
    if (npop >= 8) return;
    pop[npop].kind = kind; snprintf(pop[npop].text, sizeof pop[npop].text, "%s", text); pop[npop].t = -1; npop++;
}
void ui_popup_update(float dt)
{
    if (!npop) return;
    if (pop[0].t < 0) { pop[0].t = 0; snd_post("Play_ViperGTS_Rev", 0); }
    pop[0].t += dt;
    if (pop[0].t > POP_TIME) { memmove(pop, pop + 1, sizeof pop[0] * (npop - 1)); npop--; }
}
void ui_popup_draw(void)
{
    if (!npop || pop[0].t < 0) return;
    float t = pop[0].t, a = t < 0.3f ? t / 0.3f : t > POP_TIME - 0.4f ? (POP_TIME - t) / 0.4f : 1.0f;
    if (a < 0) a = 0;
    unsigned A = (unsigned)(a * 255);
    float y = 544 - 120 + (1 - a) * 40;
    hud_image(hud_tex(pop[0].kind == 0 ? "achievement_unlocked" : pop[0].kind == 2 ? "vehicle_unlocked" : "new_level_unlocked"), 480 - 192, y, 384, 96, 0xFFFFFF00 | A);
    if (pop[0].kind == 0) hud_text(HUD_FONT_MAIN, 480, y + 64, 20, 0xFFE070u << 8 | A, HUD_CENTER, pop[0].text);
    else hud_text(HUD_FONT_MAIN, 480, y + 64, 20, 0xFFFFFF00 | A, HUD_CENTER, pop[0].text);
}

static int ach_index(const char *name)
{
    for (int i = 0; i < FE_NACH; i++) if (!strcmp(FE_ACH[i].name, name)) return i;
    return -1;
}
int ach_count(void)
{
    int n = 0;
    for (int i = 0; i < FE_NACH; i++) if (!FE_ACH[i].hidden && (S->ach >> i & 1)) n++;
    return n;
}
int ach_visible(void)
{
    int n = 0;
    for (int i = 0; i < FE_NACH; i++) if (!FE_ACH[i].hidden) n++;
    return n;
}
void ach_unlock(const char *name)
{
    int i = ach_index(name);
    if (i < 0 || FE_ACH[i].hidden || (S->ach >> i & 1)) return;
    S->ach |= 1ull << i;
    ui_popup(0, FE_ACH[i].name);
    if (ach_count() >= 10) {
        int b = ach_index("Bleep Bloop");
        if (b >= 0 && !(S->ach >> b & 1)) { S->ach |= 1ull << b; ui_popup(0, FE_ACH[b].name); }
    }
    if (ach_visible() && ach_count() >= ach_visible()) { S->gold_shown = 1; vehicle_unlock("goldenceptor"); }   /* every achievement */
    save_write(S, g_save_path);
}

/* ------------------------------------------------------------------ widgets */
void ui_button(float cx, float cy, float w, float h, const char *label, int selected, float alpha)
{
    /* unselected: the blueprint's colours (Color 1,1,1,0.4, Color2 0,0,1,1); selected: lit up in
     * the game's cyan, with the circuit lines showing */
    float c0[4] = { 1, 1, 1, 0.4f * alpha }, c2[4] = { 0, 0, 1, 1 };
    if (selected) { c0[0] = 0.45f; c0[1] = 0.9f; c0[2] = 1.0f; c0[3] = alpha; c2[2] = 0; c2[3] = 1; }
    hud_button(hud_tex("menu_button2"), cx - w / 2, cy - h / 2, w, h, c0, c2);
    unsigned a = (unsigned)(alpha * 255);
    hud_text(HUD_FONT_MAIN, cx, cy - h * 0.24f, h * 0.48f, (selected ? 0xFFFFFF00 : 0xC8D8F000) | a, HUD_CENTER, label);
}
int ui_hit(const fe_input *in, float cx, float cy, float w, float h)
{
    return in && in->tap && fabsf(in->tx - cx) <= w * 0.5f && fabsf(in->ty - cy) <= h * 0.5f;
}
static int hit_r(const fe_input *in, float x, float y, float w, float h)    /* (a box from its top left corner) */
{
    return in && in->tap && in->tx >= x && in->tx <= x + w && in->ty >= y && in->ty <= y + h;
}
int ui_menu(const char **items, int n, int *sel, const fe_input *in, float cx, float y0, float w, float h, float gap)
{
    int moved = 0;
    if (in) {
        for (int i = 0; i < n; i++) if (ui_hit(in, cx, y0 + i * gap, w, gap < h ? gap : h) && *sel != i) { *sel = i; moved = 1; }
        if (in->up) { *sel = (*sel + n - 1) % n; moved = 1; }
        if (in->down) { *sel = (*sel + 1) % n; moved = 1; }
        if (moved) snd_post("Play_MenuMoveLowShort", 0);
    }
    for (int i = 0; i < n; i++) ui_button(cx, y0 + i * gap, w, h, items[i], i == *sel, 1);
    return moved;
}
void ui_slider(float x, float y, float w, float v, int selected)
{
    unsigned bar = hud_tex("menu_sliderbar"), knob = hud_tex("menu_slider");
    hud_image(bar, x, y - 12, w, 24, selected ? 0xFFFFFFFF : 0xFFFFFF99);
    hud_image(knob, x + v * (w - 20) - 9, y - 22, 38, 44, selected ? 0xFFFFFFFF : 0xFFFFFFB0);
}
static void toggle_sz(float x, float y, float sz, int on, int selected)
{
    hud_image(hud_tex("menu_toggle"), x - sz / 2, y - sz / 2, sz, sz, selected ? 0xFFFFFFFF : 0xFFFFFFA0);
    if (on) hud_rect(x - sz * 0.15f, y - sz * 0.15f, sz * 0.3f, sz * 0.3f, 0x7FE0FFFF);
}
void ui_toggle(float x, float y, int on, int selected) { toggle_sz(x, y, 48, on, selected); }

/* the PC's confirmation boxes: the title, the buttons one under another; Circle backs out */
void ui_dlg_open(ui_dlg *d, const char *title, int n, const char *a, const char *b, const char *c)
{
    memset(d, 0, sizeof *d);
    snprintf(d->title, sizeof d->title, "%s", title);
    d->item[0] = a; d->item[1] = b; d->item[2] = c; d->n = n < 1 ? 1 : n > 3 ? 3 : n; d->open = 1;
}
int ui_dlg_update(ui_dlg *d, const fe_input *in)
{
    if (!d->open) return -2;
    d->t += 1.0f / 60;
    if (in->up && d->sel > 0) { d->sel--; snd_post("Play_MenuMoveLowShort", 0); }
    else if (in->down && d->sel < d->n - 1) { d->sel++; snd_post("Play_MenuMoveLowShort", 0); }
    if (in->back) { d->open = 0; snd_post("Play_MenuMoveLowShort", 0); return -2; }
    float y0 = 272 - (d->n * 54) * 0.5f;
    for (int i = 0; i < d->n; i++) if (ui_hit(in, 480, y0 + 27 + i * 54, 320, 50) && d->t > 0.1f) { d->sel = i; d->open = 0; return i; }   /* touch */
    if (in->accept && d->t > 0.1f) { d->open = 0; return d->sel; }
    return -1;
}
void ui_dlg_draw(const ui_dlg *d)
{
    if (!d->open) return;
    hud_rect(0, 0, 960, 544, 0x000000D0);
    float y0 = 272 - (d->n * 54) * 0.5f;
    hud_text(HUD_FONT_TITLE, 480, y0 - 76, 40, 0xFFFFFFFF, HUD_CENTER, d->title);
    for (int i = 0; i < d->n; i++) ui_button(480, y0 + 27 + i * 54, 320, 46, d->item[i], i == d->sel, 1);
}

/* ------------------------------------------------------------------ screens */
static const char *MAIN_ITEMS[] = { "Story", "Arcade", "Achievements", "Controls", "Options", "Credits", "Quit Game" };
static const char *MAIN_VOICE[] = { "Play_Story", "Play_Arcade", "Play_Achievements", "Play_Controls", "Play_Options", "Play_Credits", "Play_Quit" };

static void vis_list(int *vis, int *nv)        /* in the PC's order (its ids run the file's lines backwards) */
{
    *nv = 0;
    for (int i = FE_NACH - 1; i >= 0 && *nv < 64; i--) if (!FE_ACH[i].hidden) vis[(*nv)++] = i;
}

void front_enter(int screen)
{
    scr = screen; scr_t = 0;
    if (screen == FE_SPLASH) { splash_i = 0; splash_t = 0; }
    /* the PC's menus sound only as a button lights up (0x6a7610): on a new screen its first one, then each move;
     * pressing, backing out, toggles and sliders make no sound of their own */
    if (screen != FE_SPLASH && screen != FE_TITLE && screen != FE_CREDITS) snd_post("Play_MenuMoveLowShort", 0);
    if (!overlay && (screen == FE_TITLE || screen == FE_MAIN)) menu_music();
    if (screen == FE_CREDITS) {
        credits_y = 560;
        snd_post("Stop_MainMenuPlaylist", 0);
        snd_post("Play_CreditsPlaylist", 0);
        menu_music_on = 0;
        ach_unlock("Made From Scratch");
    }
    if (screen == FE_LEVELS) {
        fe_set *s = &FE_SETS[levels_set];
        if (sel_level >= s->n) sel_level = 0;
        if (lv_row > 1 || lv_col > (lv_row ? 3 : 2)) { lv_row = 0; lv_col = 1; }
    }
    if (screen == FE_ACHIEVEMENTS) { ach_sel = 0; ach_btn = 0; dlg.open = 0; }
    if (screen == FE_OPTIONS) sel_opt = 0;
    if (screen == FE_CONTROLS) { sel_ctl = 0; ctl_capture = 0; }
    if (screen == FE_VEHICLES) { veh_sel = S->vehicle; veh_btn = 1; }
    if (screen == FE_TUTPROMPT) tut_sel = 0;
    if (screen == FE_LEVELRESULTS) {
        fe_level *l = &FE_SETS[levels_set].lv[sel_level];
        res_open(l->xml, l->score, 1, 99, 0, 0, 0);
    }
    if (screen == FE_GHOSTSEL) {                      /* "Select Ghosts to Challenge" (0x6e1ef0): Local, Developer if the level has it */
        const fe_level *l = &FE_SETS[levels_set].lv[sel_level];
        int n = 0; const char *it[3] = { 0 };
        it[n] = "Local"; dlg_cats[n++] = SC_LOCAL;
        if (sc_has(SC_DEVELOPER, l->xml)) { it[n] = "Developer"; dlg_cats[n++] = SC_DEVELOPER; }
        it[n] = "Back"; dlg_cats[n++] = -1;
        ui_dlg_open(&dlg, "Select Ghosts to Challenge", n, it[0], it[1], it[2]);
        for (int k = 0; k < n; k++) if (dlg_cats[k] == ghost_cat) dlg.sel = k;   /* the focus on the current category */
    }
}
int front_screen(void) { return scr; }
void front_overlay(int screen) { overlay = 1; front_enter(screen); }
static int leave(void)                               /* Back from Achievements / Controls / Options */
{
    save_write(S, g_save_path);
    if (overlay) { overlay = 0; return FE_DONE; }
    front_enter(FE_MAIN);
    return FE_STAY;
}

static int level_unlocked(const fe_level *l)
{
    save_level *sl = save_level_get(S, l->xml, 0);
    return l->unlocked || (sl && sl->unlocked);
}

static void fmt_time(char *b, size_t n, float t)    /* FormatTime 0x723b60: truncated */
{
    if (t < 0) t = 0;
    int s_ = (int)t;
    char f[16]; snprintf(f, sizeof f, "%f", fmod(t, 1.0)); f[4] = 0;
    snprintf(b, n, "%02d:%02d.%s", s_ / 60, s_ % 60, f + 2);
}
static void fmt_num(char *b, size_t n, long long v)  /* 1,234,567 */
{
    char t[32]; snprintf(t, sizeof t, "%lld", v < 0 ? -v : v);
    int L = (int)strlen(t), o = 0;
    if (v < 0 && o < (int)n - 1) b[o++] = '-';
    for (int i = 0; i < L && o < (int)n - 1; i++) {
        b[o++] = t[i];
        if ((L - 1 - i) % 3 == 0 && i < L - 1 && o < (int)n - 1) b[o++] = ',';
    }
    b[o] = 0;
}

/* the achievement's progress line (0x6cf4f0); '|' breaks the line. Empty: only Complete! / Incomplete */
static void ach_progress(int i, char *o, size_t n)
{
    const char *nm = FE_ACH[i].name; char a[32], b[32], c[32];
    o[0] = 0;
    if (!strcmp(nm, "Million Point Mark")) {
        fmt_num(a, sizeof a, S->points); float pc = S->points / 10000.0f; if (pc > 100) pc = 100;
        snprintf(b, sizeof b, "%.2f", pc); snprintf(o, n, "Lifetime Points: %s   (%s%%)", a, b);
    } else if (!strcmp(nm, "100 Grand")) { fmt_num(a, sizeof a, S->best_score); snprintf(o, n, "Best Score: %s", a); }
    else if (!strcmp(nm, "Over 9000")) {
        fmt_num(a, sizeof a, S->biggest); fmt_num(b, sizeof b, S->biggest > 9000 ? S->biggest - 9000 : 9000 - S->biggest);
        snprintf(o, n, S->biggest > 9000 ? "Best Trick: %s|(%s over 9,000)" : "Best Trick: %s|(%s under 9,000)", a, b);
    } else if (!strcmp(nm, "Self-Termination")) { fmt_num(a, sizeof a, S->hard_resets); snprintf(o, n, "Lifetime Self-Terminations: %s", a); }
    else if (!strcmp(nm, "Wreckless")) { fmt_num(a, sizeof a, S->highspeed_deaths); snprintf(o, n, "Total High-Speed Deaths: %s", a); }
    else if (!strcmp(nm, "Rush Hour")) snprintf(o, n, "Total Time Played: %0.2f hours", S->play_time / 3600.0f);
    else if (!strcmp(nm, "Wall Ride") || !strcmp(nm, "Gravity? No Care.")) {
        float mi = !strcmp(nm, "Wall Ride") ? S->wall_dist : S->ceil_dist;
        if (mi >= 1) snprintf(o, n, "Total Distance: %0.1f miles", mi);
        else { fmt_num(a, sizeof a, (long long)(mi * 5280)); snprintf(o, n, "Total Distance: %s feet", a); }
    } else if (!strcmp(nm, "Tricks Make You Cool")) {
        fmt_num(a, sizeof a, S->tricks); fmt_num(b, sizeof b, S->tricks ? S->points / S->tricks : 0);
        snprintf(o, n, "Tricks Landed: %s|Avg. Points Per Trick: %s", a, b);
    } else if (!strcmp(nm, "Toasty")) { fmt_num(a, sizeof a, S->overheats); snprintf(o, n, "Total Overheat Deaths: %s", a); }
    else if (!strcmp(nm, "Bleep Bloop")) {
        int k = ach_count(), v = ach_visible();
        snprintf(o, n, "Total Achievements Completed: %d|(%d%% of total %d)", k, v ? k * 100 / v : 0, v);
    } else if (!strcmp(nm, "Do A Barrel Roll!")) snprintf(o, n, "Total Barrel Rolls: %d", S->barrel_rolls);
    else if (!strcmp(nm, "Rampage")) {
        fmt_num(a, sizeof a, S->streetlights); fmt_num(b, sizeof b, S->screens); fmt_num(c, sizeof c, S->streetlights + S->screens);
        snprintf(o, n, "Streetlights Broken: %s|Screens Broken: %s|Total Destruction: %s", a, b, c);
    } else if (!strcmp(nm, "Smash TV")) {
        fmt_num(a, sizeof a, S->most_screens); fmt_num(b, sizeof b, FE_LEVEL_SCREENS);
        snprintf(o, n, "Most Broken In One Level: %s|Screens Broken This Level: %s", a, b);
    }
}

/* the Options rows (the PC's order; Resolution, Refresh Rate, Windowed, V-Sync, Rumble and Apply have no use here) */
enum { OPT_HUD, OPT_BLOOM, OPT_BLUR, OPT_ADV, OPT_INVERT, OPT_MUSIC, OPT_SFX, OPT_ANN, OPT_MASTER, OPT_DYNRES, OPT_BACK, OPT_N };
static const char *OPT_NAME[OPT_N] = { " Toggle HUD", " Toggle Bloom", " Toggle Motion Blur", " Use Advanced Controls", " Invert Flight Y Axis",
    "Music Volume", "SFX Volume", "Announcer Volume", "Master Volume", " Dynamic Resolution", "Back" };
static int *opt_flag(int r)
{
    switch (r) { case OPT_HUD: return &S->show_hud; case OPT_BLOOM: return &S->bloom; case OPT_BLUR: return &S->motion_blur;
                 case OPT_ADV: return &S->adv_controls; case OPT_INVERT: return &S->invert_y; case OPT_DYNRES: return &S->dyn_res; }
    return NULL;
}
static float *opt_vol(int r)
{
    switch (r) { case OPT_MUSIC: return &S->music; case OPT_SFX: return &S->sfx; case OPT_ANN: return &S->announcer; case OPT_MASTER: return &S->master; }
    return NULL;
}
/* Controls: the actions, the sticks, Use Advanced Controls, then Reset to Defaults and Back side by side */
enum { CTL_STICKS = ACT_N, CTL_ADV, CTL_AIR, CTL_RESET, CTL_BACK, CTL_N };
#define CTL_DY 33                                 /* the rows' spacing */
/* the Roll may only go on the brake's button or on a free one (it would otherwise leave two actions on one button) */
static int trick_ok(int b)
{
    for (int k = 0; k < ACT_N; k++) if (k != ACT_TRICK && k != ACT_BRAKE && S->bind[k] == b) return 0;
    return 1;
}
static void bind_set(int a, int b)
{
    int old = S->bind[a];
    if (a == ACT_TRICK && !trick_ok(b)) return;
    for (int k = 0; k < ACT_N; k++) if (k != a && S->bind[k] == b && !act_shareable(a, k)) S->bind[k] = old;
    S->bind[a] = b;
}

static int slider_drag = -1;                        /* the Options slider a finger is on */
int front_update(float dt, const fe_input *in_, int *out_set, int *out_level)
{
    fe_input I = *in_; const fe_input *in = &I;     /* (the touch screen presses the buttons in this copy) */
    scr_t += dt;
    if (!overlay) ui_popup_update(dt);
    switch (scr) {
    case FE_SPLASH: {
        splash_t += dt;
        if (in->any && splash_t > 0.3f) splash_t = 99;
        float len = 3.0f;
        if (splash_t > len) { splash_t = 0; splash_i++; }
        /* (the PC's third screen, "best experienced with audio / Xbox 360 controller recommended", is left out) */
        if (splash_i >= 2) { front_enter(FE_TITLE); snd_post("Play_NitronicRush", 0); }
        break; }
    case FE_TITLE:
        if (in->any && scr_t > 0.5f) front_enter(FE_MAIN);
        break;
    case FE_MAIN: {
        int prev = sel_main;
        for (int i = 0; i < 7; i++) if (ui_hit(in, 480, 222 + i * 45, 300, 45)) { sel_main = i; I.accept = 1; }
        if (in->up) sel_main = (sel_main + 6) % 7;
        if (in->down) sel_main = (sel_main + 1) % 7;
        if (prev != sel_main) snd_post("Play_MenuMoveLowShort", 0);
        if (in->back && scr_t > 0.3f) { front_enter(FE_TITLE); break; }
        if (in->accept) {
            snd_post(MAIN_VOICE[sel_main], 0);              /* Play_Story, Play_Arcade, ... (actions 1, 0x44, 0x4b, 2, 3, 4, 5) */
            switch (sel_main) {
            case 0: front_enter(FE_TUTPROMPT); break;          /* Story: "Play Tutorial?" (0x6e7050) */
            case 1: levels_story = 0; front_enter(FE_SETS_SCREEN); break;
            case 2: front_enter(FE_ACHIEVEMENTS); break;
            case 3: front_enter(FE_CONTROLS); break;
            case 4: front_enter(FE_OPTIONS); break;
            case 5:
                /* the original credits are a drive down a road lined with the team's billboards */
                if (nr_pak_find(g_pak, "levels/credits_level.nrl", NULL, NULL, NULL) >= 0) {
                    menu_music_on = 0; ach_unlock("Made From Scratch");
                    *out_set = -1; *out_level = 0; return FE_PLAY;
                }
                front_enter(FE_CREDITS); break;
            case 6: sel_quit = 1; front_enter(FE_QUIT); break;
            }
        }
        break; }
    case FE_TUTPROMPT: {                                     /* Yes: the Tutorial; No (action 26): the intro, then Level 1 */
        for (int i = 0; i < 3; i++) if (ui_hit(in, 480, 312 + i * 54, 300, 50)) { tut_sel = i; I.accept = 1; }
        if (in->up && tut_sel > 0) { tut_sel--; snd_post("Play_MenuMoveLowShort", 0); }
        if (in->down && tut_sel < 2) { tut_sel++; snd_post("Play_MenuMoveLowShort", 0); }
        if (in->back || (in->accept && tut_sel == 2)) { front_enter(FE_MAIN); break; }
        if (in->accept && FE_NSETS) {
            int st = story_set();
            if (tut_sel == 0) snd_post("Play_Tutorial", 0);  /* (action 0x19) */
            levels_story = 1; levels_set = st; menu_music_on = 0;
            if (tut_sel == 0 && FE_SETS[st].n && FE_SETS[st].lv[0].installed) { *out_set = st; *out_level = 0; }
            else { *out_set = st; *out_level = FE_LEVEL_INTRO; }
            return FE_PLAY;
        }
        break; }
    case FE_SETS_SCREEN: {
        int top = sel_set - 4 < 0 ? 0 : sel_set - 4;          /* (as drawn) */
        if (top > FE_NSETS - 7) top = FE_NSETS - 7 < 0 ? 0 : FE_NSETS - 7;
        for (int i = top; i < FE_NSETS && i < top + 7; i++)
            if (ui_hit(in, 300, 110 + (i - top) * 50, 360, 50)) { if (i == sel_set) I.accept = 1; else { sel_set = i; snd_post("Play_MenuMoveLowShort", 0); } }
        if (ui_hit(in, 720, 450, 200, 40)) I.back = 1;
        if (in->swipe_y) I.up = in->swipe_y > 0, I.down = in->swipe_y < 0;
        if (in->up) { sel_set = (sel_set + FE_NSETS - 1) % FE_NSETS; snd_post("Play_MenuMoveLowShort", 0); }
        if (in->down) { sel_set = (sel_set + 1) % FE_NSETS; snd_post("Play_MenuMoveLowShort", 0); }
        if (in->back) front_enter(FE_MAIN);
        if (in->accept && FE_NSETS) { levels_set = sel_set; sel_level = 0; lv_row = 0; lv_col = 1; front_enter(FE_LEVELS); }
        break; }
    case FE_LEVELS: {                                        /* Arcade's level select (0x6e4640) */
        fe_set *s = &FE_SETS[levels_set];
        int step = 0;
        /* touch: the buttons as drawn, the neighbours' pictures step, the level's picture is Go, a sideways drag steps */
        if (ui_hit(in, 290, 438, 190, 40)) { lv_row = 0; lv_col = 0; I.accept = 1; }
        if (ui_hit(in, 480, 438, 150, 40) || ui_hit(in, 480, 190, 448, 252)) { lv_row = 0; lv_col = 1; I.accept = 1; }
        if (ui_hit(in, 670, 438, 190, 40)) { lv_row = 0; lv_col = 2; I.accept = 1; }
        for (int i = 0; i < 4; i++) if (ui_hit(in, 480 + (i - 1.5f) * 214, 488, 204, 40)) { lv_row = 1; lv_col = i; I.accept = 1; }
        if (ui_hit(in, 110, 206, 200, 112) || in->swipe_x > 0) step = -1;
        if (ui_hit(in, 850, 206, 200, 112) || in->swipe_x < 0) step = 1;
        if (in->l) step = -1;
        if (in->r) step = 1;
        if (in->left && lv_col > 0) { lv_col--; snd_post("Play_MenuMoveLowShort", 0); }
        if (in->right && lv_col < (lv_row ? 3 : 2)) { lv_col++; snd_post("Play_MenuMoveLowShort", 0); }
        if (in->down && lv_row == 0) { lv_row = 1; snd_post("Play_MenuMoveLowShort", 0); }
        if (in->up && lv_row == 1) { lv_row = 0; if (lv_col > 2) lv_col = 2; snd_post("Play_MenuMoveLowShort", 0); }
        if (in->back) { front_enter(FE_SETS_SCREEN); break; }
        if (in->alt) { front_enter(FE_VEHICLES); break; }       /* Triangle: Vehicles */
        if (in->btn == BTN_SQUARE) { S->ghosts = !S->ghosts; save_write(S, g_save_path); }
        if (in->accept) {
            int b = lv_row * 3 + lv_col;                     /* 0 < Previous, 1 Go, 2 Next >, 3 Vehicles, 4 Results, 5 Ghost Replays, 6 Back */
            if (b == 0) step = -1;
            else if (b == 2) step = 1;
            else if (b == 3) { front_enter(FE_VEHICLES); break; }
            else if (b == 4 && s->n) { front_enter(FE_LEVELRESULTS); break; }
            else if (b == 5) { S->ghosts = !S->ghosts; save_write(S, g_save_path); }   /* action 0x55 */
            else if (b == 6) { front_enter(FE_SETS_SCREEN); break; }
            else if (b == 1 && s->n) {                       /* Go (action 0x4a) */
                fe_level *l = &s->lv[sel_level];
                if (!level_unlocked(l) || !l->installed) {}
                else if (S->ghosts && l->score) front_enter(FE_GHOSTSEL);
                else { menu_music_on = 0; *out_set = levels_set; *out_level = sel_level; return FE_PLAY; }
            }
        }
        if (step && s->n) sel_level = (sel_level + s->n + step) % s->n;
        break; }
    case FE_GHOSTSEL: {
        int r = ui_dlg_update(&dlg, in);
        if (r == -2 || (r >= 0 && dlg_cats[r] < 0)) front_enter(FE_LEVELS);
        else if (r >= 0) { ghost_cat = dlg_cats[r]; menu_music_on = 0; *out_set = levels_set; *out_level = sel_level; return FE_PLAY; }
        break; }
    case FE_LEVELRESULTS:
        if (res_update(dt, in) == RES_BACK) front_enter(FE_LEVELS);
        break;
    case FE_VEHICLES: {                                      /* Selected Vehicle Name, < Previous, Next >, Vehicle Description */
        int step = in->l ? -1 : in->r ? 1 : 0;
        if (ui_hit(in, 290, 446, 190, 42)) { veh_btn = 0; I.accept = 1; }
        if (ui_hit(in, 670, 446, 190, 42)) { veh_btn = 1; I.accept = 1; }
        if (ui_hit(in, 480, 496, 190, 42)) { veh_btn = 2; I.accept = 1; }
        if (in->swipe_x) step = in->swipe_x > 0 ? -1 : 1;
        if (in->left && veh_btn == 1) { veh_btn = 0; snd_post("Play_MenuMoveLowShort", 0); }
        if (in->right && veh_btn == 0) { veh_btn = 1; snd_post("Play_MenuMoveLowShort", 0); }
        if (in->down && veh_btn < 2) { veh_btn = 2; snd_post("Play_MenuMoveLowShort", 0); }
        if (in->up && veh_btn == 2) { veh_btn = 1; snd_post("Play_MenuMoveLowShort", 0); }
        if (in->accept && veh_btn < 2) step = veh_btn == 0 ? -1 : 1;
        if (step && FE_NVEH) {
            veh_sel = (veh_sel + FE_NVEH + step) % FE_NVEH;
            if (vehicle_unlocked(veh_sel)) { S->vehicle = veh_sel; save_write(S, g_save_path); }
        }
        if (in->back || (in->accept && veh_btn == 2)) front_enter(levels_story ? FE_MAIN : FE_LEVELS);
        break; }
    case FE_ACHIEVEMENTS: {
        if (dlg.open) {                                      /* "Reset ALL Achievements?" (0x6f7e80) */
            int r = ui_dlg_update(&dlg, in);
            if (r == 0) {
                S->ach = 0; S->gold_shown = 0;
                ach_unlock("Impermanence");                  /* the lifetime stats and the vehicles stay */
                save_write(S, g_save_path);
            }
            break;
        }
        int nv = ach_visible();
        {   /* touch: a row selects it, a drag scrolls, the two buttons */
            int top = ach_sel - 2 < 0 ? 0 : ach_sel - 2;
            if (top > nv - 5) top = nv - 5 < 0 ? 0 : nv - 5;
            for (int row = top; row < nv && row < top + 5; row++)
                if (hit_r(in, 60, 60 + (row - top) * 60, 840, 56) && (ach_sel != row || ach_btn)) { ach_sel = row; ach_btn = 0; snd_post("Play_MenuMoveLowShort", 0); }
            if (in->swipe_y < 0 && ach_sel < nv - 1) { ach_sel++; ach_btn = 0; snd_post("Play_MenuMoveLowShort", 0); }
            if (in->swipe_y > 0 && ach_sel > 0) { ach_sel--; ach_btn = 0; snd_post("Play_MenuMoveLowShort", 0); }
            if (ui_hit(in, 370, 494, 300, 42)) { ach_btn = 1; I.accept = 1; }
            if (ui_hit(in, 650, 494, 190, 42)) { ach_btn = 2; I.accept = 1; }
        }
        if (ach_btn == 0) {
            if (in->up && ach_sel > 0) { ach_sel--; snd_post("Play_MenuMoveLowShort", 0); }
            else if (in->down) { if (ach_sel < nv - 1) ach_sel++; else ach_btn = 1; snd_post("Play_MenuMoveLowShort", 0); }
        } else {
            if (in->up) { ach_btn = 0; snd_post("Play_MenuMoveLowShort", 0); }
            if (in->left && ach_btn == 2) { ach_btn = 1; snd_post("Play_MenuMoveLowShort", 0); }
            if (in->right && ach_btn == 1) { ach_btn = 2; snd_post("Play_MenuMoveLowShort", 0); }
            if (in->accept && ach_btn == 1) { snd_post("Play_MenuMoveLowShort", 0); ui_dlg_open(&dlg, "Reset ALL Achievements?", 2, "Yes", "No", NULL); dlg.sel = 1; break; }
        }
        if (in->back || (in->accept && ach_btn == 2)) return leave();
        break; }
    case FE_CONTROLS: {
        /* rows: the actions, the sticks, Use Advanced Controls, Reset to Defaults / Back. Left/Right steps an action
         * through the buttons; Cross waits for the button to use (5 s). A button taken by another action swaps with
         * it (the Roll may share the brake's) */
        if (ctl_capture) {
            ctl_capture_t -= dt;
            if (in->tap) { ctl_capture = 0; break; }          /* (a tap: not a button - cancel) */
            if (in->btn >= 0 && ctl_capture_t < 4.85f) {
                if (sel_ctl == ACT_TRICK && !trick_ok(in->btn)) {}
                else { bind_set(sel_ctl, in->btn); ctl_capture = 0; snd_post("Play_MenuMoveLowShort", 0); }
            }
            else if (ctl_capture_t <= 0) ctl_capture = 0;
            break;
        }
        /* touch: a row selects it (again: Set, or the switch on the two switch rows), the two buttons */
        for (int i = 0; i < CTL_RESET; i++) if (hit_r(in, 130, 56 + i * CTL_DY, 700, CTL_DY - 3)) {
            if (i != sel_ctl) { sel_ctl = i; snd_post("Play_MenuMoveLowShort", 0); if (i >= ACT_N) I.accept = 1; }
            else I.accept = 1;
        }
        if (ui_hit(in, 370, 474, 300, 42)) { sel_ctl = CTL_RESET; I.accept = 1; }
        if (ui_hit(in, 650, 474, 190, 42)) { sel_ctl = CTL_BACK; I.accept = 1; }
        if (in->up) { sel_ctl = sel_ctl == CTL_BACK ? CTL_RESET - 1 : (sel_ctl + CTL_N - 1) % CTL_N; snd_post("Play_MenuMoveLowShort", 0); }
        else if (in->down) { sel_ctl = sel_ctl >= CTL_RESET ? 0 : sel_ctl + 1; snd_post("Play_MenuMoveLowShort", 0); }
        else if (sel_ctl >= CTL_RESET && (in->left || in->right)) { sel_ctl = in->left ? CTL_RESET : CTL_BACK; snd_post("Play_MenuMoveLowShort", 0); }
        else if (sel_ctl < ACT_N && (in->left || in->right)) {
            int a = sel_ctl, b = (S->bind[a] + (in->right ? 1 : BTN_N - 1)) % BTN_N;
            if (a == ACT_TRICK) for (int k = 0; k < BTN_N && !trick_ok(b); k++) b = (b + (in->right ? 1 : BTN_N - 1)) % BTN_N;
            bind_set(a, b); snd_post("Play_MenuMoveLowShort", 0);
        } else if (sel_ctl < ACT_N && in->accept) { ctl_capture = 1; ctl_capture_t = 5; snd_post("Play_MenuMoveLowShort", 0); }
        else if (sel_ctl == CTL_STICKS && (in->accept || in->left || in->right)) S->swap_sticks = !S->swap_sticks;
        else if (sel_ctl == CTL_ADV && (in->accept || in->left || in->right)) S->adv_controls = !S->adv_controls;
        else if (sel_ctl == CTL_AIR && (in->accept || in->left || in->right)) S->air_tricks = !S->air_tricks;
        else if (sel_ctl == CTL_RESET && in->accept) {
            for (int k = 0; k < ACT_N; k++) S->bind[k] = ACT_DEFAULT[k];
            S->swap_sticks = 0; S->air_tricks = 1; snd_post("Play_MenuMoveLowShort", 0);
        }
        if (in->back || (sel_ctl == CTL_BACK && in->accept)) return leave();
        break; }
    case FE_OPTIONS: {
        /* touch: a switch row switches, a finger on a slider moves it, Back */
        if (!in->touch) slider_drag = -1;
        for (int i = 0; i < OPT_N; i++) {
            float y = 76 + i * 39;
            if (i == OPT_BACK) { if (ui_hit(in, 480, y + 6, 220, 40)) { sel_opt = i; I.accept = 1; } continue; }
            float *v = opt_vol(i);
            if (v && in->touch && (slider_drag == i || (slider_drag < 0 && in->hx >= 440 && in->hx <= 850 && fabsf(in->hy - y) <= 18))) {
                if (slider_drag != i) { slider_drag = i; sel_opt = i; }
                float nv_ = (in->hx - 490) / 310.0f; nv_ = nv_ < 0 ? 0 : nv_ > 1 ? 1 : nv_;
                if (fabsf(nv_ - *v) > 0.004f) { *v = nv_; front_apply_volumes(); if (i == OPT_MUSIC) snd_post("Resume_Music", 0); }
            }
            if (!v && hit_r(in, 130, y - 17, 700, 34)) { sel_opt = i; I.accept = 1; }
        }
        if (in->up) { sel_opt = (sel_opt + OPT_N - 1) % OPT_N; snd_post("Play_MenuMoveLowShort", 0); }
        if (in->down) { sel_opt = (sel_opt + 1) % OPT_N; snd_post("Play_MenuMoveLowShort", 0); }
        float *v = opt_vol(sel_opt); int *f = opt_flag(sel_opt);
        if (v && (in->left || in->right)) {
            *v += in->right ? 0.05f : -0.05f;
            if (*v < 0) *v = 0; if (*v > 1) *v = 1;
            front_apply_volumes();
            if (sel_opt == OPT_MUSIC) snd_post("Resume_Music", 0);
        }
        if (f && (in->accept || in->left || in->right)) *f = !*f;
        if (in->back || (sel_opt == OPT_BACK && in->accept)) return leave();
        break; }
    case FE_CREDITS:
        credits_y -= dt * 40.0f;
        if (in->tap) I.accept = 1;
        if (in->back || in->accept || credits_y < -(credits_lines * 28.0f + 400)) {
            snd_post("Stop_Music", 0);
            front_enter(FE_MAIN);
        }
        break;
    case FE_QUIT:
        if (ui_hit(in, 360, 300, 200, 56)) { sel_quit = 0; I.accept = 1; }
        if (ui_hit(in, 600, 300, 200, 56)) { sel_quit = 1; I.accept = 1; }
        if (in->left || in->right || in->up || in->down) { sel_quit = !sel_quit; snd_post("Play_MenuMoveLowShort", 0); }
        if (in->back) front_enter(FE_MAIN);
        if (in->accept) { if (sel_quit == 0) return FE_EXIT; front_enter(FE_MAIN); }      /* ("QUIT GAME!": no voice) */
        break;
    }
    return FE_STAY;
}

/* ------------------------------------------------------------------ camera over the menu scene */
void front_camera(float dt, v3 *eye, v3 *fwd, float *fade)
{
    *fade = 0;
    if (!npans) { *eye = v3make(-110, -320, -8); *fwd = norm(sub(v3make(dummy_car[0], dummy_car[1], dummy_car[2]), *eye)); return; }
    float *p = cam_pans[pan_i % npans];
    v3 a = v3make(p[0], p[1], p[2]), b = v3make(p[3], p[4], p[5]);
    float d = len(sub(b, a)), dur = d / (p[6] > 0.01f ? p[6] : 1);
    if (dur < 3) dur = 3;
    pan_t += dt;
    if (pan_t > dur) { pan_t = 0; pan_i++; p = cam_pans[pan_i % npans]; a = v3make(p[0], p[1], p[2]); b = v3make(p[3], p[4], p[5]); }
    float t = pan_t / dur; if (t > 1) t = 1;
    *eye = lerp3(a, b, t);
    v3 dc = v3make(dummy_car[0], dummy_car[1], dummy_car[2] + 0.4f), df = v3make(dummy_fly[0], dummy_fly[1], dummy_fly[2]);
    v3 target = len(sub(b, dc)) < len(sub(b, df)) ? dc : df;
    *fwd = norm(sub(target, *eye));
    float fl = 1.0f / (pan_fade > 0.1f ? pan_fade : 2.0f);           /* fade through black between shots */
    if (pan_t < fl) *fade = 1 - pan_t / fl;
    else if (pan_t > dur - fl) *fade = (pan_t - (dur - fl)) / fl;
}

/* ------------------------------------------------------------------ drawing */
static void title_logo(float y, float w, unsigned a)
{
    unsigned t = hud_tex("rush4");
    hud_image(t, 480 - w / 2, y, w, w / 2, 0xFFFFFF00 | a);
}
static void hint(const char *s) { hud_text(HUD_FONT_MAIN, 480, 518, 17, 0xC0D0E0D0, HUD_CENTER, s); }
/* text word-wrapped to about cols characters a line, from y; returns the y after it */
static float wrapped(float x, float y, float size, float gap, int cols, unsigned col, int align, const char *p)
{
    char line[128];
    while (*p) {
        const char *e = p, *last_sp = NULL;
        while (*e && *e != '|' && (e - p) < cols) { if (*e == ' ') last_sp = e; e++; }
        if (*e && *e != '|' && last_sp) e = last_sp;
        int k = (int)(e - p); if (k > 127) k = 127;
        memcpy(line, p, k); line[k] = 0;
        hud_text(HUD_FONT_MAIN, x, y, size, col, align, line);
        y += size * gap; p = *e ? e + 1 : e;
    }
    return y;
}
static void ach_desc(char *d, size_t n, int i)
{
    snprintf(d, n, "%s", FE_ACH[i].desc);
    for (char *q = d; *q; q++) if (*q == '|') *q = ' ';
    char *ps = strstr(d, "(Press Select)");                   /* the button set for Reset in Controls */
    if (ps) { ps[7] = btn_icon(S->bind[ACT_RESET]); ps[8] = ')'; ps[9] = 0; }
}

void front_draw(void)
{
    switch (scr) {
    case FE_SPLASH: {
        hud_rect(0, 0, 960, 544, 0x000000FF);
        float len = 3.0f, t = splash_t;
        float a = t < 0.6f ? t / 0.6f : t > len - 0.6f ? (len - t) / 0.6f : 1;
        if (a < 0) a = 0; if (a > 1) a = 1;
        unsigned A = (unsigned)(a * 255);
        if (splash_i == 0) hud_image(hud_tex("dplogo"), 480 - 392, 272 - 106, 784, 212, 0xFFFFFF00 | A);
        else if (splash_i == 1) hud_image(hud_tex("kysebad"), 480 - 380, 272 - 95, 760, 190, 0xFFFFFF00 | A);
        break; }
    case FE_TITLE: {
        float a = scr_t < 1 ? scr_t : 1;
        title_logo(30, 640, (unsigned)(a * 255));
        float pulse = 0.55f + 0.45f * sinf(scr_t * 3.0f);
        hud_text(HUD_FONT_MAIN, 480, 440, 30, 0xFFFFFF00 | (unsigned)(pulse * a * 255), HUD_CENTER, "Press any button to play");
        break; }
    case FE_MAIN:
        title_logo(4, 400, 255);
        for (int i = 0; i < 7; i++) ui_button(480, 222 + i * 45, 300, 52, MAIN_ITEMS[i], i == sel_main, 1);
        break;
    case FE_TUTPROMPT: {
        title_logo(4, 400, 255);
        static const char *T[3] = { "Yes", "No", "Back" };
        hud_text(HUD_FONT_TITLE, 480, 228, 40, 0xFFFFFFFF, HUD_CENTER, "Play Tutorial?");
        for (int i = 0; i < 3; i++) ui_button(480, 312 + i * 54, 300, 50, T[i], i == tut_sel, 1);
        break; }
    case FE_SETS_SCREEN: {
        hud_rect(0, 0, 960, 544, 0x00000060);
        hud_text(HUD_FONT_TITLE, 480, 22, 44, 0xFFFFFFFF, HUD_CENTER, "Arcade");
        int n = FE_NSETS, top = sel_set - 4 < 0 ? 0 : sel_set - 4;
        if (top > n - 7) top = n - 7 < 0 ? 0 : n - 7;
        for (int i = top; i < n && i < top + 7; i++) ui_button(300, 110 + (i - top) * 50, 360, 54, FE_SETS[i].name, i == sel_set, 1);
        if (n) {
            fe_set *s = &FE_SETS[sel_set];
            hud_rect(510, 100, 420, 300, 0x000000A0);
            hud_text(HUD_FONT_MAIN, 720, 112, 26, 0x9FE8FFFF, HUD_CENTER, s->name);
            wrapped(530, 160, 20, 1.3f, 34, 0xE0E8F0FF, HUD_LEFT, s->desc);
            char c[48]; snprintf(c, sizeof c, "%d levels", s->n);
            hud_text(HUD_FONT_MAIN, 720, 370, 20, 0xFFE070FF, HUD_CENTER, c);
        }
        ui_button(720, 450, 200, 40, "Back", 0, 1);
        hint("\x11" " Select     " "\x12" " Back");
        break; }
    case FE_LEVELS: case FE_GHOSTSEL: {
        fe_set *s = &FE_SETS[levels_set];
        hud_rect(0, 0, 960, 544, 0x00000070);
        hud_text(HUD_FONT_TITLE, 480, 12, 36, 0xFFFFFFFF, HUD_CENTER, s->name);
        if (!s->n) break;
        fe_level *l = &s->lv[sel_level];
        int unlocked = level_unlocked(l);
        for (int k = -1; k <= 1; k += 2) {                   /* the neighbours */
            fe_level *o = &s->lv[(sel_level + k + s->n) % s->n];
            float x = 480 + k * 370;
            hud_image(hud_tex(o->img[0] ? o->img : "levelimage_none"), x - 100, 150, 200, 112, 0xFFFFFF60);
        }
        unsigned img = hud_tex(l->img[0] ? l->img : "levelimage_none");
        if (!img) img = hud_tex("levelimage_none");
        hud_image(img, 480 - 224, 64, 448, 252, unlocked ? 0xFFFFFFFF : 0x808080FF);
        hud_image(hud_tex("levelimage_border"), 480 - 236, 56, 472, 268, 0xFFFFFFFF);
        if (!unlocked) hud_image(hud_tex("levelimage_locked"), 480 - 60, 130, 120, 120, 0xFFFFFFE0);
        hud_text(HUD_FONT_MAIN, 480, 330, 28, 0xFFFFFFFF, HUD_CENTER, l->name);
        char b[96];
        save_level *sl = save_level_get(S, l->xml, 0);
        if (!l->installed) hud_text(HUD_FONT_MAIN, 480, 364, 20, 0xFF8080FF, HUD_CENTER, "Not in the data packs");
        else if (!unlocked) hud_text(HUD_FONT_MAIN, 480, 364, 20, 0xFFB060FF, HUD_CENTER, "Locked: finish the previous level");
        else if (l->sd == 1 && sl && sl->done) { char pb[24]; fmt_num(pb, sizeof pb, sl->score); snprintf(b, sizeof b, "Best Score: %s", pb); hud_text(HUD_FONT_MAIN, 480, 364, 22, 0xFFE070FF, HUD_CENTER, b); }
        else if (sl && sl->best > 0) {
            char tb[32]; fmt_time(tb, sizeof tb, sl->best);
            snprintf(b, sizeof b, "Best Time: %s", tb);
            hud_text(HUD_FONT_MAIN, 480, 364, 22, 0xFFE070FF, HUD_CENTER, b);
        } else hud_text(HUD_FONT_MAIN, 480, 364, 20, 0xC0D0E0FF, HUD_CENTER, "Not completed yet");
        snprintf(b, sizeof b, "%d / %d", sel_level + 1, s->n);
        hud_text(HUD_FONT_MAIN, 480, 392, 17, 0x9FE8FFFF, HUD_CENTER, b);
        int f = scr == FE_LEVELS ? lv_row * 3 + lv_col : -1;
        ui_button(290, 438, 190, 40, "< Previous", f == 0, 1);
        ui_button(480, 438, 150, 40, "Go", f == 1, 1);
        ui_button(670, 438, 190, 40, "Next >", f == 2, 1);
        static const char *R2[4] = { "Vehicles", "Results", " Ghost Replays", "Back" };
        for (int i = 0; i < 4; i++) {
            float x = 480 + (i - 1.5f) * 214;
            ui_button(x, 488, 204, 40, R2[i], f == 3 + i, 1);
            if (i == 2) toggle_sz(x - 84, 488, 30, S->ghosts, f == 5);
        }
        hint("\x15" "\x16" " Level     " "\x14" " Vehicles     " "\x13" " Ghost Replays     " "\x12" " Back");
        if (scr == FE_GHOSTSEL) ui_dlg_draw(&dlg);
        break; }
    case FE_LEVELRESULTS:
        res_draw(1);
        break;
    case FE_VEHICLES: {
        hud_rect(0, 0, 960, 544, 0x00000090);
        hud_text(HUD_FONT_TITLE, 480, 14, 40, 0xFFFFFFFF, HUD_CENTER, "Vehicles");
        if (!FE_NVEH) break;
        fe_vehicle *v = &FE_VEH[veh_sel];
        int un = vehicle_unlocked(veh_sel);
        unsigned t = hud_tex(un ? v->img : (v->img_locked[0] ? v->img_locked : v->img));
        hud_image(t, 480 - 200, 66, 400, 225, un ? 0xFFFFFFFF : 0xFFFFFFC0);
        hud_text(HUD_FONT_MAIN, 480, 300, 32, un ? 0x9FE8FFFF : 0xA0A8B0FF, HUD_CENTER, v->name);
        if (un && veh_sel == S->vehicle) hud_text(HUD_FONT_MAIN, 480, 336, 18, 0xFFE070FF, HUD_CENTER, "Selected");
        wrapped(480, 362, 20, 1.25f, 64, 0xE0E8F0FF, HUD_CENTER, un ? v->desc : v->locked_desc);
        ui_button(290, 446, 190, 42, "< Previous", veh_btn == 0, 1);
        ui_button(670, 446, 190, 42, "Next >", veh_btn == 1, 1);
        ui_button(480, 496, 190, 42, "Back", veh_btn == 2, 1);
        break; }
    case FE_ACHIEVEMENTS: {
        hud_rect(0, 0, 960, 544, 0x000000A0);
        hud_text(HUD_FONT_TITLE, 480, 10, 38, 0xFFFFFFFF, HUD_CENTER, "Achievements");
        int vis[64], nv; vis_list(vis, &nv);
        char c[64]; snprintf(c, sizeof c, "%d / %d", ach_count(), nv);
        hud_text(HUD_FONT_MAIN, 900, 22, 22, 0xFFE070FF, HUD_RIGHT, c);
        int top = ach_sel - 2 < 0 ? 0 : ach_sel - 2;
        if (top > nv - 5) top = nv - 5 < 0 ? 0 : nv - 5;
        for (int row = top; row < nv && row < top + 5; row++) {
            int i = vis[row];
            float y = 60 + (row - top) * 60;
            int done = S->ach >> i & 1, cur = row == ach_sel && ach_btn == 0;
            hud_rect(60, y, 840, 56, cur ? 0x1E6EA0C0 : 0x10203080);
            hud_image(hud_tex(done ? "trophy_gold" : "trophy_silver"), 70, y + 10, 36, 36, done ? 0xFFFFFFFF : 0xFFFFFF50);
            hud_text(HUD_FONT_MAIN, 118, y + 4, 22, done ? 0xFFFFFFFF : 0xA0B0C0FF, HUD_LEFT, FE_ACH[i].name);
            char d[128]; ach_desc(d, sizeof d, i);
            hud_text(HUD_FONT_MAIN, 118, y + 31, 16, 0xC0D0E0FF, HUD_LEFT, d);
            hud_text(HUD_FONT_MAIN, 888, y + 17, 19, done ? 0x80FF80FF : 0x808890FF, HUD_RIGHT, done ? "Complete!" : "Incomplete");
        }
        if (nv) {                                            /* Selected Achievement Progress */
            int i = vis[ach_sel < nv ? ach_sel : nv - 1];
            char pr[192]; ach_progress(i, pr, sizeof pr);
            hud_rect(60, 364, 840, 92, 0x00000080);
            int done = S->ach >> i & 1;
            if (!pr[0]) snprintf(pr, sizeof pr, "%s", done ? "Complete!" : "Incomplete");
            int lines = 1; for (const char *q = pr; *q; q++) if (*q == '|') lines++;
            hud_text_lines(HUD_FONT_MAIN, 480, 410 - lines * 12, 20, 1.2f, 0xFFE070FF, HUD_CENTER, pr);
        }
        ui_button(370, 494, 300, 42, "Reset Achievements", ach_btn == 1, 1);
        ui_button(650, 494, 190, 42, "Back", ach_btn == 2, 1);
        ui_dlg_draw(&dlg);
        break; }
    case FE_CONTROLS: {
        hud_rect(0, 0, 960, 544, 0x000000B0);
        hud_text(HUD_FONT_TITLE, 480, 10, 38, 0xFFFFFFFF, HUD_CENTER, "Controls");
        char b[64];
        for (int i = 0; i < CTL_RESET; i++) {
            float y = 56 + i * CTL_DY;
            int s = i == sel_ctl;
            hud_rect(130, y, 700, CTL_DY - 3, s ? 0x1E6EA0B0 : 0x10203070);
            unsigned lc = s ? 0xFFFFFFFF : 0xC0D0E0FF;
            if (i < ACT_N) {
                int dim = i == ACT_TRICK && !S->adv_controls;
                hud_text(HUD_FONT_MAIN, 148, y + 4, 22, dim ? 0x8090A0FF : lc, HUD_LEFT, ACT_NAME[i]);
                if (s && ctl_capture) snprintf(b, sizeof b, "Press a button...");
                else snprintf(b, sizeof b, "%s%c %s%s", s ? "\x19 " : "", btn_icon(S->bind[i]), btn_name(S->bind[i]), s ? " \x1a" : "");
                hud_text(HUD_FONT_MAIN, 812, y + 4, 22, s && ctl_capture ? 0xFFE070FF : dim ? 0x708090FF : 0x9FE8FFFF, HUD_RIGHT, b);
            } else if (i == CTL_STICKS) {
                hud_text(HUD_FONT_MAIN, 148, y + 4, 22, lc, HUD_LEFT, S->adv_controls ? "Steering / Camera" : "Steering / Tricks");
                snprintf(b, sizeof b, S->adv_controls ? "%c Steer    %c Camera    \x17\x18\x19\x1a Tricks" : "%c Steer    %c or \x17\x18\x19\x1a Tricks",
                         S->swap_sticks ? HUD_ICON_RSTICK : HUD_ICON_LSTICK, S->swap_sticks ? HUD_ICON_LSTICK : HUD_ICON_RSTICK);
                hud_text(HUD_FONT_MAIN, 812, y + 4, 22, 0x9FE8FFFF, HUD_RIGHT, b);
            } else if (i == CTL_ADV) {
                hud_text(HUD_FONT_MAIN, 148, y + 4, 22, lc, HUD_LEFT, " Use Advanced Controls");
                toggle_sz(800, y + 15, 30, S->adv_controls, s);
            } else {
                hud_text(HUD_FONT_MAIN, 148, y + 4, 22, lc, HUD_LEFT, " Steering Stick Tricks In The Air");
                toggle_sz(800, y + 15, 30, S->air_tricks, s);
            }
        }
        ui_button(370, 474, 300, 42, "Reset to Defaults", sel_ctl == CTL_RESET, 1);
        ui_button(650, 474, 190, 42, "Back", sel_ctl == CTL_BACK, 1);
        if (ctl_capture) hint("Press the button to use (any button; wait to cancel)");
        else if (sel_ctl == ACT_TRICK) hint("Advanced Controls: hold it and the steering stick does the tricks (it may share the brake's button)");
        else if (sel_ctl == CTL_ADV) hint("The right stick turns the camera; the Roll button and the steering stick do the tricks");
        else if (sel_ctl == CTL_AIR) hint("In the air with the wings in, the steering stick flips and rolls the car");
        else if (sel_ctl < ACT_N) hint("\x11" " Set     " "\x19" "\x1a" " Change     " "\x12" " Back");
        else hint("\x11" " Select     " "\x12" " Back");
        break; }
    case FE_OPTIONS: {
        hud_rect(0, 0, 960, 544, 0x00000090);
        hud_text(HUD_FONT_TITLE, 480, 10, 38, 0xFFFFFFFF, HUD_CENTER, "Options");
        for (int i = 0; i < OPT_N; i++) {
            float y = 76 + i * 39;
            int s = i == sel_opt;
            if (i == OPT_BACK) { ui_button(480, y + 6, 220, 40, OPT_NAME[i], s, 1); continue; }
            hud_rect(130, y - 17, 700, 34, s ? 0x1E6EA0B0 : 0x10203070);
            hud_text(HUD_FONT_MAIN, 148, y - 12, 24, s ? 0xFFFFFFFF : 0xC0D0E0FF, HUD_LEFT, OPT_NAME[i]);
            float *v = opt_vol(i); int *f = opt_flag(i);
            if (v) ui_slider(480, y, 330, *v, s);
            else if (f) toggle_sz(800, y, 32, *f, s);
        }
        hint("\x19" "\x1a" " Change     " "\x12" " Back");
        break; }
    case FE_CREDITS: {
        hud_rect(0, 0, 960, 544, 0x000000C0);
        float y = credits_y;
        hud_image(hud_tex("credits_nitronic_rush"), 480 - 256, y - 300, 512, 256, 0xFFFFFFFF);
        const char *p = credits;
        while (p && *p) {
            const char *e = strchr(p, '\n'); if (!e) e = p + strlen(p);
            if (y > -40 && y < 560) {
                char ln[128]; int k = (int)(e - p); if (k > 127) k = 127;
                memcpy(ln, p, k); ln[k] = 0;
                int heading = k > 0 && isupper((unsigned char)ln[0]) && (k < 3 || isupper((unsigned char)ln[1]));
                hud_text(HUD_FONT_MAIN, 480, y, heading ? 26 : 22, heading ? 0x9FE8FFFF : 0xFFFFFFFF, HUD_CENTER, ln);
            }
            y += 28;
            p = *e ? e + 1 : e;
        }
        break; }
    case FE_QUIT: {
        hud_rect(0, 0, 960, 544, 0x000000A0);
        hud_text(HUD_FONT_TITLE, 480, 170, 46, 0xFFFFFFFF, HUD_CENTER, "Quit Game?");
        ui_button(360, 300, 200, 56, "Yes", sel_quit == 0, 1);
        ui_button(600, 300, 200, 56, "No", sel_quit == 1, 1);
        break; }
    }
    if (!overlay) ui_popup_draw();
}
