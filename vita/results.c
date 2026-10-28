/* The results screen (see results.h). Layout from re_N 11.3 (960x544). */
#include "results.h"
#include "scores.h"
#include "hud.h"
#include "snd.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static struct {
    char level[56]; int style, from_menu, rank, cat, sel, row;
    float run_time, t; int run_score, run_trick;
    sc_table tab;
} RS;

int res_category(void) { return RS.cat; }

static void menu_text(int font, float cx, float cy, float half_h, unsigned rgba, int align, const char *s)
{
    float scale = 9 * half_h * 544;
    hud_text(font, cx, cy - 0.642857f * half_h * 544, scale * 0.140625f, rgba, align, s);
}
static void fmt_time(char *b, size_t n, float t)    /* FormatTime 0x723b60: truncated */
{
    if (t < 0) t = 0;
    int s_ = (int)t;
    char f[16]; snprintf(f, sizeof f, "%f", fmod(t, 1.0)); f[4] = 0;
    snprintf(b, n, "%02d:%02d.%s", s_ / 60, s_ % 60, f + 2);
}

/* the categories in turn (0x6f4570): Local, Default, Developer, wrapping, the empty ones skipped (Default kept) */
static int cat_step(int cat, int dir)
{
    for (int k = 0; k < 3; k++) {
        cat += dir;
        if (cat > SC_DEVELOPER) cat = SC_LOCAL;
        if (cat < SC_LOCAL) cat = SC_DEVELOPER;
        if (sc_has(cat, RS.level)) return cat;
    }
    return cat;
}

void res_refresh(void)
{
    sc_get(RS.cat, RS.level, RS.style, &RS.tab);
    RS.row = -1;
    if (RS.from_menu) return;
    if (RS.cat == SC_LOCAL) {
        if (RS.rank <= 9) RS.row = RS.rank < RS.tab.n ? RS.rank : -1;
        else if (RS.tab.n < 12) {                     /* outside the ten: "[YOU]" as row 11 */
            sc_entry *e = &RS.tab.e[RS.tab.n]; memset(e, 0, sizeof *e);
            snprintf(e->initials, sizeof e->initials, "[YOU]"); e->time = RS.run_time; e->score = RS.run_score; e->trick = RS.run_trick;
            RS.row = RS.tab.n++;
        }
    } else if (RS.cat == SC_DEVELOPER) {              /* the run as "[YOU]" at its place (0x6f3750) */
        int r = sc_rank(&RS.tab, RS.run_time, RS.run_score, RS.run_trick);
        if (RS.tab.n < 12) {
            memmove(&RS.tab.e[r + 1], &RS.tab.e[r], (size_t)(RS.tab.n - r) * sizeof RS.tab.e[0]);
            sc_entry *e = &RS.tab.e[r]; memset(e, 0, sizeof *e);
            snprintf(e->initials, sizeof e->initials, "[YOU]"); e->time = RS.run_time; e->score = RS.run_score; e->trick = RS.run_trick;
            RS.tab.n++; RS.row = r;
        }
    } else {                                          /* the shipped table: the row at the run's place (as the PC) */
        RS.row = sc_rank(&RS.tab, RS.run_time, RS.run_score, RS.run_trick);
        if (RS.row >= RS.tab.n) RS.row = -1;
    }
}

void res_open(const char *level, int style, int from_menu, int rank, float time, int score, int trick)
{
    memset(&RS, 0, sizeof RS);
    snprintf(RS.level, sizeof RS.level, "%s", level);
    RS.style = style; RS.from_menu = from_menu; RS.rank = rank;
    RS.run_time = time; RS.run_score = score; RS.run_trick = trick;
    RS.cat = SC_LOCAL; RS.sel = from_menu ? 0 : 1;      /* the focus starts on Continue */
    res_refresh();
}

int res_update(float dt, const fe_input *in_)
{
    fe_input I = *in_; const fe_input *in = &I;     /* (touch presses the buttons in this copy) */
    RS.t += dt;
    /* touch: the categories' arrows (or a sideways drag), the buttons as drawn */
    if (ui_hit(in, 235.2f, 100, 260, 40) || in->swipe_x > 0) I.l = 1;
    if (ui_hit(in, 724.8f, 100, 260, 40) || in->swipe_x < 0) I.r = 1;
    if (RS.from_menu) { if (ui_hit(in, 480, 499.1f, 176.8f, 40)) I.accept = 1; }
    else for (int i = 0; i < 3; i++) if (ui_hit(in, 480 + (i - 1) * 180.6f, 499.1f, 176.8f, 40)) { RS.sel = i; I.accept = 1; }
    if (in->l || in->r) { RS.cat = cat_step(RS.cat, in->r ? 1 : -1); res_refresh(); }    /* (pressing makes no sound on the PC) */
    if (RS.from_menu) {
        if ((in->accept || in->back) && RS.t > 0.2f) return RES_BACK;
        return RES_NONE;
    }
    if (in->left && RS.sel > 0) { RS.sel--; snd_post("Play_MenuMoveLowShort", 0); }
    if (in->right && RS.sel < 2) { RS.sel++; snd_post("Play_MenuMoveLowShort", 0); }
    if (in->accept && RS.t > 0.3f) return RS.sel == 0 ? RES_QUIT : RS.sel == 1 ? RES_CONTINUE : RES_RESTART;
    return RES_NONE;
}

void res_draw(float alpha)
{
    char b[64], c2[64];
    hud_rect(0, 0, 960, 544, (unsigned)(0.75f * 255 * alpha));     /* HudBlackSquare */
    menu_text(HUD_FONT_TITLE, 480, 57.1f, 0.08f, 0xFFFFFFFF, HUD_CENTER, "Results");
    snprintf(b, sizeof b, "%s Scores", sc_category_name(RS.cat));
    menu_text(HUD_FONT_MAIN, 480, 94.1f, 0.048f, 0xFFFFFFFF, HUD_CENTER, b);
    int prev = cat_step(RS.cat, -1), next = cat_step(RS.cat, 1);
    snprintf(b, sizeof b, "< %s \x15", sc_category_name(prev));
    menu_text(HUD_FONT_MAIN, 235.2f, 93.8f, 0.0325f, 0xC8D8F0FF, HUD_CENTER, b);
    snprintf(b, sizeof b, "\x16 %s >", sc_category_name(next));
    menu_text(HUD_FONT_MAIN, 724.8f, 93.8f, 0.0325f, 0xC8D8F0FF, HUD_CENTER, b);
    /* the table: headers (Scale 0.3H, line top 0.65H up), then the rows */
    const float sz = 0.3f * 544 * 0.140625f;
    hud_text(HUD_FONT_MAIN, 115.2f, 190.4f, sz, 0xFFFFFFE6, HUD_LEFT, "Time");
    hud_text(HUD_FONT_MAIN, 614.4f, 190.4f, sz, 0xFFFFFFE6, HUD_RIGHT, "Score");
    hud_text(HUD_FONT_MAIN, 864.0f, 190.4f, sz, 0xFFFFFFE6, HUD_RIGHT, "Best Trick");
    float y = 212.25f;
    for (int i = 0; i < RS.tab.n && i < 11; i++) {
        const sc_entry *e = &RS.tab.e[i];
        unsigned col = 0xFFFFFFE6;
        if (i == RS.row) {                            /* the run's row pulses pale green - green (1.57 s) */
            float k = 0.5f + 0.5f * sinf(RS.t * 6.2831853f / 1.57f);
            col = (unsigned)((0.685f + (0.1f - 0.685f) * k) * 255) << 24 | (unsigned)((0.98f + (0.95f - 0.98f) * k) * 255) << 16 |
                  (unsigned)((0.685f + (0.1f - 0.685f) * k) * 255) << 8 | 0xFF;
        }
        char rk[8]; sc_format_rank(rk, sizeof rk, i);
        snprintf(b, sizeof b, "%s. %s", rk, i >= 10 ? "[YOU]" : e->initials);
        hud_text(HUD_FONT_MAIN, 105.6f, y, sz, col, HUD_RIGHT, b);
        fmt_time(c2, sizeof c2, e->time); hud_text(HUD_FONT_MAIN, 115.2f, y, sz, col, HUD_LEFT, c2);
        snprintf(c2, sizeof c2, "%d", e->score); hud_text(HUD_FONT_MAIN, 614.4f, y, sz, col, HUD_RIGHT, c2);
        snprintf(c2, sizeof c2, "%d", e->trick); hud_text(HUD_FONT_MAIN, 864.0f, y, sz, col, HUD_RIGHT, c2);
        /* the next row: the lowest glyph of this row's rank and name (60/512, 'Q' 66/512, '_' 70/512) */
        float low = strchr(b, '_') ? 70 : strchr(b, 'Q') ? 66 : 60;
        y += 0.3f * 544 * low / 512 + 0.005f * 544;
    }
    if (RS.from_menu) ui_button(480, 499.1f, 176.8f, 35.4f, "Back", 1, alpha);
    else {
        static const char *BTN[3] = { "Quit", "Continue", "Restart" };
        for (int i = 0; i < 3; i++) ui_button(480 + (i - 1) * 180.6f, 499.1f, 176.8f, 35.4f, BTN[i], i == RS.sel, fminf(alpha, RS.t * 3));
    }
}
