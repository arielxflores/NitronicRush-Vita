/* How long the GPU took over each frame, and the dynamic resolution that goes by it.
 *
 * With the 60 fps cap on, a frame's time says only whether it made its refresh: 16.7 ms whether the GPU was done in
 * 9 ms or in 16, and 33 ms once it was not. Going by missed refreshes alone, the dynamic resolution could only act
 * after the picture had already stuttered, and had to try a bigger picture now and then to find out whether it
 * would stutter again. The GPU's own time tells how much room there is before anything is missed.
 *
 * vitaGL ends every scene (the 3D scene's target, each screen pass, the final picture) with a notification: a
 * number, one more with each scene, that the GPU writes into a word of memory when that scene's pixels are done
 * (vitaGL uses it for its occlusion queries: query_fence, source/buffers.c and gxm.c's scene_end). A small thread
 * here watches that word for the numbers of a frame's scenes and notes the times (see the clock below for which,
 * and why not simply the last). */
#include "gputime.h"
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ the controller
 * A frame's GPU time is taken as F + P x size^2: F the part the 3D scene's size does not change (its triangles, the
 * screen passes, the HUD), P the part that goes with its pixel count. F is learnt at each change of size from the
 * times just before and just after (when both were steady); P is then whatever the last frame shows. */
void dr_reset(dr_t *d)
{
    memset(d, 0, sizeof *d);
    d->scale = 1; d->fixed = 8.0f; d->up_back = 1; d->since_up = 99;
}
void dr_time(dr_t *d, float dt)
{
    d->since_up += dt; d->calm += dt; d->lost += dt;
    if (d->up_wait > 0) d->up_wait -= dt;
    if (d->calm > 30) d->up_back = 1;
}
float dr_hi = DR_HI, dr_goal = DR_GOAL, dr_up = DR_UP, dr_min = DR_MIN;
static float dr_snap(float s)                  /* down to a step, within dr_min..1 */
{
    s = floorf(s / DR_STEP + 1e-3f) * DR_STEP;
    return s < dr_min ? dr_min : s > 1 ? 1 : s;
}
/* a GPU time measured at one size, as it would be at another, with f ms of it fixed */
static float dr_at(float ms, float from, float to, float f) { return f + (ms - f) * (to * to) / (from * from); }
/* the fixed part to reckon with for a time of ms: what was learnt, but at least 15% of any frame goes with the pixels */
static float dr_fixed(const dr_t *d, float ms) { return d->fixed < 0.85f * ms ? d->fixed : 0.85f * ms; }
static float spread3(const float *g) { float lo = fminf(g[0], fminf(g[1], g[2])), hi = fmaxf(g[0], fmaxf(g[1], g[2])); return hi - lo; }

/* F from a change of size: the three frames before it and the first three after it, when neither three were on the move
 * (a scene getting heavier while it is shrunk would look as if the shrinking had not helped) */
static void dr_learn(dr_t *d, float ms, float scale)
{
    if (fabsf(scale - d->bs) < 0.01f) {        /* (still, or again, the size before) */
        if (d->bn < 3) d->bg[d->bn++] = ms; else { d->bg[0] = d->bg[1]; d->bg[1] = d->bg[2]; d->bg[2] = ms; }
        return;
    }
    if (!d->learning) {
        if (d->bn == 3) { d->learning = 1; d->as = scale; d->ag[0] = ms; d->an = 1; return; }
    } else if (fabsf(scale - d->as) < 0.01f) {
        d->ag[d->an++] = ms;
        if (d->an < 3) return;
        float before = (d->bg[0] + d->bg[1] + d->bg[2]) / 3, after = (d->ag[0] + d->ag[1] + d->ag[2]) / 3;
        float den = d->bs * d->bs - d->as * d->as;
        if (fabsf(den) > 0.02f && spread3(d->bg) < 1.0f && spread3(d->ag) < 1.0f) {
            float p = (before - after) / den, f = before - p * d->bs * d->bs, top = fminf(before, after) - 0.3f;
            f = f > top ? top : f; f = f < 3 ? 3 : f;
            d->fixed = 0.5f * d->fixed + 0.5f * f;
        }
        d->learning = 0; d->bs = d->as; memcpy(d->bg, d->ag, sizeof d->bg); d->bn = 3;
        return;
    }
    d->learning = 0; d->bs = scale; d->bg[0] = ms; d->bn = 1;     /* (no clean pair: from this size on) */
}

float dr_sample(dr_t *d, float ms, float scale)
{
    if (!(ms > 0.2f && ms < 200) || !(scale > 0.3f && scale < 1.01f)) return d->scale;
    d->lost = 0;
    dr_learn(d, ms, scale);
    /* The first frame at the size of a step down tells whether the step helped. Not at all (under a quarter of the
     * drop reckoned) while the scene was not getting heavier: the time is not the pixels' - further steps down would
     * only blur the picture (on the Vita: warp_speed's finish, 15 ms at any size, went down to 60% a step after
     * another and stayed at 30 fps all the same). No more steps down then till the scene gets heavier than it was or
     * the size goes back up */
    const int now_size = fabsf(scale - d->scale) < 0.01f;
    if (d->trig && now_size) {
        const float want = dr_at(d->trig_ms, d->trig_s, scale, dr_fixed(d, d->trig_ms));
        const int rising = ms > d->trig_ms + 1.0f;
        if (!rising && ms > d->trig_ms - 0.25f * (d->trig_ms - want)) d->stuck = 1, d->stuck_ms = d->trig_ms;
        d->trig = 0;
    }
    float g = now_size ? ms : dr_at(ms, scale, d->scale, dr_fixed(d, ms));
    d->pk[d->ipk] = g; d->ipk = (d->ipk + 1) % DR_WIN; if (d->npk < DR_WIN) d->npk++;
    /* One frame alone that took long (a hitch: something else had the GPU or the bus) is gone before any size could
     * answer it, and says nothing of the next: the time to go by is the shorter of the last two. Getting heavier two
     * frames running: where it will be by the time a change shows (two frames on) */
    float ahead = g, last = g;
    if (d->npk >= 2) { float g1 = d->pk[(d->ipk + DR_WIN - 2) % DR_WIN]; if (g1 < g) ahead = g1; }
    else if (g > dr_hi) ahead = dr_hi;                 /* (the first frame at this size: wait for the second) */
    g = ahead;
    if (d->npk >= 3) {
        float g1 = d->pk[(d->ipk + DR_WIN - 2) % DR_WIN], g2 = d->pk[(d->ipk + DR_WIN - 3) % DR_WIN], s1 = last - g1, s2 = g1 - g2;
        if (s1 > 0.5f && s2 > 0.5f && last > dr_hi - 1.5f) ahead = last + 2 * fminf(s1, s2);
    }
    if (d->stuck && ahead > d->stuck_ms + 1.0f) d->stuck = 0;     /* (heavier than when the step did not help: try again) */
    if (ahead > dr_hi && d->stuck && d->scale > dr_min + 1e-3f) d->holds++;
    if (ahead > dr_hi && d->scale > dr_min + 1e-3f && !d->stuck) {
        /* too long: the size that brings it to DR_GOAL, at least a step down. Where the size hardly counts (under a
         * quarter of the time goes with the pixels) not below 75%: little would be gained for the blur */
        const float f = dr_fixed(d, ahead), floor_s = (ahead - d->fixed) < 0.25f * ahead && dr_min < 0.75f ? 0.75f : dr_min;
        float k = (dr_goal - f) / (ahead - f);
        float ns = dr_snap(d->scale * sqrtf(k > 0.05f ? k : 0.05f));
        if (ns > d->scale - DR_STEP + 1e-3f) ns = dr_snap(d->scale - DR_STEP);
        if (g <= dr_hi && ns < d->scale - 2 * DR_STEP) ns = dr_snap(d->scale - 2 * DR_STEP + 1e-3f);   /* (only seen coming: two steps at most) */
        if (ns < floor_s) ns = floor_s;
        if (ns < d->scale - 1e-3f) {
            /* (straight after a step up: that step did not hold - the next waits longer. Later: the scene's own doing) */
            if (d->since_up < 0.3f) d->up_back = d->up_back * 2 > 4 ? 4 : d->up_back * 2;
            d->up_wait = d->up_back; d->calm = 0;
            d->trig = 1; d->trig_ms = ms; d->trig_s = scale;
            d->scale = ns; d->npk = 0; d->ipk = 0; d->downs++;
        }
    } else if (d->npk >= DR_WIN && d->up_wait <= 0 && d->scale < 1 - 1e-3f) {
        /* room: up to the biggest size at which the last half second's worst frame would still be under DR_UP, counting
         * more of its time as going with the pixels than was learnt (the careful side) */
        float pk = 0, pk2 = 0;                          /* (but for the one longest: a hitch, as above) */
        for (int i = 0; i < DR_WIN; i++) { if (d->pk[i] > pk) { pk2 = pk; pk = d->pk[i]; } else if (d->pk[i] > pk2) pk2 = d->pk[i]; }
        pk = pk2;
        float f = fminf(d->fixed - 1.5f, 0.4f * pk), su = d->scale;
        f = f > 0 ? f : 0;
        while (su < 1 - 1e-3f && dr_at(pk, d->scale, su + DR_STEP > 1 ? 1 : su + DR_STEP, f) < dr_up) su = su + DR_STEP > 1 ? 1 : su + DR_STEP;
        if (su > d->scale + 1e-3f) { d->scale = su; d->since_up = 0; d->npk = 0; d->ipk = 0; d->ups++; d->stuck = 0; }
    }
    return d->scale;
}

/* ------------------------------------------------------------------ the GPU's clock
 * A frame is several scenes: the 3D scene and the glow passes, each into a picture of its own, then the final pass
 * (and the HUD) into the screen's next buffer. The first can run as soon as they are handed over. The final pass
 * cannot: with the 60 fps cap on, the game is two frames ahead of the screen, and the buffer a frame's final pass
 * draws into is still the one on screen until the next refresh - the GPU holds that scene back till then. Timed
 * from the hand-over to the end of the final pass, every frame would read as about a refresh long however little
 * work it was. So the two parts are timed apart: the 3D scene and glow passes from their hand-over (or from the
 * end of the frame before, if the GPU was still on that) to the notification of the last of them - no waiting in
 * that; and the final pass from there to its own notification, which is its work only where it did not have to
 * wait (the cap off: the bench and the cap-off test runs). The dynamic resolution goes by the first plus a set
 * figure for the final pass (gt_final_ms: the same work whatever the 3D scene's size). */
#define GQ 8
static struct { uint32_t want_3d, want_off, want; uint64_t kick; float scale; int bad, tag; } Q[GQ];
static struct { float off, fin, scale, s3d; int tag; } RES[GQ];
float gt_take_3d = -1; int gt_take_tag;
static volatile uint32_t qh, qt, rh, rt;       /* Q: qh the next to wait for, qt the next free; RES the same */
static volatile uint64_t last_done, last_result;
static int gt_on;
float gt_final_ms = GT_FINAL_MS;

#ifdef NR_PC
#include <stdlib.h>
#include <stdio.h>
/* the test machine has no such clock. GPUSIM="F,P[,T,P2]" makes one up: F + P x size^2 ms a frame (P2 instead of P
 * every other T seconds), gt_final_ms of it the final pass, each known two frames later - to see the controller at
 * work in the game */
static float sim_f, sim_p, sim_t, sim_p2; static uint64_t sim_frames;
void gt_init(void (*log)(const char *fmt, ...))
{
    const char *e = getenv("GPUSIM");
    if (!e) return;
    gt_on = sscanf(e, "%f,%f,%f,%f", &sim_f, &sim_p, &sim_t, &sim_p2) >= 2;
    if (gt_on && log) log("GPU clock: made up (%.1f + %.1f x size^2 ms)", sim_f, sim_p);
}
void gt_mark_offscreen(void) {}
void gt_mark_scene3d(void) {}
void gt_submit(uint64_t kick_us, float scale, int tag)
{
    (void)kick_us;
    if (!gt_on) return;
    float p = sim_t > 0 && ((uint64_t)(sim_frames / 60.0f / sim_t) & 1) ? sim_p2 : sim_p;
    sim_frames++;
    RES[rt % GQ].off = sim_f + p * scale * scale - gt_final_ms; RES[rt % GQ].fin = gt_final_ms; RES[rt % GQ].scale = scale; RES[rt % GQ].tag = tag; rt++;
}
void gt_confirm(void) {}
int gt_take(float *off_ms, float *fin_ms, float *scale)
{
    if (!gt_on || rt - rh <= 2) return 0;              /* (two frames late, as the Vita's) */
    *off_ms = RES[rh % GQ].off; *fin_ms = RES[rh % GQ].fin; *scale = RES[rh % GQ].scale; gt_take_tag = RES[rh % GQ].tag; rh++;
    return 1;
}
int gt_working(void) { return gt_on; }
#else
#include <psp2/gxm.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
extern SceGxmNotification query_fence;         /* vitaGL's: see the top */
#define GT_POLL_US 200
static SceUID gt_sema = -1, gt_th = -1;
static uint32_t off_mark, s3d_mark;            /* the number of this frame's last scene before the final pass, and of its 3D scene (0: none yet) */
static uint64_t gt_now(void) { return sceKernelGetProcessTimeWide(); }
/* waits for the scene numbered want to be done; the time then, or 0 if it took over a quarter of a second from t0
 * (the game put aside, or nothing drawn) */
static uint64_t gt_wait(const volatile uint32_t *addr, uint32_t want, uint64_t t0)
{
    uint64_t t = gt_now();
    while ((int32_t)(*addr - want) < 0) {
        sceKernelDelayThread(GT_POLL_US);
        t = gt_now();
        if (t - t0 > 250000) return 0;
    }
    return t;
}
static int gt_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    const volatile uint32_t *addr = query_fence.address;
    for (;;) {
        sceKernelWaitSema(gt_sema, 1, NULL);
        const uint32_t i = qh % GQ;
        const uint64_t t0 = gt_now();
        const uint64_t t_3d = Q[i].want_3d ? gt_wait(addr, Q[i].want_3d, t0) : 0;
        const uint64_t t_off = gt_wait(addr, Q[i].want_off, t0);
        const uint64_t t_fin = t_off ? gt_wait(addr, Q[i].want, t0) : 0;
        const uint64_t start = Q[i].kick > last_done ? Q[i].kick : last_done;
        if (t_fin && !Q[i].bad && t_off > start) {
            RES[rt % GQ].off = (t_off - start) / 1000.0f; RES[rt % GQ].fin = (t_fin - t_off) / 1000.0f; RES[rt % GQ].scale = Q[i].scale;
            RES[rt % GQ].s3d = t_3d > start && t_3d <= t_off ? (t_3d - start) / 1000.0f : -1; RES[rt % GQ].tag = Q[i].tag;
            rt++; last_result = t_fin;
        }
        last_done = t_fin ? t_fin : gt_now();
        qh++;
    }
    return 0;
}
void gt_init(void (*log)(const char *fmt, ...))
{
    if (!query_fence.address) { if (log) log("GPU clock: none (no notification word)"); return; }
    gt_sema = sceKernelCreateSema("nr_gt", 0, 0, GQ, NULL);
    /* (above the game thread, on the I/O thread's core: it only looks at a word and sleeps) */
    gt_th = sceKernelCreateThread("nr_gpu_clock", gt_thread, 0x10000100 - 16, 0x2000, 0, 0x20000, NULL);
    if (gt_sema < 0 || gt_th < 0 || sceKernelStartThread(gt_th, 0, NULL) < 0) { if (log) log("GPU clock: none (no thread)"); return; }
    uint64_t t0 = gt_now();
    for (int i = 0; i < 20; i++) sceKernelDelayThread(GT_POLL_US);
    if (log) log("GPU clock: on (looks every %.2f ms; the final pass counted as %.1f ms)", (gt_now() - t0) / 20000.0f, gt_final_ms);
    gt_on = 1;
}
void gt_mark_offscreen(void) { off_mark = query_fence.value; }
void gt_mark_scene3d(void) { s3d_mark = query_fence.value; }
static uint32_t gt_last;
void gt_submit(uint64_t kick_us, float scale, int tag)
{
    const uint32_t mark = off_mark, m3 = s3d_mark; off_mark = s3d_mark = 0;
    /* (no scene before the final pass - the screen effects off - or the list full: no measurement) */
    if (!gt_on || !mark || !kick_us || qt - qh >= GQ) { gt_last = GQ; return; }
    const uint32_t i = qt % GQ;
    Q[i].want_off = mark; Q[i].want_3d = m3 && (int32_t)(mark - m3) > 0 ? m3 : 0;
    Q[i].want = query_fence.value + 1;                 /* (the scene in hand, ended by the hand-over) */
    Q[i].kick = kick_us; Q[i].scale = scale; Q[i].tag = tag; Q[i].bad = (int32_t)(Q[i].want - mark) <= 0;
    gt_last = i; qt++;
    sceKernelSignalSema(gt_sema, 1);
}
void gt_confirm(void)
{
    if (gt_on && gt_last < GQ && query_fence.value != Q[gt_last].want) Q[gt_last].bad = 1;
}
int gt_take(float *off_ms, float *fin_ms, float *scale)
{
    if (!gt_on || rt == rh) return 0;
    if (rt - rh > GQ) rh = rt - GQ;                    /* (not taken for a while: the oldest are gone) */
    *off_ms = RES[rh % GQ].off; *fin_ms = RES[rh % GQ].fin; *scale = RES[rh % GQ].scale; gt_take_3d = RES[rh % GQ].s3d; gt_take_tag = RES[rh % GQ].tag; rh++;
    return 1;
}
int gt_working(void) { return gt_on && gt_now() - last_result < 500000; }
#endif
