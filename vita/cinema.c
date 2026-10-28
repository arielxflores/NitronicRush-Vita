/* The story's cinematics: see cinema.h (docs re_O). */
#include "cinema.h"
#include "hud.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NBLK 5
static const char *BLK_NAME[NBLK] = { "UpTheBlueBuilding", "CoreEyeTurnOn", "ZoomBackThroughCity", "ExplainingCar", "CityRestoring" };
static struct { float speed; int n; char line[24][100]; } BLK[NBLK];
static const char *STORY_KEY[6] = { "UpTheTowerText0", "UpTheTowerText1", "UpTheTowerText2", "FlickerOnText", "ZoomBackText", "FinalText" };
static struct { float time; char text[100]; } STORY[6];

/* the cinematic helper (this+0x50): the terminal (5.5), the story line (5.6), the ending's timers */
static struct {
    int intro, outro;
    float e4, e8, fc; int cursor, block, prev_block, lines, idx;
    char term[8][104]; float term_a[8];
    int story, sstate; float stimer, sdur, salpha;
    float t1, t2, f2, f3;
} H;
static float skip_t;                       /* SkipIntroLogic's time_val_ */

int cin_init(nr_pak *pak)
{
    uint32_t sz; char *t = nr_pak_read(pak, "ui/cinema_config.txt", &sz);
    memset(BLK, 0, sizeof BLK); memset(STORY, 0, sizeof STORY);
    for (int k = 0; k < NBLK; k++) BLK[k].speed = 0.03f;
    if (!t) return -1;
    for (char *ln = strtok(t, "\n"); ln; ln = strtok(NULL, "\n")) {
        char *eq = strchr(ln, '=');
        if (!eq) continue;
        *eq = 0; const char *key = ln; char *val = eq + 1;
        size_t vl = strlen(val); while (vl && (val[vl - 1] == '\r' || val[vl - 1] == '\n')) val[--vl] = 0;
        for (int k = 0; k < NBLK; k++) {
            size_t bl = strlen(BLK_NAME[k]);
            if (strncmp(key, BLK_NAME[k], bl)) continue;
            const char *rest = key + bl;
            if (!strcmp(rest, "TextSpeed")) BLK[k].speed = (float)atof(val);
            else if (!strcmp(rest, "NumLines")) { BLK[k].n = atoi(val); if (BLK[k].n > 24) BLK[k].n = 24; }
            else if (*rest >= '0' && *rest <= '9') { int i = atoi(rest); if (i >= 0 && i < 24) snprintf(BLK[k].line[i], sizeof BLK[k].line[i], "%s", val); }
        }
        for (int j = 0; j < 6; j++) {
            size_t kl = strlen(STORY_KEY[j]);
            if (strncmp(key, STORY_KEY[j], kl)) continue;
            if (!key[kl]) snprintf(STORY[j].text, sizeof STORY[j].text, "%s", val);
            else if (!strcmp(key + kl, "Time")) STORY[j].time = (float)atof(val);
        }
    }
    free(t);
    return BLK[0].n > 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ the terminal (0x6d5850) */
static void term_reset(void)
{
    H.e4 = 0; H.e8 = FLT_MAX; H.fc = 0; H.cursor = 0; H.block = H.prev_block = 5; H.lines = 0; H.idx = 0;
    memset(H.term, 0, sizeof H.term);
    for (int i = 0; i < 8; i++) H.term_a[i] = i == 0 ? 0.1f : i == 1 ? 0.2f : i == 2 ? 0.4f : 1.0f;
}
static void set_terminal(int k)            /* 0x6d39d0 (e8 is not reset) */
{
    if (H.block == k) return;
    H.block = k; H.e4 = 0; H.fc = 0; H.cursor = 0; H.idx = 0; H.lines = BLK[k].n;
}
static void advance_line(int n)            /* 0x6d5290: everything one slot up (a trailing cursor dropped) */
{
    if (n >= H.lines) return;
    H.idx = n;
    for (int i = 0; i < 7; i++) {
        size_t l = strlen(H.term[i + 1]);
        if (l && H.term[i + 1][l - 1] == '_') H.term[i + 1][l - 1] = 0;
        memcpy(H.term[i], H.term[i + 1], sizeof H.term[i]);
    }
    H.e4 = 0; H.fc = 0; H.cursor = 0;
}
static void terminal(float dt)
{
    if (dt > 0.0666667f) dt = 0.0666667f;
    char s[104] = "";
    float frac = H.e8 > 0 ? H.e4 / H.e8 : 0; frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;   /* (last frame's e8) */
    if (H.block >= 0 && H.block <= 4) {
        H.e4 += dt;
        const char *ln = BLK[H.block].line[H.idx];
        int L = (int)strlen(ln), k = (int)(L * frac);
        H.e8 = L * BLK[H.block].speed;
        if (k > L) k = L; if (k > 100) k = 100;
        memcpy(s, ln, k); s[k] = 0;
    }
    if (frac > 0.9999f) {                    /* the line is typed */
        H.fc += dt; if (H.fc > 0.35f) { H.cursor = !H.cursor; H.fc = 0; }
        if (H.e4 > H.e8 + 0.14f && s[0]) {
            advance_line(H.idx + 1);
            snprintf(H.term[7], sizeof H.term[7], "%s", strcmp(s, "|") ? s : "");
        }
        if (H.idx == 0 && H.block != 0 && H.block != 4 && H.prev_block != H.block) advance_line(0);   /* a new block: one line up */
        H.prev_block = H.block;
        if (H.cursor) strcat(s, "_");
    }
    if (s[0]) snprintf(H.term[7], sizeof H.term[7], "%s", strcmp(s, "|") ? s : "");
}

/* ------------------------------------------------------------------ the story line (0x6d4eb0) */
static void set_story(int j, int force)    /* 0x6d3970: never cuts into a line fading in or holding */
{
    if (H.story != j && (H.sstate == 2 || H.sstate == 3) && (force || H.stimer >= H.sdur)) { H.stimer = 0; H.story = j; H.sdur = 0.5f; H.sstate = 0; }
}
static void story(float dt)
{
    switch (H.sstate) {
    case 0: H.stimer += dt; H.salpha = fminf(1, fmaxf(0, H.stimer / H.sdur));
            if (H.stimer > H.sdur) { H.stimer = 0; H.sdur = H.story <= 5 ? STORY[H.story].time : 0; H.sstate = 1; } break;
    case 1: H.stimer += dt; if (H.stimer > H.sdur) { H.stimer = 0; H.sdur = 0.5f; H.sstate = 2; } break;
    case 2: H.stimer += dt; H.salpha = fminf(1, fmaxf(0, 1 - H.stimer / H.sdur));
            if (H.stimer > H.sdur) { H.stimer = 0.5f; H.sdur = 0.5f; H.sstate = 3; } break;
    }
}

void cin_draw(void)
{
    if (!H.intro && !H.outro) return;
    /* the 8 terminal lines (hud_cinema: 0.2H, from 0.025W / 0.085H up, 0.03H apart), the faint ones on top */
    for (int i = 0; i < 8; i++) {
        float a = H.term_a[i]; if (a <= 0.002f || !H.term[i][0]) continue;
        hud_text(HUD_FONT_MAIN, 24.78f, 497.68f - (7 - i) * 16.32f, 15.30f, 0xFFFFFF00 | (unsigned)(fminf(1, a) * 255), HUD_LEFT, H.term[i]);
    }
    if (H.intro && H.story <= 5 && H.salpha > 0.002f && STORY[H.story].text[0])   /* right-aligned at 0.95W, 0.3H */
        hud_text(HUD_FONT_MAIN, 912, 27.5f, 22.95f, 0xFFFFFF00 | (unsigned)(fminf(1, H.salpha) * 255), HUD_RIGHT, STORY[H.story].text);
}
void cin_end(void) { memset(&H, 0, sizeof H); H.story = 6; H.sstate = 3; }
static void fade_texts(float v)            /* 0x6d47f0: the terminal's alphas times v, every frame */
{
    if (v < 0) v = 0;
    for (int i = 0; i < 8; i++) H.term_a[i] *= v;
}

/* ------------------------------------------------------------------ the skip prompt (SkipIntroLogic 0x6ba880) */
void cin_skip_update(float dt, int any_down)
{
    if (any_down) skip_t = 1.25f;          /* on_time 0.25 + fade_time 1.0 */
    else if (skip_t > 0) skip_t -= dt;
}
void cin_skip_draw(void)
{
    float a = fminf(1, skip_t / 1.0f);
    if (a <= 0) return;
    hud_text_lines(HUD_FONT_MAIN, 931.2f, 489.6f, 22.95f, 1.0f, 0xFFFFFF00 | (unsigned)(a * 255), HUD_RIGHT, "Hold " "\x14" "|to Skip");
}

/* ------------------------------------------------------------------ the intro (CampaignIntroLevel + corespincam) */
typedef struct { float A[3], B[3], QA[4], QB[4], T, D; } cam_rec;
static struct {
    int phase; float timer, dur, fade, bright, spin;
    int strobe, eye_i; float eye_glow, lightning, F;
    int cs; cam_rec rec[5];
    float pos[3], R[3][3];                 /* columns: right, up, back (it looks along -col2) */
} I;

static void quat_from(const float R[3][3], float q[4])     /* (w, x, y, z) */
{
    float tr = R[0][0] + R[1][1] + R[2][2];
    if (tr > 0) { float s = sqrtf(tr + 1) * 2; q[0] = 0.25f * s; q[1] = (R[2][1] - R[1][2]) / s; q[2] = (R[0][2] - R[2][0]) / s; q[3] = (R[1][0] - R[0][1]) / s; }
    else if (R[0][0] > R[1][1] && R[0][0] > R[2][2]) { float s = sqrtf(1 + R[0][0] - R[1][1] - R[2][2]) * 2; q[0] = (R[2][1] - R[1][2]) / s; q[1] = 0.25f * s; q[2] = (R[0][1] + R[1][0]) / s; q[3] = (R[0][2] + R[2][0]) / s; }
    else if (R[1][1] > R[2][2]) { float s = sqrtf(1 + R[1][1] - R[0][0] - R[2][2]) * 2; q[0] = (R[0][2] - R[2][0]) / s; q[1] = (R[0][1] + R[1][0]) / s; q[2] = 0.25f * s; q[3] = (R[1][2] + R[2][1]) / s; }
    else { float s = sqrtf(1 + R[2][2] - R[0][0] - R[1][1]) * 2; q[0] = (R[1][0] - R[0][1]) / s; q[1] = (R[0][2] + R[2][0]) / s; q[2] = (R[1][2] + R[2][1]) / s; q[3] = 0.25f * s; }
}
static void mat_from(const float q[4], float R[3][3])
{
    float w = q[0], x = q[1], y = q[2], z = q[3];
    R[0][0] = 1 - 2 * (y * y + z * z); R[0][1] = 2 * (x * y - w * z); R[0][2] = 2 * (x * z + w * y);
    R[1][0] = 2 * (x * y + w * z); R[1][1] = 1 - 2 * (x * x + z * z); R[1][2] = 2 * (y * z - w * x);
    R[2][0] = 2 * (x * z - w * y); R[2][1] = 2 * (y * z + w * x); R[2][2] = 1 - 2 * (x * x + y * y);
}
static void nlerp(const float a[4], const float b_[4], float s, float o[4])   /* hemisphere-corrected, normalised */
{
    float b[4] = { b_[0], b_[1], b_[2], b_[3] };
    if (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0) for (int k = 0; k < 4; k++) b[k] = -b[k];
    float l = 0;
    for (int k = 0; k < 4; k++) { o[k] = a[k] + (b[k] - a[k]) * s; l += o[k] * o[k]; }
    l = l > 1e-12f ? 1 / sqrtf(l) : 1;
    for (int k = 0; k < 4; k++) o[k] *= l;
}
static void lerp3f(const float a[3], const float b[3], float s, float o[3]) { for (int k = 0; k < 3; k++) o[k] = a[k] + (b[k] - a[k]) * s; }
static void look_at(const float pos[3], const float tgt[3], const float hint[3])   /* 0x678120 */
{
    float z[3] = { pos[0] - tgt[0], pos[1] - tgt[1], pos[2] - tgt[2] }, l = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    if (l < 1e-6f) return;
    for (int k = 0; k < 3; k++) z[k] /= l;
    float x[3] = { hint[1] * z[2] - hint[2] * z[1], hint[2] * z[0] - hint[0] * z[2], hint[0] * z[1] - hint[1] * z[0] };
    l = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    if (l < 1e-6f) { x[0] = 1; x[1] = x[2] = 0; } else for (int k = 0; k < 3; k++) x[k] /= l;
    float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
    for (int k = 0; k < 3; k++) { I.R[k][0] = x[k]; I.R[k][1] = y[k]; I.R[k][2] = z[k]; I.pos[k] = pos[k]; }
}
static float cam_progress(void) { cam_rec *r = &I.rec[I.cs]; return I.cs ? fminf(1, fmaxf(0, r->T / r->D)) : 0; }   /* 0x404220 */
static void cam_update(float dt)            /* SpinFocusCamera, intro mode (4.3 / 4.4) */
{
    if (I.cs == 0) {
        float p = fminf(1, fmaxf(0, I.spin)); I.spin = p;
        float a = (2 * p + 1.5f) * 3.14152f, r = 500 * (1 - p) + 348 * p;   /* (the PC's slightly-off pi) */
        float pos[3] = { r * cosf(a), 690 + r * sinf(a), -66.7f * (1 - p) + 300 * p }, tgt[3] = { 0, 690, 9.5f * (1 - p) + 337 * p }, up[3] = { 0, 0, 1 };
        look_at(pos, tgt, up);
        return;
    }
    int k = I.cs; cam_rec *R = &I.rec[k];
    R->T += dt;
    float t = fminf(R->T / R->D, 1), u = k == 1 ? t : (t + 0.2f) / 1.2f, s = (1 - cosf(3.14159265f * u)) * 0.5f;
    float P[3], Q[4]; lerp3f(R->A, R->B, s, P); nlerp(R->QA, R->QB, s, Q);
    if (k < 4 && t > 0.8f) {                /* the last 20 %: into the next segment's start */
        cam_rec *N = &I.rec[k + 1];
        float w = (t - 0.8f) * 5, s2 = (1 - cosf(3.14159265f * (t - 0.8f) / 1.2f)) * 0.5f, P2[3], Q2[4], Qo[4];
        lerp3f(N->A, N->B, s2, P2); nlerp(N->QA, N->QB, s2, Q2);
        lerp3f(P, P2, w, P); nlerp(Q, Q2, w, Qo); memcpy(Q, Qo, sizeof Q);
    }
    memcpy(I.pos, P, sizeof P); mat_from(Q, I.R);
    if (k < 4 && R->T >= R->D) I.cs = k + 1;
}
static void flicker(int active)             /* FlickerOnHack: "10101110101" a frame each (2.0 / 0.1), then 2.0 */
{
    static const char PAT[] = "10101110101";
    if (!active) { I.eye_i = 0; I.eye_glow = 0.1f; return; }
    if (I.eye_i < 11) I.eye_glow = PAT[I.eye_i++] == '1' ? 2.0f : 0.1f;
    else I.eye_glow = 2.0f;
}

void cin_intro_begin(void)
{
    memset(&I, 0, sizeof I);
    I.phase = 0; I.dur = 0.3f; I.eye_glow = 0.1f;
    static const float M1[3][3] = { { 0.956025f, -0.0260175f, 0.292128f }, { 0.293284f, 0.08481f, -0.952256f }, { 0, 0.996057f, 0.0887111f } };
    static const float M2[3][3] = { { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } };
    static const float M4[3][3] = { { 1, 0, 0 }, { 0, 0.258819f, -0.965926f }, { 0, 0.965926f, 0.258819f } };
    float q1[4], q2[4], q4[4]; quat_from(M1, q1); quat_from(M2, q2); quat_from(M4, q4);
    static const float B1[3] = { 324.509f, -458.197f, 257.947f }, B2[3] = { 0, 500, -90 }, B3[3] = { 0, -2200, -90 }, B4[3] = { 0, -2600, 150 };
    memcpy(I.rec[1].B, B1, 12); memcpy(I.rec[1].QB, q1, 16); I.rec[1].D = 3.0f;
    memcpy(I.rec[2].A, B1, 12); memcpy(I.rec[2].B, B2, 12); memcpy(I.rec[2].QA, q1, 16); memcpy(I.rec[2].QB, q2, 16); I.rec[2].D = 3.5f;
    memcpy(I.rec[3].A, B2, 12); memcpy(I.rec[3].B, B3, 12); memcpy(I.rec[3].QA, q2, 16); memcpy(I.rec[3].QB, q2, 16); I.rec[3].D = 4.0f;
    memcpy(I.rec[4].A, B3, 12); memcpy(I.rec[4].B, B4, 12); memcpy(I.rec[4].QA, q2, 16); memcpy(I.rec[4].QB, q4, 16); I.rec[4].D = 4.0f;
    cin_end();
    skip_t = 0;
    cam_update(0);
}
static void start_intro(void)               /* 0x6d63e0 */
{
    memset(&H, 0, sizeof H);
    term_reset();
    H.story = 6; H.sstate = 3; H.intro = 1;
}
static void intro_logic(void)               /* 0x6d4920 */
{
    if (I.cs == 0) {
        float v = I.dur > 0 ? fminf(1, fmaxf(0, I.timer / I.dur)) : 0;
        switch (I.phase) {
        case 1: set_terminal(0);
                if (I.spin > 0.025f && I.spin < 0.4f) set_story(0, 1);
                else if (I.spin > 0.4f && I.spin < 0.7f) set_story(1, 0);
                else if (I.spin > 0.7f) set_story(2, 0);
                break;
        case 2: I.F = 12 * v; break;
        case 3: I.F = 12 + 8 * v; set_story(3, 0); break;
        case 4: if (v > 0.7f) set_terminal(1); I.F = 20 + 5 * v; break;
        }
        return;
    }
    float p = cam_progress();
    switch (I.cs) {
    case 1: I.F = 25 + 10 * p; break;
    case 2: I.F = 35 + 4 * p; break;
    case 3: set_terminal(2); set_story(4, 0); I.F = 39 + 2 * p; break;
    case 4: set_terminal(3); set_story(5, 0); I.F = 41 + 2 * p; break;
    }
}
int cin_intro_update(float dt, cin_view *v)
{
    int done = 0;
    if (I.fade < 1) { I.bright = I.fade; I.fade += 0.2f * dt; }     /* in over 5 s */
    else if (I.fade < 2) { I.bright = 1; I.fade = 2.5f; }
    if (H.intro) { intro_logic(); terminal(dt); story(dt); }
    switch (I.phase) {                       /* 0, 1, 2, 3, 4, 7, 5, 6, 8 */
    case 0: I.timer += dt; if (I.timer > I.dur) { I.timer = 0; I.dur = 1; start_intro(); I.phase = 1; } break;
    case 1: I.spin += dt * 0.05f; if (I.spin > 1) I.phase = 2; break;               /* the spiral: 20 s */
    case 2: I.timer += dt; if (I.timer > I.dur) { I.timer = 0; I.dur = 0.25f; I.phase = 3; } break;
    case 3: I.timer += dt;                                                          /* the eye strobes, the screen flashes */
            if (I.timer > I.dur) { I.timer = 0; I.dur = 1.5f; I.phase = 4; break; }
            I.strobe = !I.strobe; flicker(I.strobe); I.bright = I.strobe ? 1.5f : 1.0f;
            I.lightning = I.timer / I.dur * 100;
            break;
    case 4: I.timer += dt; if (I.timer > I.dur) { I.timer = 1.35f; I.phase = 7; }
            I.bright = 1; I.lightning = 100;
            break;
    case 7: if (I.cs == 0) {                 /* SetState(1) 0x6d0fa0: from where it is */
                I.cs = 1; I.rec[1].T = 0; memcpy(I.rec[1].A, I.pos, 12); quat_from(I.R, I.rec[1].QA);
            }
            if (I.cs == 4 && cam_progress() > 0.999999f) { I.timer = 0; I.dur = 1.5f; I.phase = 5; }
            break;
    case 5: I.timer += dt; I.bright = 1 - I.timer / I.dur; if (I.timer >= I.dur) { I.timer = 0; I.dur = 1.5f; I.phase = 6; } break;
    case 6: I.timer += dt; I.bright = 0; if (I.timer >= I.dur) { I.timer = 0; I.dur = 1.75f; I.phase = 8; } break;
    case 8: I.timer += dt; I.bright = 0; fade_texts(1 - I.timer / I.dur); if (I.timer >= I.dur) done = 1; break;
    }
    if (I.phase >= 4 && I.phase != 3) flicker(1);   /* (phase 4 on: the eye stays on) */
    cam_update(dt);
    for (int k = 0; k < 3; k++) { v->eye[k] = I.pos[k]; v->fwd[k] = -I.R[k][2]; v->up[k] = I.R[k][1]; }
    v->bright = fmaxf(0, I.bright); v->f2 = v->f3 = I.F; v->eye_glow = I.eye_glow; v->lightning = I.lightning;
    return done;
}

/* ------------------------------------------------------------------ the ending (0x6d4590 / 0x6d60b0 / 0x6d4650) */
void cin_outro_begin(void)
{
    memset(&H, 0, sizeof H);
    term_reset();
    H.story = 6; H.sstate = 3; H.outro = 1;
    H.f2 = 0; H.f3 = 46;                     /* the whole city red */
    skip_t = 0;
}
void cin_outro_update(float dt, float path_t, float fade, cin_view *v)
{
    float p = fminf(1, fmaxf(0, path_t / 25));
    if (p > 0.34f) { H.t2 += dt; H.f2 = 44 * fminf(1, H.t2 / 9); }               /* the ring runs out over 9 s */
    if (p > 0.6f) { H.t1 += dt; H.f3 = fmaxf(0, 46 - 46 * H.t1 / 35); }          /* the red shrinks back over 35 s: blue again */
    if (p > 0.98f) set_terminal(4);                                              /* CityRestoring */
    terminal(dt);
    if (fade < 1) fade_texts(fade);
    v->f2 = H.f2; v->f3 = H.f3;
}
