/* The PC test machine: vita/main.c (the whole game) built for Linux over Mesa's headless GLES 2,
 * with the Vita calls it makes stood in for here. Input comes from a script and/or a command
 * file polled every frame; frames can be saved as pictures; the sound is mixed in step with the
 * game's clock into a WAV; black frames and slow frames are reported.
 *
 * The clock: while the game runs with vsync on, every vglSwapBuffers is exactly 1/60 s later (so
 * a run is the same however slow the PC renders it); with vsync off (the loading screens) the
 * clock follows real time, as on the Vita. Inside a frame it runs in real time, so the game's own
 * timing log measures the PC's work.
 *
 * Environment:
 *   NRPC_OUT     folder for pictures and reports (default /tmp/nrpc/out)
 *   NRPC_SCRIPT  input script (see below)
 *   NRPC_CMD     a file read and emptied every frame: the same commands, sent while it runs
 *   NRPC_WAV     write the mixed sound there (48 kHz stereo 16-bit)
 *   NRPC_MAXT    quit after this many seconds of game time
 *   NRPC_SHOTS   save a small picture every this many seconds (thumbs/NNNNN.ppm)
 *   NRPC_LOADSHOTS  and every frame drawn with vsync off (the loading screens)
 *   NRPC_CHECK   look at every frame: report black frames (and their luminance in frames.csv)
 *   NRPC_FRAMES  per-frame CSV: frame, game time, PC ms, luminance, sound RMS, sound peak
 *   NRPC_REAL    the clock follows real time throughout
 *   NRPC_DET     the loading screens on the frame clock too (1/30 s a frame), so a run repeats exactly
 * Script lines (blank lines and # comments ignored):
 *   [@T | +T] command args      @T: at T seconds of game time; +T: T after the previous line
 *   press BTN [BTN..]   held for 3 frames       hold BTN SECS     down BTN    up BTN
 *   stick lx|ly|rx|ry V (-1..1)                 shot NAME         shots SECS (0 = off)
 *   waitlog TEXT        wait until the game's log gets TEXT (from now on)   quit   echo TEXT
 *   sound               list what the mixer is playing
 *   scale V             draw the 3D scene at V of the screen (gfx_scene_scale)
 *   warp N | finish     put the car at checkpoint N (0 = the first) or at the finish
 *   frametime MS        each frame with vsync takes MS of game time (a slow Vita: the dynamic resolution)
 *   post EVENT          posts a sound event (object 0)
 *   BTN: cross circle square triangle l r up down left right start select */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "vitaGL.h"
#include "../vita/snd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>

#ifndef DATA
#define DATA "/tmp/nrpc/data/"
#endif
#define W 960
#define H 544

static char OUT[256] = "/tmp/nrpc/out";
static uint64_t vbase, rbase, calls;  /* game clock (us) */
static uint64_t frame_us = 16667;     /* a frame with vsync ("frametime MS": a slow Vita, for the dynamic resolution) */
static int vsync = 1, real_clock, det_clock;
static long frame;
static uint64_t real_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000000ull + ts.tv_nsec / 1000; }
/* with vsync: the frame's time, plus 1 us a call (so it never stands still or runs backwards);
 * without: real time */
SceUInt64 sceKernelGetProcessTimeWide(void)
{
    static uint64_t r0; if (!r0) r0 = real_us();
    if (real_clock) return real_us() - r0;
    if (vsync || det_clock) return vbase + (calls < 999 ? ++calls : calls);
    return vbase + (real_us() - rbase);
}
static void clock_rebase(void) { vbase = sceKernelGetProcessTimeWide(); rbase = real_us(); calls = 0; }
static double vt(void) { return sceKernelGetProcessTimeWide() / 1e6; }
#define say(...) do { printf("[nrpc %8.3f f%-6ld] ", vt(), frame); printf(__VA_ARGS__); putchar('\n'); fflush(stdout); } while (0)

/* ------------------------------------------------------------------ input */
static const struct { const char *n; unsigned m; } BTN[] = {
    {"cross", SCE_CTRL_CROSS}, {"circle", SCE_CTRL_CIRCLE}, {"square", SCE_CTRL_SQUARE}, {"triangle", SCE_CTRL_TRIANGLE},
    {"l", SCE_CTRL_LTRIGGER}, {"r", SCE_CTRL_RTRIGGER}, {"up", SCE_CTRL_UP}, {"down", SCE_CTRL_DOWN}, {"left", SCE_CTRL_LEFT},
    {"right", SCE_CTRL_RIGHT}, {"start", SCE_CTRL_START}, {"select", SCE_CTRL_SELECT} };
static unsigned held;                 /* buttons down */
static long rel_frame[16];            /* press: let go at this frame */
static double rel_time[16];           /* hold: let go at this time */
static float stick[4] = {0, 0, 0, 0}; /* lx ly rx ry */
static int btn_index(const char *n) { for (int i = 0; i < 12; i++) if (!strcmp(n, BTN[i].n)) return i; return -1; }
int sceCtrlSetSamplingMode(int mode) { (void)mode; return 0; }
/* the touch screen: script "tap x y" (a short touch), "touch x y" (down, or moved there), "untouch"; screen points 960x544 */
static int touch_on, tap_left; static float touch_x, touch_y;
int sceTouchSetSamplingState(int port, int state) { (void)port; (void)state; return 0; }
int sceTouchPeek(int port, SceTouchData *d, int count)
{
    (void)count; memset(d, 0, sizeof *d);
    if (port != SCE_TOUCH_PORT_FRONT) return 1;
    if (tap_left > 0) { tap_left--; if (!tap_left) touch_on = 0; }
    if (touch_on) { d->reportNum = 1; d->report[0].x = (unsigned short)(touch_x * 2); d->report[0].y = (unsigned short)(touch_y * 2); }
    return 1;
}
int sceCtrlPeekBufferPositive(int port, SceCtrlData *pad, int count)
{
    (void)port; (void)count;
    memset(pad, 0, sizeof *pad);
    pad->buttons = held;
    unsigned char *ax[4] = { &pad->lx, &pad->ly, &pad->rx, &pad->ry };
    for (int i = 0; i < 4; i++) { int v = (int)lroundf(128 + stick[i] * 127); *ax[i] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); }
    return 1;
}

/* ------------------------------------------------------------------ other Vita calls */
int sceKernelGetThreadId(void) { return 1; }
int sceKernelChangeThreadCpuAffinityMask(int thid, int mask) { (void)thid; (void)mask; return 0; }
int sceIoMkdir(const char *dir, int mode) { return mkdir(dir, mode); }
int sceIoGetstat(const char *file, SceIoStat *st) { struct stat s; if (stat(file, &s)) return -1; if (st) st->st_size = s.st_size; return 0; }
size_t vglMemFree(int type) { (void)type; return 100u << 20; }
void vglWaitVblankStart(GLboolean enable) { if ((enable != 0) != vsync) { clock_rebase(); vsync = enable != 0; } }

/* ------------------------------------------------------------------ sound: mixed in step with the clock */
static FILE *wav; static uint32_t wav_frames;
static uint64_t snd_done;             /* samples mixed so far (in blocks of 256, as the mixer works) */
static double frame_rms, frame_peak;
static void wav_close(void)
{
    if (!wav) return;
    uint32_t data = wav_frames * 4, riff = 36 + data;
    fseek(wav, 4, SEEK_SET); fwrite(&riff, 4, 1, wav);
    fseek(wav, 40, SEEK_SET); fwrite(&data, 4, 1, wav);
    fclose(wav); wav = NULL;
}
static void sound_to(uint64_t t_us)
{
    static int16_t buf[48000 * 2];
    frame_rms = frame_peak = 0;
    uint64_t want = t_us * 48 / 1000;
    double s2 = 0; long tot = 0;
    while (want >= snd_done + 256) {
        int n = (int)((want - snd_done) & ~255ull); if (n > 47872) n = 47872;
        snd_render(buf, n);
        for (int i = 0; i < n * 2; i++) { double x = buf[i] / 32768.0; s2 += x * x; if (fabs(x) > frame_peak) frame_peak = fabs(x); }
        tot += n * 2;
        if (wav) { fwrite(buf, 4, n, wav); wav_frames += n; }
        snd_done += n;
    }
    if (tot) frame_rms = sqrt(s2 / tot);
}
static void snd_line(const char *s) { say("  %s", s); }

/* ------------------------------------------------------------------ pictures */
static unsigned char px[W * H * 4];
static void save_ppm(const char *path, int step)
{
    FILE *f = fopen(path, "wb"); if (!f) { say("cannot write %s", path); return; }
    int w = W / step, h = H / step;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    unsigned char *row = malloc(w * 3);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) for (int c = 0; c < 3; c++) {
            int s = 0;
            for (int yy = 0; yy < step; yy++) for (int xx = 0; xx < step; xx++) s += px[((H - 1 - (y * step + yy)) * W + x * step + xx) * 4 + c];
            row[x * 3 + c] = (unsigned char)(s / (step * step));
        }
        fwrite(row, 3, w, f);
    }
    free(row); fclose(f);
}
static double luma(void)
{
    double s = 0; int n = 0;
    for (int y = 0; y < H; y += 4) for (int x = 0; x < W; x += 4) { const unsigned char *p = px + (y * W + x) * 4; s += 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]; n++; }
    return s / n;
}

/* ------------------------------------------------------------------ script */
typedef struct { double t; int rel; char cmd[200]; } line_t;
static line_t *SL; static int nsl, isl; static double sl_prev;   /* script lines, next line, previous line's time */
static double shots_every, next_shot; static int nshot;
char shot_req[64]; static int check_frames, dump_sound, load_shots;
static char wait_text[128]; static long wait_from;     /* waitlog: only what the log gets after the command */
static FILE *frames_csv;
static double maxt;
static void load_script(const char *p)
{
    FILE *f = fopen(p, "r"); if (!f) { say("no script %s", p); return; }
    char ln[256];
    while (fgets(ln, sizeof ln, f)) {
        ln[strcspn(ln, "\r\n")] = 0;
        char *s = ln; while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') continue;
        SL = realloc(SL, (nsl + 1) * sizeof *SL);
        line_t *l = &SL[nsl++]; l->t = 0; l->rel = 1;
        if (*s == '@' || *s == '+') { l->rel = *s == '+'; l->t = strtod(s + 1, &s); while (*s == ' ') s++; }
        snprintf(l->cmd, sizeof l->cmd, "%s", s);
    }
    fclose(f);
    say("script %s: %d lines", p, nsl);
}
static void finish(int code)
{
    sound_to(sceKernelGetProcessTimeWide());
    wav_close();
    if (frames_csv) fclose(frames_csv);
    say("end (code %d)", code);
    _exit(code);
}
static void run_cmd(char *c)
{
    char a[64] = "", b[64] = "", d[64] = "";
    sscanf(c, "%63s %63s %63s", a, b, d);
    if (!strcmp(a, "press")) {
        char *s = c + 5, *tok;
        while ((tok = strtok_r(s, " ", &s))) { int i = btn_index(tok); if (i < 0) say("no button %s", tok); else { held |= BTN[i].m; rel_frame[i] = frame + 3; rel_time[i] = 0; } }
    } else if (!strcmp(a, "hold")) { int i = btn_index(b); if (i >= 0) { held |= BTN[i].m; rel_time[i] = vt() + atof(d); rel_frame[i] = 0; } }
    else if (!strcmp(a, "down")) { int i = btn_index(b); if (i >= 0) { held |= BTN[i].m; rel_time[i] = 0; rel_frame[i] = 0; } }
    else if (!strcmp(a, "up")) { int i = btn_index(b); if (i >= 0) held &= ~BTN[i].m; }
    else if (!strcmp(a, "stick")) { static const char *ax[] = {"lx", "ly", "rx", "ry"}; for (int i = 0; i < 4; i++) if (!strcmp(b, ax[i])) stick[i] = (float)atof(d); }
    else if (!strcmp(a, "shot")) snprintf(shot_req, sizeof shot_req, "%s", b[0] ? b : "shot");
    else if (!strcmp(a, "shots")) { shots_every = atof(b); next_shot = vt(); }
    else if (!strcmp(a, "waitlog")) {
        snprintf(wait_text, sizeof wait_text, "%s", c + 8);
        FILE *f = fopen(DATA "boot.log", "r"); wait_from = 0;
        if (f) { fseek(f, 0, SEEK_END); wait_from = ftell(f); fclose(f); }
    }
    else if (!strcmp(a, "sound")) dump_sound = 1;
    else if (!strcmp(a, "frametime")) { double ms = atof(b); frame_us = (uint64_t)((ms > 1 ? ms : 16.667) * 1000); }
    else if (!strcmp(a, "warp")) { extern int nrpc_warp; nrpc_warp = !strcmp(b, "finish") ? 999 : atoi(b); }
    else if (!strcmp(a, "wreck")) { extern int nrpc_wreck; nrpc_wreck = 1; }
    else if (!strcmp(a, "tap")) { touch_x = (float)atof(b); touch_y = (float)atof(d); touch_on = 1; tap_left = 3; }
    else if (!strcmp(a, "touch")) { touch_x = (float)atof(b); touch_y = (float)atof(d); touch_on = 1; tap_left = 0; }
    else if (!strcmp(a, "untouch")) { touch_on = 0; tap_left = 0; }
    else if (!strcmp(a, "aim")) { extern int nrpc_aim; extern float nrpc_aim_speed, nrpc_aim_sign; float sp = 60, sg = 1; sscanf(c, "%*s %*s %f %f", &sp, &sg); nrpc_aim_speed = sp; nrpc_aim_sign = sg; nrpc_aim = atoi(b); }
    else if (!strcmp(a, "scale")) { extern float gfx_scene_scale; gfx_scene_scale = (float)atof(b); }
    else if (!strcmp(a, "post")) { int r = snd_post(b, 0); say("posted %s: %d", b, r); }     /* a sound event, as the game would */
    else if (!strcmp(a, "echo")) say("%s", c + 5);
    else if (!strcmp(a, "quit")) finish(0);
    else say("unknown command: %s", c);
}
static int log_has(const char *text)
{
    FILE *f = fopen(DATA "boot.log", "r"); if (!f) return 0;
    static char buf[1 << 20]; fseek(f, wait_from, SEEK_SET); size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
    return strstr(buf, text) != NULL;
}
static void input_step(void)
{
    double t = vt();
    for (int i = 0; i < 12; i++) {
        if (rel_frame[i] && frame >= rel_frame[i]) { held &= ~BTN[i].m; rel_frame[i] = 0; }
        if (rel_time[i] > 0 && t >= rel_time[i]) { held &= ~BTN[i].m; rel_time[i] = 0; }
    }
    if (wait_text[0]) { if ((frame % 15) == 0 && log_has(wait_text)) { say("log has \"%s\"", wait_text); wait_text[0] = 0; sl_prev = t; } }
    while (!wait_text[0] && isl < nsl) {
        line_t *l = &SL[isl];
        double at = l->rel ? sl_prev + l->t : l->t;
        if (t < at) break;
        sl_prev = t; isl++;
        say("> %s", l->cmd);
        run_cmd(l->cmd);
    }
    const char *cf = getenv("NRPC_CMD");
    if (cf && (frame % 6) == 0) {
        FILE *f = fopen(cf, "r");
        if (f) {
            char ln[256]; int any = 0;
            while (fgets(ln, sizeof ln, f)) { ln[strcspn(ln, "\r\n")] = 0; if (ln[0] && ln[0] != '#') { say(">> %s", ln); run_cmd(ln); any = 1; } }
            fclose(f);
            if (any) { f = fopen(cf, "w"); if (f) fclose(f); }
        }
    }
}

/* ------------------------------------------------------------------ the frame */
void vglSwapBuffers(GLboolean has_commondialog)
{
    (void)has_commondialog;
    static uint64_t r_prev; uint64_t rn = real_us();
    double pc_ms = r_prev ? (rn - r_prev) / 1000.0 : 0; r_prev = rn;
    {   static int nerr; GLenum e;
        while ((e = glGetError()) != GL_NO_ERROR) if (nerr++ < 40) say("GL error 0x%x", e);
    }
    int want_shot = shot_req[0] != 0, want_thumb = (shots_every > 0 && vt() >= next_shot) || (load_shots && !vsync);
    double lum = -1;
    if (want_shot || want_thumb || check_frames) {
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
        if (check_frames) {
            lum = luma();
            static int dark_run; static double dark_from;
            if (lum < 2.0) { if (!dark_run) dark_from = vt(); dark_run++; }
            else { if (dark_run >= 3) say("BLACK %d frames from %.2f s", dark_run, dark_from); dark_run = 0; }
        }
        if (want_shot) { char p[400]; snprintf(p, sizeof p, "%s/%s.ppm", OUT, shot_req); save_ppm(p, 1); say("picture %s", p); shot_req[0] = 0; }
        if (want_thumb) {
            char p[400]; snprintf(p, sizeof p, "%s/thumbs/%05d.ppm", OUT, nshot++); save_ppm(p, 4);
            if (shots_every > 0 && vt() >= next_shot) next_shot += shots_every;
        }
    } else glFinish();
    /* the clock */
    if (!real_clock) {
        if (vsync) { vbase += frame_us; calls = 0; rbase = real_us(); }
        else if (det_clock) { vbase += 33333; calls = 0; }      /* (a loading screen frame: 1/30 s) */
        else clock_rebase();
    }
    sound_to(sceKernelGetProcessTimeWide());
    if (dump_sound) { dump_sound = 0; snd_debug_dump(snd_line); }
    if (frames_csv) fprintf(frames_csv, "%ld,%.4f,%.2f,%.1f,%.4f,%.4f,%d\n", frame, vt(), pc_ms, lum, frame_rms, frame_peak, vsync);
    frame++;
    if (maxt > 0 && vt() > maxt) { say("time limit"); finish(0); }
    input_step();
}

/* ------------------------------------------------------------------ start */
static void on_crash(int sig)
{
    void *bt[64]; int n = backtrace(bt, 64);
    fprintf(stderr, "\n[nrpc] CRASH signal %d at game time %.3f, frame %ld\n", sig, vt(), frame);
    backtrace_symbols_fd(bt, n, 2);
    wav_close();
    _exit(128 + sig);
}
int sceKernelExitProcess(int res) { finish(res); return 0; }
int vglInitExtended(int legacy_pool_size, int width, int height, int ram_threshold, SceGxmMultisampleMode msaa)
{
    (void)legacy_pool_size; (void)width; (void)height; (void)ram_threshold; (void)msaa;
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay d = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint ma, mi; if (!eglInitialize(d, &ma, &mi)) { say("no EGL"); _exit(2); }
    EGLint ca[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                    EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE };
    EGLConfig c; EGLint n; eglChooseConfig(d, ca, &c, 1, &n); if (!n) { say("no EGL config"); _exit(2); }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext x = eglCreateContext(d, c, EGL_NO_CONTEXT, xa);
    EGLint pa[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    EGLSurface s = eglCreatePbufferSurface(d, c, pa);
    if (!eglMakeCurrent(d, s, s, x)) { say("no GL context"); _exit(2); }
    say("GL: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    return 0;
}
__attribute__((constructor)) static void nrpc_start(void)
{
    const char *o = getenv("NRPC_OUT"); if (o) snprintf(OUT, sizeof OUT, "%s", o);
    char p[400]; mkdir(OUT, 0777); snprintf(p, sizeof p, "%s/thumbs", OUT); mkdir(p, 0777);
    real_clock = getenv("NRPC_REAL") != NULL;
    det_clock = getenv("NRPC_DET") != NULL;            /* loading screens on the frame clock too: runs repeat exactly */
    rbase = real_us(); vbase = 0;
    if (getenv("NRPC_SCRIPT")) load_script(getenv("NRPC_SCRIPT"));
    if (getenv("NRPC_WAV")) {
        wav = fopen(getenv("NRPC_WAV"), "wb");
        if (wav) {
            uint32_t z = 0, fmt = 16, rate = 48000, br = 48000 * 4; uint16_t pcm = 1, ch = 2, ba = 4, bits = 16;
            fwrite("RIFF", 1, 4, wav); fwrite(&z, 4, 1, wav); fwrite("WAVEfmt ", 1, 8, wav); fwrite(&fmt, 4, 1, wav);
            fwrite(&pcm, 2, 1, wav); fwrite(&ch, 2, 1, wav); fwrite(&rate, 4, 1, wav); fwrite(&br, 4, 1, wav);
            fwrite(&ba, 2, 1, wav); fwrite(&bits, 2, 1, wav); fwrite("data", 1, 4, wav); fwrite(&z, 4, 1, wav);
        }
    }
    if (getenv("NRPC_SHOTS")) { shots_every = atof(getenv("NRPC_SHOTS")); next_shot = 0; }
    check_frames = getenv("NRPC_CHECK") != NULL;
    load_shots = getenv("NRPC_LOADSHOTS") != NULL;     /* every frame of the loading screens */
    if (getenv("NRPC_FRAMES")) { frames_csv = fopen(getenv("NRPC_FRAMES"), "w"); if (frames_csv) fprintf(frames_csv, "frame,t,pc_ms,luma,rms,peak,vsync\n"); }
    if (getenv("NRPC_MAXT")) maxt = atof(getenv("NRPC_MAXT"));
    signal(SIGSEGV, on_crash); signal(SIGABRT, on_crash); signal(SIGFPE, on_crash); signal(SIGBUS, on_crash);
}
