/* TrickSystem2, ported from NitronicRush.exe (see nr_tricks.h). Values from its defaults
 * (0x6db1d0); points and announcer choice from 0x6da470 and 0x6dbcf0. */
#include "nr_tricks.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define LAND_T        0.08f      /* ground time before the air time resets */
#define MAX_T         5.0f       /* a trick that stops growing for this long is dropped */
#define EASE_TWIST    0.55f      /* cone (x 30 degrees) of rolls and flips */
#define EASE_FRISBEE  1.0f       /* cone (x 30 degrees) of frisbees */
#define WHEELIE_DOT   0.95f      /* max. dot of ground normal and car up */
#define SIDE_DOT      0.88f
#define NOSE_DOT      0.9f
#define WHEELIE_PPS   400.0f
#define NOSE_PPS      600.0f
#define SIDE_PPS      500.0f
#define WHEELIE_MIN   0.15f
#define WHEELIE_MPH   16.0f
#define GRACE         0.4f       /* short breaks in a wheelie or ride still count */
#define WALL_UPZ      0.18f
#define CEIL_UPZ      -0.6f
#define WALL_MIN_MI   0.01f
#define CEIL_MIN_MI   0.0340909f
#define RIDE_PPF      0.5f
#define RIDE_MIN_MPH  20.0f
#define GRIND_MIN_FT  1.2f       /* per frame */
#define GRIND_MIN     100.0f
#define GRIND_PPF     2.0f
#define GRIND_GAP_FT  10.0f
#define TWO_PI        6.2831853f

void nr_tricks_reset(nr_tricks *t)
{
    float total = t->total_miles; int c[4]; memcpy(c, t->contact, sizeof c);
    memset(t, 0, sizeof *t);
    for (int k = 0; k < 4; k++) t->wheelie_gap[k] = GRACE + 1;
    t->grind_gap = 999;
    t->total_miles = total; memcpy(t->contact, c, sizeof c);
}

/* points for n of a kind: n x base up to 5, then the game's tables (diminishing) */
static int kind_points(int n, int base, int a, int b, const int s[5])
{
    if (n <= 5) return base * n;
    static const int edge[6] = { 7, 9, 12, 15, 20, 50 };
    if (n <= 7) return a * n + b;
    int p = a * 7 + b;
    for (int i = 0; i < 5; i++) {
        int lo = edge[i], hi = edge[i + 1];
        if (n <= hi) return p + s[i] * (n - lo);
        p += s[i] * (hi - lo);
    }
    return p + (n - 50);
}

static void compute(nr_tricks *t)
{
    static const int R[5] = { 100, 75, 50, 25, 10 }, BF[5] = { 200, 150, 100, 50, 20 }, FF[5] = { 250, 150, 75, 30, 10 },
                     FR[5] = { 325, 250, 150, 75, 30 }, TF[5] = { 350, 275, 175, 90, 35 }, TB[5] = { 300, 225, 150, 75, 30 };
    int p = 0; float m = 0;
#define KIND(n, base, a, b, s) if ((n) > 0) { p += kind_points((n), base, a, b, s); m += 1; }
    KIND(t->rolls_r, 150, 125, 125, R)
    KIND(t->rolls_l, 150, 125, 125, R)
    KIND(t->backflips, 300, 250, 250, BF)
    KIND(t->frontflips, 400, 325, 375, FF)
    KIND(t->frisbee_l, 500, 400, 500, FR)
    KIND(t->frisbee_r, 500, 400, 500, FR)
    KIND(t->twist_f[0] + t->twist_f[1], 550, 425, 625, TF)
    KIND(t->twist_b[0] + t->twist_b[1], 450, 350, 500, TB)
#undef KIND
    static const float PPS[4] = { WHEELIE_PPS, NOSE_PPS, SIDE_PPS, SIDE_PPS };
    for (int k = 0; k < 4; k++) if (t->wheelie[k] > WHEELIE_MIN) { p += (int)(PPS[k] * t->wheelie[k] + 0.5f); m += 1; }
    if (t->wall_dist > WALL_MIN_MI) { p += (int)(RIDE_PPF * t->wall_dist * 5280 + 0.5f); m += 1; }
    if (t->ceil_dist > CEIL_MIN_MI) { p += (int)(RIDE_PPF * t->ceil_dist * 5280 + 0.5f); m += 1; }
    if (t->grind_dist > GRIND_MIN) { p += (int)(GRIND_PPF * t->grind_dist + 0.5f); m += 1; }
    t->points = p; t->mult = m;
}

/* a full turn (or most of one on landing): which trick it was by the turning axis */
static void classify(nr_tricks *t)
{
    float l = sqrtf(t->rot[0] * t->rot[0] + t->rot[1] * t->rot[1] + t->rot[2] * t->rot[2]);
    if (l > 1e-6f) {
        float nx = t->rot[0] / l, ny = t->rot[1] / l, nz = t->rot[2] / l;
        float c1 = cosf(EASE_TWIST * 3.14159265f / 6), c2 = cosf(EASE_FRISBEE * 3.14159265f / 6);
        if (ny > c1) t->rolls_r++;
        else if (ny < -c1) t->rolls_l++;
        else if (nx > c1) t->backflips++;
        else if (nx < -c1) t->frontflips++;
        else if (nz > c2) t->frisbee_l++;
        else if (nz < -c2) t->frisbee_r++;
        else if (nx > 0 && ny > 0) t->twist_b[0]++;
        else if (nx < 0 && ny > 0) t->twist_f[0]++;
        else if (nx < 0 && ny < 0) t->twist_f[1]++;
        else if (nx > 0 && ny < 0) t->twist_b[1]++;
    }
    t->rot[0] = t->rot[1] = t->rot[2] = 0;
}

static float ndot(const float *n, const float *u) { return n[0] * u[0] + n[1] * u[1] + n[2] * u[2]; }

static void wheelies(nr_tricks *t, const nr_trick_in *in, float dt, const float *up)
{
    if (in->speed_mph < WHEELIE_MPH) { for (int k = 0; k < 4; k++) t->wheelie_gap[k] += dt; return; }
    const int *c = t->contact;
    int cond[4] = {
        !c[0] && !c[1] && (c[2] || c[3]) && ndot(in->wheel_n[c[2] ? 2 : 3], up) <= WHEELIE_DOT,
        c[0] && c[1] && !c[2] && !c[3] && ndot(in->wheel_n[c[0] ? 0 : 1], up) <= NOSE_DOT,
        c[0] && !c[1] && c[2] && !c[3] && ndot(in->wheel_n[c[2] ? 2 : 0], up) <= SIDE_DOT,
        !c[0] && c[1] && !c[2] && c[3] && ndot(in->wheel_n[c[3] ? 3 : 1], up) <= SIDE_DOT,
    };
    for (int k = 0; k < 4; k++) {
        if (cond[k]) {
            t->wheelie[k] += dt;
            if (t->wheelie_gap[k] < GRACE) t->wheelie[k] += t->wheelie_gap[k];
            t->wheelie_gap[k] = 0; t->timer = 0;
        } else if (t->wheelie_gap[k] < GRACE) t->wheelie_gap[k] += dt;
    }
}

static void rides(nr_tricks *t, float dt, float mph, const float *up)
{
    float mi = dt * mph * (1.0f / 3600.0f);
    /* ceiling */
    if (t->ncontact > 1 && up[2] < CEIL_UPZ) {
        if (!t->ceil_pend && !t->ceil_on && mph > RIDE_MIN_MPH) {
            if (t->ceil_gap < GRACE) t->ceil_on = 1; else t->ceil_pend = 1;
        }
        if (t->ceil_pend && !t->ceil_on) {
            t->ceil_pdur += dt; t->ceil_pdist += mi;
            if (t->ceil_pdist > CEIL_MIN_MI) {
                t->ceil_dur += t->ceil_pdur; t->ceil_dist += t->ceil_pdist;
                t->ceil_pend = 0; t->ceil_on = 1; t->ceil_pdur = t->ceil_pdist = 0;
            }
            t->timer = 0;
        }
        if (t->ceil_on) { t->ceil_dur += dt; t->ceil_dist += mi; t->ceil_gap = 0; t->timer = 0; }
    } else {
        t->ceil_gap += dt; t->ceil_pend = t->ceil_on = 0; t->ceil_pdur = t->ceil_pdist = 0;
    }
    /* wall */
    if (t->ncontact > 1 && up[2] < WALL_UPZ && !t->ceil_pend && !t->ceil_on) {
        if (!t->wall_pend && !t->wall_on && mph > RIDE_MIN_MPH) {
            if (t->wall_gap < GRACE) t->wall_on = 1; else t->wall_pend = 1;
        }
        if (t->wall_pend && !t->wall_on) {
            t->wall_pdur += dt; t->wall_pdist += mi;
            if (t->wall_pdist > WALL_MIN_MI) {
                t->wall_dur += t->wall_pdur; t->wall_dist += t->wall_pdist;
                t->wall_pend = 0; t->wall_on = 1; t->wall_pdur = t->wall_pdist = 0;
            }
            t->timer = 0;
        }
        if (t->wall_on) { t->wall_dur += dt; t->wall_dist += mi; t->wall_gap = 0; t->timer = 0; }
    } else {
        t->wall_gap += dt; t->wall_pend = t->wall_on = 0; t->wall_pdur = t->wall_pdist = 0;
    }
}

static void rotations(nr_tricks *t, const nr_trick_in *in, float dt)
{
    for (int j = 0; j < 3; j++)
        t->rot[j] += (in->rot[0][j] * in->angvel[0] + in->rot[1][j] * in->angvel[1] + in->rot[2][j] * in->angvel[2]) * dt;
    float l2 = t->rot[0] * t->rot[0] + t->rot[1] * t->rot[1] + t->rot[2] * t->rot[2];
    if (l2 > TWO_PI * TWO_PI) { classify(t); t->timer = 0; }
    if (t->ncontact > 2) {
        l2 = t->rot[0] * t->rot[0] + t->rot[1] * t->rot[1] + t->rot[2] * t->rot[2];
        if (l2 > TWO_PI * TWO_PI * 0.5f) classify(t);
        t->rot[0] = t->rot[1] = t->rot[2] = 0; t->timer = 0;
    }
}

static void grind(nr_tricks *t, const nr_trick_in *in, float dt)
{
    float ft = fabsf(in->body_speed * 3.423f) * dt;
    if (t->ncontact <= 0 && in->body_contacts >= 1 && ft >= GRIND_MIN_FT) {
        if (t->grind_gap < GRIND_GAP_FT) t->grind_dist += t->grind_gap;
        t->grind_dist += ft; t->grind_gap = 0; t->timer = 0;
    } else if (t->grind_gap < GRIND_GAP_FT) t->grind_gap += ft;
}

static void award(nr_tricks *t, unsigned rnd, nr_trick_award *aw)
{
    memset(aw, 0, sizeof *aw);
    aw->points = t->points; aw->kinds = (int)(t->mult + 0.5f);
    float pm = t->points * t->mult;
    aw->score = (int)(pm + 0.5f);
    aw->heat_relief = pm / 450.0f; if (aw->heat_relief > 1) aw->heat_relief = 1; if (aw->heat_relief < 0) aw->heat_relief = 0;
    aw->wall_mi = t->wall_dist; aw->ceil_mi = t->ceil_dist;
    /* the announcer (0x6dbcf0) */
    float m = t->mult, r01 = (rnd & 0xFFFF) / 65536.0f;
    unsigned r100 = (rnd >> 16) % 100, r10 = (rnd >> 16) % 10;
    const char *ev = NULL;
    if (m > 1 && m <= 2) {
        if (r100 < 7 && t->wall_dist > WALL_MIN_MI && t->ceil_dist > CEIL_MIN_MI) ev = "Play_Ninja";
    } else if (m > 0 && m <= 1) {
        int tf = t->twist_f[0] + t->twist_f[1], tb = t->twist_b[0] + t->twist_b[1];
        if (t->rolls_r == 1 || t->rolls_l == 1) ev = "Play_BarrelRoll";
        else if (t->backflips == 1) ev = "Play_Backflip";
        else if (t->frontflips == 1) ev = "Play_FrontFlip";
        else if (t->frisbee_l == 1 || t->frisbee_r == 1) ev = "Play_Frisbee";
        else if (tf == 1) ev = "Play_FrontTwist";
        else if (tb == 1) ev = "Play_BackTwist";
        else if (tf == 2 || tb == 2) ev = "Play_TwistAndShout";
        else if (t->wall_dist > WALL_MIN_MI || t->ceil_dist > CEIL_MIN_MI) {
            float d = t->wall_dist > WALL_MIN_MI ? t->wall_dist : t->ceil_dist;
            if (r01 < 0.5f) ev = d < 0.0757576f ? "Play_Yeah0" : d < 0.151515f ? "Play_Yeah1" : "Play_Yeah2";
            else ev = t->wall_dist > WALL_MIN_MI ? "Play_WallRide" : "Play_CeilingRide";
        } else if (t->wheelie[0] > WHEELIE_MIN) {
            if (r01 < 0.5f) ev = t->wheelie[0] < 1.5f ? "Play_Cool" : t->wheelie[0] < 3 ? "Play_Cool0" : "Play_Cool1";
            else ev = "Play_Wheelie";
        } else if (t->wheelie[1] > WHEELIE_MIN) ev = "Play_NoseWheelie";
        else if (t->wheelie[2] > WHEELIE_MIN || t->wheelie[3] > WHEELIE_MIN) ev = "Play_SideWheelie";
        else if (t->grind_dist > GRIND_MIN) ev = "Play_Grind";
    } else if (m >= 5 && r10 < 5) ev = "Play_UltraCombo";
    aw->event = ev ? ev : "Play_Rand_TrickAwesomePhrases";
    aw->barrel_roll = ev && !strcmp(ev, "Play_BarrelRoll");
    /* the lines on screen (0x6da9f0) */
    char *o = aw->text; size_t left = sizeof aw->text;
#define LINE(...) do { int n_ = snprintf(o, left, __VA_ARGS__); if (n_ > 0 && (size_t)n_ < left) { o += n_; left -= n_; } } while (0)
    if (t->rolls_l) LINE("Left Barrel Roll x%d|", t->rolls_l);
    if (t->rolls_r) LINE("Right Barrel Roll x%d|", t->rolls_r);
    if (t->backflips) LINE("Backflip x%d|", t->backflips);
    if (t->frontflips) LINE("Frontflip x%d|", t->frontflips);
    if (t->frisbee_l) LINE("Left Frisbee x%d|", t->frisbee_l);
    if (t->frisbee_r) LINE("Right Frisbee x%d|", t->frisbee_r);
    if (t->twist_f[0] + t->twist_f[1]) LINE("Front Twist x%d|", t->twist_f[0] + t->twist_f[1]);
    if (t->twist_b[0] + t->twist_b[1]) LINE("Back Twist x%d|", t->twist_b[0] + t->twist_b[1]);
    if (t->wheelie[0] > WHEELIE_MIN) LINE("Wheelie (%0.2fs)|", t->wheelie[0]);
    if (t->wheelie[1] > WHEELIE_MIN) LINE("Nose Wheelie (%0.2fs)|", t->wheelie[1]);
    if (t->wheelie[2] > WHEELIE_MIN) LINE("Left Side Wheelie (%0.2fs)|", t->wheelie[2]);
    if (t->wheelie[3] > WHEELIE_MIN) LINE("Right Side Wheelie (%0.2fs)|", t->wheelie[3]);
    if (t->wall_dist > WALL_MIN_MI) LINE("Wall Ride (%d ft)|", (int)(t->wall_dist * 5280));
    if (t->ceil_dist > CEIL_MIN_MI) LINE("Ceiling Ride (%d ft)|", (int)(t->ceil_dist * 5280));
    if (t->grind_dist > GRIND_MIN) LINE("Grind (%d ft)|", (int)t->grind_dist);
#undef LINE
    nr_tricks_reset(t);
}

int nr_tricks_update(nr_tricks *t, const nr_trick_in *in, float dt, unsigned rnd, nr_trick_award *aw)
{
    t->ncontact = 0;
    for (int i = 0; i < 4; i++) { t->contact[i] = in->contact[i] != 0; t->ncontact += t->contact[i]; }
    float up[3] = { in->rot[0][2], in->rot[1][2], in->rot[2][2] };
    if (t->points > 0) t->timer += dt; else t->timer = 0;
    wheelies(t, in, dt, up);
    rides(t, dt, in->speed_mph, up);
    rotations(t, in, dt);
    grind(t, in, dt);
    compute(t);
    int awarded = 0;
    if (t->ncontact > 2 && t->points > 0) {
        int riding = t->wall_pend || t->wall_on || t->ceil_pend || t->ceil_on;
        if (!riding || t->was_air) { award(t, rnd, aw); awarded = 1; }
    }
    if (t->ncontact > 0) { t->ground_t += dt; if (t->ground_t > LAND_T) t->air_t = 0; }
    else { t->ground_t = 0; t->air_t += dt; if (t->air_t > LAND_T && t->points > 0) t->was_air = 1; }
    if (t->timer > MAX_T) nr_tricks_reset(t);
    if (t->ncontact > 0) t->total_miles += dt * in->speed_mph * (1.0f / 3600.0f);
    return awarded;
}
