/* The test drive with the real physics, along a recorded run (see chase.h). */
#include "chase.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define TICK 0.06f

static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float dist2(const float *a, const float *b)
{
    float d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
    return dot3(d, d);
}
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static void q2m(const float q[4], float R[3][3])            /* qw qx qy qz -> rows (columns right, forward, up) */
{
    float w = q[0], x = q[1], y = q[2], z = q[3];
    R[0][0] = 1 - 2 * (y * y + z * z); R[0][1] = 2 * (x * y - w * z); R[0][2] = 2 * (x * z + w * y);
    R[1][0] = 2 * (x * y + w * z); R[1][1] = 1 - 2 * (x * x + z * z); R[1][2] = 2 * (y * z - w * x);
    R[2][0] = 2 * (x * z - w * y); R[2][1] = 2 * (y * z + w * x); R[2][2] = 1 - 2 * (x * x + y * y);
}

int ch_init(ch_state *s, const uint8_t *data, uint32_t size)
{
    memset(s, 0, sizeof *s);
    gh_trackpt *t; int n = gh_track(data, size, &t);
    if (n < 10) { free(t); return -1; }
    /* the replay's jumps (before the line is cleaned): the speed along its up jumps from about nothing to
     * about the jump's 15 */
    uint8_t *jmp = calloc(n, 1);
    if (!jmp) { free(t); return -1; }
    for (int i = 1; i + 1 < n; i++) {
        if (!t[i - 1].alive || !t[i].alive || !t[i + 1].alive) continue;
        float R[3][3]; q2m(t[i].q, R);
        float up[3] = { R[0][2], R[1][2], R[2][2] }, v0[3], v1[3];
        for (int k = 0; k < 3; k++) { v0[k] = (t[i].p[k] - t[i - 1].p[k]) / TICK; v1[k] = (t[i + 1].p[k] - t[i].p[k]) / TICK; }
        float a = dot3(v0, up), b = dot3(v1, up);
        if (a > -15 && a < 6 && b - a > 9 && !t[i].wings) jmp[i] = 1;     /* (off a bump too) */
    }
    /* the line, cleaned: the shortest way through the replay, where two moments it was within 8 m of itself
     * (going the same way, or either one standing still) join up if what lies between was time lost - a
     * crash and the redrive, a stop, backing up, tumbling, or a short stretch. (A level that comes back to
     * the same place by a long way round keeps its long way.) */
    float (*dir)[3] = malloc(sizeof *dir * n); float *dp = malloc(sizeof(float) * n), *spd = malloc(sizeof(float) * n);
    int *from = malloc(sizeof(int) * n), *keep = malloc(sizeof(int) * n), nk = 0;
    float *L = malloc(sizeof(float) * (n + 1)); int *D = malloc(sizeof(int) * (n + 1)), *W = malloc(sizeof(int) * (n + 1));
    if (!L || !D || !W) { free(L); free(D); free(W); free(t); free(jmp); free(dir); free(dp); free(spd); free(from); free(keep); return -1; }
    if (!dir || !dp || !spd || !from || !keep) { free(t); free(jmp); free(dir); free(dp); free(spd); free(from); free(keep); return -1; }
    for (int i = 0; i < n; i++) {
        int a_ = i > 0 && t[i - 1].alive ? i - 1 : i, b_ = i + 1 < n && t[i + 1].alive ? i + 1 : i;
        float d[3] = { t[b_].p[0] - t[a_].p[0], t[b_].p[1] - t[a_].p[1], t[b_].p[2] - t[a_].p[2] };
        float l = sqrtf(dot3(d, d)), dtt = (b_ - a_) * TICK;
        spd[i] = dtt > 0 ? l / dtt : 0;
        if (l > 1e-4f) for (int k = 0; k < 3; k++) dir[i][k] = d[k] / l; else dir[i][0] = dir[i][1] = dir[i][2] = 0;
        dp[i] = 1e30f; from[i] = -1;
    }
    /* running sums: distance driven, ticks without the car, ticks of lost time (standing, backing, tumbling) */
    L[0] = 0; D[0] = 0; W[0] = 0;
    for (int i = 0; i < n; i++) {
        float l = i > 0 && t[i].alive && t[i - 1].alive ? sqrtf(dist2(t[i].p, t[i - 1].p)) : 0;
        L[i + 1] = L[i] + (l < 40 ? l : 0);
        D[i + 1] = D[i] + !t[i].alive;
        int lost = 0;
        if (t[i].alive) {
            float R[3][3]; q2m(t[i].q, R);
            float f[3] = { R[0][1], R[1][1], R[2][1] };
            lost = spd[i] < 3 || (spd[i] > 3 && dot3(f, dir[i]) < -0.3f) || (spd[i] > 8 && dot3(f, dir[i]) < 0.3f && !t[i].wings);
        }
        W[i + 1] = W[i] + lost;
    }
    int i0 = 0; while (i0 < n && !t[i0].alive) i0++;
    int last = n - 1; while (last > i0 && !t[last].alive) last--;
    if (i0 < n) dp[i0] = 0;
    for (int i = i0; i <= last; i++) {
        if (dp[i] >= 1e29f || !t[i].alive) continue;
        /* on to the next moment (a respawn after a crash: allowed, but dear) */
        int nx = i + 1; while (nx <= last && !t[nx].alive) nx++;
        if (nx <= last) {
            float l = sqrtf(dist2(t[nx].p, t[i].p)), c = dp[i] + (nx == i + 1 && l < 40 ? l : 2000);
            if (c < dp[nx]) { dp[nx] = c; from[nx] = i; }
        }
        int top = i + 700 < last ? i + 700 : last;
        for (int j = i + 2; j <= top; j++) {
            if (!t[j].alive) continue;
            float d2_ = dist2(t[j].p, t[i].p);
            if (d2_ > 64) continue;
            int still = spd[i] < 4 || spd[j] < 4;
            if (!still && dot3(dir[i], dir[j]) < 0.5f) continue;
            if (!(D[j] - D[i + 1] > 0 || L[j] - L[i] < 60 || W[j] - W[i + 1] >= 6)) continue;   /* a real stretch of road */
            float c = dp[i] + sqrtf(d2_);
            if (c < dp[j]) { dp[j] = c; from[j] = i; }
        }
    }
    for (int i = last; i >= 0; i = from[i]) { keep[nk++] = i; if (i == i0 || from[i] < 0) break; }
    for (int a_ = 0, b_ = nk - 1; a_ < b_; a_++, b_--) { int tmp = keep[a_]; keep[a_] = keep[b_]; keep[b_] = tmp; }
    /* (a jump in a left-out stretch counts at the next point kept) */
    for (int k = 1; k < nk; k++) for (int m = keep[k - 1] + 1; m < keep[k]; m++) if (jmp[m]) jmp[keep[k]] = 1;
    free(dir); free(dp); free(spd); free(from); free(L); free(D); free(W);
    gh_trackpt *t2 = malloc(sizeof *t2 * (nk ? nk : 1)); uint8_t *j2 = calloc(nk ? nk : 1, 1); int *tick = malloc(sizeof(int) * (nk ? nk : 1));
    if (!t2 || !j2 || !tick) { free(t); free(jmp); free(keep); free(t2); free(j2); free(tick); return -1; }
    for (int k = 0; k < nk; k++) { t2[k] = t[keep[k]]; j2[k] = jmp[keep[k]]; tick[k] = keep[k]; }
    free(t); free(jmp); free(keep);
    t = t2; n = nk;
    if (n < 10) { free(t); free(j2); free(tick); return -1; }
    s->pt = t; s->n = n; s->tick = tick;
    s->s = calloc(n, sizeof *s->s); s->jump = j2;
    if (!s->s || !s->jump) { ch_free(s); return -1; }
    for (int i = 1; i < n; i++) {
        float l = sqrtf(dist2(t[i].p, t[i - 1].p));
        s->s[i] = s->s[i - 1] + (l < 40 ? l : 0);
    }
    ch_restart(s);
    return 0;
}
/* the line from the level's route (the road pieces' chain), for the levels with no recorded run: driven at a
 * steady 75 units a second, no jumps or wings known */
int ch_init_route(ch_state *s, const float (*path)[3], int n)
{
    memset(s, 0, sizeof *s);
    if (n < 3) return -1;
    s->pt = calloc(n, sizeof *s->pt); s->s = calloc(n, sizeof *s->s); s->jump = calloc(n, 1); s->tick = calloc(n, sizeof *s->tick);
    if (!s->pt || !s->s || !s->jump || !s->tick) { ch_free(s); return -1; }
    for (int i = 0; i < n; i++) {
        gh_trackpt *t = &s->pt[i];
        memcpy(t->p, path[i], 12); t->p[2] += 0.6f; t->alive = 1;
        int a = i > 0 ? i - 1 : i, b = i + 1 < n ? i + 1 : i;
        float f[3] = { path[b][0] - path[a][0], path[b][1] - path[a][1], path[b][2] - path[a][2] }, l = sqrtf(dot3(f, f));
        if (l < 1e-4f) { f[0] = 0; f[1] = 1; f[2] = 0; l = 1; }
        for (int k = 0; k < 3; k++) f[k] /= l;
        /* a frame with this forward and the world's up: as a quaternion (qw qx qy qz) */
        float r[3] = { f[1], -f[0], 0 }, rl = sqrtf(r[0] * r[0] + r[1] * r[1]);
        if (rl < 1e-4f) { r[0] = 1; r[1] = 0; rl = 1; }
        r[0] /= rl; r[1] /= rl;
        float u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
        float M[3][3] = { { r[0], f[0], u[0] }, { r[1], f[1], u[1] }, { r[2], f[2], u[2] } };
        float tr = M[0][0] + M[1][1] + M[2][2];
        if (tr > 0) { float q = sqrtf(tr + 1) * 2; t->q[0] = 0.25f * q; t->q[1] = (M[2][1] - M[1][2]) / q; t->q[2] = (M[0][2] - M[2][0]) / q; t->q[3] = (M[1][0] - M[0][1]) / q; }
        else { t->q[0] = 1; }
        if (i) s->s[i] = s->s[i - 1] + sqrtf(dist2(path[i], path[i - 1]));
        s->tick[i] = (int)(s->s[i] / (75 * TICK));
    }
    s->n = n;
    ch_restart(s);
    return 0;
}
void ch_free(ch_state *s) { free(s->pt); free(s->s); free(s->jump); free(s->tick); memset(s, 0, sizeof *s); }
void ch_restart(ch_state *s)
{
    s->idx = 0; s->last_jump = -1; s->force = -1; s->deaths_here = 0; s->off_t = s->slow_t = s->flip_t = 0; s->best_s = 0; s->best_t = 0; s->wing_cool = 0; s->t = 0;
    while (s->idx < s->n - 1 && !s->pt[s->idx].alive) s->idx++;
}
float ch_length(const ch_state *s) { return s->n ? (s->tick[s->n - 1] + 1) * TICK : 0; }
float ch_time(const ch_state *s) { return s->n ? s->tick[s->idx] * TICK : 0; }

/* the line's point nearest the car, searching ahead (a little cost per tick ahead keeps it from
 * skipping to a later pass over the same place) */
static int nearest(const ch_state *s, const float p[3], int from, int back, int ahead)
{
    int best = -1; float bc = 1e30f;
    int a = from - back < 0 ? 0 : from - back, b = from + ahead >= s->n ? s->n - 1 : from + ahead;
    for (int i = a; i <= b; i++) {
        if (!s->pt[i].alive) continue;
        float c = sqrtf(dist2(s->pt[i].p, p)) + 0.05f * (i > from ? i - from : (from - i) * 4);
        if (c < bc) { bc = c; best = i; }
    }
    return best;
}
void ch_died(ch_state *s, const float pos[3])
{
    float d2 = dist2(pos, s->death_p);
    s->deaths_here = d2 < 40 * 40 ? s->deaths_here + 1 : 1;
    memcpy(s->death_p, pos, 12);
    s->death_idx = s->idx;
}
void ch_resync(ch_state *s, const float pos[3])
{
    int i = nearest(s, pos, s->idx, 600, s->n);
    if (i >= 0) s->idx = i;
    s->off_t = s->slow_t = s->flip_t = 0; s->best_s = s->s[s->idx]; s->best_t = s->t;
    s->last_jump = s->idx - 1;
    s->resyncs++;
    /* a second wreck at the same place: past it (an assist) */
    if (s->deaths_here >= 2) { s->force = s->death_idx + 12 < s->n ? s->death_idx + 12 : s->n - 1; s->deaths_here = 0; }
}

int ch_drive(ch_state *s, const nr_car *c, int wings, float dt, nr_car_input *in, int *toggle_wings,
             float rp[3], float rR[3][3], float rv[3])
{
    memset(in, 0, sizeof *in); *toggle_wings = 0;
    if (s->n < 2 || dt <= 0) return 0;
    s->t += dt;
    if (s->wing_cool > 0) s->wing_cool -= dt;
    int i = nearest(s, c->pos, s->idx, 12, 400);
    if (i >= 0) s->idx = i;
    const gh_trackpt *P = s->pt;
    float right[3] = { c->rot[0][0], c->rot[1][0], c->rot[2][0] }, fwd[3] = { c->rot[0][1], c->rot[1][1], c->rot[2][1] },
          up[3] = { c->rot[0][2], c->rot[1][2], c->rot[2][2] };
    float speed = sqrtf(dot3(c->vel, c->vel)), vf = dot3(c->vel, fwd);
    float off = sqrtf(dist2(P[s->idx].p, c->pos));

    /* the replay's speed here (ahead a little) */
    int j = s->idx; while (j + 1 < s->n && j < s->idx + 4 && P[j + 1].alive) j++;
    float vref = j > s->idx ? (s->s[j] - s->s[s->idx]) / ((s->tick[j] - s->tick[s->idx]) * TICK) : speed;

    /* where to steer: a point on the line ahead, further at speed */
    int air = c->grounded < 2;
    float look = air && wings ? clampf(14 + 0.45f * speed, 8, 50) : clampf(8 + 0.25f * speed, 10, 40);
    int ti = s->idx;
    while (ti + 1 < s->n && P[ti + 1].alive && s->s[ti + 1] - s->s[s->idx] < look) ti++;
    float T[3] = { P[ti].p[0] - c->pos[0], P[ti].p[1] - c->pos[1], P[ti].p[2] - c->pos[2] };
    float dx = dot3(T, right), dy = dot3(T, fwd);
    float yaw_rate = dot3(c->angvel, up), pitch_rate = dot3(c->angvel, right), roll_rate = dot3(c->angvel, fwd);

    /* the wings: as the replay has them, a few ticks ahead */
    int want = 0;
    for (int k = s->idx; k <= s->idx + 2 && k < s->n; k++) want |= P[k].wings;
    if (want != wings && s->wing_cool <= 0 && (want ? air : 1)) { *toggle_wings = 1; s->wing_cool = 0.35f; }

    if (!air) {
        /* pure pursuit: the curve that reaches the point ahead, as a wheel angle, over what the steering can do at
         * this speed (CarLogic's lock falls off above Steer Reduction Start: 0.45 / (1 + 0.2 (v - 22))) */
        float L = sqrtf(dx * dx + dy * dy);
        if (L < 1) L = 1;
        float alpha = atan2f(-dx, dy);
        float kappa = 2 * sinf(alpha) / L;
        float delta = atanf(kappa * 2.2f);
        float maxd = 0.45f / (1 + fmaxf(vf - 22, 0) * 0.2f);
        float want_rate = vf * kappa;                         /* the turn rate that curve needs */
        in->steer = clampf(delta / maxd + (want_rate - yaw_rate) * 0.08f / fmaxf(maxd * 10, 0.05f), -1, 1);
        if (dy < 0 && fabsf(alpha) > 2.0f) in->steer = alpha > 0 ? 1 : -1;      /* (behind: turn round) */
        if (vf < vref - 2.0f) in->throttle = 1;
        else if (vf > vref + 15.0f) { in->brake = clampf((vf - vref - 15) / 30, 0.05f, 0.6f); }
        else in->throttle = vf > vref + 4 ? 0 : 0.6f;
        if (vf < vref + 1 && vref > 60 && s->heat < 0.85f) in->boost = 1;      /* (the developers hold it: 115 u/s needs it) */
        /* the replay's jumps, where it jumped: pressed as the car reaches that spot */
        for (int k = s->idx > 2 ? s->idx - 2 : 0; k <= s->idx + 8 && k < s->n; k++) {
            if (!s->jump[k] || k <= s->last_jump) continue;
            float to[3] = { P[k].p[0] - c->pos[0], P[k].p[1] - c->pos[1], P[k].p[2] - c->pos[2] };
            float ahead = dot3(to, fwd);
            if (ahead < speed * 0.035f + 0.5f && ahead > -6) { in->jump = 1; s->last_jump = k; }
            break;
        }
    } else if (wings) {
        /* flying: the car goes where its nose points, so the nose is held at the replay's pitch and heading a
         * little ahead, corrected toward where the replay is (height, sideways) */
        int k = s->idx + 3 < s->n ? s->idx + 3 : s->n - 1, k2 = s->idx + 8 < s->n ? s->idx + 8 : s->n - 1;
        float Rr[3][3]; q2m(P[k].q, Rr);
        float rf[3] = { Rr[0][1], Rr[1][1], Rr[2][1] };
        float pitch_now = asinf(clampf(fwd[2], -1, 1)), pitch_want = asinf(clampf(rf[2], -1, 1));
        pitch_want += clampf((P[k2].p[2] - c->pos[2]) * 0.03f, -0.35f, 0.35f);
        float head_now = atan2f(fwd[1], fwd[0]), head_want = atan2f(rf[1], rf[0]);
        float fl[2] = { fwd[0], fwd[1] }, fll = sqrtf(fl[0] * fl[0] + fl[1] * fl[1]);
        if (fll > 1e-3f) {
            float lx_ = -fl[1] / fll, ly_ = fl[0] / fll;          /* left, flat */
            float side = (P[k2].p[0] - c->pos[0]) * lx_ + (P[k2].p[1] - c->pos[1]) * ly_;
            head_want += clampf(side * 0.03f, -0.35f, 0.35f);
        }
        float dh = head_want - head_now;
        while (dh > 3.14159f) dh -= 6.28318f;
        while (dh < -3.14159f) dh += 6.28318f;
        in->pitch = clampf((pitch_want - pitch_now) * 4.0f - pitch_rate * 0.3f, -1, 1);
        in->steer = clampf(dh * 4.0f - yaw_rate * 0.3f, -1, 1);
    } else {
        /* in the air without wings: turn to the replay's attitude a few ticks on */
        int k = s->idx + 2 < s->n && P[s->idx + 2].alive ? s->idx + 2 : s->idx;
        float Rd[3][3]; q2m(P[k].q, Rd);
        /* E = R^T Rd: the turn still to go, in the car's frame */
        float E[3][3];
        for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) {
            float v = 0; for (int m = 0; m < 3; m++) v += c->rot[m][a] * Rd[m][b];
            E[a][b] = v;
        }
        float ex = 0.5f * (E[2][1] - E[1][2]), ey = 0.5f * (E[0][2] - E[2][0]);
        float pc = clampf(ex * 2.0f - pitch_rate * 0.35f, -1, 1), rc = clampf(ey * 2.0f - roll_rate * 0.35f, -1, 1);
        if (fabsf(pc) > 0.08f || fabsf(rc) > 0.08f) { in->roll_x = rc; in->roll_y = -pc; }
        float yaw_err = atan2f(-dx, fmaxf(dy, 0.5f));
        in->steer = clampf(yaw_err * 1.5f, -1, 1);
        in->throttle = 1;
    }

    /* an assist: off the line for a while, making no headway, or on its roof */
    float sh = s->s[s->idx];
    if (sh > s->best_s + 2) { s->best_s = sh; s->best_t = s->t; }
    s->off_t = off > (air ? 30 : 14) ? s->off_t + dt : 0;
    s->flip_t = !air && up[2] < -0.3f ? s->flip_t + dt : (up[2] < -0.3f ? s->flip_t : 0);
    const char *why = NULL;
    if (s->force >= 0) { why = "wrecked twice here"; s->idx = s->force; s->force = -1; }
    else if (s->off_t > 1.2f) why = "off the line";
    else if (s->t - s->best_t > 5.0f) why = "no headway";
    else if (s->flip_t > 2.0f) why = "on its roof";
    if (!why) return 0;
    snprintf(s->why, sizeof s->why, "%s", why);
    /* back onto the line a little before here, at the replay's speed */
    int r = s->idx;
    if (strcmp(why, "wrecked twice here")) for (int b = 0; b < 8 && r > 0 && P[r - 1].alive; b++) r--;
    q2m(P[r].q, rR);
    memcpy(rp, P[r].p, 12);
    int r2 = r + 1 < s->n && P[r + 1].alive ? r + 1 : r;
    for (int k = 0; k < 3; k++) rv[k] = r2 > r ? (P[r2].p[k] - P[r].p[k]) / TICK : 0;
    s->idx = r; s->assists++; s->last_jump = r - 1;
    s->off_t = s->flip_t = 0; s->best_s = s->s[r]; s->best_t = s->t;
    return 1;
}
