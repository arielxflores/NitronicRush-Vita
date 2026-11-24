/* The dynamic resolution's controller (vita/gputime.c) against made-up scenes (the PC test machine):
 *   cc -O2 -Ivita tools/pc/drtest.c vita/gputime.c -DNR_PC -lm -o build/drtest && build/drtest [-v] [-h]
 * A scene costs the GPU F + P x size^2 ms a frame (the Vita's benches gave such pairs: the same view at 100% and at 50%),
 * with a little noise; a frame's time is known two frames later, as on the Vita. For each scene: the frames that would
 * miss their refresh (over 16.5 ms) and the mean size, with the new controller and with the old way (a step of 5%
 * after two missed refreshes in half a second, a try back up after a calm spell). */
#include "gputime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { const char *name; float secs; void (*at)(float t, float *F, float *P); } scene_t;
static float ramp(float t, float t0, float t1, float a, float b) { return t <= t0 ? a : t >= t1 ? b : a + (b - a) * (t - t0) / (t1 - t0); }
/* KillItWithFire: fire walls every few seconds (all fill: 9 to 25 ms at full size) */
static void s_fire(float t, float *F, float *P) { float k = fmodf(t, 5.0f); *F = 6.5f; *P = 3.5f + 15.0f * (k > 2 && k < 4.2f ? fminf(1, fminf((k - 2) / 0.25f, (4.2f - k) / 0.25f)) : 0); }
/* moto's end: mostly triangles (the size hardly helps) */
static void s_moto(float t, float *F, float *P) { *F = ramp(t, 2, 4, 9.5f, 12.7f); *P = 3.6f; }
/* Level 6's start with ghost cars, easing off */
static void s_l6(float t, float *F, float *P) { *F = 9.0f; *P = ramp(t, 6, 14, 9.5f, 4.0f); }
/* warp_speed: a wall of scrolling glow from one frame to the next, then gone */
static void s_jump(float t, float *F, float *P) { *F = 9.8f; *P = t > 3 && t < 9 ? 9.2f : 3.0f; }
/* MountainTop: slow swells round the limit */
static void s_swell(float t, float *F, float *P) { *F = 6.8f; *P = 8.0f + 2.6f * sinf(t * 0.9f); }
/* a light level: nothing to do */
static void s_light(float t, float *F, float *P) { (void)t; *F = 6.0f; *P = 4.0f; }
/* warp_speed's finish (Vita, dev30): 1.5 s where the time hardly follows the size (15 ms at any size) - shrinking
 * cannot help, the scene should stay as it is */
static void s_finish(float t, float *F, float *P) { int on = t > 3 && t < 4.5f; *F = on ? 14.0f : 9.0f; *P = on ? 1.2f : 3.0f; }
/* moto (Vita bench, dev30): mostly triangles, 11.5 ms of 3D work of which 2.7 go with the size, a final pass of 5.3
 * (counted as 3: 2.3 unseen), peaks round the limit now and then */
static void s_moto2(float t, float *F, float *P) { *F = 10.6f + 1.2f * sinf(t * 1.7f) * sinf(t * 0.41f); *P = 3.6f; }
static const scene_t SC[] = { { "fire walls", 40, s_fire }, { "triangles (moto)", 20, s_moto }, { "ghost cars easing off", 20, s_l6 },
                              { "a sudden wall of glow", 14, s_jump }, { "swells round the limit", 40, s_swell }, { "a light level", 10, s_light },
                              { "size does not help (finish)", 8, s_finish }, { "moto, peaks", 40, s_moto2 } };

static uint32_t seed = 7;
static float noise(void) { seed = seed * 1103515245u + 12345u; return ((int)((seed >> 16) & 1023) - 512) / 512.0f; }

int main(int argc, char **argv)
{
    int verbose = 0, hitch = 0;                        /* -v: every change of size; -h: a frame 4 ms longer now and then */
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "-v")) verbose = 1; if (!strcmp(argv[i], "-h")) hitch = 1; }
    for (unsigned k = 0; k < sizeof SC / sizeof SC[0]; k++) {
        for (int old = 0; old < 2; old++) {
            dr_t d; dr_reset(&d);
            struct { float scale, win, hold, back, since_up, calm_t; int slow; } O = { 1, 0, 0, 3, 99, 0, 0 };
            float q_ms[4] = { 0 }, q_sc[4] = { 0 }; int nq = 0;
            float scale = 1, t = 0, sum = 0, minsc = 1; int frames = 0, missed = 0, changes = 0;
            seed = 7;
            while (t < SC[k].secs) {
                float F, P; SC[k].at(t, &F, &P);
                float g = F + P * scale * scale + 0.35f * noise();
                if (hitch && frames % 47 == 46) g += 4.0f;
                int miss = g > 16.5f;
                float dt = miss ? 1 / 30.0f : 1 / 60.0f;
                frames++; missed += miss; sum += scale; if (scale < minsc) minsc = scale;
                float prev = scale;
                if (!old) {
                    dr_time(&d, dt);
                    q_ms[nq] = g; q_sc[nq] = scale; nq++;
                    if (nq > 2) { scale = dr_sample(&d, q_ms[0], q_sc[0]); memmove(q_ms, q_ms + 1, sizeof(float) * 3); memmove(q_sc, q_sc + 1, sizeof(float) * 3); nq--; }
                } else {
                    O.win += dt; O.since_up += dt;
                    if (miss) O.slow++;
                    if (O.hold > 0) O.hold -= dt;
                    if (O.win >= 0.5f) {
                        if (O.slow >= 2) { if (O.since_up < 2.5f) O.back = fminf(O.back * 2, 20); if (O.scale > 0.71f) O.scale -= 0.05f; O.hold = O.back; O.calm_t = 0; }
                        else { O.calm_t += O.win; if (O.calm_t > 30) O.back = 3;
                               if (O.hold <= 0 && O.scale < 0.99f && O.calm_t >= 2) { O.scale = fminf(1, O.scale + 0.05f); O.since_up = 0; O.hold = 2; } }
                        O.win = 0; O.slow = 0;
                    }
                    scale = O.scale;
                }
                if (fabsf(scale - prev) > 0.01f) { changes++; if (verbose && !old) printf("    %6.2f s: %3.0f%% -> %3.0f%% (GPU %.1f ms, fixed %.1f; really %.1f + %.1f)\n", t, prev * 100, scale * 100, g, d.fixed, F, P); }
                t += dt;
            }
            printf("%-26s %s: %4d of %5d frames miss their refresh, mean size %5.1f%%, smallest %3.0f%%, %d changes\n",
                   SC[k].name, old ? "old" : "new", missed, frames, 100 * sum / frames, minsc * 100, changes);
        }
    }
    return 0;
}
