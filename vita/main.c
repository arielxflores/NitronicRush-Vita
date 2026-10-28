/* Nitronic Rush on PS Vita.
 * Plays the game's own data (converted from your install by converter/nr_pack.py and
 * converter/nr_audio.py into ux0:data/nitronic/): levels, car, menus, HUD art, sound and music.
 *
 * Controls (defaults; see controls.h): R gas, L brake/reverse, left stick steer (and roll/pitch in the air),
 *   Cross jump, Circle boost, Square wings, right stick or d-pad tricks, Triangle change view, Select reset the car,
 *   Start pause. Advanced Controls (Options): hold the Roll button (L) in the air and the left stick does the
 *   tricks; the right stick turns the camera.
 * Switch files in ux0:data/nitronic/ (empty files):
 *   autoplay   walk through the menus, then let the computer drive each level; log; quit
 *   autoplay_shots   with autoplay: pictures of each race's start and finish, first sparks and wing trail
 *   no_mips / rgba_tex / no_fog / no_cutout / no_cull / no_tex_cache / discard_cutout /
 *   no_transfer_wait / cg_shaders / msaa2x / msaa4x   (graphics fallbacks)
 *   no_post    draw straight to the screen (no bloom, speed blur)   no_bloom   no glow
 *   no_sound   don't start the sound engine */
#include <vitasdk.h>
#include <vitaGL.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NR_PC
#include <time.h>
#endif
#include "../src/nr_pak.h"
#include "../src/nr_data.h"
#include "../src/nr_coll.h"
#include "../src/nr_car.h"
#include "../src/nr_tricks.h"
#include "../src/nr_objs.h"
#include "gfx.h"
#include "hud.h"
#include "snd.h"
#include "save.h"
#include "front.h"
#include "particles.h"
#include "controls.h"
#include "camera.h"
#include "ghost.h"
#include "scores.h"
#include "chase.h"
#include "results.h"
#include "cinema.h"
#include "io.h"

#ifndef DATA
#define DATA "ux0:data/nitronic/"   /* (the PC test build points it elsewhere) */
#endif
static const char *SAVE_PATH = DATA "save.txt";   /* autoplay keeps its own file */
#ifndef NR_BUILD_LABEL
#define NR_BUILD_LABEL "dev"
#endif
int _newlib_heap_size_user = 192 * 1024 * 1024;

/* ------------------------------------------------------------------ log */
static int g_log;                              /* the log is open (written by io.c's thread) */
static uint64_t t0;
static uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }
static float phys_us;                          /* car physics time in the current stats window */
/* where the game thread's frame goes (race screens, microseconds): game logic, building the 3D
 * frame, the HUD, and waiting in vglSwapBuffers for the GPU and the display */
static uint64_t T_logic, T_draw, T_hud, T_swap, T_all_busy, T_all_swap; static int T_n;
static int log_buffered;                       /* (no longer used: the log is written by a thread, io.c) */
static void logm(const char *fmt, ...)
{
    if (!g_log) return;
    char b[1400];
    int n = snprintf(b, sizeof b, "[%7.2f] ", (now_us() - t0) / 1e6);
    va_list ap; va_start(ap, fmt);
    int m = vsnprintf(b + n, sizeof b - n - 1, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    if (n + m > (int)sizeof b - 2) m = (int)sizeof b - 2 - n;
    b[n + m] = '\n';
    io_log(b, (size_t)(n + m + 1));
}
static void snd_log_line(const char *s) { logm("    %s", s); }
static const uint32_t BTN_MASK[BTN_N] = { SCE_CTRL_CROSS, SCE_CTRL_CIRCLE, SCE_CTRL_SQUARE, SCE_CTRL_TRIANGLE, SCE_CTRL_LTRIGGER,
    SCE_CTRL_RTRIGGER, SCE_CTRL_UP, SCE_CTRL_DOWN, SCE_CTRL_LEFT, SCE_CTRL_RIGHT, SCE_CTRL_START, SCE_CTRL_SELECT };
#ifdef NR_PC
int nrpc_warp = -1, nrpc_wreck, nrpc_aim = -1; float nrpc_aim_speed = 60, nrpc_aim_sign = 1;
#endif
static int has_switch(const char *n)
{
    SceIoStat st; char p[256];
    snprintf(p, sizeof p, DATA "%s", n);
    return sceIoGetstat(p, &st) >= 0;
}

/* ------------------------------------------------------------------ data */
static nr_pak pak;
static save_t SAVE;
static void dyn_res_reset(void);
static void dyn_res_step(float dt, int on);
static uint32_t act_mask(int a) { int b = SAVE.bind[a]; return b >= 0 && b < BTN_N ? BTN_MASK[b] : 0; }
static level_rt L, MENU, CAR, FLAME, PORTALM;   /* PORTALM: the tunnels' Portal (models/portal.nrl) */
static int menu_loaded;
/* the pieces the car breaks into (models/gibs.txt) and the ones flying about */
static struct { level_rt m; float off[3]; char ghost[48]; } GIBM[16]; static int ngibm;   /* ghost: its ReplayRecorder name */
typedef struct { int model; v3 pos, vel, w; float rot[3][3]; float life; } gib_t;
static gib_t GIBS[16]; static int ngibs;
static nr_coll COLL;
static nr_car_params carp;

enum { OBJ_CAR = 1, OBJ_CP = 2, OBJ_RING = 3, OBJ_BOOM = 4, OBJ_SPARK = 5, OBJ_EMIT0 = 16, OBJ_NEMIT = 40 };
typedef struct { char ev[40]; int begin; float p[3]; int slot; int armed; float radius; } emitter;
typedef struct { float p[3], ax[3], radius, strength, heal; float cool; } ring_t;
typedef struct { float p[3], s[3]; } cooler_t;
static struct {
    char name[96]; float start[3]; int has_start; float finish[8][3]; int nfinish; float cp[64][3]; int ncp;
    float cpR[64][3][3]; int cp_has_R[64]; float cp_scl[64][3];
    float fbox[8][15]; int nfbox;                  /* LevelSwitch triggers: position, orientation, scale */
    emitter snd[600]; int nsnd; ring_t ring[64]; int nring; cooler_t cool[32]; int ncool;
} G;
static int emit_slot_used[OBJ_NEMIT];
static gfx_light LIGHTS[1024]; static int NLIGHTS;       /* the level's static lights (for lighting the car) */

/* ------------------------------------------------------------------ level objects (src/nr_objs.c) */
#ifdef NR_PC
static void haz_list(void);
#endif
static nr_objs OBJS; static int objs_on;
#define MAX_DRAWOBJ 480
#define MAX_OMODEL 400
static level_rt OMODEL[MAX_OMODEL]; static char OMNAME[MAX_OMODEL][48]; static int NOMODEL;
enum { OBJ_OSND0 = 64, OBJ_NOSND = 48, OBJ_FX0 = 112, OBJ_NFX = 16 };
static int osnd_used[OBJ_NOSND], fx_next;
static int om_model(void *u, const char *name, float mn[3], float mx[3])
{
    (void)u;
    for (int i = 0; i < NOMODEL; i++) if (!strcmp(OMNAME[i], name)) { memcpy(mn, OMODEL[i].bmin, 12); memcpy(mx, OMODEL[i].bmax, 12); return i; }
    if (NOMODEL >= MAX_OMODEL) return -1;
    char p[80]; snprintf(p, sizeof p, "models/%s.nrl", name);
    if (!gfx_load_nrl(&OMODEL[NOMODEL], &pak, p, name)) return -1;
    snprintf(OMNAME[NOMODEL], sizeof OMNAME[0], "%s", name);
    memcpy(mn, OMODEL[NOMODEL].bmin, 12); memcpy(mx, OMODEL[NOMODEL].bmax, 12);
    return NOMODEL++;
}
static uint8_t *om_read(void *u, const char *p, uint32_t *sz) { (void)u; return (uint8_t *)nr_pak_read(&pak, p, sz); }
static int objs_quiet;                             /* (the objects' head start before the countdown: no sounds) */
static void om_sound(void *u, const char *ev, const float *p)
{
    if (objs_quiet) return;
    (void)u; int s = OBJ_FX0 + (fx_next++ % OBJ_NFX);
    snd_set_obj_pos(s, p); snd_post(ev, s);
}
static void om_rtpc(void *u, int i, const char *n, float v)
{
    (void)u;
    if (i >= 0 && i < OBJS.n && OBJS.o[i].snd_slot >= 0) snd_set_obj_param(OBJ_OSND0 + OBJS.o[i].snd_slot, n, v);
}
static void om_event(void *u, const char *what, int arg);
static float kvf_line(const char *line, const char *key)
{
    char b[48]; return nr_objs_value(line, key, b, sizeof b, 0) ? strtof(b, NULL) : 0;
}
static void obj_sounds_off(void)
{
    for (int i = 0; i < OBJS.n; i++) if (OBJS.o[i].snd_slot >= 0) { snd_stop_obj(OBJ_OSND0 + OBJS.o[i].snd_slot, 300); OBJS.o[i].snd_slot = -1; }
    memset(osnd_used, 0, sizeof osnd_used);
}
/* objects' looping sounds (Phantom, Begin Playing): while the car is near, following them */
static void update_obj_sounds(v3 cpos)
{
    static int tick; tick++;
    for (int i = 0; i < OBJS.n; i++) {
        nr_obj *o = &OBJS.o[i];
        if (o->snd_slot >= 0 && o->alive) snd_set_obj_pos(OBJ_OSND0 + o->snd_slot, o->pos);
        if ((tick & 3) || !o->snd[0] || !o->sndloop) continue;
        float dx = o->pos[0] - cpos.x, dy = o->pos[1] - cpos.y, dz = o->pos[2] - cpos.z, d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (o->snd_slot < 0 && o->alive && d < 140 + o->radius * 0.5f) {
            int s = -1;
            for (int k = 0; k < OBJ_NOSND; k++) if (!osnd_used[k]) { s = k; break; }
            if (s < 0) continue;
            osnd_used[s] = 1; o->snd_slot = s;
            snd_set_obj_pos(OBJ_OSND0 + s, o->pos);
            snd_post(o->snd, OBJ_OSND0 + s);
        } else if (o->snd_slot >= 0 && (!o->alive || d > 170 + o->radius * 0.5f)) {
            snd_stop_obj(OBJ_OSND0 + o->snd_slot, 400);
            osnd_used[o->snd_slot] = 0; o->snd_slot = -1;
        }
    }
}
/* the level's particle emitters (weather): ParticleEmitter values from their obj lines */
typedef struct { int obj; nr_pemit e; nr_pemit_state st; float follow, scale; } emitter_rt;
static emitter_rt PEMIT[32]; static int NPEMIT, particles_ok;
static void add_emitter(int i)
{
    const nr_obj *o = &OBJS.o[i];
    {
        emitter_rt *m = &PEMIT[NPEMIT++]; memset(m, 0, sizeof *m);
        nr_pemit *e = &m->e; const char *L = o->line; char b[96];
#define PF(field, key, def) e->field = nr_objs_value(L, "ParticleEmitter." key, b, sizeof b, 0) ? strtof(b, NULL) : def;
#define PV(field, key) if (nr_objs_value(L, "ParticleEmitter." key, b, sizeof b, 0)) sscanf(b, "%f,%f,%f", &e->field[0], &e->field[1], &e->field[2]);
        PF(color_idx, "Color_Index", 1) PF(color_rand, "Color_Index_Randomness", 0) PF(inten, "Color_Intensity", 1)
        PF(tex_idx, "Texture_Index", 1) PF(tex_rand, "Texture_Index_Randomness", 0) PF(tex_delta, "Texture_Delta", 0)
        PF(size, "Size_Intensity", 1) PF(size_rand, "Size_Randomness", 0) PF(rot, "Rotation", 0) PF(rot_rand, "Rotation_Randomness", 0)
        PF(drot, "Delta_Rotation", 0) PF(angle, "Angle", 0) PF(speed, "Speed", 0) PF(speed_rand, "Speed_Randomness", 0)
        PF(gravity, "Gravity", 0) PF(life, "Particle_Lifetime", 1) PF(freq, "Spawn_Frequency", 0)
        PV(pos, "Position") PV(pos_rand, "Position_Randomness") PV(dir, "Direction")
#undef PF
#undef PV
        m->obj = i;
        m->follow = nr_objs_value(L, "CameraFollower.Distance", b, sizeof b, 0) ? strtof(b, NULL) : -1;
        /* the weather keeps 16000 flakes about on the PC; a tenth of them here */
        m->scale = e->freq * e->life > 2000 ? 0.1f : e->freq * e->life > 400 ? 0.3f : 1.0f;
    }
}
static void load_emitters(void)
{
    NPEMIT = 0; particles_clear();
    for (int i = 0; i < OBJS.n && NPEMIT < 32; i++) if (OBJS.o[i].alive && OBJS.o[i].kind == OK_EMITTER) add_emitter(i);
    if (NPEMIT) logm("  %d particle emitters%s", NPEMIT, particles_ok ? "" : " (no particle textures)");
}
static void run_emitters(v3 eye, v3 fwd, float dt)
{
    for (int i = 0; i < NPEMIT;) {                /* gone: drop; new ones (spawned by the logic): add */
        const nr_obj *o = &OBJS.o[PEMIT[i].obj];
        if (PEMIT[i].obj >= OBJS.n || !o->alive || o->kind != OK_EMITTER) PEMIT[i] = PEMIT[--NPEMIT]; else i++;
    }
    for (int i = 0; i < OBJS.n && NPEMIT < 32; i++) {
        if (!OBJS.o[i].alive || OBJS.o[i].kind != OK_EMITTER) continue;
        int known = 0;
        for (int k = 0; k < NPEMIT && !known; k++) known = PEMIT[k].obj == i;
        if (!known) add_emitter(i);
    }
    for (int i = 0; i < NPEMIT; i++) {
        emitter_rt *m = &PEMIT[i];
        const nr_obj *o = &OBJS.o[m->obj];
        float M[3][4], pos[3] = { o->pos[0], o->pos[1], o->pos[2] };
        if (m->follow >= 0) { pos[0] = eye.x + fwd.x * m->follow; pos[1] = eye.y + fwd.y * m->follow; pos[2] = eye.z + fwd.z * m->follow; }
        for (int r = 0; r < 3; r++) { for (int k = 0; k < 3; k++) M[r][k] = o->R[r][k] * o->scale[k]; M[r][3] = pos[r]; }
        particles_emit(&m->e, &m->st, M, dt, m->scale);
    }
}
static void trk_draw(int pass);
static void draw_particles_hook(v3 eye, v3 fwd, v3 up) { trk_draw(1); if (particles_ok) particles_draw(eye, fwd, up); }
static void draw_decal_hook(v3 eye, v3 fwd, v3 up) { (void)eye; (void)fwd; (void)up; trk_draw(0); }

/* ------------------------------------------------------------------ the announcer */
/* AudioLogic's announcer queue (0x6dc6e0, played by its update 0x6dd210): the voice lines (the tutorial's, the
 * tricks', the deaths', the checkpoint, the finish, the places) wait for the one before; each takes 1.25 s,
 * however long it is, then the next starts */
static struct { uint32_t ev[16]; int n; float t; } ANNQ;
static void ann_queue(const char *ev) { if (ev && ANNQ.n < 16) ANNQ.ev[ANNQ.n++] = snd_hash(ev); }
static void ann_clear(void) { ANNQ.n = 0; ANNQ.t = 0; }
static void ann_update(float dt)
{
    if (ANNQ.n && ANNQ.t < 0.0001f) {
        snd_post_id(ANNQ.ev[0], 0);
        memmove(ANNQ.ev, ANNQ.ev + 1, (size_t)--ANNQ.n * sizeof ANNQ.ev[0]);
        ANNQ.t = 1.25f;
    }
    if (ANNQ.t > 0.0001f) ANNQ.t -= dt;
}
static int rand_in(int lo, int hi) { return lo + (int)((unsigned)rand() % (unsigned)(hi - lo + 1)); }   /* (0x661230: both ends in) */
static void ann_phrase(const char *name) { char ev[48]; snprintf(ev, sizeof ev, "Play_%s", name); ann_queue(ev); }
/* the PC's phrase tables (0x7934a8: the tricks'; 0x793668: crashes, then from 36 overheating; 0x793738: self-terminations) */
static const char *const PH_GOOD[111] = {
    "Amazing", "Awesome", "AwesomeSauce", "Bangin_", "BetterThanSlicedBread", "Blazin_", "BombDigity", "Boss",
    "CookingWithGas", "Cool", "Cowabunga", "DoubleRainbow", "Dynomite", "Epic", "Fantabulous", "FarOut", "Funkadelic",
    "Gnarly", "Groovy", "Hardcore", "HotDog", "Jammin_", "Juggernaut", "Mega", "NoHandlebars", "NoWay", "Nuclear",
    "OhYeah", "OutStanding", "Radical", "Snazzy", "SoGood", "Spiffy", "Super", "Superfly", "That_sFantastic",
    "TotallyAwesome", "TotallyTubular", "TwisterCity", "WayGood", "Whoa", "Yeah0", "Intense", "Killer", "Unreal",
    "Goin_TheDistance", "Shabang", "Shabango", "TheFinalCountdown", "Excellent", "KaBamSon", "It_sBusinessTime",
    "BuhBam", "Money", "InTheBank", "GoodStuff", "Crazy", "Ridiculous", "TechnicalExcellence", "Carnival", "ChaChing",
    "Dude", "Fabulous", "GoodShow", "Nice", "OhSnap", "OhYeah_01", "Score", "Tight", "TrickMania", "Trickster",
    "Wonderful", "Legendary", "Winning", "AllRight", "CarFu", "Extreme", "Rad", "RightOn", "Rockin", "RockNRoll",
    "Smooth", "Sweet", "TwistAndShout", "WellDone", "Wicked", "Incredible", "Unbelievable", "Alright", "Astounding",
    "BoomShakalaka", "Clever", "Cunning", "FlipItTurnWays", "GreatJob", "Insane", "LikeABoss", "Neato", "Nifty",
    "Ninja", "OutOfThePark", "Professional", "Serious", "SpecialDelivery", "SpinninAndGrinnin", "SpinToWin",
    "Stunning", "Supreme", "Thriller", "TrulyOutragous", "Whirlwind" };
static const char *const PH_BAD[51] = {
    "DrivingMissDaisy", "CarBoom", "RoadKill", "StepOnIt", "StudentDriver", "That_sWhack", "Lamesauce", "Weaksauce",
    "KnockOut", "Nitragic", "NoCigar", "You_reOutOfControl", "Bummer", "ItKeepsHappening", "MondoDisaster",
    "BucketsOfTears", "Denied", "Destroyed", "Fragged", "Fragmented", "Rejected", "SadDay", "SoSad", "TakeDown",
    "TooBad", "Uncool", "Unfortunate", "Wrecked", "Wreckless", "HolyToledo", "Sayonara", "ShutDown", "AwwThatsTooBad",
    "Bogus", "CriticalError", "MayDay", "OverHeat", "TooHot", "TireFire", "Nuclear", "BurnOut", "Toasty",
    "AccessViolation", "BlownAGasket", "BurningUp", "Explosive", "FlamingHot", "IWarnedYouBro", "ShortCircuit",
    "SoClose", "Vaporized" };
static const char *const PH_BOOM[30] = {
    "CarBoom", "Crikey", "DrivingMissDaisy", "KnockOut", "Lamesauce", "NoCigar", "Nuclear", "RoadKill", "StepOnIt",
    "StudentDriver", "That_sWhack", "Weaksauce", "Bummer", "ItKeepsHappening", "BucketsOfTears", "Rejected", "SadDay",
    "SoSad", "TooBad", "Unfortunate", "Sayonara", "ShutDown", "AccessViolation", "AwwThatsTooBad", "Bogus",
    "CriticalError", "DivisionByZero", "Farewell", "Fireworks", "NullTerminated" };
static int ph_hist[10] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 }, ph_hist_i;   /* (0x7fe7d0 +0x20..+0x48: not reset) */
static int race_overheats;                     /* overheat deaths since Race Start (0x7fe7d0 +0) */
/* a trick's phrase when no trick named it (Trick Complete 0x6fb7f0 -> 0x6fadd0): from the tricks' table, none of the
 * last ten; over 25,000 points "Cheater!" three times in five. (The PC hands this picker trick counts that are always
 * 0 (0x6dbe7e), so the phrases that need two twists or three turns never come; Neato and Nifty not over 3,000.) */
static const char *trick_phrase(int points)
{
    for (int tries = 0; tries < 200; tries++) {
        if (points > 25000 && rand_in(0, 4) > 1) return "Cheater";
        int i;
        for (;;) {                                 /* 0x6fad20 */
            i = rand_in(0, 110); int seen = 0;
            for (int k = 0; k < 10; k++) if (ph_hist[k] == i) seen = 1;
            if (!seen) break;
        }
        ph_hist[ph_hist_i] = i; ph_hist_i = (ph_hist_i + 1) % 10;
        const char *e = PH_GOOD[i];
        if (!strcmp(e, "TwistAndShout") || !strcmp(e, "SpinninAndGrinnin") || !strcmp(e, "SpinToWin") || !strcmp(e, "FlipItTurnWays")) continue;
        if (points > 3000 && (!strcmp(e, "Neato") || !strcmp(e, "Nifty"))) continue;
        return e;
    }
    return "Awesome";
}
/* the "Car Death" phrase (0x6fae50) by the cause: 1 self-termination, 2 a deadly surface, 3 a crash, 4 overheating,
 * 5 out of the level */
static void death_phrase(int cause, float speed2)
{
    if (cause == 1) { ann_phrase(PH_BOOM[rand_in(0, 29)]); return; }
    if (cause == 4) {
        if (++race_overheats <= 2) ann_phrase("OverHeat");
        else ann_phrase(PH_BAD[36 + rand_in(0, 14)]);
        return;
    }
    if (cause == 3) {
        if ((float)rand() / RAND_MAX <= 0.1f && speed2 >= 6000) { ann_phrase("InertiaSucks"); return; }
        const char *e = PH_BAD[rand_in(0, 35)];
        static const char *const SP[4] = { "NiceTry", "NitroniCrashed", "ThatsGottaHurt", "WrongTurn" };
        int r = rand_in(0, 39); if (r <= 3) e = SP[r];
        ann_phrase(e);
        return;
    }
    const char *e = PH_BAD[rand_in(0, 35)];
    if (rand_in(0, 36) == 36) e = rand_in(0, 1) ? "Farewell" : "Error404";
    ann_phrase(e);
}

static void rank_voice(int rank)
{
    if (rank == 0) { ann_queue("Play_FirstPlace"); ach_unlock("First!"); }
    else if (rank == 1) ann_queue("Play_SecondPlace");
    else if (rank == 2) ann_queue("Play_ThirdPlace");
}

/* LightPulse's clock: the music's beats (tempos from the game's sound bank) */
static float beat_t, beat_period = 60.0f / 128;
static float music_bpm(const char *layer)
{
    static const struct { const char *n; float bpm; } T[] = {
        { "TurboTunnel", 115 }, { "EnterTheMatrix", 132 }, { "Gladiator", 150 }, { "PurpleEighties", 135 }, { "Nitroception", 130 },
        { "GreenRMX", 105 }, { "ItsChristmas", 85 }, { "Stunt_1", 132 }, { "FirstPlayable", 120 }, { "NeonSkyway", 120 },
        { "BetaRed", 120 }, { "PinkClockwork", 120 } };
    for (unsigned i = 0; layer && i < sizeof T / sizeof T[0]; i++) if (!strcmp(T[i].n, layer)) return T[i].bpm;
    return 128;
}

static void read_nrg(const char *lvname)
{
    memset(&G, 0, sizeof G);
    NLIGHTS = 0;
    snprintf(G.name, sizeof G.name, "%s", lvname);
    char p[96]; snprintf(p, sizeof p, "levels/%s.nrg", lvname);
    uint32_t sz; char *gi = nr_pak_read(&pak, p, &sz);
    for (char *ln = gi ? strtok(gi, "\n") : NULL; ln; ln = strtok(NULL, "\n")) {
        float x, y, z, a, b, c, d, e, f, g; int bg; char ev[40];
        if (!strncmp(ln, "name ", 5)) snprintf(G.name, sizeof G.name, "%s", ln + 5);
        else if (sscanf(ln, "start %f %f %f", &x, &y, &z) == 3) { G.start[0] = x; G.start[1] = y; G.start[2] = z; G.has_start = 1; }
        else if (sscanf(ln, "finish %f %f %f", &x, &y, &z) == 3 && G.nfinish < 8) { G.finish[G.nfinish][0] = x; G.finish[G.nfinish][1] = y; G.finish[G.nfinish++][2] = z; }
        else if (!strncmp(ln, "finishbox ", 10) && G.nfbox < 8) {
            float *b = G.fbox[G.nfbox];
            if (sscanf(ln, "finishbox %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7], &b[8], &b[9],
                       &b[10], &b[11], &b[12], &b[13], &b[14]) == 15) G.nfbox++;
        }
        else if (sscanf(ln, "checkpoint %f %f %f", &x, &y, &z) == 3 && G.ncp < 64) {
            float r9[9];
            float sc3[3] = { 1, 1, 1 };
            int nf = sscanf(ln, "checkpoint %*f %*f %*f %f %f %f %f %f %f %f %f %f %f %f %f", &r9[0], &r9[1], &r9[2], &r9[3], &r9[4], &r9[5], &r9[6], &r9[7], &r9[8], &sc3[0], &sc3[1], &sc3[2]);
            G.cp_has_R[G.ncp] = nf >= 9;
            if (G.cp_has_R[G.ncp]) memcpy(G.cpR[G.ncp], r9, 36);
            G.cp_scl[G.ncp][0] = nf >= 12 ? sc3[0] : 1; G.cp_scl[G.ncp][1] = nf >= 12 ? sc3[1] : 1; G.cp_scl[G.ncp][2] = nf >= 12 ? sc3[2] : 1;
            G.cp[G.ncp][0] = x; G.cp[G.ncp][1] = y; G.cp[G.ncp++][2] = z;
        }
        else if (sscanf(ln, "sound %39s %d %f %f %f", ev, &bg, &x, &y, &z) == 5 && G.nsnd < 600) {
            emitter *m = &G.snd[G.nsnd++]; snprintf(m->ev, sizeof m->ev, "%s", ev); m->begin = bg; m->p[0] = x; m->p[1] = y; m->p[2] = z; m->slot = -1; m->armed = 1;
            m->radius = 25; sscanf(ln, "sound %*s %*d %*f %*f %*f %f", &m->radius);
        } else if (sscanf(ln, "ring %f %f %f %f %f %f %f %f %f %f", &x, &y, &z, &a, &b, &c, &d, &e, &f, &g) == 10 && G.nring < 64) {
            ring_t *r = &G.ring[G.nring++]; r->p[0] = x; r->p[1] = y; r->p[2] = z; r->ax[0] = a; r->ax[1] = b; r->ax[2] = c;
            r->radius = d * 1.2f; r->strength = f; r->heal = g;
        } else if (sscanf(ln, "cooler %f %f %f %f %f %f", &x, &y, &z, &a, &b, &c) == 6 && G.ncool < 32) {
            cooler_t *k = &G.cool[G.ncool++]; k->p[0] = x; k->p[1] = y; k->p[2] = z; k->s[0] = a; k->s[1] = b; k->s[2] = c;
        } else if (sscanf(ln, "dummy CarDummy %f %f %f", &x, &y, &z) == 3 && !strncmp(ln, "dummy CarDummy ", 15)) { float q[3] = { x, y, z }; front_set_dummies(q, NULL); }
        else if (sscanf(ln, "dummy CarDummyFly %f %f %f", &x, &y, &z) == 3) { float q[3] = { x, y, z }; front_set_dummies(NULL, q); }
        else if (!strncmp(ln, "light ", 6) && NLIGHTS < 1024) {
            gfx_light *l = &LIGHTS[NLIGHTS];
            if (sscanf(ln, "light %d %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f", &l->type, &l->pos[0], &l->pos[1], &l->pos[2],
                       &l->col[0], &l->col[1], &l->col[2], &l->amb, &l->near_, &l->far_, &l->axis[0], &l->axis[1], &l->axis[2],
                       &l->half, &l->cos_in, &l->cos_out) == 16 && l->far_ > l->near_) NLIGHTS++;
        }
    }
    free(gi);
}

/* the loading screen (0x610640): drawn again now and then while the level loads, so its light
 * sweep runs (screenload.fx's Time); no vsync meanwhile */
static uint64_t load_t0, load_last;
static void loading_draw(void)
{
    /* the sweep runs along the lights in 2.4 s (Time from -0.8 to 1.6) and starts again, so every
     * loading screen shows it (on the PC only the first one did: its clock never restarts) */
    float t = (now_us() - load_t0) / 1e6f;
    hud_begin(); hud_loading_screen(hud_tex("loading2"), fmodf(t, 2.4f) - 0.8f); hud_end();
    vglSwapBuffers(GL_FALSE);
    load_last = now_us();
}
static void loading_tick(void) { if (now_us() - load_last > 30000) loading_draw(); }
static void loading_begin(void)
{
    vglWaitVblankStart(GL_FALSE);
    load_t0 = now_us(); loading_draw();
    gfx_load_tick = loading_tick;
}
static void loading_end(void)
{
    loading_draw();
    gfx_load_tick = NULL;
    vglWaitVblankStart(GL_TRUE);
}

static int load_race_level(const char *xml)
{
    if (objs_on) { obj_sounds_off(); nr_objs_free(&OBJS); objs_on = 0; }
    gfx_free_level(&L);
    nr_coll_free(&COLL);
    char p[96]; snprintf(p, sizeof p, "levels/%s.nrl", xml);
    uint64_t t = now_us();
    logm("loading level %s", xml);
    int ok = gfx_load_nrl(&L, &pak, p, xml);
    if (gfx_load_tick) gfx_load_tick();
    snprintf(p, sizeof p, "levels/%s.nrc", xml);
    uint32_t sz; uint8_t *cb = nr_pak_read(&pak, p, &sz);
    if (!cb || nr_coll_parse(cb, sz, &COLL)) logm("  no collision for this level");
    free(cb);
    read_nrg(xml);
    gfx_set_lights(&L, LIGHTS, NLIGHTS);
    {   /* the level's objects that move and react */
        snprintf(p, sizeof p, "levels/%s.nrg", xml);
        char *gt = nr_pak_read(&pak, p, &sz);
        nr_objs_host H = { NULL, om_model, om_read, om_sound, om_rtpc, om_event };
        if (gt) { nr_objs_load(&OBJS, gt, &COLL, &H); objs_on = 1; free(gt); }
        logm("  %d level objects, %d object models", OBJS.n, NOMODEL);
#ifdef NR_PC
        haz_list();
#endif
        load_emitters();
    }
    gfx_prewarm();
    logm("  \"%s\": %d checkpoints, %d finish points, %d sound emitters, %d boost rings, %d coolers, %d lights, loaded in %.2f s",
         G.name, G.ncp, G.nfinish, G.nsnd, G.nring, G.ncool, NLIGHTS, (now_us() - t) / 1e6);
    return ok;
}

/* ------------------------------------------------------------------ race */
enum { MODE_FRONT, MODE_RACE, MODE_OUTRO, MODE_INTRO };
enum { RS_COUNT, RS_RACE, RS_PAUSE, RS_DEAD, RS_FINISH };
#define HEAT_MAX 100.0f

static struct {
    int set, level; fe_level *lv; int story;
    int rs, rs_before_pause; float t, race_t, dead_t, done_time;
    nr_car car; nr_ai ai; float bad_time, phys_acc;
    float heat, heat_cool_delay; int warned, alarm, boosting, wings, jets, skid, scrape;
    float rpm, rev, air_rev;
    int cp_hit[64], cp_count, last_cp; float cp_ly[64];
    int wrecks, new_best, unlocked_next;
    int pause_sel, result_sel;
    int cam_mode; v3 eye, cam_fwd; float shake; v3 view_fwd, view_up; float wings_cool;
    v3 cam_up, cam_base; float fov, blur, shake_amp, shake_t;   /* the chase camera, speed blur, camera shake */
    const char *layer;
    v3 boom; float boom_t;
    float msg_t; char msg[64];
    float upside_t, wall_t;
    int frame, credits;
    /* tricks: rotation gathered in the air, scored on landing */
    nr_tricks tricks; int score, tricks_level; char trick[256], trick_head[32]; float trick_t;
    nr_objs_hit hit;                           /* this frame's contacts, judged as CarLogic does */
    /* where the car comes back (CarLogic +0x1a4/+0x1b0): the start, the last checkpoint, or the
     * last TriggerRespawnLocation the car went through */
    int resp_kind; float resp_pos[3], resp_R[3][3];
    float cool_rate; int car_cooler; float cool_t;   /* CooldownPadLogic / CarCooler this frame; the cooling glow */
    int finish_obj, nearly_fell;               /* a finish line crossed; Down, Not Out */
    float play_acc;
    /* stunt mode (StuntLevel, re_I): a 5-minute clock, the score is the tricks' points */
    int stunt, stunt_over, stunt_voice, biggest; float stunt_left, settle_t, coast_t;
    /* the replay recording (ReplayRecorder ids of the car and the wreck pieces) and the end of a run:
     * fin_stage 0 the finish (1.7 s), 1 name entry, 2 results (re_N 11) */
    int rec_car, rec_gib[16], rec_wings;
    int fin_stage, fin_rank, name_col;
    /* the jets (CarLogic 0x703400): out in roll mode, each firing for its side of the trick stick (bits: 1 back,
     * 2 front, 4 left, 8 right, the order of Car.xml's Jets); the time since the car (re)appeared (CarLogic +0x60) */
    int roll_mode, jet_thrust; float alive_t;
    /* the countdown's clock (BaseLevel +0x48: -6 at its start, GO at 0, running 1.3 times as fast as real time) and
     * whether its voice has come (at -3); the warp-in / warp-out's portal (world), from a tunnel or not */
    float cd; int cd_voice; v3 warp_portal; int warp_tunnel;
} R;
/* TutorialInfoDisplay (re_F2 2.4): the tutorial's text box at the top of the screen */
static struct { char text[320]; float dur, el, size, alpha; int active, shown; } TUT;
static char *credits_text;

static void om_event(void *u, const char *what, int arg)
{
    (void)u;
    if (!strcmp(what, "achievement")) {         /* TriggerAchievement: the PC's achievement ids */
        static const struct { int id; const char *name; } A[] = { { 24, "I Know Car Fu" }, { 40, "Top of the CORE" }, { 45, "Homage" }, { 46, "Great Scott!" }, { 47, "Outta Time" } };
        for (unsigned k = 0; k < sizeof A / sizeof A[0]; k++) if (A[k].id == arg) ach_unlock(A[k].name);
        logm("  achievement %d from the level", arg);
    }
    else if (!strcmp(what, "EndStory")) R.finish_obj = 1;          /* TriggerEndStory: LevelFinishedMessage(success) */
    else if (!strcmp(what, "streetlight") || !strcmp(what, "screen")) {   /* StreetlightBroken 0x6bd608 / ScreenBroken 0x6fbb30 */
        SAVE.smashed++;
        if (!strcmp(what, "screen")) {
            SAVE.screens++; FE_LEVEL_SCREENS = arg;
            if (arg >= 5) ach_unlock("Smash TV");
            if (arg > SAVE.most_screens) SAVE.most_screens = arg;
        } else SAVE.streetlights++;
        if (SAVE.streetlights + SAVE.screens >= 20) ach_unlock("Rampage");
    }
}

#define PATH ((const float (*)[3])L.d.path)

static void layer_event(const char *what)   /* Set_Volume_<this level's track>_<what> */
{
    if (!R.layer) return;
    char ev[64]; snprintf(ev, sizeof ev, "Set_Volume_%s_%s", R.layer, what);
    snd_post(ev, 0);
}

/* ------------------------------------------------------------------ tutorial texts */
/* TriggerTutorialText's Sound: an index in the game's sorted table of Wwise events (0x792f38) */
static const char *tutorial_voice(int i)
{
    /* the PC's pad lines (its keyboard ones, 225/226/228, are in the keyboard branch of the tutorial). They name the
     * 360 pad's buttons, so a line is said only while its action is on that button's place on the Vita (R = RT, L = LT,
     * Cross = A, Square = X, Circle = B: ACT_PADPOS) and the sticks are not swapped */
    static const struct { int i; const char *ev; int act; } T[] = {
        { 0, "Play_Ain_tNothin_ButChickenWing", -1 }, { 5, "Play_Bangin_", -1 }, { 221, "Play_TutorialDisplayDown", -1 },
        { 222, "Play_TutorialDisplayUp", -1 }, { 223, "Play_HoldRTForGas", ACT_GAS }, { 224, "Play_HoldLTForBrake", ACT_BRAKE },
        { 227, "Play_UseLSForTurning", -2 }, { 229, "Play_UsLSAndRSForTightTurning", -2 }, { 231, "Play_UseLBAndAToBoost", ACT_BOOST },
        { 235, "Play_PressXToHop", ACT_JUMP }, { 237, "Play_PressBToFly", ACT_WINGS } };
    for (unsigned k = 0; k < sizeof T / sizeof T[0]; k++) if (T[k].i == i) {
        if (T[k].act >= 0 && SAVE.bind[T[k].act] != ACT_PADPOS[T[k].act]) return NULL;
        if (T[k].act == -2 && SAVE.swap_sticks) return NULL;
        return T[k].ev;
    }
    return NULL;
}
/* the text with the Vita's buttons (as set in Controls, drawn as icons) for the 360 pad's glyphs,
 * '|' between lines. The PC's pad has two buttons for boost (A, LB) and jump (X, RB); here each
 * action has one. */
static void tutorial_text(const char *in, char *out, int n)
{
    enum { STEER = 100, TRICK };
    static const struct { const char *a; int act; } S[] = {
        { "%A or %LB", ACT_BOOST }, { "%A or %RB", ACT_BOOST }, { "%RB or %X", ACT_JUMP },
        { "%LT", ACT_BRAKE }, { "%RT", ACT_GAS }, { "%LB", ACT_BOOST }, { "%RB", ACT_JUMP },
        { "%LS", STEER }, { "%RS", TRICK }, { "%A", ACT_BOOST }, { "%B", ACT_WINGS },
        { "%X", ACT_JUMP }, { "%Y", ACT_VIEW }, { "%s", ACT_PAUSE }, { "%b", ACT_RESET } };
    int o = 0;
    for (const char *p = in; *p && o < n - 1;) {
        unsigned k;
        if (!strncmp(p, "<br>", 4)) { out[o++] = '|'; p += 4; continue; }
        for (k = 0; k < sizeof S / sizeof S[0]; k++) if (!strncmp(p, S[k].a, strlen(S[k].a))) break;
        if (k < sizeof S / sizeof S[0]) {
            int a = S[k].act;
            if (SAVE.adv_controls && !strcmp(S[k].a, "%LB")) a = ACT_TRICK;        /* the Advanced scheme: LB rolls, RB boosts */
            else if (SAVE.adv_controls && !strcmp(S[k].a, "%RB")) a = ACT_BOOST;
            out[o++] = a == STEER ? (SAVE.swap_sticks ? HUD_ICON_RSTICK : HUD_ICON_LSTICK)
                     : a == TRICK ? (SAVE.swap_sticks ? HUD_ICON_LSTICK : HUD_ICON_RSTICK) : btn_icon(SAVE.bind[a]);
            p += strlen(S[k].a);
        } else out[o++] = *p == '\n' ? '|' : *p, p++;
    }
    out[o] = 0;
    /* no spaces around the line breaks */
    char *w = out;
    for (char *r = out; *r; r++) {
        if (*r == ' ' && (r[1] == '|' || (w > out && w[-1] == '|'))) continue;
        *w++ = *r;
    }
    *w = 0;
}
static void tutorial_show(const char *line)
{
    char raw[320];
    if (!nr_objs_value(line, "TriggerTutorialText.Text", raw, sizeof raw, 1)) return;
    if (kvf_line(line, "TriggerTutorialText.Hides_Tutorial") != 0) { TUT.active = 0; return; }
    tutorial_text(raw, TUT.text, sizeof TUT.text);
    TUT.dur = kvf_line(line, "TriggerTutorialText.Duration"); if (TUT.dur <= 0) TUT.dur = 5;
    TUT.el = 0; TUT.active = 1;
    const char *v = tutorial_voice((int)kvf_line(line, "TriggerTutorialText.Sound"));
    if (v) ann_queue(v);                        /* the announcer's queue */
    logm("  tutorial: %s", TUT.text);
}
static void tutorial_update(float dt)
{
    if (TUT.active) { TUT.el += dt; if (TUT.el > TUT.dur) TUT.active = 0; }
    float ts = TUT.active ? 1.0f : 0.0f;
    if (TUT.active && !TUT.shown) { snd_post("Play_TutorialDisplayUp", 0); TUT.shown = 1; }
    if (!TUT.active && TUT.shown) { snd_post("Play_TutorialDisplayDown", 0); TUT.shown = 0; }
    float k = 1.0f - powf(0.875f, dt * 60.0f);  /* 0.125 a frame on the PC */
    TUT.size += (ts - TUT.size) * k; TUT.alpha += (ts - TUT.alpha) * k;
}
static void tutorial_draw(void)
{
    if (TUT.alpha < 0.01f || !TUT.text[0]) return;
    float a = (0.35f + 0.65f * (0.5f + 0.5f * sinf(R.t * 6.0f))) * TUT.alpha;   /* pulses */
    int lines = 1; for (const char *p = TUT.text; *p; p++) if (*p == '|') lines++;
    float sz = 30.0f * TUT.size, y = 48 - (lines - 1) * sz * 1.15f * 0.5f - sz * 0.5f;
    hud_text_lines(HUD_FONT_MAIN, 480, y, sz, 1.15f, 0xFFFFFF00 | (unsigned)(fminf(1, a) * 255), HUD_CENTER, TUT.text);
}

static void car_sounds_off(void)
{
    if (R.boosting) { snd_post("Stop_Boost", OBJ_CAR); layer_event("Boost_Off"); }
    if (R.jet_thrust) snd_post("Stop_CarJet", OBJ_CAR);
    if (R.skid) snd_post("Stop_Rand_TireSkid", OBJ_CAR);
    if (R.scrape) snd_post("Stop_Rand_Scrape", OBJ_CAR);
    if (R.wings) layer_event("Wings_Off");
    if (R.alarm) layer_event("Alarm_Off");
    R.boosting = R.jets = R.skid = R.scrape = R.wings = R.alarm = 0;
    R.jet_thrust = R.roll_mode = 0;
}
/* leaving a level (BaseLevel exit 0x712400 and the car's end 0x704760): the crash states back to Normal, the music's
 * jump layers off, the announcer's queue emptied (its lines are the level's) */
static void level_sounds_exit(void)
{
    snd_set_state("Crash_States", "Normal"); snd_set_state("Announcer_Crash", "Normal");
    snd_post("Stop_All_Jump", 0);
    snd_stop_obj(OBJ_SPARK, 100);                      /* (the Commander's sparks end with the car object) */
    ann_clear();
}

static float part_t[3];                       /* (defined with the vehicle's parts below) */
static const float part_tmax[3];
/* the chase camera (camera.c) */
static nr_cam CAM;
static void cam_reset(void) { cam_reset_to(&CAM, &R.car, &COLL); R.eye = CAM.pos; R.blur = 0; }
static void cam_switch_view(void) { cam_next_view(&CAM); }

static void respawn_at(const float *pt)
{
    uint32_t ri = nr_route_nearest(PATH, L.d.npath, pt, 0, L.d.npath);
    nr_car_place_on_route(&R.car, &R.ai, PATH, L.pathlen, L.d.npath, (int)ri);
    R.heat = 0; R.warned = 0; nr_tricks_reset(&R.tricks);
    cam_reset(); CAM.fade = 0;
}

/* CarLogic's respawn (0x702380): a ray 25 units down the respawn point's up axis, the car on what
 * it hits, turned as the point is, standing still */
static void respawn_oriented(const float *p, const float Rm[3][3])
{
    float up[3] = { Rm[0][2], Rm[1][2], Rm[2][2] }, down[3] = { -up[0], -up[1], -up[2] }, at[3] = { p[0], p[1], p[2] };
    float dist, n[3]; uint32_t mat;
    uint32_t saved = COLL.ndyn; COLL.ndyn = 0;
    if (COLL.ntri && nr_coll_ray(&COLL, p, down, 25.0f, &dist, n, &mat)) for (int k = 0; k < 3; k++) at[k] = p[k] + down[k] * dist + up[k] * 1.0f;
    COLL.ndyn = saved;
    nr_car_reset_rot(&R.car, at, Rm);
    R.ai.idx = nr_route_nearest(PATH, L.d.npath, at, 0, L.d.npath);
    R.heat = 0; R.warned = 0; nr_tricks_reset(&R.tricks);
    cam_reset(); CAM.fade = 0;
}
/* the level's start (models/starts.txt): the StartTunnelLogic object's transform, where the PC puts the car
 * (the shipped replays start there to a few centimetres), and its scale; the end tunnel's (the first object named
 * EndTunnel), where the warp-out's portal is */
typedef struct { char lv[56]; float p[3], R[3][3], s[3]; } tunnel_tf;
static tunnel_tf STARTS[128], ENDS[128], PORTALS[256]; static int nstarts, nends, nportals;   /* PORTALS: tunnel2 / EndTunnel2's Portal child */
/* "<kw> <level name, maybe with spaces> x y z r00 .. r22 [sx sy sz]" */
static int parse_tunnel(char *ln, const char *kw, tunnel_tf *o)
{
    size_t kl = strlen(kw);
    if (strncmp(ln, kw, kl) || ln[kl] != ' ') return 0;
    char *s0 = ln + kl + 1, *q = s0 + strlen(s0);
    while (q > s0 && (q[-1] == ' ' || q[-1] == '\r')) q--;
    float v[15]; int n = 0;
    while (n < 15) {                               /* numbers from the end */
        char *b = q; while (b > s0 && b[-1] != ' ') b--;
        if (b == s0) break;                         /* (the name stays) */
        char tok[32]; size_t tl = (size_t)(q - b); if (tl == 0 || tl >= sizeof tok) break;
        memcpy(tok, b, tl); tok[tl] = 0;
        char *e; float x = strtof(tok, &e); if (*e) break;
        v[n++] = x; q = b; while (q > s0 && q[-1] == ' ') q--;
    }
    if (n < 12) return 0;
    if (n > 12 && n < 15) n = 12;                  /* (a name ending in numbers: they stay in it) */
    float f[15]; for (int i = 0; i < n; i++) f[i] = v[n - 1 - i];
    /* the name: from s0 up to the first of the n numbers */
    char *nm_end = ln + strlen(ln); { int k = 0; char *r = nm_end; while (r > s0 && (r[-1] == ' ' || r[-1] == '\r')) r--;
        while (k < n && r > s0) { while (r > s0 && r[-1] != ' ') r--; k++; while (r > s0 && r[-1] == ' ') r--; } nm_end = r; }
    size_t nl = (size_t)(nm_end - s0); if (nl >= sizeof o->lv) nl = sizeof o->lv - 1;
    memcpy(o->lv, s0, nl); o->lv[nl] = 0;
    memcpy(o->p, f, 12); memcpy(o->R, f + 3, 36);
    for (int k = 0; k < 3; k++) o->s[k] = n == 15 ? f[12 + k] : 1.0f;
    return 1;
}
static void load_starts(void)
{
    uint32_t sz; char *t = nr_pak_read(&pak, "models/starts.txt", &sz);
    for (char *ln = t ? strtok(t, "\n") : NULL; ln; ln = strtok(NULL, "\n")) {
        if (nstarts < 128 && parse_tunnel(ln, "start", &STARTS[nstarts])) nstarts++;
        else if (nends < 128 && parse_tunnel(ln, "end", &ENDS[nends])) nends++;
        else if (nportals < 256 && parse_tunnel(ln, "portal", &PORTALS[nportals])) nportals++;
    }
    free(t);
    logm("level starts %d, end tunnels %d, tunnel portals %d", nstarts, nends, nportals);
}
static const tunnel_tf *find_tunnel(const tunnel_tf *T, int n)
{
    for (int i = 0; R.lv && i < n; i++) if (!strcmp(T[i].lv, R.lv->xml)) return &T[i];
    return NULL;
}
static void place_at_start(void)
{
    for (int i = 0; R.lv && i < nstarts; i++) if (!strcmp(STARTS[i].lv, R.lv->xml)) {
        const float (*Rm)[3] = STARTS[i].R;
        float at[3] = { STARTS[i].p[0] + Rm[0][2], STARTS[i].p[1] + Rm[1][2], STARTS[i].p[2] + Rm[2][2] };   /* (1 up: it settles) */
        nr_car_reset_rot(&R.car, at, STARTS[i].R);
        R.ai.idx = nr_route_nearest(PATH, L.d.npath, at, 0, L.d.npath);
        R.heat = 0; R.warned = 0; nr_tricks_reset(&R.tricks);
        cam_reset(); CAM.fade = 0;
        return;
    }
    if (G.has_start) respawn_at(G.start); else nr_car_place_on_route(&R.car, &R.ai, PATH, L.pathlen, L.d.npath, 0);
}
static void respawn_point(void)
{
    if (R.resp_kind == 2) respawn_oriented(R.resp_pos, R.resp_R);
    else if (R.last_cp >= 0 && G.cp_has_R[R.last_cp]) respawn_oriented(G.cp[R.last_cp], G.cpR[R.last_cp]);   /* the gate's transform, ray down */
    else if (R.last_cp >= 0) respawn_at(G.cp[R.last_cp]);
    else place_at_start();
    R.nearly_fell = 0;
    if (objs_on) nr_objs_checkpoint_reset(&OBJS);      /* PlayerResetAtCheckpointMessage */
}

/* ---- ghost cars (re_N): the run's replay is recorded in every mode; in Arcade the Local table's
 * replays race along (LevelEnter 0x7155b0, LoadLevelGhosts 0x711dd0) */
static int ghosts_ok, ghosts_dirty = 1, g_autoplay, pilot_on, pilot_had_car, pilot_coast;
/* the test drive with the real physics along a recorded run (switch file "chase"; see chase.h) */
static ch_state CH; static int chase_on, chase_wanted;
static int nodraw;                                  /* (the PC test machine, NODRAW: the race not drawn - fast physics runs) */
static void frc_stop(void);
static void frc_off(void);
static const char *veh_ghost_name(void)       /* the vehicle's ReplayRecorder "Replay Object Name" */
{
    const char *k = SAVE.vehicle >= 0 && SAVE.vehicle < FE_NVEH ? FE_VEH[SAVE.vehicle].key : "";
    if (!strcmp(k, "goldenceptor")) return "CarGhost_Gold";
    if (!strcmp(k, "avenger") || !strcmp(k, "knightcycle")) return "VanGhost";
    if (!strcmp(k, "commander")) return "DeloreanGhost";
    return "CarGhost";
}
/* autoplay drives the car along the level's shipped replay (the developers' best run; Developer/ for the
 * levels without one), so the unattended test drive looks like a real run; the computer driver takes the
 * levels with none */
static float pilot_s;                           /* pilot_on 2: the distance along the level's route */
static int credits_stress;                      /* autoplay: the credits driven by the computer with boost, hops and wings */
#ifdef NR_PC
/* (the PC test machine, AUDIT=1: the shipped replay's car checked against the port's collision as it drives -
 * the body inside walls or objects the PC car went through, no ground under its wheels where the PC car was
 * driving; each stretch logged as an AUDIT line) */
static int audit_on; static uint8_t *audit_air; static int audit_n;
static struct { int on; float t0, t1, worst, p[3], n[3]; uint32_t mat; int kind; float probe[6]; } AUD[2];
static void audit_load(const void *d, uint32_t sz)
{
    free(audit_air); audit_air = NULL; audit_n = 0;
    if (!audit_on || !d) return;
    gh_trackpt *t; int n = gh_track(d, sz, &t);
    if (n <= 2) { free(t); return; }
    audit_air = calloc(n, 1); audit_n = n;
    for (int i = 1; i + 1 < n; i++) {
        if (!t[i - 1].alive || !t[i].alive || !t[i + 1].alive || t[i].wings) { audit_air[i] = 1; continue; }
        /* in the air when it moves as a free body does: gravity (25) and the drag (1 + 0.02 v + 0.005 v^2, BodyRigid) */
        float v[3], a[3], sp;
        for (int k = 0; k < 3; k++) { v[k] = (t[i + 1].p[k] - t[i - 1].p[k]) / 0.12f; a[k] = (t[i + 1].p[k] - 2 * t[i].p[k] + t[i - 1].p[k]) / (0.06f * 0.06f); }
        sp = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        float dr = sp > 1e-3f ? (carp.drag[0] + carp.drag[1] * sp + carp.drag[2] * sp * sp) / sp : 0, e2 = 0;
        for (int k = 0; k < 3; k++) { float af = -dr * v[k] - (k == 2 ? carp.gravity : 0); e2 += (a[k] - af) * (a[k] - af); }
        audit_air[i] = e2 < 14 * 14;
    }
    /* (a tick between two in the air is in the air; a lone grounded tick in the air too) */
    for (int i = 1; i + 1 < n; i++) if (audit_air[i - 1] && audit_air[i + 1]) audit_air[i] = 1;
    free(t);
}
/* (the PC test machine, HAZLOG=1: the saws and other moving hazards - which the level has, and every time the car's
 * hull touches one: how deep, how fast, whether the car is the replay's or driven) */
static int haz_on;
static struct { int on; float t0, t1, worst, mind, spd; } HZ[1024];
static const char *haz_bp(const nr_obj *o, char *b, int n)
{
    const char *q = o->line ? strstr(o->line, "bp=") : NULL; int k = 0;
    if (q) for (q += 3; *q && *q != ' ' && k < n - 1; q++) b[k++] = *q;
    b[k] = 0; return b;
}
static void haz_list(void)
{
    if (!haz_on) return;
    for (int i = 0; i < OBJS.n; i++) {
        nr_obj *o = &OBJS.o[i]; char b[48]; haz_bp(o, b, sizeof b);
        if (o->kind != OK_SAW && o->kind != OK_BAFSAW && !strstr(b, "saw") && !strstr(b, "Saw")) continue;
        logm("HAZOBJ %s %d kind %d bp %s model %d cmodel %d (%s, r %.1f) cmat %d mid %d ds %.0f at %.1f %.1f %.1f scale %.2f %.2f %.2f angvel %.2f %.2f %.2f",
             R.lv ? R.lv->xml : "?", i, o->kind, b, o->model, o->cmodel, o->cmodel >= 0 ? OBJS.cm[o->cmodel].name : "-", o->cmodel >= 0 ? OBJS.cm[o->cmodel].r : 0,
             o->cmat, o->matid, o->death_speed, o->pos[0], o->pos[1], o->pos[2], o->scale[0], o->scale[1], o->scale[2], o->angvel[0], o->angvel[1], o->angvel[2]);
        if (o->kind == OK_BAFSAW) logm("HAZBAF %s %d extents %.2f mult %.3f rot %.1f axis %d base %.2f %.2f %.2f dir %.3f %.3f %.3f t %.3f box %.2f %.2f %.2f / %.2f %.2f %.2f",
             R.lv->xml, i, o->f[1], o->f[2], o->f[0], o->s[0], o->f[3], o->f[4], o->f[5], o->f[6], o->f[7], o->f[8], o->t, o->bmin[0], o->bmin[1], o->bmin[2], o->bmax[0], o->bmax[1], o->bmax[2]);
    }
    memset(HZ, 0, sizeof HZ);
}
static float dot3f(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float tri_dist2(const float p[3], const float a[3], const float b[3], const float c[3])
{   /* (Ericson, closest point on a triangle) */
    float ab[3], ac[3], ap[3], q[3];
    for (int k = 0; k < 3; k++) { ab[k] = b[k] - a[k]; ac[k] = c[k] - a[k]; ap[k] = p[k] - a[k]; }
    float d1 = dot3f(ab, ap), d2 = dot3f(ac, ap);
    if (d1 <= 0 && d2 <= 0) { memcpy(q, a, 12); goto done; }
    float bp_[3] = { p[0] - b[0], p[1] - b[1], p[2] - b[2] }, d3 = dot3f(ab, bp_), d4 = dot3f(ac, bp_);
    if (d3 >= 0 && d4 <= d3) { memcpy(q, b, 12); goto done; }
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) { float v = d1 / (d1 - d3); for (int k = 0; k < 3; k++) q[k] = a[k] + v * ab[k]; goto done; }
    float cp_[3] = { p[0] - c[0], p[1] - c[1], p[2] - c[2] }, d5 = dot3f(ab, cp_), d6 = dot3f(ac, cp_);
    if (d6 >= 0 && d5 <= d6) { memcpy(q, c, 12); goto done; }
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) { float w = d2 / (d2 - d6); for (int k = 0; k < 3; k++) q[k] = a[k] + w * ac[k]; goto done; }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) { float w = (d4 - d3) / ((d4 - d3) + (d5 - d6)); for (int k = 0; k < 3; k++) q[k] = b[k] + w * (c[k] - b[k]); goto done; }
    { float den = 1.0f / (va + vb + vc), v = vb * den, w = vc * den; for (int k = 0; k < 3; k++) q[k] = a[k] + ab[k] * v + ac[k] * w; }
done:
    { float d[3] = { p[0] - q[0], p[1] - q[1], p[2] - q[2] }; return dot3f(d, d); }
}
static void haz_flush(int i)
{
    if (!HZ[i].on) return;
    HZ[i].on = 0;
    char b[48]; haz_bp(&OBJS.o[i], b, sizeof b);
    if (HZ[i].worst > 0)
        logm("HAZHIT %s obj %d %s %.2f-%.2f s, %.2f deep, car speed %.0f, %s", R.lv->xml, i, b, HZ[i].t0, HZ[i].t1, HZ[i].worst, HZ[i].spd, pilot_on ? "replay (no deaths)" : "driven");
    else
        logm("HAZNEAR %s obj %d %s %.2f-%.2f s, closest gap %.2f, car speed %.0f, %s", R.lv->xml, i, b, HZ[i].t0, HZ[i].t1, HZ[i].mind, HZ[i].spd, pilot_on ? "replay" : "driven");
}
static void haz_frame(void)
{
    if (getenv("HAZCSV")) {
        static FILE *hf; if (!hf) hf = fopen(getenv("HAZCSV"), "w");
        if (hf) {
            float st = -1; for (int i = 0; i < OBJS.n; i++) if (OBJS.o[i].kind == OK_BAFSAW) { st = OBJS.o[i].t; break; }
            fprintf(hf, "%s,%.4f,%.3f,%.3f,%.3f,%.4f,%d\n", R.lv->xml, R.race_t, R.car.pos[0], R.car.pos[1], R.car.pos[2], st, pilot_on);
        }
    }
    float pen[1024]; uint8_t seen[1024]; memset(seen, 0, sizeof seen);
    for (uint32_t t = 0; t < COLL.ndyn; t++) {
        const nr_ctri *T = &COLL.dyn[t]; uint32_t oi = NR_TRI_OBJ(T->mat);
        if (oi >= 1024 || (int)oi >= OBJS.n) continue;
        nr_obj *o = &OBJS.o[oi];
        if (o->kind == OK_BARREL || o->kind == OK_DEBRIS || o->dynamic) continue;
        if (!seen[oi]) { seen[oi] = 1; pen[oi] = -1e9f; }
        for (int h = 0; h < carp.nhull; h++) {
            float c[3];
            for (int i = 0; i < 3; i++) c[i] = R.car.pos[i] + R.car.rot[i][0] * carp.hull[h][0] + R.car.rot[i][1] * carp.hull[h][1] + R.car.rot[i][2] * carp.hull[h][2];
            float d = sqrtf(tri_dist2(c, T->v[0], T->v[1], T->v[2])), p = carp.hull[h][3] - d;
            if (p > pen[oi]) pen[oi] = p;
        }
    }
    float spd = sqrtf(R.car.vel[0] * R.car.vel[0] + R.car.vel[1] * R.car.vel[1] + R.car.vel[2] * R.car.vel[2]);
    for (int i = 0; i < OBJS.n && i < 1024; i++) {
        if (seen[i] && pen[i] > -1.5f) {
            if (!HZ[i].on) { HZ[i].on = 1; HZ[i].t0 = R.race_t; HZ[i].worst = 0; HZ[i].mind = 1e9f; HZ[i].spd = spd; }
            HZ[i].t1 = R.race_t;
            if (pen[i] > HZ[i].worst) { HZ[i].worst = pen[i]; HZ[i].spd = spd; }
            if (-pen[i] < HZ[i].mind) HZ[i].mind = -pen[i];
        } else if (HZ[i].on && R.race_t - HZ[i].t1 > 0.3f) haz_flush(i);
    }
}
static void audit_flush(int k)
{
    if (!AUD[k].on) return;
    AUD[k].on = 0;
    if (AUD[k].kind == 0)
        logm("AUDIT %s inside geometry %.1f-%.1f s, %.2f deep at %.1f %.1f %.1f, normal %.2f %.2f %.2f, %s triangle, material %u, object %u",
             R.lv->xml, AUD[k].t0, AUD[k].t1, AUD[k].worst, AUD[k].p[0], AUD[k].p[1], AUD[k].p[2], AUD[k].n[0], AUD[k].n[1], AUD[k].n[2],
             (AUD[k].mat & NR_TRI_DYN) ? "moving" : "static", NR_TRI_MATID(AUD[k].mat), NR_TRI_OBJ(AUD[k].mat));
    else
        logm("AUDIT %s no ground %.1f-%.1f s at %.1f %.1f %.1f (nearest surface: below %.1f above %.1f right %.1f left %.1f ahead %.1f behind %.1f)",
             R.lv->xml, AUD[k].t0, AUD[k].t1, AUD[k].p[0], AUD[k].p[1], AUD[k].p[2], AUD[k].probe[0], AUD[k].probe[1], AUD[k].probe[2], AUD[k].probe[3], AUD[k].probe[4], AUD[k].probe[5]);
}
static void audit_frame(void)
{
    int tick = (int)(R.race_t / 0.06f);
    int air = audit_air && tick >= 0 && tick < audit_n ? audit_air[tick] : 1;
    /* the body: deepest wall-like overlap of the hull's spheres */
    float best = 0, bp[3] = { 0 }, bn[3] = { 0 }; uint32_t bm = 0;
    float up[3] = { R.car.rot[0][2], R.car.rot[1][2], R.car.rot[2][2] };
    for (int h = 0; h < carp.nhull; h++) {
        float c[3];
        for (int i = 0; i < 3; i++) c[i] = R.car.pos[i] + R.car.rot[i][0] * carp.hull[h][0] + R.car.rot[i][1] * carp.hull[h][1] + R.car.rot[i][2] * carp.hull[h][2];
        float push[3], n[3];
        if (!nr_coll_sphere(&COLL, c, carp.hull[h][3], push, n)) continue;
        float dpt = sqrtf(push[0] * push[0] + push[1] * push[1] + push[2] * push[2]);
        float upn = n[0] * up[0] + n[1] * up[1] + n[2] * up[2];
        if (upn > 0.7f && dpt < 0.8f) continue;          /* (the floor under the car: landings, bumps) */
        if (dpt > best) { best = dpt; memcpy(bp, c, 12); memcpy(bn, n, 12); bm = nr_coll_last_mat; }
    }
    if (best > 0.35f) {
        if (!AUD[0].on) { AUD[0].on = 1; AUD[0].t0 = R.race_t; AUD[0].worst = 0; AUD[0].kind = 0; }
        AUD[0].t1 = R.race_t;
        if (best > AUD[0].worst) { AUD[0].worst = best; memcpy(AUD[0].p, bp, 12); memcpy(AUD[0].n, bn, 12); AUD[0].mat = bm; }
    } else if (AUD[0].on && R.race_t - AUD[0].t1 > 0.3f) audit_flush(0);
    /* the wheels: ground within reach of the springs where the replay was driving */
    int hits = 0;
    if (!air) for (int w = 0; w < carp.nwheels; w++) {
        float o[3], d3[3] = { -up[0], -up[1], -up[2] }, t, n[3];
        for (int i = 0; i < 3; i++) o[i] = R.car.pos[i] + R.car.rot[i][0] * carp.wheel_pos[w][0] + R.car.rot[i][1] * carp.wheel_pos[w][1] + R.car.rot[i][2] * (carp.wheel_pos[w][2] + 0.3f);
        if (nr_coll_ray(&COLL, o, d3, carp.spring_rest + carp.radius + 1.0f, &t, n, NULL)) hits++;
    }
    if (!air && hits == 0) {
        if (!AUD[1].on) {
            AUD[1].on = 1; AUD[1].t0 = R.race_t; AUD[1].kind = 1; memcpy(AUD[1].p, R.car.pos, 12);
            for (int k = 0; k < 6; k++) {                 /* the surfaces around the car: in its frame, -up first */
                int ax = k < 2 ? 2 : k < 4 ? 0 : 1; float sg = (k & 1) ? 1.0f : -1.0f;
                if (k >= 2) sg = -sg;
                float d3[3] = { R.car.rot[0][ax] * sg, R.car.rot[1][ax] * sg, R.car.rot[2][ax] * sg }, t, n[3];
                AUD[1].probe[k] = nr_coll_ray(&COLL, R.car.pos, d3, 30, &t, n, NULL) ? t : 99;
            }
        }
        AUD[1].t1 = R.race_t;
    } else if (AUD[1].on && R.race_t - AUD[1].t1 > 0.3f) { if (AUD[1].t1 - AUD[1].t0 >= 0.1f) audit_flush(1); else AUD[1].on = 0; }
}
#endif
static void pilot_load(void)
{
    pilot_on = 0;
    if (g_autoplay && R.credits && L.d.npath > 2 && !credits_stress) { pilot_on = 2; logm("  test drive: along the route"); return; }   /* (the credits: the player drives there) */
    if (!g_autoplay || !ghosts_ok || !R.lv || R.credits) return;
#ifdef NR_PC
    if (getenv("AUTOPLAY_IDLE")) return;              /* (the test machine: the script drives) */
#endif
    /* the developers' clean run in the standard car (dev_<level>: no crashes), else the shipped table's best */
    uint32_t sz = 0; void *d = NULL;
#ifdef NR_PC
    if (getenv("AUDIT_SRC") && !strncmp(getenv("AUDIT_SRC"), "default", 7)) d = sc_default_ghost(R.lv->xml, atoi(getenv("AUDIT_SRC") + 7), &sz);
    if (getenv("AUDIT_SRC") && !strncmp(getenv("AUDIT_SRC"), "default", 7) && !d) { logm("  test drive: no replay %s", getenv("AUDIT_SRC")); return; }
#endif
    if (!d) { char p[96]; snprintf(p, sizeof p, "ghosts/dev_%s.0.ngb", R.lv->xml); d = nr_pak_read(&pak, p, &sz); }
    if (!d) d = sc_default_ghost(R.lv->xml, 0, &sz);
#ifdef NR_PC
    audit_load(d, sz);
#endif
    if (chase_on) { ch_free(&CH); chase_on = 0; }
    if (d && chase_wanted && !R.stunt) {
        if (ch_init(&CH, d, sz) == 0) chase_on = 1;
        free(d);
#ifdef NR_PC
        if (chase_on && getenv("CHASE_DUMP")) {          /* (the PC test machine: the cleaned line) */
            FILE *f = fopen(getenv("CHASE_DUMP"), "w");
            for (int i = 0; f && i < CH.n; i++) fprintf(f, "%d,%.2f,%.1f,%.1f,%.1f,%d,%d\n", CH.tick[i], CH.tick[i] * 0.06f, CH.pt[i].p[0], CH.pt[i].p[1], CH.pt[i].p[2], CH.jump[i], CH.pt[i].wings);
            if (f) fclose(f);
        }
#endif
        logm("  test drive: %s", chase_on ? "steered along the shipped replay (real physics)" : "the computer driver");
        return;
    }
    if (!d && chase_wanted && !R.stunt && L.d.npath > 3 && ch_init_route(&CH, (const float (*)[3])L.d.path, (int)L.d.npath) == 0) {
        chase_on = 1; logm("  test drive: steered along the level's route (real physics)"); return;
    }
    if (d && gh_pilot_load(d, sz) >= 0) pilot_on = 1;
    logm("  test drive: %s", pilot_on ? "along the shipped replay" : "the computer driver");
}
static void ghosts_load_level(void)
{
    ghosts_dirty = 0;
    io_flush();                                     /* (a replay just saved is on the card) */
    gh_clear();
    pilot_load();
    if (!ghosts_ok || R.credits || R.story || !SAVE.ghosts || !R.lv || !R.lv->score) return;
    sc_table t; sc_get(SC_LOCAL, R.lv->xml, R.lv->score, &t);
    io_flush();                                     /* (the first time, the shipped replays were just copied) */
    for (int r = 0; r < 10; r++) {
        char p[256]; sc_ghost_path(p, sizeof p, R.lv->xml, r);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *d = n > 24 && n < (16 << 20) ? malloc((size_t)n) : NULL;
        if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
        fclose(f);
        if (d && pilot_on && gh_pilot_same(d, (uint32_t)n)) { free(d); continue; }   /* (autoplay: not the pilot's twin) */
        int idx = pilot_on ? r : gh_count();            /* the name by load order, as the PC (autoplay: its rank, the pilot's twin being left out) */
        if (gh_load(d, (uint32_t)n, idx < t.n ? t.e[idx].initials : "???") < 0) logm("  ghost %s: not a ghost file", p);
    }
    char st[256]; gh_stats(st, sizeof st);
    logm("  %s", st);
}
static void rec_reset_all(void)
{
    gh_rec_reset();
    R.rec_car = -1; R.rec_wings = 0;
    for (int i = 0; i < 16; i++) R.rec_gib[i] = -1;
}
static void rec_car_appears(void)
{
    if (!gh_rec_on()) return;
    if (R.rec_car >= 0) gh_rec_remove(R.rec_car);
    R.rec_car = gh_rec_add(veh_ghost_name()); R.rec_wings = 0;
    if (R.rec_car >= 0) gh_rec_pose(R.rec_car, R.car.rot, R.car.pos);
}

static void explode(const char *why, int cause);
static void pilot_route(float dt)                   /* the car glides along the route at 30 units a second */
{
    uint32_t n = L.d.npath;
    if (R.rs != RS_RACE || n < 2) return;
    pilot_s += 30 * dt;
    uint32_t i = 0;
    while (i + 2 < n && L.pathlen[i + 1] < pilot_s) i++;
    float seg = L.pathlen[i + 1] - L.pathlen[i], t = seg > 1e-4f ? (pilot_s - L.pathlen[i]) / seg : 0;
    if (t > 1) t = 1;
    v3 a = v3make(PATH[i][0], PATH[i][1], PATH[i][2]), b = v3make(PATH[i + 1][0], PATH[i + 1][1], PATH[i + 1][2]);
    v3 p = lerp3(a, b, t), f = norm(sub(b, a)), up = v3make(0, 0, 1);
    v3 r = norm(cross(f, up)); up = cross(r, f);
    v3 old = v3make(R.car.pos[0], R.car.pos[1], R.car.pos[2]);
    p = add(p, mul(up, 0.6f));
    R.car.pos[0] = p.x; R.car.pos[1] = p.y; R.car.pos[2] = p.z;
    for (int k = 0; k < 3; k++) { R.car.rot[k][0] = (&r.x)[k]; R.car.rot[k][1] = (&f.x)[k]; R.car.rot[k][2] = (&up.x)[k]; }
    v3 v = dt > 0 ? mul(sub(p, old), 1 / dt) : v3make(0, 0, 0);
    if (len(v) > 60) v = mul(f, 30);
    R.car.vel[0] = v.x; R.car.vel[1] = v.y; R.car.vel[2] = v.z;
    memset(R.car.angvel, 0, 12); R.car.grounded = 4; R.ai.idx = i;
}
static void pilot_drive(float dt)
{
    if (pilot_on == 2) { pilot_route(dt); return; }
    float W[4][4]; int wings = 0, alive = 0;
    if (!gh_pilot_car(W, &wings, &alive)) {                     /* the replay is over (it ends a tick before the PC car reached
                                                                 * the finish): the car's own physics and the computer driver
                                                                 * take it on from where it is, as it was going */
        if (R.rs == RS_RACE && pilot_on == 1) {
            pilot_on = 0; pilot_coast = 1; R.phys_acc = 0;
            {   float v[3], p[3], Rm[3][3]; memcpy(v, R.car.vel, 12); memcpy(p, R.car.pos, 12); memcpy(Rm, R.car.rot, 36);
                nr_car_reset_rot(&R.car, p, (const float (*)[3])Rm); memcpy(R.car.vel, v, 12); }    /* (the physics' own orientation too) */
            uint32_t best = 0; float bd = 1e30f;
            for (uint32_t i = 0; i < L.d.npath; i++) {
                float dx = PATH[i][0] - R.car.pos[0], dy = PATH[i][1] - R.car.pos[1], dz = PATH[i][2] - R.car.pos[2], d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < bd) { bd = d2; best = i; }
            }
            R.ai.idx = best;
            logm("  test drive: the replay is over at %.1f s; the car drives on by itself (at %.1f %.1f %.1f going %.1f %.1f %.1f, route point %u of %u)",
                 R.race_t, R.car.pos[0], R.car.pos[1], R.car.pos[2], R.car.vel[0], R.car.vel[1], R.car.vel[2], best, L.d.npath);
#ifdef NR_PC
            for (int i = 0; i < G.nfbox; i++) {
                const float *b_ = G.fbox[i]; float d3[3] = { R.car.pos[0] - b_[0], R.car.pos[1] - b_[1], R.car.pos[2] - b_[2] }, l[3];
                for (int k = 0; k < 3; k++) l[k] = (d3[0] * b_[3 + k] + d3[1] * b_[6 + k] + d3[2] * b_[9 + k]) / (fabsf(b_[12 + k]) > 1e-4f ? b_[12 + k] : 1);
                logm("    finish box %d at %.1f %.1f %.1f: the car at local %.1f %.1f %.1f (scale %.2f %.2f %.2f)", i, b_[0], b_[1], b_[2], l[0], l[1], l[2], b_[12], b_[13], b_[14]);
            }
#endif
        }
        return;
    }
    if (!alive) { if (R.rs == RS_RACE && pilot_had_car) { pilot_had_car = 0; explode("the replay's crash", 3); } return; }
    pilot_had_car = 1;
    int was_dead = R.rs == RS_DEAD;
    float np[3] = { W[0][3], W[1][3], W[2][3] }, d2 = 0;
    for (int k = 0; k < 3; k++) d2 += (np[k] - R.car.pos[k]) * (np[k] - R.car.pos[k]);
    for (int k = 0; k < 3; k++) R.car.vel[k] = dt > 0 && d2 < 30 * 30 && !was_dead ? (np[k] - R.car.pos[k]) / dt : 0;
    memcpy(R.car.pos, np, 12);
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) R.car.rot[r][c] = W[r][c];
    memset(R.car.angvel, 0, 12);
    R.car.grounded = wings ? 0 : 4;
    if (wings != R.wings) { R.wings = wings; snd_post(wings ? "Play_WingOut" : "Play_WingIn", OBJ_CAR); layer_event(wings ? "Wings_On" : "Wings_Off"); }
    if (was_dead) {                                            /* the replay's respawn */
        R.rs = RS_RACE; R.heat = 0;
        snd_post("Play_CarEngine", OBJ_CAR);
        snd_set_state("Crash_States", "Normal"); snd_set_state("Announcer_Crash", "Normal");
        rec_car_appears(); R.alive_t = 0;
        cam_reset(); CAM.fade = 0;
    }
}

/* The level's objects run through the countdown (4.6 s) but its first 0.12 s, so at GO they have run 4.5 s
 * (OBJS_AT_GO) as on the PC: the developers' replays (recorded at GO) pass every sliding saw in the story levels
 * cleanly only with the saws' clock at about 4.5 s at GO (Campaign 3, 4, 5: 13 saws, one window 4.4-4.6 s).
 * OBJS_HEAD_START: seconds run at once before the countdown (none now; OBJ_PREROLL on the PC test machine) */
static float OBJS_HEAD_START = 0.0f;
static void objs_head_start(void)
{
    float pre = OBJS_HEAD_START;
#ifdef NR_PC
    if (getenv("OBJ_PREROLL")) pre = (float)atof(getenv("OBJ_PREROLL"));
#endif
    if (pre <= 0) return;
    nr_objs_frame of; memset(&of, 0, sizeof of);
    memcpy(of.car_pos, R.car.pos, 12); memcpy(of.car_vel, R.car.vel, 12); memcpy(of.car_rot, R.car.rot, 36);
    for (int k = 0; k < 3; k++) of.car_half[k] = (carp.hull_max[k] - carp.hull_min[k]) * 0.5f;
    of.car_active = 0;
    memcpy(of.cam_pos, R.car.pos, 12);
    for (int k = 0; k < 3; k++) { of.cam_rot[k][0] = R.car.rot[k][0]; of.cam_rot[k][1] = R.car.rot[k][2]; of.cam_rot[k][2] = -R.car.rot[k][1]; }
    objs_quiet = 1;
#ifdef NR_PC
    struct timespec ts0; clock_gettime(CLOCK_MONOTONIC, &ts0);
#endif
    uint64_t t0 = now_us();
    int n = (int)(pre * 60.0f + 0.5f);
    for (int i = 0; i < n; i++) nr_objs_update(&OBJS, &of, 1.0f / 60.0f);
    objs_quiet = 0;
#ifdef NR_PC
    struct timespec ts1; clock_gettime(CLOCK_MONOTONIC, &ts1);
    logm("  the objects' head start: %.1f s in %.2f ms (real)", pre, (ts1.tv_sec - ts0.tv_sec) * 1e3 + (ts1.tv_nsec - ts0.tv_nsec) / 1e6);
#else
    logm("  the objects' head start: %.1f s in %.1f ms", pre, (now_us() - t0) / 1000.0);
#endif
}
static int race_quiet;                             /* the countdown without its pictures and voice (a Challenge restart) */
static void shock_reset(void);
static void warp_in_portal(void);
static void trk_clear(void);
static void emitters_reset(void);
/* (the log: what the car's effects made in this race: sparks, wing-tip trail particles, tyre track points and the
 * frames with glowing tracks) */
static unsigned fx_sparks, fx_wing, fx_trk, fx_trk_glow;
static void fx_log(void)
{
    logm("  effects: %s, sparks %u, wing trail particles %u, tyre track points %u (glowing in %u frames)",
         SAVE.vehicle >= 0 && SAVE.vehicle < FE_NVEH ? FE_VEH[SAVE.vehicle].name : "?", fx_sparks, fx_wing, fx_trk, fx_trk_glow);
}
/* (automatic tests, with the switch file "autoplay_shots": pictures of the finished frame at chosen moments, the
 * warp-in and warp-out, the first wing trail and the first sparks of each race, as TGA files in test_shots/) */
static int test_shots; static char test_shot_req[80]; static float test_spark_t = -1;
static void test_shot_take(void)
{
    if (!test_shot_req[0]) return;
    const int w = 960, h = 544;
    uint8_t *px = malloc((size_t)w * h * 4);
    if (px) {
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        char path[160]; snprintf(path, sizeof path, DATA "test_shots/%s.tga", test_shot_req);
        FILE *f = fopen(path, "wb");
        if (f) {
            uint8_t hd[18] = { 0, 0, 2 }; hd[12] = w & 255; hd[13] = w >> 8; hd[14] = h & 255; hd[15] = h >> 8; hd[16] = 24;
            fwrite(hd, 1, 18, f);
            for (int i = 0; i < w * h; i++) { uint8_t t = px[i * 4]; px[i * 3] = px[i * 4 + 2]; px[i * 3 + 1] = px[i * 4 + 1]; px[i * 3 + 2] = t; }
            fwrite(px, 3, (size_t)w * h, f); fclose(f);
            logm("  test picture %s", test_shot_req);
        }
        free(px);
    }
    test_shot_req[0] = 0;
}
#define CD_T0 (-6.0f)            /* the countdown's clock starts at -6 (0x715440) */
#define CD_RATE 1.3f             /* state 1 (0x712a10) adds dt and then 0.3 dt */
#define WARP_RATE 0.217f         /* IntroCarLogic's progress a second */
#define OBJS_AT_GO 4.5f          /* the level's objects' clock at GO (see objs_head_start) */
#define CD_OBJS (CD_T0 + CD_RATE * (-CD_T0 / CD_RATE - OBJS_AT_GO))   /* the countdown's clock when they start */
static void start_race_ex(int challenge_retry)
{
    if (challenge_retry) frc_off();                 /* (the failed attempts stay: the results replay them all) */
    else frc_stop();
    race_quiet = challenge_retry;
    if (ghosts_dirty || (g_autoplay && !pilot_on && R.lv && !R.credits)) ghosts_load_level();
    gh_rewind(); gh_labels_fade(0); rec_reset_all(); pilot_had_car = 0; pilot_s = 0; pilot_coast = 0;
    R.fin_stage = 0; R.fin_rank = 99;
    car_sounds_off();
    level_sounds_exit(); R.alive_t = 0; race_overheats = 0;      /* ("Race Start" 0x6fac80: the death counts) */
    if (SAVE.vehicle >= 0 && SAVE.vehicle < FE_NVEH && !strcmp(FE_VEH[SAVE.vehicle].key, "commander")) snd_post("Play_CMD_Sparks", OBJ_SPARK);
    shock_reset();
    memset(&R.ai, 0, sizeof R.ai); memset(R.cp_hit, 0, sizeof R.cp_hit);
    R.cp_count = 0; R.last_cp = -1; memset(R.cp_ly, 0, sizeof R.cp_ly); R.wrecks = 0; R.new_best = 0; R.unlocked_next = 0;
    R.resp_kind = 0; R.cool_t = 0; R.finish_obj = 0; R.nearly_fell = 0;
    R.stunt_left = 300; R.stunt_over = 0; R.stunt_voice = 0; R.settle_t = R.coast_t = 0; R.biggest = 0;   /* {120,300,600,1200}[1] */
    memset(&TUT, 0, sizeof TUT);
    FE_LEVEL_SCREENS = 0;                          /* Smash TV counts one attempt */
    place_at_start();
    if (chase_on) ch_restart(&CH);
    CAM.far_view = CAM.bumper = 0; CAM.lb = CAM.bb = 0;
    cam_reset(); CAM.fade = 0; CAM.fov = 60;
    R.fov = 60; R.blur = 0; R.shake_amp = 0;
    R.rs = RS_COUNT; R.t = 0; R.race_t = 0; R.bad_time = 0; R.phys_acc = 0; R.heat = 0; R.boom_t = 0;
    R.cd = CD_T0; R.cd_voice = 0;                     /* BeginCountdown (0x715440): the clock at -6 */
    warp_in_portal(); trk_clear();
    emitters_reset(); fx_sparks = fx_wing = fx_trk = fx_trk_glow = 0; test_spark_t = -1;
    R.msg_t = 0;
    nr_tricks_reset(&R.tricks); R.score = 0; R.tricks_level = 0; R.trick_t = 0;
    for (int i = 0; i < G.nsnd; i++) G.snd[i].armed = 1;
    if (objs_on) {
        obj_sounds_off(); nr_objs_restart(&OBJS);
        objs_head_start();
        load_emitters();
    }
    snd_post("Play_WarpIn", OBJ_CAR);                  /* BeginRace 0x715440 -> the car placed at the start (0x7062d0) */
    /* (the music and "3, 2, 1, Rush!" come at the clock's -3: countdown_step) */
}
/* the level's music waits for the countdown's -3 the first time (ChangeMusicMessage 0x17 with Play_NitronicRushStart) */
static const char *music_wait;
/* the countdown, a frame (state 1, 0x712a10): the clock; at -3 the music (first time) and the voice "3, 2, 1,
 * Rush!" (not after a Challenge death: +0x151 off) */
static void countdown_step(float dt)
{
    R.cd += CD_RATE * dt;
    if (R.cd >= -3.0f && !R.cd_voice) {
        R.cd_voice = 1;
        if (music_wait) { snd_post(music_wait, 0); music_wait = NULL; beat_t = 0; }
        if (!race_quiet) snd_post("Play_NitronicRushStart", 0);
    }
}
static void start_race(void) { start_race_ex(0); }

/* the vehicle's moving parts (models/veh_<name>.parts): wing doors, wings, rear wings,
 * boosters and back doors, jets, with the game's AnimBasic movements and FlameLogic flames */
typedef struct { int parent, group, model, jet; float pos[3], ori[9], scl[3], t0, t1, dpos[3], drot, axis[3]; } cpart;
typedef struct { int part; float pos[3], ori[9], scl[3]; } cflame;
static cpart CP[28]; static int ncp; static level_rt CPM[28]; static int ncpm;
static cflame CF[24]; static int ncf;
static float part_t[3];                       /* animation time of wings, boosters, jets */
static const float part_tmax[3] = { 0.6f, 0.4f, 0.07f };

/* the parts' particle emitters (the wing tips' sparkle trail: CarWingLeftOuter / RightOuter's ParticleEmitter, 160 a
 * second that stay where they are made for 0.5 s) */
typedef struct { int part; nr_pemit e; nr_pemit_state st; } cemit;
static cemit CE[4]; static int nce;
static void emitters_reset(void) { for (int i = 0; i < nce; i++) { CE[i].st.primed = 0; CE[i].st.acc = 0; } }
static void load_parts(const char *key)
{
    for (int i = 0; i < ncpm; i++) gfx_free_level(&CPM[i]);
    ncp = ncpm = ncf = nce = 0;
    char p[64]; snprintf(p, sizeof p, "models/veh_%s.parts", key);
    uint32_t sz; char *t = nr_pak_read(&pak, p, &sz);
    for (char *ln = t ? strtok(t, "\n") : NULL; ln; ln = strtok(NULL, "\n")) {
        float f[32]; int idx, par, grp; char model[48];
        if (!strncmp(ln, "part ", 5) && ncp < 28) {
            if (sscanf(ln, "part %d %d %d %47s %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f",
                       &idx, &par, &grp, model, &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &f[7], &f[8], &f[9], &f[10], &f[11],
                       &f[12], &f[13], &f[14], &f[15], &f[16], &f[17], &f[18], &f[19], &f[20], &f[21], &f[22], &f[23]) != 28) continue;
            cpart *c = &CP[ncp++];
            c->parent = par; c->group = grp; c->model = -1;
            c->jet = -1; if (grp == 2) { int nj = 0; for (int q = 0; q < ncp - 1; q++) nj += CP[q].group == 2; c->jet = nj; }
            memcpy(c->pos, f, 12); memcpy(c->ori, f + 3, 36); memcpy(c->scl, f + 12, 12);
            c->t0 = f[15]; c->t1 = f[16]; memcpy(c->dpos, f + 17, 12); c->drot = f[20]; memcpy(c->axis, f + 21, 12);
            if (strcmp(model, "-") && ncpm < 28) {
                char mp[64]; snprintf(mp, sizeof mp, "models/%s.nrl", model);
                if (gfx_load_nrl(&CPM[ncpm], &pak, mp, model)) c->model = ncpm++;
            }
        } else if (!strncmp(ln, "emit ", 5) && nce < 4) {   /* part, then nr_pemit's 26 numbers */
            float g[26]; int n = 0; char *q = ln + 5, *e;
            int pt = (int)strtol(q, &e, 10); q = e;
            while (n < 26) { float x = strtof(q, &e); if (e == q) break; g[n++] = x; q = e; }
            if (n == 26 && pt >= 0 && pt < ncp) {
                cemit *c = &CE[nce++]; memset(c, 0, sizeof *c); c->part = pt;
                nr_pemit *m = &c->e;
                m->color_idx = g[0]; m->color_rand = g[1]; m->inten = g[2]; m->tex_idx = g[3]; m->tex_rand = g[4]; m->tex_delta = g[5];
                m->size = g[6]; m->size_rand = g[7]; memcpy(m->pos, g + 8, 12); memcpy(m->pos_rand, g + 11, 12);
                m->rot = g[14]; m->rot_rand = g[15]; m->drot = g[16]; memcpy(m->dir, g + 17, 12);
                m->angle = g[20]; m->speed = g[21]; m->speed_rand = g[22]; m->gravity = g[23]; m->life = g[24]; m->freq = g[25];
            }
        } else if (!strncmp(ln, "flame ", 6) && ncf < 24) {
            cflame *c = &CF[ncf];
            if (sscanf(ln, "flame %d %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f", &c->part, &c->pos[0], &c->pos[1], &c->pos[2],
                       &c->ori[0], &c->ori[1], &c->ori[2], &c->ori[3], &c->ori[4], &c->ori[5], &c->ori[6], &c->ori[7], &c->ori[8],
                       &c->scl[0], &c->scl[1], &c->scl[2]) == 16) ncf++;
        }
    }
    free(t);
    logm("  moving parts %d (%d models), flames %d, particle emitters %d", ncp, ncpm, ncf, nce);
}

static void m4mul(float r[4][4], const float a[4][4], const float b[4][4])
{
    float t[4][4];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { float s = 0; for (int k = 0; k < 4; k++) s += a[i][k] * b[k][j]; t[i][j] = s; }
    memcpy(r, t, sizeof t);
}
/* T(pos) * R * S as a 4x4 (R row-major 3x3) */
static void m4trs(float m[4][4], const float pos[3], const float R[9], const float scl[3])
{
    for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) m[i][j] = R[i * 3 + j] * scl[j]; m[i][3] = pos[i]; }
    m[3][0] = m[3][1] = m[3][2] = 0; m[3][3] = 1;
}
static void axis_rot(float R[9], const float ax[3], float a)
{
    float l = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    float x = l > 1e-6f ? ax[0] / l : 0, y = l > 1e-6f ? ax[1] / l : 0, z = l > 1e-6f ? ax[2] / l : 1;
    float c = cosf(a), s = sinf(a), C = 1 - c;
    float r[9] = { c + x * x * C, x * y * C - z * s, x * z * C + y * s, y * x * C + z * s, c + y * y * C, y * z * C - x * s, z * x * C - y * s, z * y * C + x * s, c + z * z * C };
    memcpy(R, r, sizeof r);
}
/* world matrices of all parts for the car matrix M */
static float PW[28][4][4];
static void pose_parts(const float M[4][4])
{
    for (int i = 0; i < ncp; i++) {
        cpart *c = &CP[i];
        float tt = part_t[c->group < 0 || c->group > 2 ? 0 : c->group];
        float f = c->t1 > c->t0 ? (tt - c->t0) / (c->t1 - c->t0) : 0;
        if (f < 0) f = 0; if (f > 1) f = 1;
        float pos[3] = { c->pos[0] + c->dpos[0] * f, c->pos[1] + c->dpos[1] * f, c->pos[2] + c->dpos[2] * f };
        float Ra[9], R[9]; axis_rot(Ra, c->axis, c->drot * f);
        for (int r = 0; r < 3; r++) for (int k = 0; k < 3; k++) {
            float s = 0; for (int q = 0; q < 3; q++) s += c->ori[r * 3 + q] * Ra[q * 3 + k];
            R[r * 3 + k] = s;
        }
        float L[4][4]; m4trs(L, pos, R, c->scl);
        m4mul(PW[i], c->parent >= 0 && c->parent < i ? PW[c->parent] : M, L);
    }
}

/* ------------------------------------------------------------------ the Commander's sparks (ShockNearbyLogic, 0x6b5b70):
 * between Min Vel and Max Vel (100-150 u/s) up to Max Spark Frequency (250) bolts a second, as far into that range as
 * the car is; each a ray (0x6b5620) of up to 2 x Max Spark Distance (25) ahead and round the car, direction (x, 2y, z)
 * in the car's frame with y >= 0. One that meets the level becomes a SparkBetweenLogic bolt (sparkdead1-4: 0.2-0.3 s,
 * brightness 10, width 0.25) from the body's point furthest that way (0x630230, carried along with the car) to where
 * it hit. The numbers and those body points come from models/veh_<car>.shock */
#define SHOCK_MAX 128
static struct {
    int on; float minv, maxv, freq, dist, acc;
    float (*hull)[3]; int nhull;
    int made, most;                                         /* (the log: bolts since the last report, the most at once) */
    struct { float a[3], b[3], age, life, alpha; int m; } s[SHOCK_MAX]; int n;
    uint32_t x, y, z, w;
} SHK = { .x = 123456789u, .y = 362436069u, .z = 521288629u, .w = 88675123u };
static level_rt SPARKM[4]; static int sparkm_n;
static uint32_t shk_rand(void)
{
    uint32_t t = SHK.x ^ (SHK.x << 11); SHK.x = SHK.y; SHK.y = SHK.z; SHK.z = SHK.w;
    return SHK.w = SHK.w ^ (SHK.w >> 19) ^ t ^ (t >> 8);
}
static float shk_frand(void) { return (shk_rand() & 0xffffff) * (1.0f / 16777216.0f); }
static void shock_load(const char *key)
{
    free(SHK.hull); SHK.hull = NULL; SHK.nhull = 0; SHK.on = 0; SHK.n = 0; SHK.acc = 0;
    char p[64]; snprintf(p, sizeof p, "models/veh_%s.shock", key);
    uint32_t sz; char *t = nr_pak_read(&pak, p, &sz);
    if (!t) return;
    int cap = 0;
    for (char *l = t; l && *l; ) {
        char *e = strchr(l, '\n'); if (e) *e = 0;
        float a, b, c, d;
        if (sscanf(l, "shock %f %f %f %f", &a, &b, &c, &d) == 4) { SHK.minv = a; SHK.maxv = b; SHK.freq = c; SHK.dist = d; SHK.on = 1; }
        else if (sscanf(l, "p %f %f %f", &a, &b, &c) == 3) {
            if (SHK.nhull == cap) { cap = cap ? cap * 2 : 128; void *q = realloc(SHK.hull, sizeof *SHK.hull * cap); if (!q) break; SHK.hull = q; }
            SHK.hull[SHK.nhull][0] = a; SHK.hull[SHK.nhull][1] = b; SHK.hull[SHK.nhull][2] = c; SHK.nhull++;
        }
        l = e ? e + 1 : NULL;
    }
    free(t);
    if (!SHK.nhull || SHK.maxv <= SHK.minv || !sparkm_n) SHK.on = 0;
    if (SHK.on) logm("  sparks (ShockNearbyLogic): %.0f-%.0f u/s, %.0f a second, %.0f u, from %d body points", SHK.minv, SHK.maxv, SHK.freq, SHK.dist, SHK.nhull);
}
static void shock_bolt(void)
{
    float x, y, z, l2;
    do { x = shk_frand() * 2 - 1; y = shk_frand(); z = shk_frand() * 2 - 1; l2 = x * x + y * y + z * z; } while (l2 >= 1 || l2 == 0);
    float il = 1.0f / sqrtf(l2), D[3] = { x * il * SHK.dist, y * il * 2 * SHK.dist, z * il * SHK.dist }, W[3];
    for (int i = 0; i < 3; i++) W[i] = R.car.rot[i][0] * D[0] + R.car.rot[i][1] * D[1] + R.car.rot[i][2] * D[2];
    float L = sqrtf(W[0] * W[0] + W[1] * W[1] + W[2] * W[2]), u[3] = { W[0] / L, W[1] / L, W[2] / L }, t, n[3];
    if (!COLL.ntri || !nr_coll_ray(&COLL, R.car.pos, u, L, &t, n, NULL)) return;
    int bi = 0; float best = -10.01f;                       /* the body's vertex furthest along the ray (0x630230) */
    for (int i = 0; i < SHK.nhull; i++) {
        float d = SHK.hull[i][0] * D[0] + SHK.hull[i][1] * D[1] + SHK.hull[i][2] * D[2];
        if (d > best) { best = d; bi = i; }
    }
    if (SHK.n >= SHOCK_MAX) return;
    __typeof__(SHK.s[0]) *b = &SHK.s[SHK.n++];
    memcpy(b->a, SHK.hull[bi], 12);
    for (int i = 0; i < 3; i++) b->b[i] = R.car.pos[i] + u[i] * t;
    b->m = (int)(shk_rand() % (uint32_t)sparkm_n);
    b->age = 0; b->life = 0.2f + (shk_rand() % 1024) * (0.1f / 1024.0f); b->alpha = 10;
    SHK.made++; if (SHK.n > SHK.most) SHK.most = SHK.n;
}
static void shock_reset(void) { SHK.n = 0; SHK.acc = 0; }
static void shock_update(float dt, int racing)
{
    int w = 0;                                              /* SparkBetweenLogic (0x6a6c20): fades out over its life */
    for (int i = 0; i < SHK.n; i++) {
        if (SHK.s[i].age > SHK.s[i].life) continue;
        SHK.s[i].alpha = (1 - SHK.s[i].age / SHK.s[i].life) * 10; SHK.s[i].age += dt;
        SHK.s[w++] = SHK.s[i];
    }
    SHK.n = w;
    if (!SHK.on || !racing) return;
    float t = (nr_car_speed(&R.car) - SHK.minv) / (SHK.maxv - SHK.minv);
    if (!(t < 1)) t = 1;
    if (t < 0) t = 0;
    SHK.acc += SHK.freq * t * dt;
    while (SHK.acc > 0) { shock_bolt(); SHK.acc -= 1; }
}
static int shock_draw(gfx_obj *objs, int nobj, int max, v3 eye)
{
    for (int i = 0; i < SHK.n && nobj < max; i++) {
        float *a = SHK.s[i].a, p1[3], d[3];
        for (int k = 0; k < 3; k++) p1[k] = R.car.pos[k] + R.car.rot[k][0] * a[0] + R.car.rot[k][1] * a[1] + R.car.rot[k][2] * a[2];
        for (int k = 0; k < 3; k++) d[k] = SHK.s[i].b[k] - p1[k];
        float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (l < 0.01f) continue;
        v3 dir = v3make(d[0] / l, d[1] / l, d[2] / l), tc = v3make(eye.x - p1[0], eye.y - p1[1], eye.z - p1[2]);
        v3 nn = norm(cross(dir, tc)), zz = cross(nn, dir);
        gfx_obj *g = &objs[nobj++]; memset(g, 0, sizeof *g);
        float c0[3] = { nn.x, nn.y, nn.z }, c1[3] = { dir.x, dir.y, dir.z }, c2[3] = { zz.x, zz.y, zz.z };
        for (int r = 0; r < 3; r++) { g->m[r][0] = c0[r] * 0.25f * l; g->m[r][1] = c1[r] * l; g->m[r][2] = c2[r] * l; g->m[r][3] = p1[r]; }
        g->m[3][3] = 1;
        g->model = &SPARKM[SHK.s[i].m]; g->glow = 1; g->use_col = 1;
        g->col[0] = 0.4f; g->col[1] = 0.15f; g->col[2] = 0.6f; g->col[3] = SHK.s[i].alpha;
    }
    return nobj;
}

/* ---- the warp-in and warp-out (IntroCarLogic 0x68d0e0; BeginCountdown 0x715440 -> 0x7062d0, CarFinish 0x705da0):
 * the car's stand-in, in loose triangles (models/veh_<car>_intro.nrl, phong_ref_intro.fx), gathers from the start
 * tunnel's portal through the countdown (progress 0 -> 1 at 0.217 a second: 4.6 s, whole at GO) and from the finish
 * leaves for the end tunnel's (0.85 -> 0 at the same rate) */
static level_rt WARPM;
/* the portal: the start tunnel's at (0, -36 sy, sz) in it (0x7062d0); none: the stand-in itself */
static void warp_in_portal(void)
{
    const tunnel_tf *t = find_tunnel(STARTS, nstarts);
    R.warp_tunnel = t != NULL;
    if (!t) { R.warp_portal = v3make(R.car.pos[0], R.car.pos[1], R.car.pos[2]); return; }
    float l[3] = { 0, -36.0f * t->s[1], t->s[2] };
    R.warp_portal = v3make(t->p[0] + t->R[0][0] * l[0] + t->R[0][1] * l[1] + t->R[0][2] * l[2],
                           t->p[1] + t->R[1][0] * l[0] + t->R[1][1] * l[1] + t->R[1][2] * l[2],
                           t->p[2] + t->R[2][0] * l[0] + t->R[2][1] * l[1] + t->R[2][2] * l[2]);
}
/* the finish (0x705da0): the end tunnel's at (0, -37 sy, sz); none: 10 above the car */
static void warp_out_portal(void)
{
    const tunnel_tf *t = find_tunnel(ENDS, nends);
    R.warp_tunnel = t != NULL;
    if (!t) { R.warp_portal = v3make(R.car.pos[0], R.car.pos[1], R.car.pos[2] + 10); return; }
    float l[3] = { 0, -37.0f * t->s[1], t->s[2] };
    R.warp_portal = v3make(t->p[0] + t->R[0][0] * l[0] + t->R[0][1] * l[1] + t->R[0][2] * l[2],
                           t->p[1] + t->R[1][0] * l[0] + t->R[1][1] * l[1] + t->R[1][2] * l[2],
                           t->p[2] + t->R[2][0] * l[0] + t->R[2][1] * l[1] + t->R[2][2] * l[2]);
}
/* the stand-in at the car's matrix M with progress p; the portal in its space (IntroCarLogic: PortalPositionLS,
 * direction and distance; at the stand-in itself the direction stays 0) */
static void warp_obj(gfx_obj *o, const float M[4][4], float p)
{
    memset(o, 0, sizeof *o);
    o->model = &WARPM; memcpy(o->m, M, sizeof o->m); o->no_cull = 1;
    o->intro_p = p < 0 ? 0 : p > 1 ? 1 : p;
    float d[3] = { R.warp_portal.x - M[0][3], R.warp_portal.y - M[1][3], R.warp_portal.z - M[2][3] }, l[3];
    for (int k = 0; k < 3; k++) l[k] = M[0][k] * d[0] + M[1][k] * d[1] + M[2][k] * d[2];
    float L = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
    if (L > 1e-4f) { o->portal[0] = l[0] / L; o->portal[1] = l[1] / L; o->portal[2] = l[2] / L; o->portal[3] = L; }
}

/* ---- tyre tracks (TireTrailLogic 0x6bbc20 / 0x6bbb90 from CarLogic 0x7065d0, tire_track.fx, TireTrailFadeLogic
 * 0x6bb8a0): a wheel that is down and slipping (its grip scaled below the vehicle's Tire Slip Amount: the skid's
 * rule, 0x701ca0) at more than 5 lays one track while it does: a curve from where it began, through a point about
 * half way along (taken at the doubling counts), to the wheel; 0.32 wide, 0.01 off the ground. When it stops, the
 * track stays 25 s and fades over 5 s. Slow tracks are a dark tread, fast ones glow (blue, the fastest a rainbow) */
#define TRK_MAX 48
#define TRK_SEG 32
typedef struct {
    int on, live, count, k;
    float frac, len, avg, age;
    float A[3], cur[3], M1[3], M2[3], side[3], n[3];
} trk_t;
static trk_t TRK[TRK_MAX]; static int trk_slot[4] = { -1, -1, -1, -1 };
static float tire_slip_amount = 0.9f;
static GLuint trk_tex, trk_rainbow;
static uint8_t TRKV[TRK_MAX * TRK_SEG * 4 * NR_VERTEX_SIZE]; static int trk_nq, trk_dark, trk_glow;
static void trk_clear(void) { memset(TRK, 0, sizeof TRK); for (int w = 0; w < 4; w++) trk_slot[w] = -1; trk_nq = 0; }
static void trk_end(int w)                         /* 0x6bbb90: shorter than 2: straight to the fade */
{
    int i = trk_slot[w]; if (i < 0) return;
#ifdef NR_PC
    if (getenv("TRKLOG")) logm("  tyre track (wheel %d): %d points, %.1f long, mean speed %.0f", w, TRK[i].count, TRK[i].len, TRK[i].avg);
#endif
    TRK[i].live = 0; TRK[i].age = TRK[i].len < 2 ? 25 : 0;
    trk_slot[w] = -1;
}
static void trk_point(trk_t *t, const float p[3])  /* 0x6bb990 */
{
    t->count++;
    if (t->count == 1) { memcpy(t->A, p, 12); memcpy(t->cur, p, 12); memcpy(t->M1, p, 12); memcpy(t->M2, p, 12); t->frac = 0.5f; }
    else if (t->count == 2) { memcpy(t->cur, p, 12); memcpy(t->M1, t->A, 12); memcpy(t->M2, t->cur, 12); t->k = 0; t->frac = 0.5f; }
    else if (t->count == 3) memcpy(t->M1, t->M2, 12);
    else {
        memcpy(t->cur, p, 12);
        float x = log2f(t->count * 0.5f) + 0.25f; int k = (int)x;
        if (k != t->k) { memcpy(t->M1, t->M2, 12); memcpy(t->M2, p, 12); t->k = k; }
        t->frac = x - (float)k;
    }
}
static void trk_lay(int w, const float p[3], const float n[3], const float side[3], float dist, float speed)
{
    int i = trk_slot[w];
    if (i < 0) {                                   /* a new track: a free one, else the oldest that has ended */
        float oldest = -1;
        for (int j = 0; j < TRK_MAX && i < 0; j++) if (!TRK[j].on) i = j;
        if (i < 0) for (int j = 0; j < TRK_MAX; j++) if (!TRK[j].live && TRK[j].age > oldest) { oldest = TRK[j].age; i = j; }
        if (i < 0) return;
        memset(&TRK[i], 0, sizeof TRK[i]); TRK[i].on = TRK[i].live = 1; trk_slot[w] = i;
    }
    trk_t *t = &TRK[i];
    trk_point(t, p);
    t->len += dist; fx_trk++;
    t->avg = t->avg * (1 - 1.0f / t->count) + speed / t->count;
    memcpy(t->side, side, 12); memcpy(t->n, n, 12);
}
static void trk_update(float dt, int racing)
{
    for (int w = 0; w < 4; w++) {
        int lay = 0;
        int force = 0;
#ifdef NR_PC
        if (getenv("TRKFORCE")) force = atoi(getenv("TRKFORCE"));   /* (the test machine: every wheel lays a track, at this mean speed) */
#endif
        if (racing && w < carp.nwheels && R.car.wheel_contact[w] && (force || tire_slip_amount > R.car.wheel_slip[w])) {
            const float *cp = R.car.wheel_cp[w], *n = R.car.wheel_n[w];
            float com[3], r[3], v[3];
            for (int k = 0; k < 3; k++) com[k] = R.car.pos[k] + R.car.rot[k][0] * carp.com[0] + R.car.rot[k][1] * carp.com[1] + R.car.rot[k][2] * carp.com[2];
            for (int k = 0; k < 3; k++) r[k] = cp[k] - com[k];
            v[0] = R.car.vel[0] + R.car.angvel[1] * r[2] - R.car.angvel[2] * r[1];
            v[1] = R.car.vel[1] + R.car.angvel[2] * r[0] - R.car.angvel[0] * r[2];
            v[2] = R.car.vel[2] + R.car.angvel[0] * r[1] - R.car.angvel[1] * r[0];
            float sd[3] = { v[1] * n[2] - v[2] * n[1], v[2] * n[0] - v[0] * n[2], v[0] * n[1] - v[1] * n[0] };   /* v x n */
            float sl = sqrtf(sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2]), sp = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (sl > 1e-6f && sp > 5) {
                for (int k = 0; k < 3; k++) sd[k] /= sl;
                trk_lay(w, cp, n, sd, sp * dt, force ? (float)force : sp); lay = 1;
            }
        }
        if (!lay) trk_end(w);
    }
    for (int i = 0; i < TRK_MAX; i++) if (TRK[i].on && !TRK[i].live) { TRK[i].age += dt; if (TRK[i].age >= 30) TRK[i].on = 0; }
}
static float smooth01(float x) { x = x < 0 ? 0 : x > 1 ? 1 : x; return x * x * (3 - 2 * x); }
/* this frame's quads (TRKV): each track's 32 pieces, as tire_track.fx's vertex shader bends tire_track.msh */
static void trk_build(v3 eye)
{
    trk_nq = 0; trk_dark = trk_glow = 0;
    if (!trk_tex) return;
    for (int i = 0; i < TRK_MAX; i++) {
        const trk_t *t = &TRK[i];
        if (!t->on || t->count < 2) continue;
        float dA = len(sub(v3make(t->A[0], t->A[1], t->A[2]), eye)), dC = len(sub(v3make(t->cur[0], t->cur[1], t->cur[2]), eye));
        if (dA > 600 && dC > 600 && t->len < 400) continue;
        float fade = t->live || t->age < 25 ? 1.0f : 1.0f - (t->age - 25) / 5.0f;
        if (fade <= 0) continue;
        float L = fminf(0.25f * t->len, 25.5f), W = floorf(fminf(0.2f * t->avg, 25.5f) * 10) / 10;
        float ga = smooth01((W - 19.2f) * 0.625f), gb = smooth01((W - 22.4f) * 10.0f);
        if (ga < 0.999f) trk_dark = 1;
        if (ga > 0.001f && !trk_glow) { trk_glow = 1; fx_trk_glow++; }
        float C[3], E[3], H[3];
        for (int k = 0; k < 3; k++) {
            C[k] = t->M1[k] * (1 - t->frac) + t->M2[k] * t->frac - t->A[k];
            E[k] = t->cur[k] - t->A[k];
            H[k] = 0.5f * E[k] - C[k];
        }
        for (int j = 0; j < TRK_SEG; j++) {
            uint8_t *v = TRKV + (size_t)trk_nq * 4 * NR_VERTEX_SIZE;
            for (int c = 0; c < 4; c++, v += NR_VERTEX_SIZE) {
                float sv = (float)(j + (c == 2 || c == 3)) / TRK_SEG, x = (c == 1 || c == 2) ? 0.16f : -0.16f;
                float a = 2 * sv, b = 2 * sv * (2 * sv - 1), f[5];
                for (int k = 0; k < 3; k++) f[k] = t->A[k] + a * C[k] + b * H[k] + t->side[k] * x + t->n[k] * 0.01f;
                f[3] = x > 0 ? 1.0f : 0.0f; f[4] = 0.5f * sv * L;
                float cap = L <= 1 ? sv : sv < 0.5f ? fminf(sv * L, 0.5f) : fmaxf(0.5f, (sv - 1) * L + 1);
                memcpy(v, f, 20);
                v[20] = (uint8_t)(int8_t)(ga * 127); v[21] = (uint8_t)(int8_t)(gb * 127); v[22] = v[23] = 0;
                v[24] = (uint8_t)(cap * 255 + 0.5f); v[25] = (uint8_t)(fade * 255 + 0.5f); v[26] = v[27] = 0;
                v[28] = v[29] = v[30] = v[31] = 0;
            }
            trk_nq++;
        }
    }
}
static void trk_draw(int pass)
{
    if (!trk_nq || (pass == 0 && !trk_dark) || (pass == 1 && !trk_glow)) return;
    for (int q = 0; q < trk_nq; q += 1024)
        gfx_tracks(TRKV + (size_t)q * 4 * NR_VERTEX_SIZE, trk_nq - q < 1024 ? trk_nq - q : 1024, trk_tex, trk_rainbow, pass);
}

/* ---- the car's sparks (CarLogic::HandleCollisions 0x709290, SparkBitch): at each of the body's contacts this frame the
 * emitter is moved there, pointed along the contact's normal, and makes (int)(5 x min(sliding speed / 3, 15)) sparks at
 * once (re_F2.md 4.6). The PC's body is its convex hull; the physics here stands in for it with spheres, which don't
 * reach its flat underside, so the hull's bottom corners (the lowest vertices of CarHull.mdc, DeloreanHull.mdc and
 * VanConvex.mdc) spark too where they touch the ground (only the sparks: the physics is left as it is) */

#define HULL_REACH 0.41f          /* (the hull corner's ray: from 0.4 above it to 1 cm below) */
#define SPARKS_PER_FRAME 150      /* (the Vita: at most this many sparks a frame; the PC makes up to 75 per contact) */
static const float HULL_CAR[8][3] = { { -0.836f, -0.872f, -0.383f }, { -0.836f, 1.376f, -0.383f }, { -0.714f, 1.645f, -0.365f },
    { -0.434f, 1.763f, -0.365f }, { 0.434f, 1.763f, -0.365f }, { 0.714f, 1.645f, -0.365f }, { 0.836f, -0.872f, -0.383f },
    { 0.836f, 1.376f, -0.383f } };
static const float HULL_DELOREAN[9][3] = { { -0.865f, 1.192f, -0.422f }, { -0.833f, -0.367f, -0.432f }, { -0.803f, -1.46f, -0.415f },
    { -0.699f, 1.814f, -0.393f }, { 0, 1.867f, -0.393f }, { 0.699f, 1.814f, -0.393f }, { 0.803f, -1.46f, -0.415f },
    { 0.833f, -0.367f, -0.432f }, { 0.865f, 1.192f, -0.422f } };
static const float HULL_VAN[7][3] = { { -0.999f, -1.993f, -0.736f }, { -0.999f, 2.313f, -0.736f }, { -0.787f, 2.571f, -0.736f },
    { 0, 2.651f, -0.736f }, { 0.787f, 2.571f, -0.736f }, { 0.999f, -1.993f, -0.736f }, { 0.999f, 2.313f, -0.736f } };
static void car_sparks(void)
{
    if (!particles_ok) return;
    int budget = SPARKS_PER_FRAME;
    for (int k = 0; k < R.car.ncontact && k < 16; k++)
        particles_spark_emit(R.car.contact[k].p, R.car.contact[k].n, fminf(R.car.contact[k].slide / 3.0f, 15.0f), &budget);
    /* (not while the test drive follows a replay: its poses, interpolated between the recording's ticks, cut through
     * the road's crests, so the hull's corners would be in the road there all the time) */
    if (!COLL.ntri || R.car.grounded == 0 || pilot_on) return;
    const char *key = SAVE.vehicle >= 0 && SAVE.vehicle < FE_NVEH ? FE_VEH[SAVE.vehicle].key : "";
    const float (*hp)[3] = HULL_CAR; int nh = 8;
    if (!strcmp(key, "commander")) { hp = HULL_DELOREAN; nh = 9; }
    else if (!strcmp(key, "avenger")) { hp = HULL_VAN; nh = 7; }
    float up[3] = { R.car.rot[0][2], R.car.rot[1][2], R.car.rot[2][2] }, com[3];
    for (int k = 0; k < 3; k++) com[k] = R.car.pos[k] + R.car.rot[k][0] * carp.com[0] + R.car.rot[k][1] * carp.com[1] + R.car.rot[k][2] * carp.com[2];
    for (int i = 0; i < nh && budget > 0; i++) {
        const float *l = hp[i];
        float P[3], o[3], d[3] = { -up[0], -up[1], -up[2] }, t, n[3];
        for (int k = 0; k < 3; k++) P[k] = R.car.pos[k] + R.car.rot[k][0] * l[0] + R.car.rot[k][1] * l[1] + R.car.rot[k][2] * l[2];
        for (int k = 0; k < 3; k++) o[k] = P[k] + up[k] * 0.4f;
        if (!nr_coll_ray(&COLL, o, d, HULL_REACH, &t, n, NULL)) continue;      /* (the corner at the ground, or in it) */
        float r[3] = { P[0] - com[0], P[1] - com[1], P[2] - com[2] }, v[3];
        v[0] = R.car.vel[0] + R.car.angvel[1] * r[2] - R.car.angvel[2] * r[1];
        v[1] = R.car.vel[1] + R.car.angvel[2] * r[0] - R.car.angvel[0] * r[2];
        v[2] = R.car.vel[2] + R.car.angvel[0] * r[1] - R.car.angvel[1] * r[0];
        float vn = v[0] * n[0] + v[1] * n[1] + v[2] * n[2], vt[3] = { v[0] - vn * n[0], v[1] - vn * n[1], v[2] - vn * n[2] };
        float slide = sqrtf(vt[0] * vt[0] + vt[1] * vt[1] + vt[2] * vt[2]);
        float at[3] = { o[0] + d[0] * t, o[1] + d[1] * t, o[2] + d[2] * t };
        particles_spark_emit(at, n, fminf(slide / 3.0f, 15.0f), &budget);
    }
}

/* the vehicle picked on the level screen: its model and handling */
static int car_loaded = -1, infinite_heat;
static void load_vehicle(int i)
{
    if (i == car_loaded || i < 0 || i >= FE_NVEH) return;
    char p[64]; snprintf(p, sizeof p, "models/veh_%s.nrl", FE_VEH[i].key);
    level_rt m;
    if (!gfx_load_nrl(&m, &pak, p, FE_VEH[i].name)) return;
    gfx_free_level(&CAR); CAR = m;
    nr_car_default_params(&carp);
    snprintf(p, sizeof p, "models/veh_%s.cfg", FE_VEH[i].key);
    uint32_t csz; char *cfg = nr_pak_read(&pak, p, &csz);
    int n = cfg ? nr_car_load_params(&carp, cfg) : 0;
    {   const char *q = cfg ? strstr(cfg, "CarLogic.Tire_Slip_Amount ") : NULL;   /* (the tyre tracks) */
        tire_slip_amount = q ? strtof(q + strlen("CarLogic.Tire_Slip_Amount "), NULL) : 0.9f; }
    free(cfg);
    gfx_free_level(&WARPM);
    snprintf(p, sizeof p, "models/veh_%s_intro.nrl", FE_VEH[i].key);
    if (!gfx_load_nrl(&WARPM, &pak, p, "warp-in")) gfx_free_level(&WARPM);
    {   /* the collision hull's box (CarHull / VanConvex / DeloreanHull .mdc): the car's inertia and body */
        static const struct { const char *key; float mn[3], mx[3]; } HULLS[] = {
            { "avenger",  { -0.999f, -2.522f, -0.736f }, { 0.999f, 2.651f, 0.915f } },
            { "commander", { -0.909f, -1.971f, -0.432f }, { 0.909f, 2.042f, 0.502f } },
        };
        for (unsigned h = 0; h < sizeof HULLS / sizeof HULLS[0]; h++)
            if (!strcmp(FE_VEH[i].key, HULLS[h].key)) nr_car_set_hull(&carp, HULLS[h].mn, HULLS[h].mx);
    }
    infinite_heat = !strcmp(FE_VEH[i].key, "goldenceptor");      /* "can't overheat" */
    {   /* CarLogic's Added Camera Leash Distance and BumperCamPos_ (Car / Car_Gold / Delorean, Van) */
        int van = !strcmp(FE_VEH[i].key, "avenger"), cyc = strstr(FE_VEH[i].key, "cycle") != NULL;
        CAM.added = van ? 1.33f : cyc ? -0.5f : 0; CAM.bump[0] = 0;
        CAM.bump[1] = van ? 1.92f : cyc ? -0.135f : 1.0f; CAM.bump[2] = van ? 0.42f : cyc ? 0.702f : 0.35f;
    }
    load_parts(FE_VEH[i].key);
    shock_load(FE_VEH[i].key);
    car_loaded = i;
    logm("vehicle %s: %d handling values", FE_VEH[i].name, n);
}

static fe_level CREDITS_LEVEL = { "credits_level", "Credits", 4, 1, 9, 1, 0, "", 1 };
static void begin_level(int set, int level, int autoplay)
{
    R.set = set; R.level = level;
    R.credits = set < 0;
    R.lv = R.credits ? &CREDITS_LEVEL : &FE_SETS[set].lv[level];
    /* Story mode ("In Story Mode"): the campaign set entered from "Story" (the same set from Arcade is not) */
    R.story = !R.credits && strstr(FE_SETS[set].file, "Campaign") != NULL && front_story();   /* (autoplay: Arcade, with the ghosts) */
    R.stunt = !R.credits && R.lv->sd == 1;          /* sd_level 1: StuntLevel */
    loading_begin();                                  /* the PC's loading screen */
    snd_post("Stop_CarEngine", OBJ_CAR);
    for (int i = 0; i < OBJ_NEMIT; i++) if (emit_slot_used[i]) { snd_stop_obj(OBJ_EMIT0 + i, 300); emit_slot_used[i] = 0; }
    load_race_level(R.lv->xml);
    load_vehicle(SAVE.vehicle);
    loading_end();
    dyn_res_reset();
    if (SAVE.vehicle >= 0 && SAVE.vehicle < FE_NVEH) snprintf(OBJS.vehicle, sizeof OBJS.vehicle, "%s", FE_VEH[SAVE.vehicle].name);
    OBJS.advanced = SAVE.adv_controls;               /* TriggerControlScheme: the tutorial's Advanced texts */
    R.layer = music_layer_name(R.lv->music);
    music_wait = music_event(R.lv->music);           /* (posted at the countdown's -3) */
    beat_t = 0; beat_period = 60.0f / music_bpm(R.layer);
    layer_event("Boost_Off"); layer_event("Wings_Off"); layer_event("Alarm_Off");
    snd_set_param("CarEngine_Volume", 1.0f);
    snd_set_param("CarEngineRPM", 0.1f);
    snd_set_param("Heat_Level", 0.0f);
    snd_post("Play_CarEngine", OBJ_CAR);
    R.cam_mode = 0;
    ghosts_dirty = 1;
    start_race();
    (void)autoplay;
#ifdef NR_PC
    if (getenv("LEVELCHECK")) {     /* (the PC test machine: is the level sound - the start on the ground, a finish to reach) */
        float d3[3] = { 0, 0, -1 }, t = 99, n[3], o[3] = { R.car.pos[0], R.car.pos[1], R.car.pos[2] + 1 };
        if (!nr_coll_ray(&COLL, o, d3, 50, &t, n, NULL)) t = 99;
        float best = 1e9f;
        if (L.d.npath > 1) {
            const float *e = L.d.path[L.d.npath - 1];
            for (int i = 0; i < G.nfinish; i++) { float dx = e[0] - G.finish[i][0], dy = e[1] - G.finish[i][1], dz = e[2] - G.finish[i][2]; float dd = sqrtf(dx * dx + dy * dy + dz * dz); if (dd < best) best = dd; }
        }
        logm("LEVELCHECK %s: start %.1f above the ground, finish triggers %d (+%d objects, %d points), route %u points, route end to finish %.0f, checkpoints %d, %s",
             R.lv->xml, t - 1, G.nfbox, objs_on ? nr_objs_count(&OBJS, OK_FINISH) : 0, G.nfinish, L.d.npath, best, G.ncp, R.stunt ? "stunt (ends on the clock)" : "race");
    }
#endif
}

static void spawn_gibs(void);
/* CarLogic_Die (0x707db0) and the "Car Death" event; cause: 1 self-termination (the reset button), 2 a deadly surface,
 * 3 a high-speed crash, 4 overheating, 5 out of the level */
static void explode(const char *why, int cause)
{
    if (R.rs != RS_RACE) return;
    int highspeed = cause == 3;
    float v2 = R.car.vel[0] * R.car.vel[0] + R.car.vel[1] * R.car.vel[1] + R.car.vel[2] * R.car.vel[2];
    logm("  car destroyed (%s) at %.1f s", why, R.race_t);
    if (chase_on) ch_died(&CH, R.car.pos);
    car_sounds_off();
    R.rs = RS_DEAD; R.dead_t = 0; R.wrecks++; SAVE.wrecks++; R.nearly_fell = 0;
    R.boom = v3make(R.car.pos[0], R.car.pos[1], R.car.pos[2]); R.boom_t = 0.001f;
    float bp[3] = { R.boom.x, R.boom.y, R.boom.z };
    snd_set_obj_pos(OBJ_BOOM, bp);
    snd_post("Stop_CarEngine", OBJ_CAR);              /* (CarLogic's Stop Engine Sound Event Name) */
    snd_post("Play_Rand_Car_Explosion", OBJ_BOOM);
    snd_post("Play_Rand_Electric_Arc", OBJ_BOOM);
    snd_set_state("Crash_States", "Crashed"); snd_set_state("Announcer_Crash", "Crashed");   /* muffled until the respawn */
    death_phrase(cause, v2);
    R.shake = 0.35f;
    for (int i = 0; i < 16; i++) if (R.rec_gib[i] >= 0) { gh_rec_remove(R.rec_gib[i]); R.rec_gib[i] = -1; }
    spawn_gibs();
    /* Die 0x707db0: the pieces join the recording, then the car leaves it */
    for (int i = 0; i < ngibs && i < 16; i++) if (gh_rec_on() && GIBM[GIBS[i].model].ghost[0]) {
        float gp[3] = { GIBS[i].pos.x, GIBS[i].pos.y, GIBS[i].pos.z };
        R.rec_gib[i] = gh_rec_add(GIBM[GIBS[i].model].ghost);
        if (R.rec_gib[i] >= 0) gh_rec_pose(R.rec_gib[i], GIBS[i].rot, gp);
    }
    if (R.rec_car >= 0) { gh_rec_remove(R.rec_car); R.rec_car = -1; }
    if (highspeed) {
        SAVE.highspeed_deaths++;
        if (SAVE.highspeed_deaths >= 10) ach_unlock("Wreckless");
    }
}

/* BaseLevel::Finish / StuntLevel::Finish (re_I 6.1, re_N 4.4): the recording stops; the run's place in the
 * Local table (ComputeRank 0x6f3d80); a top-ten run keeps its replay as <level>.<rank>.NrGhostBin (SaveGhost
 * 0x6f4260: the files below it move one down). The table itself takes the run when the name is accepted. */
/* FailedRunCameraLogic (re_N 12.2, blueprint FailedRunCamera): behind name entry and the results, 3 s after
 * the finish, this run's replay ("S" copy, the lit car) and the ghosts start again from GO, and the camera
 * follows the copy on a lazy leash (6 units, target height 1.2, FoV 60 widening 10 a second once it crashed
 * or ended); a black cut each time it picks the car up, fading in at 1.5 a second; it loops. */
static struct { int on, state, restart; float t, fade, fov; v3 pos, look; } FRC;
static void frc_stop(void) { FRC.on = 0; gh_self_clear(); }
static void frc_off(void) { FRC.on = 0; }
static void frc_start(void)
{
    if (R.lv && R.lv->sd == 3) frc_off();            /* Challenge: the failed attempts kept, this run added last (12.1) */
    else frc_stop();
    memset(&FRC, 0, sizeof FRC);
    FRC.on = gh_self_add() >= 0; FRC.fade = 1; FRC.fov = 60; FRC.pos = R.eye; FRC.look = add(R.eye, R.view_fwd);
}
static void frc_update(float dt)
{
    if (!FRC.on || dt <= 0) return;
    if (FRC.fade < 1) FRC.fade = fminf(1, FRC.fade + 1.5f * dt);
    float W[4][4]; int car = gh_self_car(W), st = gh_self_state();
    switch (FRC.state) {
    case 0: FRC.t += dt; if (FRC.t > 3.0f) { FRC.t = 0; FRC.state = 1; FRC.restart = 1; } break;      /* Initial Wait Time */
    case 1: FRC.fade = 0; FRC.state = 2; break;                                                      /* the cut: black */
    case 2: if (FRC.restart) gh_restart_all(); FRC.fade = 0; FRC.state = 3; break;
    case 3: FRC.fade = 0; if (car) { FRC.fov = 60; FRC.state = 5; } break;                          /* the car's first tick */
    case 5: if (!car) FRC.state = 6; if (st != 3) FRC.state = 7; break;
    case 6: FRC.fov += 10 * dt; if (car) { FRC.state = 1; FRC.restart = 0; } if (st != 3) FRC.state = 7; break;   /* wrecked: hold */
    case 7: FRC.t += dt; FRC.fov += 10 * dt; if (FRC.t > 0.65f) { FRC.t = 0; FRC.state = 1; FRC.restart = 1; } break;   /* Loop Wait Time */
    }
    if (FRC.state == 5 && car) {                    /* Follow 0x68c200: keeps its bearing, pulled to the leash */
        const float L = 6;
        v3 T = v3make(W[0][3], W[1][3], W[2][3]), U = v3make(0, 0, 1), D = sub(T, FRC.pos);
        float h = len(sub(D, mul(U, dot(U, D)))), h2 = h < L ? 0.9f * h + 0.1f * L : L, r = h2 / L;
        float d = len(D), d2 = d > L ? 0.7f * d + 0.3f * L : 0.9f * d + 0.1f * L;
        v3 want = sub(T, mul(d > 1e-4f ? mul(D, 1 / d) : v3make(0, 1, 0), d2));
        v3 dir = sub(want, T); float dl = len(dir);
        if (dl > 1e-4f && COLL.ntri) {               /* the static world clamps it */
            float o[3] = { T.x, T.y, T.z }, dd[3] = { dir.x / dl, dir.y / dl, dir.z / dl }, dist, n[3]; uint32_t mat;
            uint32_t saved = COLL.ndyn; COLL.ndyn = 0;
            if (nr_coll_ray(&COLL, o, dd, dl, &dist, n, &mat)) want = add(T, mul(dir, dist / dl));
            COLL.ndyn = saved;
        }
        FRC.pos = want; FRC.look = add(T, mul(U, 1.2f * r));
    }
}
static int frc_active(void) { return FRC.on && FRC.state >= 1; }

static void run_finished(float t, int success)
{
    gh_rec_stop(); gh_labels_fade(0);
    R.fin_stage = 0; R.fin_rank = 99; R.name_col = 0;
    if (!success || R.credits || !R.lv || !R.lv->score) return;
    frc_start();                                     /* CampaignLevel PostUpdate 0x7110d0: the copy, FailedRunCamera */
    sc_table tb; sc_get(SC_LOCAL, R.lv->xml, R.lv->score, &tb);
    R.fin_rank = sc_rank(&tb, t, R.score, R.biggest);
    if (R.fin_rank <= 9) {
        char p[256]; sc_ghost_shift(R.lv->xml, R.fin_rank); sc_ghost_path(p, sizeof p, R.lv->xml, R.fin_rank);
        int ok = gh_rec_save(p) == 0;
        if (ok) ghosts_dirty = 1;
        logm("  local rank %d: replay of %d frames %s", R.fin_rank + 1, gh_rec_frames(), ok ? "saved" : "NOT saved");
    } else logm("  local rank %d (outside the ten)", R.fin_rank + 1);
}

/* StuntLevel::Finish(true) (0x723f40): Play_Finish, the run into the local table (score, then the
 * biggest trick), the results */
static void stunt_results(void)
{
    if (R.rs == RS_FINISH) return;
    R.rs = RS_FINISH; R.t = 0; R.done_time = 300; R.result_sel = 0;
    warp_out_portal();
    car_sounds_off();
    nr_tricks_reset(&R.tricks);
    snd_post("Play_WarpOut", 0);                       /* (CarFinish 0x705da0) */
    ann_queue("Play_Finish");
    save_level *sl = save_level_get(&SAVE, R.lv->xml, 1);
    R.new_best = sl && R.score >= sl->score && (R.score > 0 || !sl->done);   /* a tie puts the new run first */
    if (sl) { if (R.new_best) sl->score = R.score; sl->done = 1; sl->unlocked = 1; }
    run_finished(300, 1);                            /* (a stunt entry's time is the round's length) */
    save_write(&SAVE, SAVE_PATH);
    logm("RESULT %s stunt score %d, biggest trick %d", R.lv->xml, R.score, R.biggest); fx_log();
}

static void spawn_gibs(void)
{
    ngibs = 0;
    v3 c = v3make(R.car.pos[0], R.car.pos[1], R.car.pos[2]);
    for (int i = 0; i < ngibm && ngibs < 16; i++) {
        gib_t *g = &GIBS[ngibs++];
        g->model = i;
        float *o = GIBM[i].off;
        v3 wo = v3make(R.car.rot[0][0] * o[0] + R.car.rot[0][1] * o[1] + R.car.rot[0][2] * o[2],
                       R.car.rot[1][0] * o[0] + R.car.rot[1][1] * o[1] + R.car.rot[1][2] * o[2],
                       R.car.rot[2][0] * o[0] + R.car.rot[2][1] * o[1] + R.car.rot[2][2] * o[2]);
        g->pos = add(c, wo);
        /* TV_RadialImpulse of the explosion: outwards from the centre, capped */
        v3 d = len(wo) > 0.05f ? norm(wo) : v3make(0, 0, 1);
        float imp = (i == 0 ? 6.0f : 12.0f + (i * 37 % 10));
        g->vel = add(mul(v3make(R.car.vel[0], R.car.vel[1], R.car.vel[2]), 0.6f), add(mul(d, imp), v3make(0, 0, 6.0f + (i * 13 % 6))));
        g->w = v3make(((i * 7) % 11 - 5) * 1.3f, ((i * 5) % 9 - 4) * 1.5f, ((i * 3) % 7 - 3) * 1.2f);
        memcpy(g->rot, R.car.rot, sizeof g->rot);
        g->life = 3.0f + (i % 5) * 0.2f;
    }
}
static void update_gibs(float dt)
{
    for (int i = 0; i < ngibs; i++) {
        gib_t *g = &GIBS[i];
        if (g->life <= 0) continue;
        g->life -= dt;
        g->vel.z -= 12.0f * dt;
        g->pos = add(g->pos, mul(g->vel, dt));
        float p[3] = { g->pos.x, g->pos.y, g->pos.z }, push[3], n[3];
        if (COLL.ntri && nr_coll_sphere(&COLL, p, 0.3f, push, n)) {
            g->pos = add(g->pos, v3make(push[0], push[1], push[2]));
            v3 nn = v3make(n[0], n[1], n[2]);
            float vn = dot(g->vel, nn);
            if (vn < 0) { g->vel = sub(g->vel, mul(nn, 1.35f * vn)); g->vel = mul(g->vel, 0.75f); g->w = mul(g->w, 0.7f); }
        }
        float wx = g->w.x * dt, wy = g->w.y * dt, wz = g->w.z * dt;
        for (int j = 0; j < 3; j++) {
            float x = g->rot[0][j], y = g->rot[1][j], z = g->rot[2][j];
            g->rot[0][j] += wy * z - wz * y; g->rot[1][j] += wz * x - wx * z; g->rot[2][j] += wx * y - wy * x;
        }
        /* keep the matrix orthonormal */
        v3 f = norm(v3make(g->rot[0][1], g->rot[1][1], g->rot[2][1])), u = v3make(g->rot[0][2], g->rot[1][2], g->rot[2][2]);
        u = norm(sub(u, mul(f, dot(u, f)))); v3 r = cross(f, u);
        g->rot[0][0] = r.x; g->rot[1][0] = r.y; g->rot[2][0] = r.z; g->rot[0][1] = f.x; g->rot[1][1] = f.y; g->rot[2][1] = f.z;
        g->rot[0][2] = u.x; g->rot[1][2] = u.y; g->rot[2][2] = u.z;
    }
}

static void respawn_after_death(void)
{
    respawn_point();
    rec_car_appears();                          /* CarLogic state 9: a new spawn in the recording */
    snd_post("Play_CarEngine", OBJ_CAR);            /* (CarLogic state 9: the engine only; the warp-in is the race start's) */
    snd_set_state("Crash_States", "Normal"); snd_set_state("Announcer_Crash", "Normal");   /* (Respawn 0x7014d0) */
    R.rs = RS_RACE; R.alive_t = 0;
}

/* sound emitters placed in the level: loops start when the car is near, one-shots fire once
 * as the car passes */
static int emit_no_car;                        /* the ending: no car to set off the one-shot emitters */
static void update_emitters(v3 cpos)
{
    static float acc; acc += 1;
    int slow = ((int)acc & 7) != 0;
    for (int i = 0; i < G.nsnd; i++) {
        emitter *m = &G.snd[i];
        if (slow && m->begin != 2) continue;
        if (emit_no_car && m->begin == 2) continue;
        if (!strcmp(m->ev, "Play_NitronicRushCheckpoint")) continue;      /* played by the checkpoints */
        float dx = m->p[0] - cpos.x, dy = m->p[1] - cpos.y, dz = m->p[2] - cpos.z, d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (m->begin == 2) {                     /* OneShotRadiusAudioLogic: as the car comes within the radius */
            if (m->armed && d < m->radius) {
                int s = (i % 6) + OBJ_NEMIT - 6;
                snd_set_obj_pos(OBJ_EMIT0 + s, m->p); snd_post(m->ev, OBJ_EMIT0 + s); m->armed = 0;
            } else if (!m->armed && d > m->radius * 1.5f + 5) m->armed = 1;
        } else if (m->begin) {
            if (m->slot < 0 && d < 140) {
                int s = -1;
                for (int k = 0; k < OBJ_NEMIT; k++) if (!emit_slot_used[k]) { s = k; break; }
                if (s < 0) continue;
                emit_slot_used[s] = 1; m->slot = s;
                snd_set_obj_pos(OBJ_EMIT0 + s, m->p);
                snd_post(m->ev, OBJ_EMIT0 + s);
            } else if (m->slot >= 0 && d > 170) {
                snd_stop_obj(OBJ_EMIT0 + m->slot, 400);
                emit_slot_used[m->slot] = 0; m->slot = -1;
            }
        } else {
            if (m->armed && d < 25) {
                int s = (i % 6) + OBJ_NEMIT - 6;             /* a few shared slots for one-shots */
                snd_set_obj_pos(OBJ_EMIT0 + s, m->p);
                snd_post(m->ev, OBJ_EMIT0 + s);
                m->armed = 0;
            } else if (!m->armed && d > 60) m->armed = 1;
        }
    }
}
static void stop_emitters(void)
{
    obj_sounds_off();
    for (int i = 0; i < G.nsnd; i++) if (G.snd[i].slot >= 0) { snd_stop_obj(OBJ_EMIT0 + G.snd[i].slot, 300); G.snd[i].slot = -1; }
    memset(emit_slot_used, 0, sizeof emit_slot_used);
}

/* the level's objects in view (not behind the camera, not past the fog) */
static int add_level_objects(gfx_obj *objs, int nobj, v3 eye, v3 fwd)
{
    if (!objs_on) return nobj;
    float far = L.d.light.sky_far + 300;
    /* pixels per unit of size at distance 1 (the vertical FOV as the projection works it out) */
    float fy = 2.0f * atanf(tanf((R.fov > 1 ? R.fov : 60) * 3.14159265f / 360.0f) / sqrtf(960.0f / 544.0f));
    float ppu = 272.0f / tanf(fy * 0.5f);
    for (int i = 0; i < OBJS.n && nobj < MAX_DRAWOBJ; i++) {
        const nr_obj *o = &OBJS.o[i];
        if (!o->alive || !o->visible || o->model < 0) continue;
        v3 d = sub(v3make(o->pos[0], o->pos[1], o->pos[2]), eye);
        float along = dot(d, fwd), dl = len(d);
        if (along < -o->radius || dl - o->radius > far) continue;
        if (dl > o->radius && along < dl * 0.45f - o->radius) continue;      /* well outside the view cone */
        if (o->radius * ppu < 0.6f * dl) continue;                           /* smaller than a pixel on screen */
        gfx_obj *g = &objs[nobj++];
        g->model = &OMODEL[o->model]; nr_objs_matrix(o, g->m); g->glow = 1; g->use_col = 1; memcpy(g->col, o->col, 16);
        g->phase = 0.32f * i; g->phase -= floorf(g->phase);         /* traffic / screens: fract(0.32 id) */
    }
    return nobj;
}

/* ------------------------------------------------------------------ the story's end (CampaignOutroLevel, re_E 7):
 * core_demo, the camera flies round the core while it blows apart, fade out, the credits */
static struct { float t, ct, pt, fade, bright, focus[3], amp1, amp2, ph1, ph2, hold; int phase, prompt; } OUT;
/* the cinematics' red/blue screen effect round the CORE's eye (postprocessintro's position) */
static void redblue_set(float f2, float f3)
{
    gfx_redblue.on = 1;
    gfx_redblue.e[0] = -0.133037f; gfx_redblue.e[1] = 650.920776f; gfx_redblue.e[2] = 337.262360f;
    gfx_redblue.r2 = powf(1.2f, f2); gfx_redblue.r3 = powf(1.2f, f3);
}
/* "SkipIntro" (0x72ac90 step 4): the first button held only arms it; then Triangle (the pad's Y) held 1.5 s */
static int cinema_skip(float *hold, int *armed, uint32_t buttons, float dt)
{
    if (!buttons) { *hold = 0; return 0; }
    if (!*armed) { *hold = 0; *armed = 1; return 0; }
    if (buttons & SCE_CTRL_TRIANGLE) { *hold += dt; return *hold > 1.5f; }
    *hold = 0; return 0;
}
static int begin_outro(void)
{
    car_sounds_off(); stop_emitters(); level_sounds_exit(); snd_post("Stop_CarEngine", OBJ_CAR); snd_post("Stop_Music", 0);
    loading_begin();
    gfx_redblue_init();                               /* (before the level: its programs are prewarmed with it) */
    int lok = load_race_level("core_demo");
    loading_end();
    dyn_res_reset();
    if (!lok) { logm("ending: no core_demo level"); return 0; }
    OBJS.outro = 1;
    for (int i = 0; i < G.nsnd; i++) { G.snd[i].armed = 1; G.snd[i].slot = -1; }   /* the level's own sounds, heard from the camera */
    emit_no_car = 1;
    const char *mus = music_event(15);               /* ChangeMusicMessage 15 */
    if (mus) snd_post(mus, 0);
    memset(&OUT, 0, sizeof OUT);
    OUT.focus[0] = -0.133f; OUT.focus[1] = 650.921f; OUT.focus[2] = -20.262f;
    OUT.phase = 1;                                    /* phase 0 lasts no time: the camera path starts at once */
    beat_t = 0;
    cin_outro_begin(); redblue_set(0, 46);           /* the city all red (Float3 46) */
    vehicle_unlock("avenger");                        /* 0x725500: "Complete the Story to Unlock" */
    logm("ending: the core comes down");
    return 1;
}
static void end_cinema(void)
{
    cin_end(); gfx_redblue.on = 0;
    OBJS.outro = 0; emit_no_car = 0; stop_emitters();
}
/* one frame of the ending; 1 when it is over (or skipped: hold Start 1.5 s) */
static int outro_frame(float dt, uint32_t buttons, uint32_t pressed)
{
    OUT.t += dt;
    if (OUT.fade < 1) { OUT.bright = OUT.fade; OUT.fade += 0.1f * dt; }        /* in over 10 s */
    else if (OUT.fade < 2) { OUT.bright = 1; OUT.fade = 2.5f; }
    if (cinema_skip(&OUT.hold, &OUT.prompt, buttons, dt)) return 1;
    cin_skip_update(dt, pressed != 0);
    OUT.ct += dt;
    float text = 1;
    switch (OUT.phase) {
    case 1: if (OUT.ct / 25.0f > 0.3f) { nr_objs_outro_start(&OBJS); OUT.phase = 2; OUT.pt = 0; } break;   /* 7.5 s in */
    case 2: OUT.pt += dt; if (OUT.pt > 25) { OUT.phase = 3; OUT.pt = 0; } break;
    case 3: OUT.pt += dt; OUT.bright = 1 - OUT.pt / 5; if (OUT.pt >= 5) { OUT.phase = 4; OUT.pt = 0; } break;
    case 4: OUT.pt += dt; OUT.bright = 0; text = 1 - fminf(OUT.pt / 2, 1); if (OUT.pt >= 2) return 1; break;
    }
    {   cin_view cv; cin_outro_update(dt, OUT.ct, text, &cv); redblue_set(cv.f2, cv.f3); }   /* the terminal, the red / blue */
    /* the SpinFocusCamera's path and its focus */
    float e3[3]; nr_objs_outro_camera(&OBJS, OUT.ct, dt, e3, OUT.focus);
    v3 eye = v3make(e3[0], e3[1], e3[2]), fwd = norm(sub(v3make(OUT.focus[0], OUT.focus[1], OUT.focus[2]), eye)), up = v3make(0, 0, 1);
    beat_t += dt;
    { float bk = 1.0f - fmodf(beat_t, beat_period); gfx_beat = bk < 0 ? 0 : bk > 1 ? 1 : bk; gfx_ab = fmodf(beat_t, beat_period); }
    nr_objs_frame of; memset(&of, 0, sizeof of);
    v3 rt = norm(cross(fwd, up)), uu = cross(rt, fwd);
    for (int k = 0; k < 3; k++) { of.cam_rot[k][0] = (&rt.x)[k]; of.cam_rot[k][1] = (&uu.x)[k]; of.cam_rot[k][2] = -(&fwd.x)[k]; }
    memcpy(of.cam_pos, e3, 12); memcpy(of.car_pos, e3, 12);
    for (int k = 0; k < 3; k++) { of.car_rot[k][k] = 1; of.car_half[k] = 0.1f; }
    of.beat = gfx_beat;
    nr_objs_update(&OBJS, &of, dt);
    /* the camera shake (0x6d0520): kicked by the core, x 50/s decaying 5, y 70/s decaying 7 */
    OUT.amp1 = fmaxf(OUT.amp1, of.shake); OUT.amp2 = fmaxf(OUT.amp2, of.shake);
    OUT.amp1 -= OUT.amp1 * 5 * dt; OUT.amp2 -= OUT.amp2 * 7 * dt;
    if (OUT.amp1 > 0.0001f) { OUT.ph1 += 50 * dt; float a = sinf(OUT.ph1) * OUT.amp1; fwd = norm(add(mul(fwd, cosf(a)), mul(uu, sinf(a)))); } else OUT.amp1 = OUT.ph1 = 0;
    if (OUT.amp2 > 0.0001f) { OUT.ph2 += 70 * dt; float a = sinf(OUT.ph2) * OUT.amp2; fwd = norm(add(mul(fwd, cosf(a)), mul(rt, sinf(a)))); } else OUT.amp2 = OUT.ph2 = 0;
    float lis[3] = { eye.x, eye.y, eye.z }, lf[3] = { fwd.x, fwd.y, fwd.z }, lu[3] = { 0, 0, 1 };
    snd_set_listener(lis, lf, lu);
    if (objs_on) update_obj_sounds(eye);
    update_emitters(eye);
    if (particles_ok) { run_emitters(eye, fwd, dt); particles_update(dt); }
    static gfx_obj objs[MAX_DRAWOBJ]; memset(objs, 0, sizeof objs);
    int nobj = add_level_objects(objs, 0, eye, fwd);
    gfx_post post; memset(&post, 0, sizeof post);
    gfx_time += dt;
    dyn_res_step(dt, SAVE.dyn_res);
    gfx_draw_frame(&L, eye, fwd, up, 45, objs, nobj, NULL, 0, &post);       /* corespincam: FOV 45 */
    hud_begin();
    if (OUT.bright < 1) hud_rect(0, 0, 960, 544, (unsigned)(fminf(1, 1 - OUT.bright) * 255));   /* the screen's brightness */
    cin_draw();                                       /* hud_cinema: the typed terminal lines (CityRestoring) */
    cin_skip_draw();
    ui_popup_update(dt); ui_popup_draw();             /* (the Avenger's "Vehicle Unlocked") */
    hud_end();
    vglSwapBuffers(GL_FALSE);
    return 0;
}

/* ------------------------------------------------------------------ the story's intro (CampaignIntroLevel, re_O):
 * core_demo, the camera spirals up the CORE tower, the eye turns on, the camera pulls back down the tunnel; the
 * typed terminal and the story lines; the city blue with the red spreading from the eye; then Level 1 */
static struct { int eye, arms, armed; float hold; } INTRO;
static int story_set_index(void)
{
    for (int s = 0; s < FE_NSETS; s++) if (strstr(FE_SETS[s].file, "Campaign")) return s;
    return 0;
}
static int begin_intro(void)
{
    car_sounds_off(); stop_emitters(); level_sounds_exit(); snd_post("Stop_CarEngine", OBJ_CAR); snd_post("Stop_Music", 0);
    loading_begin();
    gfx_redblue_init();
    int lok = load_race_level("core_demo");
    loading_end();
    dyn_res_reset();
    if (!lok) { logm("intro: no core_demo level"); return 0; }
    OBJS.outro = 0;                                    /* (CoreExploder stays in state 0) */
    for (int i = 0; i < G.nsnd; i++) { G.snd[i].armed = 1; G.snd[i].slot = -1; }
    emit_no_car = 1;
    const char *mus = music_event(14);                 /* ChangeMusicMessage 14: Play_IntroCutscene */
    if (mus) snd_post(mus, 0);
    memset(&INTRO, 0, sizeof INTRO);
    INTRO.eye = nr_objs_find(&OBJS, "coreeye"); INTRO.arms = nr_objs_find(&OBJS, "CoreArmsUpper");
    cin_intro_begin(); redblue_set(0, 0);
    beat_t = 0;
    logm("intro: up the CORE tower (eye %d, arms %d)", INTRO.eye, INTRO.arms);
    return 1;
}
/* one frame of the intro; 1 when it is over (or skipped) */
static int intro_frame(float dt, uint32_t buttons, uint32_t pressed)
{
    int skip = cinema_skip(&INTRO.hold, &INTRO.armed, buttons, dt);
    cin_skip_update(dt, pressed != 0);
    cin_view cv; int done = cin_intro_update(dt, &cv);
    v3 eye = v3make(cv.eye[0], cv.eye[1], cv.eye[2]), fwd = v3make(cv.fwd[0], cv.fwd[1], cv.fwd[2]), up = v3make(cv.up[0], cv.up[1], cv.up[2]);
    beat_t += dt;
    { float bk = 1.0f - fmodf(beat_t, beat_period); gfx_beat = bk < 0 ? 0 : bk > 1 ? 1 : bk; gfx_ab = fmodf(beat_t, beat_period); }
    nr_objs_frame of; memset(&of, 0, sizeof of);
    v3 rt = norm(cross(fwd, up));
    for (int k = 0; k < 3; k++) { of.cam_rot[k][0] = (&rt.x)[k]; of.cam_rot[k][1] = (&up.x)[k]; of.cam_rot[k][2] = -(&fwd.x)[k]; }
    memcpy(of.cam_pos, cv.eye, 12); memcpy(of.car_pos, cv.eye, 12);
    for (int k = 0; k < 3; k++) { of.car_rot[k][k] = 1; of.car_half[k] = 0.1f; }
    of.beat = gfx_beat;
    nr_objs_update(&OBJS, &of, dt);
    if (INTRO.eye >= 0) OBJS.o[INTRO.eye].col[3] = cv.eye_glow;            /* FlickerOnHack */
    if (INTRO.arms >= 0) OBJS.o[INTRO.arms].f[1] = cv.lightning;           /* CoreLightningLogic's reach */
    snd_set_listener(cv.eye, cv.fwd, cv.up);
    if (objs_on) update_obj_sounds(eye);
    update_emitters(eye);
    if (particles_ok) { run_emitters(eye, fwd, dt); particles_update(dt); }
    static gfx_obj objs[MAX_DRAWOBJ]; memset(objs, 0, sizeof objs);
    int nobj = add_level_objects(objs, 0, eye, fwd);
    redblue_set(cv.f2, cv.f3);
    gfx_post post; memset(&post, 0, sizeof post);
    post.gain = cv.bright > 1 ? cv.bright - 1 : 0;    /* the eye's flashes: the scene at 1.5 */
    gfx_time += dt;
    dyn_res_step(dt, SAVE.dyn_res);
    gfx_draw_frame(&L, eye, fwd, up, 45, objs, nobj, NULL, 0, &post);
    hud_begin();
    if (cv.bright < 1) hud_rect(0, 0, 960, 544, (unsigned)(fminf(1, 1 - cv.bright) * 255));
    cin_draw();
    cin_skip_draw();
    hud_end();
    vglSwapBuffers(GL_FALSE);
    return done || skip;
}

static void fmt_points(char *b, size_t n, int v)     /* 5,934 */
{
    if (v >= 1000000) snprintf(b, n, "%d,%03d,%03d", v / 1000000, (v / 1000) % 1000, v % 1000);
    else if (v >= 1000) snprintf(b, n, "%d,%03d", v / 1000, v % 1000);
    else snprintf(b, n, "%d", v);
}
static void fmt_time(char *b, size_t n, float t)    /* FormatTime 0x723b60: truncated, "%0f" of the fraction cut to 2 places */
{
    if (t < 0) t = 0;
    int s_ = (int)t;
    char f[16]; snprintf(f, sizeof f, "%f", fmod(t, 1.0)); f[4] = 0;
    snprintf(b, n, "%02d:%02d.%s", s_ / 60, s_ % 60, f + 2);
}

/* data/credits.txt: the first line is the level; then "Name$Role|Role|..." or a line of its own */
static struct { char whole[96], name[96], roles[256]; } CR[48]; static int NCR;
static void parse_credits(const char *t)
{
    NCR = 0;
    const char *p = t ? strchr(t, '\n') : NULL;            /* (skip the level line) */
    while (p && *p && NCR < 48) {
        p++;
        const char *e = strchr(p, '\n'); if (!e) e = p + strlen(p);
        char ln[96 + 160]; int k = (int)(e - p); if (k > 95 + 160) k = 95 + 160;
        memcpy(ln, p, k); ln[k] = 0;
        while (k > 0 && (ln[k - 1] == '\r' || ln[k - 1] == ' ')) ln[--k] = 0;
        if (k > 0) {
            char *dl = strchr(ln, '$');
            memset(&CR[NCR], 0, sizeof CR[0]);
            if (dl && dl > ln && dl[1]) { *dl = 0; snprintf(CR[NCR].name, sizeof CR[0].name, "%.95s", ln); snprintf(CR[NCR].roles, sizeof CR[0].roles, "%.255s", dl + 1); }
            else { if (dl) *dl = 0; snprintf(CR[NCR].whole, sizeof CR[0].whole, "%.95s", ln[0] == '|' ? "" : ln); }
            NCR++;
        }
        p = *e ? e : NULL;
    }
}
/* a credits line: words a fixed gap apart (the Text component's Word Gap, 0.05 of its scale) */
static void credit_line(float x, float y, float sz, unsigned col, const char *s)
{
    char w[64];
    while (*s) {
        while (*s == ' ') s++;
        int n = 0; while (*s && *s != ' ' && n < 63) w[n++] = *s++;
        w[n] = 0;
        if (n) x += hud_text(HUD_FONT_MAIN, x, y, sz, col, HUD_LEFT, w) + 14.4f;
    }
}

/* the race HUD: the game's bar meshes, heat meter and text */
static void draw_race_hud(void)
{
    char b[64];
    if (R.credits) {
        /* CreditsLevel2 (docs/re/re_L.md): one entry at a time, chosen by how far the car is down the
         * tunnel (start to finish, 4680 units); it fades in over 0.3125 of its stretch, holds, fades
         * out. Name at the top left, the roles under it; left-aligned, hemihead at 0.3 x the screen
         * width (k 0.5625), words 0.05 of that apart. Frozen once the car is through. */
        static int cidx = -1; static float calpha;
        if (R.rs == RS_RACE && NCR && G.has_start && G.nfinish) {
            v3 st = v3make(G.start[0], G.start[1], G.start[2]), en = v3make(G.finish[0][0], G.finish[0][1], G.finish[0][2]);
            float D = len(sub(en, st)), d = len(sub(en, v3make(R.car.pos[0], R.car.pos[1], R.car.pos[2])));
            float pr = D > 1 ? 1 - d / D : 0; pr = pr < 0 ? 0 : pr > 1 ? 1 : pr;
            float f = NCR * pr; int idx = (int)f; if (idx >= NCR) idx = NCR - 1;
            float u = f - idx;
            if (u < 0.3125f) calpha = sinf(u / 0.3125f);
            else if (u > 0.6875f) calpha = sinf((1 - u) / 0.3125f);
            cidx = idx;
        } else if (R.rs == RS_COUNT) { cidx = -1; calpha = 0; }
        if (cidx >= 0 && cidx < NCR && calpha > 0.004f) {
            unsigned col = 0xFFFFFF00 | (unsigned)(fminf(1, calpha) * 255);
            const float k = 0.5625f, sz = 72 * k;
            if (CR[cidx].whole[0]) credit_line(48, 81.6f, sz, col, CR[cidx].whole);
            if (CR[cidx].name[0]) credit_line(48, 81.6f, sz, col, CR[cidx].name);
            const char *r = CR[cidx].roles; float y = 136;
            while (*r) {
                char ln[96]; int n = 0;
                while (*r && *r != '|' && n < 95) ln[n++] = *r++;
                ln[n] = 0; if (*r == '|') r++;
                credit_line(48, y, sz, col, ln); y += sz;
            }
        }
    } else if (SAVE.show_hud && R.rs != RS_FINISH) {
        float h = R.heat / HEAT_MAX;
        unsigned hc = 0xFFFFFFE6;
        if (R.car_cooler) {                         /* CarCooler: the HUD pulses cyan to magenta */
            float k = 0.5f + 0.5f * sinf(R.t * 5.0f);
            hc = (unsigned)(k * 255) << 24 | (unsigned)((1 - 0.8f * k) * 255) << 16 | 0xFF00u | 0xE6u;
        }
        hud_mesh(HUD_MESH_MAIN, hud_tex("hud"), hc);
        hud_heat_meter(hud_tex("hud_heat"), h, h * h * 0.9f);
        if (h > 0.05f) {
            hud_additive(1);
            float fl = 0.6f + 0.4f * sinf(R.t * 23.0f);
            hud_mesh(HUD_MESH_HEATLINES, hud_tex("hud_flame"), 0x40FF6000 | (unsigned)(fminf(1, h * 1.3f) * fl * 255));
            hud_additive(0);
        }
        /* along the bottom of the screen, as on the PC */
        if (R.stunt) {                               /* MM:SS of the time left, truncated */
            int sl_ = (int)fmaxf(0, R.stunt_left);
            snprintf(b, sizeof b, "%02d:%02d", sl_ / 60, sl_ % 60);
        } else fmt_time(b, sizeof b, R.rs == RS_FINISH ? R.done_time : R.race_t);
        hud_text(HUD_FONT_MAIN, 480, 505, 30, 0xFFFFFFE6, HUD_CENTER, b);
        float mph = nr_car_mph(&R.car);
        snprintf(b, sizeof b, "%d", (int)(mph + 0.5f));
        hud_text(HUD_FONT_MAIN, 746, 483, 24, 0xFFFFFFE6, HUD_RIGHT, b);
        hud_text(HUD_FONT_MAIN, 756, 488, 16, 0xFFFFFFE6, HUD_LEFT, "MPH");
        fmt_points(b, sizeof b, R.score);
        hud_text(HUD_FONT_MAIN, 166, 488, 16, 0xFFFFFFE6, HUD_LEFT, "Score");
        hud_text(HUD_FONT_MAIN, 228, 483, 24, 0xFFFFFFE6, HUD_LEFT, b);
        if (h > 0.75f && R.rs == RS_RACE) {
            float pulse = 0.5f + 0.5f * sinf(R.t * 12.0f);
            hud_text(HUD_FONT_MAIN, 480, 440, 26, 0xFF402000 | (unsigned)(pulse * 255), HUD_CENTER, "WARNING!");
        }
    }
    if (R.trick_t > 0 && R.rs == RS_RACE) {
        float a = R.trick_t > 0.4f ? 1 : R.trick_t / 0.4f;
        unsigned c = 0xFFFFFF00 | (unsigned)(a * 240);
        hud_text(HUD_FONT_MAIN, 14, 92, 40, c, HUD_LEFT, R.trick_head);
        hud_text_lines(HUD_FONT_MAIN, 14, 138, 25, 1.15f, c, HUD_LEFT, R.trick);
    }
    tutorial_draw();
    if (R.msg_t > 0 && R.rs == RS_RACE) {
        float a = R.msg_t > 0.3f ? 1 : R.msg_t / 0.3f;
        hud_text(HUD_FONT_TITLE, 480, 140, 44, 0xA0FFA000 | (unsigned)(a * 255), HUD_CENTER, R.msg);
    }
    /* countdown (state 1, 0x712a10): countdown_3 / _2 / _1 made as the clock passes -3, -2, -1 (0.77 s apart: it runs
     * 1.3 times as fast), countdown_rush at GO. Each (LevelStartTextLogic 0x68e0c0) sits in the middle of the screen,
     * 0.4 x the width x its Text Scale (0.2; RUSH 0.75) either way, for its Time To Interp (1 s; RUSH 1.75 s), its
     * Float1 going 0 -> 2: countdown.fx sweeps it in and out along its height map */
    if (R.rs == RS_COUNT || R.rs == RS_RACE) {
        float ct = R.rs == RS_COUNT ? (R.cd - CD_T0) / CD_RATE : -CD_T0 / CD_RATE + R.race_t;
        static const struct { const char *img, *hgt; float at, ts, life; } CDN[4] = {
            { "countdown_3", "countdown_3height", 3.0f / CD_RATE, 0.2f, 1.0f }, { "countdown_2", "countdown_2height", 4.0f / CD_RATE, 0.2f, 1.0f },
            { "countdown_1", "countdown_1height", 5.0f / CD_RATE, 0.2f, 1.0f }, { "countdown_rush", "countdown_rushheight", 6.0f / CD_RATE, 0.75f, 1.75f } };
        for (int k = 0; k < 4 && !race_quiet; k++) {
            float age = ct - CDN[k].at;
            if (k == 3 && R.rs == RS_RACE) age = R.race_t;       /* (RUSH: from GO itself) */
            if (k == 3 && R.rs == RS_COUNT) continue;
            if (age < 0 || age >= CDN[k].life) continue;
            hud_countdown(hud_tex(CDN[k].img), hud_tex(CDN[k].hgt), 480, 272, 0.8f * 960 * CDN[k].ts, 2.0f * age / CDN[k].life);
        }
        if (R.rs == RS_COUNT && !race_quiet) {         /* the level's name: in over the clock's -5.5 .. -4.5, out over -2.5 .. -2 */
            float na = R.cd < -2.5f ? R.cd + 5.5f : -2.0f * (R.cd + 2.0f);
            na = na < 0 ? 0 : na > 1 ? 1 : na;
            unsigned col = 0xC0E8FF00 | (unsigned)(na * 0xE0);
            if (na > 0.004f) {
                /* (0x7158dd: 0.65 of the height up from the bottom; the PC's footage: a third of the way down) */
                hud_text(HUD_FONT_MAIN, 480, 150, 26, col, HUD_CENTER, R.lv ? R.lv->name : G.name);
                if (R.lv && !R.lv->wings && !R.credits) hud_text(HUD_FONT_MAIN, 480, 182, 23, col, HUD_CENTER, "(Wings Disabled)");   /* 0x715a21: 0.45 / 0.5 of the name */
            }
        }
    }
}

/* ---- the pause menu (0x6e9500) and the PC's confirmation boxes, also behind the results' Quit, Restart and Continue */
static const char *PAUSE_ITEMS[8] = { "Resume", "Restart", "Achievements", "High Scores", "Controls", "Options", "Main Menu", "Quit Game" };
enum { PM_NONE = -1, PM_MENU, PM_DLG, PM_SCORES, PM_FRONT };
enum { DK_RESTART, DK_MAINMENU, DK_QUITGAME, DK_QUIT, DK_GHOSTS };
enum { PA_NONE, PA_RESUME, PA_RESTART, PA_MENU, PA_QUIT_APP, PA_NEXT };
static struct { int state, sel, kind, after, from_results, cats[3]; ui_dlg d; } PM = { PM_NONE };
/* "Select Ghosts to Challenge" comes before an Arcade restart or the next level when the ghosts race */
static int pm_ghost_step(void) { return !R.story && !R.credits && SAVE.ghosts && R.lv && R.lv->score && !g_autoplay; }
static int arcade_has_next(void)            /* 0x6f8c30 */
{
    if (R.credits || R.story || R.stunt || R.set < 0) return 0;
    fe_set *fs = &FE_SETS[R.set];
    if (R.level + 1 >= fs->n || !fs->lv[R.level + 1].installed) return 0;
    save_level *nx = save_level_get(&SAVE, fs->lv[R.level + 1].xml, 0);
    return fs->lv[R.level + 1].unlocked || (nx && nx->unlocked);
}
static void pm_dialog(int kind)
{
    PM.kind = kind; PM.state = PM_DLG;
    switch (kind) {
    case DK_RESTART: ui_dlg_open(&PM.d, "Restart Current Level?", 2, "Yes, Restart", "No", NULL); break;
    case DK_MAINMENU: ui_dlg_open(&PM.d, "Quit To Main Menu?", 2, "Yes", "No", NULL); break;
    case DK_QUITGAME: ui_dlg_open(&PM.d, "Quit Game?", 2, "Yes", "No", NULL); break;
    case DK_QUIT: ui_dlg_open(&PM.d, "Quit?", 3, "Main Menu", "Quit Game", "Back"); break;
    case DK_GHOSTS: {
        int n = 0; const char *it[3] = { 0 };
        it[n] = "Local"; PM.cats[n++] = SC_LOCAL;
        if (sc_has(SC_DEVELOPER, R.lv->xml)) { it[n] = "Developer"; PM.cats[n++] = SC_DEVELOPER; }
        it[n] = "Back"; PM.cats[n++] = -1;
        ui_dlg_open(&PM.d, "Select Ghosts to Challenge", n, it[0], it[1], it[2]);
        for (int k = 0; k < n; k++) if (PM.cats[k] == front_ghost_cat()) PM.d.sel = k;
        break; }
    }
}
static int pm_dialog_result(int r)
{
    if (r == -1) return PA_NONE;
    int k = PM.kind;
    if (k == DK_GHOSTS && r >= 0 && PM.cats[r] >= 0) { front_set_ghost_cat(PM.cats[r]); ghosts_dirty = 1; PM.state = PM_NONE; return PM.after; }
    if (k == DK_RESTART && r == 0) {
        if (pm_ghost_step()) { PM.after = PA_RESTART; pm_dialog(DK_GHOSTS); return PA_NONE; }
        PM.state = PM_NONE; return PA_RESTART;
    }
    if ((k == DK_MAINMENU && r == 0) || (k == DK_QUIT && r == 0)) { PM.state = PM_NONE; return PA_MENU; }
    if ((k == DK_QUITGAME && r == 0) || (k == DK_QUIT && r == 1)) { PM.state = PM_NONE; return PA_QUIT_APP; }
    PM.state = PM.from_results ? PM_NONE : PM_MENU;  /* No / Back */
    return PA_NONE;
}
static int pause_update(float dt, const fe_input *in, int pause_pressed)
{
    switch (PM.state) {
    case PM_MENU: {
        if (pause_pressed || in->back) return PA_RESUME;
        fe_input I = *in; in = &I;                  /* (touch: a button chooses it) */
        for (int i = 0; i < 8; i++) if (ui_hit(in, 480, 128 + i * 50, 300, 46)) { PM.sel = i; I.accept = 1; }
        if (in->up) { PM.sel = (PM.sel + 7) % 8; snd_post("Play_MenuMoveLowShort", 0); }
        if (in->down) { PM.sel = (PM.sel + 1) % 8; snd_post("Play_MenuMoveLowShort", 0); }
        if (in->accept) {
            /* the action switch's voices (0x4b Achievements, 0x32 High Scores, 2, 3, 0x50 Main Menu, 5 Quit Game) */
            static const char *const PV[8] = { NULL, NULL, "Play_Achievements", "Play_HighScores", "Play_Controls", "Play_Options", "Play_MainMenu", "Play_Quit" };
            if (PV[PM.sel]) snd_post(PV[PM.sel], 0);
            if (PM.sel == 1 || PM.sel == 3 || PM.sel >= 6) snd_post("Play_MenuMoveLowShort", 0);   /* (a dialog's, the table's first button) */
            switch (PM.sel) {
            case 0: return PA_RESUME;
            case 1: pm_dialog(DK_RESTART); break;
            case 2: front_overlay(FE_ACHIEVEMENTS); PM.state = PM_FRONT; break;
            case 3: res_open(R.lv->xml, R.lv->score, 1, 99, 0, 0, 0); PM.state = PM_SCORES; break;
            case 4: front_overlay(FE_CONTROLS); PM.state = PM_FRONT; break;
            case 5: front_overlay(FE_OPTIONS); PM.state = PM_FRONT; break;
            case 6: pm_dialog(DK_MAINMENU); break;
            case 7: pm_dialog(DK_QUITGAME); break;
            }
        }
        break; }
    case PM_DLG: return pm_dialog_result(ui_dlg_update(&PM.d, in));
    case PM_SCORES: if (res_update(dt, in) == RES_BACK) { PM.state = PM_MENU; snd_post("Play_MenuMoveLowShort", 0); } break;
    case PM_FRONT: {
        int a, b;
        if (front_update(dt, in, &a, &b) == FE_DONE) { PM.state = PM_MENU; front_apply_volumes(); OBJS.advanced = SAVE.adv_controls; snd_post("Play_MenuMoveLowShort", 0); }
        break; }
    }
    return PA_NONE;
}
static void pause_draw(void)
{
    if (PM.state == PM_SCORES) { res_draw(1); return; }
    if (PM.state == PM_FRONT) { front_draw(); return; }
    hud_rect(0, 0, 960, 544, 0x00000090);
    if (PM.state == PM_DLG) { ui_dlg_draw(&PM.d); return; }        /* (the box takes the menu's place) */
    hud_text(HUD_FONT_TITLE, 480, 28, 52, 0xFFFFFFFF, HUD_CENTER, "Paused");
    for (int i = 0; i < 8; i++) ui_button(480, 128 + i * 50, 300, 44, PAUSE_ITEMS[i], i == PM.sel, 1);
}

/* ------------------------------------------------------------------ main */
/* ---- the end of a run: name entry and results (re_N 11.3 / 11.4; positions for 960x544) */
static void results_open(void)
{
    float t = R.stunt ? 300.0f : R.done_time;
    if (R.fin_rank <= 9) sc_insert_local(R.lv->xml, R.lv->score, R.fin_rank, SAVE.initials, t, R.score, R.biggest);   /* 0x6f4ae0 */
    save_write(&SAVE, SAVE_PATH);
    R.fin_stage = 2;
    res_open(R.lv->xml, R.lv->score, 0, R.fin_rank, t, R.score, R.biggest);
    PM.state = PM_NONE; PM.from_results = 1;
    rank_voice(R.fin_rank);
    logm("  results: %s, local rank %d", SAVE.initials, R.fin_rank + 1);
}
/* a menu item's text: Scale = 9 x half-height x H, capitals centred (0x6a7ae0); hemihead line = Scale x 72/512 */
static void menu_text(int font, float cx, float cy, float half_h, unsigned rgba, int align, const char *s)
{
    float scale = 9 * half_h * 544;
    hud_text(font, cx, cy - 0.642857f * half_h * 544, scale * 0.140625f, rgba, align, s);
}
static void draw_finish(void)
{
    float a = fminf(1, R.t / 0.6f);
    if (!R.lv->score) {                           /* the tutorials: the screen fades out */
        float k = fminf(1, R.t / 2.5f);
        hud_rect(0, 0, 960, 544, (unsigned)(k * 255));
        return;
    }
    if (R.fin_stage == 0) return;
    /* HudBlackSquare (Color1 alpha 0.75): name entry fades it in to 0.7 over 7 frames */
    if (R.fin_stage == 1) hud_rect(0, 0, 960, 544, (unsigned)(fminf(1, (R.t - 1.7f) / 0.117f) * 0.7f * 255));
    if (R.fin_stage == 1) {
        menu_text(HUD_FONT_MAIN, 480, 190.4f, 0.05f, 0xFFFFFFFF, HUD_CENTER, "Enter Name...");
        for (int c = 0; c < 3; c++) {
            float x = 480 + (c - 1) * 35.36f;
            unsigned arrow = R.name_col == c ? 0x9FE8FFFF : 0xFFFFFFB0;
            menu_text(HUD_FONT_MAIN, x, 244.8f, 0.0325f, arrow, HUD_CENTER, "^");
            char l[2] = { SAVE.initials[c] ? SAVE.initials[c] : 'A', 0 };
            ui_button(x, 284.0f, 32.6f, 35.4f, l, R.name_col == c, 1);
            menu_text(HUD_FONT_MAIN, x, 323.1f, 0.0325f, arrow, HUD_CENTER, "v");
        }
        ui_button(844.4f, 499.1f, 176.8f, 35.4f, "Accept", R.name_col == 3, 1);
        hud_text(HUD_FONT_MAIN, 24, 492, 16, 0xC0D0E0D0, HUD_LEFT, "\x17\x18 Letter    \x19\x1a Move    \x11 Next");
        return;
    }
    (void)a;
    res_draw(1);
}

static void fatal_screen(void)
{
    io_shutdown(); g_log = 0;
    for (int i = 0; i < 600; i++) { glClearColor(0.5f, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT); vglSwapBuffers(GL_FALSE); }
    sceKernelExitProcess(0);
}

/* Dynamic Resolution (Options, on by default): when frames start taking three of the screen's
 * refreshes (under 30 fps) the 3D scene is drawn smaller, 5% at a time down to 70%, and it grows
 * back after a calm spell; at 30 fps and above nothing changes, so the picture stays full size
 * wherever the Vita keeps up. A step up that brings the slow frames back makes the next try wait
 * longer (3 s doubling to 20 s). The HUD and menus are always full size. */
static struct { float scale, win, hold, back, since_up, calm_t; int slow; } DYN = { 1, 0, 0, 3, 99, 0, 0 };
static void dyn_res_reset(void) { DYN.scale = 1; DYN.win = DYN.hold = DYN.calm_t = 0; DYN.back = 3; DYN.since_up = 99; DYN.slow = 0; }
static void dyn_res_step(float dt, int on)
{
    if (!on) { DYN.scale = 1; gfx_scene_scale = 1; return; }
    DYN.win += dt; DYN.since_up += dt;
    if (dt > 0.040f && dt < 0.099f) DYN.slow++;           /* (0.1: a load or a hitch, not the drawing) */
    if (DYN.hold > 0) DYN.hold -= dt;
    if (DYN.win >= 0.5f) {
        if (DYN.slow >= 2) {
            if (DYN.since_up < 2.5f) DYN.back = fminf(DYN.back * 2, 20);
            if (DYN.scale > 0.71f) DYN.scale -= 0.05f;
            DYN.hold = DYN.back; DYN.calm_t = 0;
        } else {
            DYN.calm_t += DYN.win;
            if (DYN.calm_t > 30) DYN.back = 3;
            if (DYN.hold <= 0 && DYN.scale < 0.99f && DYN.calm_t >= 2) {
                DYN.scale = fminf(1, DYN.scale + 0.05f); DYN.since_up = 0; DYN.hold = 2;
            }
        }
        DYN.win = 0; DYN.slow = 0;
        static float logged = 1;
        if (fabsf(DYN.scale - logged) > 0.01f) { logm("  dynamic resolution: 3D scene at %.0f%%", DYN.scale * 100); logged = DYN.scale; }
    }
    gfx_scene_scale = DYN.scale;
}

/* the autoplay bench (see the race's drawing): its state and its steps */
static int bench_mode = -1, bench_frames, bench_done[2], bench_rs, bench_on;
static float bench_ms; static uint64_t bench_busy0;
static const char *const BENCH_NAMES[] = { "everything", "no level objects", "objects: no glow pass", "objects: no solid pass",
    "no air mines", "no CORE building", "3D scene at 70%", "no screen effects", "no speed blur", "no glowing level parts",
    "no level geometry", "everything (again)" };
enum { BENCH_N = 12, BENCH_SETTLE = 15, BENCH_FRAMES = 70 };
static void bench_step(float dt)
{
    int bi = R.lv ? (!strcmp(R.lv->xml, "Campaign_Level1") ? 0 : !strcmp(R.lv->xml, "Campaign_Level5") ? 1 : -1) : -1;
    if (bench_mode < 0) {
        /* (autoplay, without the switch file too: Level 5 at 2 s, its heaviest view, the CORE city and the mines) */
        if (bi < 0 || bench_done[bi] || R.rs != RS_RACE || R.race_t < (bi == 1 ? 2.0f : 10.0f) || (bi == 0 && !bench_on)) return;
        bench_done[bi] = 1; bench_mode = 0; bench_frames = 0; bench_rs = R.rs; R.rs = RS_PAUSE;
        vglWaitVblankStart(GL_FALSE);
        logm("BENCH %s: the same view drawn %d frames each way, vsync off", R.lv->xml, BENCH_FRAMES - BENCH_SETTLE);
        return;
    }
    uint64_t busy = T_all_busy + T_logic + T_draw + T_hud;
    bench_frames++;
    if (bench_frames == BENCH_SETTLE) { bench_ms = 0; bench_busy0 = busy; }
    else if (bench_frames > BENCH_SETTLE) bench_ms += dt * 1000;
    if (bench_frames < BENCH_FRAMES) return;
    int n = BENCH_FRAMES - BENCH_SETTLE;
    logm("BENCH   %-28s %6.2f ms a frame (%5.1f fps), game thread busy %5.2f ms", BENCH_NAMES[bench_mode], bench_ms / n, 1000.0f * n / bench_ms,
         (busy - bench_busy0) / 1000.0f / n);
    bench_frames = 0;
    if (++bench_mode >= BENCH_N) {
        bench_mode = -1; R.rs = bench_rs;
        GSW.no_bloom = GSW.no_wide = GSW.no_post = 0; gfx_scene_scale = 1; gfx_dbg = 0;
        vglWaitVblankStart(GL_TRUE);
    }
}

int main(void)
{
    t0 = now_us();
    sceIoMkdir(DATA, 0777);
    io_init(DATA "boot.log"); g_log = 1;
    logm("Nitronic Rush Vita, build " NR_BUILD_LABEL " (" __DATE__ " " __TIME__ ")");
#ifndef NR_PC
    /* the clocks the user chose: CPU 444, bus 222, GPU 222 MHz - the most a retail game may ask for
     * (homebrew starts at CPU 333 / GPU 111); the GPU crossbar stays at its default */
    int cpu_set = scePowerSetArmClockFrequency(444), bus_set = scePowerSetBusClockFrequency(222), gpu_set = scePowerSetGpuClockFrequency(222);
    logm("clocks: CPU %d MHz (0x%08x), bus %d MHz (0x%08x), GPU %d MHz (0x%08x), GPU crossbar %d MHz",
         scePowerGetArmClockFrequency(), cpu_set, scePowerGetBusClockFrequency(), bus_set, scePowerGetGpuClockFrequency(), gpu_set,
         scePowerGetGpuXbarClockFrequency());
#endif
    GSW.no_mips = has_switch("no_mips"); GSW.rgba = has_switch("rgba_tex");
    GSW.no_fog = has_switch("no_fog"); GSW.no_cutout = has_switch("no_cutout");
    GSW.no_cull = has_switch("no_cull"); GSW.no_tex_cache = has_switch("no_tex_cache");
    GSW.discard = has_switch("discard_cutout"); GSW.no_transfer_wait = has_switch("no_transfer_wait");
    GSW.cg = has_switch("cg_shaders");
    GSW.no_post = has_switch("no_post"); GSW.no_bloom = has_switch("no_bloom");
    int sw_no_bloom = GSW.no_bloom;
    int autoplay = has_switch("autoplay"), no_sound = has_switch("no_sound");
    bench_on = has_switch("bench");
    int nocap = has_switch("nocap");
    chase_wanted = has_switch("chase");
#ifdef NR_PC
    if (getenv("CHASE")) chase_wanted = 1;
    if (getenv("NODRAW")) nodraw = 1;
    if (getenv("AUDIT")) audit_on = 1;
    if (getenv("HAZLOG")) haz_on = 1;
#endif
    g_autoplay = autoplay;
    test_shots = autoplay && has_switch("autoplay_shots");
    if (test_shots) sceIoMkdir(DATA "test_shots", 0777);
    SceGxmMultisampleMode ms = has_switch("msaa4x") ? SCE_GXM_MULTISAMPLE_4X : has_switch("msaa2x") ? SCE_GXM_MULTISAMPLE_2X : SCE_GXM_MULTISAMPLE_NONE;
    logm("switches: autoplay=%d no_sound=%d no_mips=%d rgba_tex=%d no_fog=%d no_cull=%d msaa=%d no_post=%d no_bloom=%d", autoplay, no_sound,
         GSW.no_mips, GSW.rgba, GSW.no_fog, GSW.no_cull, (int)ms, GSW.no_post, GSW.no_bloom);
    gfx_set_log(logm);
    /* the game thread (logic, physics and the GL calls) keeps core 0 to itself: the sound mixer
     * and the file loader live on the other cores (see snd_init) */
    int pin = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);

    vglInitExtended(0, 960, 544, 32 * 1024 * 1024, ms);
    vglWaitVblankStart(GL_TRUE);
    logm("vitaGL up, free memory %.1f MB", vglMemFree(VGL_MEM_ALL) / 1048576.0);
    int r = nr_pak_open(&pak, DATA "nitronic.idx");
    if (r) { logm("cannot open " DATA "nitronic.idx (%d)", r); fatal_screen(); }
    logm("data: %d packs, %u files", pak.nf, pak.count);
    int hud_ok = hud_init(&pak) == 0;
    logm(hud_ok ? "HUD ready" : "HUD fonts missing");
    /* the loading screen while the shaders are built (the first start after an update takes about
     * 25 s) and the menu, the car and the sounds load, rather than a black screen */
    if (hud_ok) loading_begin();
    if (gfx_init()) { logm("no usable shaders"); fatal_screen(); }
    {   /* the HUD's and the popups' textures now, not when first shown in a race (a hitch) */
        static const char *PRE[] = { "hud", "hud_heat", "hud_flame", "countdown_1", "countdown_2", "countdown_3", "countdown_rush",
            "countdown_1_line", "countdown_2_line", "countdown_3_line", "countdown_rush_line", "achievement_unlocked", "vehicle_unlocked",
            "new_level_unlocked", "trophy_gold", "trophy_silver", "trophy_bronze", "loading2", "menu_button", "menu_button2",
            "menu_button_new", "menu_toggle", "menu_slider", "menu_sliderbar", "rush4" };
        for (unsigned k = 0; k < sizeof PRE / sizeof PRE[0]; k++) hud_tex(PRE[k]);
    }
    if (autoplay) SAVE_PATH = DATA "save_autoplay.txt";
    save_load(&SAVE, SAVE_PATH);
    if (!no_sound) {
        int sr = snd_init(&pak, 1);
        logm(sr ? "sound: no sound data (%d)" : "sound engine running", sr);
    }
    {
        int mc = -1, lc = -1; snd_cores(&mc, &lc);
        logm("cores: game 0%s, sound mixer %d, file loader %d%s", pin < 0 ? " (could not pin)" : "", mc, lc,
             lc == 3 ? " (system core, CapUnlocker)" : "");
    }
    if (front_init(&pak, &SAVE, SAVE_PATH)) logm("front end: no level sets in the data (old packs?)");
    logm(cin_init(&pak) == 0 ? "cinematics: texts ready" : "cinematics: no ui/cinema_config.txt (old packs?)");
    logm("front end: %d level sets, %d achievements", FE_NSETS, FE_NACH);
    ach_unlock("Better Handling");
    { uint32_t csz; credits_text = nr_pak_read(&pak, "ui/credits.txt", &csz); parse_credits(credits_text); }                      /* "Play using a gamepad." - always true here */
    if (!gfx_load_nrl(&CAR, &pak, "models/car.nrl", "car")) logm("no car model");
    gfx_load_nrl(&FLAME, &pak, "models/flame.nrl", "flame");
    if (nr_pak_find(&pak, "models/portal.nrl", NULL, NULL, NULL) >= 0) gfx_load_nrl(&PORTALM, &pak, "models/portal.nrl", "portal");
    for (int k = 0; k < 4; k++) {                    /* the bolts (the level objects' own copies: o_sparkdead1-4) */
        char sp_[48]; snprintf(sp_, sizeof sp_, "models/o_sparkdead%d.nrl", k + 1);
        if (gfx_load_nrl(&SPARKM[sparkm_n], &pak, sp_, "spark")) sparkm_n++;
    }
    particles_ok = particles_init(&pak) == 0;
    { uint32_t tsz; if (nr_pak_find(&pak, "textures/tire_trail.nrt", NULL, NULL, &tsz) >= 0) {   /* (the tyre tracks' textures: v44 data) */
        trk_tex = gfx_texture(&pak, "tire_trail"); trk_rainbow = gfx_texture(&pak, "trail_rainbow"); } }
    logm(particles_ok ? "particles ready" : "particles: no textures (old packs)");

    {
        uint32_t gsz; char *gt = nr_pak_read(&pak, "models/gibs.txt", &gsz);
        for (char *ln = gt ? strtok(gt, "\n") : NULL; ln && ngibm < 16; ln = strtok(NULL, "\n")) {
            char nm[32], bp[48] = ""; float x, y, z;
            if (sscanf(ln, "%31s %f %f %f %47s", nm, &x, &y, &z, bp) < 4) continue;
            char p[64]; snprintf(p, sizeof p, "models/%s.nrl", nm);
            if (gfx_load_nrl(&GIBM[ngibm].m, &pak, p, nm)) {
                GIBM[ngibm].off[0] = x; GIBM[ngibm].off[1] = y; GIBM[ngibm].off[2] = z;
                /* the piece's ReplayRecorder name: DeadCar<X> -> DeadCarGhost<X> (BackLightLeft is "BackLeftLight"
                 * there); the wing pieces have none */
                GIBM[ngibm].ghost[0] = 0;
                if (!strncmp(bp, "DeadCar", 7) && !strstr(bp, "Wing"))
                    snprintf(GIBM[ngibm].ghost, sizeof GIBM[ngibm].ghost, "DeadCarGhost%s", !strcmp(bp + 7, "BackLightLeft") ? "BackLeftLight" : bp + 7);
                ngibm++;
            }
        }
        free(gt);
        logm("car pieces for explosions: %d", ngibm);
    }
    ghosts_ok = gh_init(&pak) == 0;
    load_starts();
    if (autoplay) sceIoMkdir(DATA "autoplay_scores", 0777);   /* (autoplay keeps its own score tables; "autoplay" is the switch file) */
    sc_init(&pak, autoplay ? DATA "autoplay_scores/" : DATA);
    logm(ghosts_ok ? "ghost cars ready" : "ghost cars: no models/ghosts.txt (old packs)");
    menu_loaded = gfx_load_nrl(&MENU, &pak, "levels/an_menu_level.nrl", "menu");
    if (menu_loaded) { read_nrg("an_menu_level"); gfx_set_lights(&MENU, LIGHTS, NLIGHTS); }
    nr_car_default_params(&carp);
    {
        uint32_t csz; char *cfg = nr_pak_read(&pak, "models/car.cfg", &csz);
        logm("car handling: %d values from the game's Car blueprint", cfg ? nr_car_load_params(&carp, cfg) : 0);
        free(cfg);
    }

    if (gfx_load_tick) loading_end();
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);   /* the menus take taps */
    if (autoplay)   /* (the autoplay drives the default car, the Interceptor: its own save may still have the Commander of its last step) */
        for (int k = 0; k < FE_NVEH; k++) if (!strcmp(FE_VEH[k].key, "interceptor")) { SAVE.vehicle = k; logm("autoplay: driving the %s", FE_VEH[k].name); }
    int mode = MODE_FRONT;
    front_enter(FE_SPLASH);
    /* autoplay: a tour of the menus, then each story level driven by the computer */
    static const int AUTO_SCREENS[] = { FE_TITLE, FE_MAIN, FE_OPTIONS, FE_ACHIEVEMENTS, FE_CONTROLS, FE_SETS_SCREEN, FE_CREDITS };
    int auto_screen = 0, auto_level = 0; float auto_t = 0;
    int auto_set = 0, auto_outro = 0, auto_intro = 0, auto_credits2 = 0, auto_cmd = 0;
    for (int s = 0; s < FE_NSETS; s++) if (strstr(FE_SETS[s].file, "Campaign")) auto_set = s;
#ifdef NR_PC
    if (getenv("AUTOPLAY_ALL")) auto_set = 0;
    if (getenv("AUTOPLAY_NOMENU")) auto_screen = (int)(sizeof AUTO_SCREENS / sizeof AUTO_SCREENS[0]);
    if (getenv("AUTOPLAY_FROM")) { auto_level = atoi(getenv("AUTOPLAY_FROM")); auto_intro = 1; }   /* (the PC test machine: start at this level of the set) */
#endif

    uint32_t prev_buttons = 0;
    uint64_t last = now_us(), win_start = last;
    int win_frames = 0, win_over = 0; float win_worst = 0, win_cpu = 0;
    static float win_gpu, win_gpu_sum; static int win_gpu_n;
    int vsync_on = 1, quit_app = 0;
    float rep_t = 0; int rep_dir = 0;
    int t_down = 0, t_moved = 0; float t_x0 = 0, t_y0 = 0, t_sx = 0, t_sy = 0, t_time = 0;   /* the finger on the touch screen */
    float play_time_acc = 0;

    for (;;) {
        uint64_t tn = now_us();
        float dt = (tn - last) / 1e6f; last = tn;
        if (dt > 0.1f) dt = 0.1f;
        R.frame++;
        if (bench_mode < 0) GSW.no_bloom = sw_no_bloom || !SAVE.bloom;   /* Options: Toggle Bloom */
#ifndef NR_PC
        {   /* (switch file "nocap": autoplay races with the 60 fps cap off, each frame taking what it really costs, so the
             * log shows how much room every stretch has; tearing shows. Otherwise the test runs as the game is played) */
            int want = !(autoplay && nocap && !bench_on && (mode == MODE_RACE || mode == MODE_OUTRO || mode == MODE_INTRO));
            if (want != vsync_on) { vglWaitVblankStart(want ? GL_TRUE : GL_FALSE); vsync_on = want; }
        }
#endif
        if (mode == MODE_RACE && R.rs == RS_RACE) play_time_acc += dt;   /* Time Played counts racing only */
        if (play_time_acc > 10) {
            SAVE.play_time += play_time_acc; play_time_acc = 0;
            if (SAVE.play_time > 3600) ach_unlock("Rush Hour");
        }
        /* the PC shaders' clocks: Time (0..1, 0.05 a second) and the seconds since the music's beat */
        gfx_T += dt * 0.05f; if (gfx_T > 1.0f) gfx_T -= 1.0f;

        SceCtrlData pad; memset(&pad, 0, sizeof pad);
        sceCtrlPeekBufferPositive(0, &pad, 1);
        uint32_t pressed = pad.buttons & ~prev_buttons; prev_buttons = pad.buttons;
        float lx = (pad.lx - 128) / 128.0f, ly = (pad.ly - 128) / 128.0f;
        float rx = (pad.rx - 128) / 128.0f, ry = (pad.ry - 128) / 128.0f;
        if (fabsf(lx) < 0.15f) lx = 0; if (fabsf(ly) < 0.15f) ly = 0;
        if (fabsf(rx) < 0.15f) rx = 0; if (fabsf(ry) < 0.15f) ry = 0;

        /* menu input with stick / d-pad repeat */
        fe_input in; memset(&in, 0, sizeof in);
        int dir = (pad.buttons & SCE_CTRL_UP) || ly < -0.6f ? 1 : (pad.buttons & SCE_CTRL_DOWN) || ly > 0.6f ? 2 :
                  (pad.buttons & SCE_CTRL_LEFT) || lx < -0.6f ? 3 : (pad.buttons & SCE_CTRL_RIGHT) || lx > 0.6f ? 4 : 0;
        if (dir != rep_dir) { rep_dir = dir; rep_t = 0.4f; if (dir) { in.up = dir == 1; in.down = dir == 2; in.left = dir == 3; in.right = dir == 4; } }
        else if (dir) { rep_t -= dt; if (rep_t <= 0) { rep_t = 0.12f; in.up = dir == 1; in.down = dir == 2; in.left = dir == 3; in.right = dir == 4; } }
        in.accept = (pressed & SCE_CTRL_CROSS) != 0; in.back = (pressed & SCE_CTRL_CIRCLE) != 0;
        in.start = (pressed & SCE_CTRL_START) != 0; in.l = (pressed & SCE_CTRL_LTRIGGER) != 0; in.r = (pressed & SCE_CTRL_RTRIGGER) != 0;
        in.alt = (pressed & SCE_CTRL_TRIANGLE) != 0;
        in.any = pressed != 0;
        in.btn = -1;
        for (int b = 0; b < BTN_N; b++) if (pressed & BTN_MASK[b]) { in.btn = b; break; }
        {   /* the front touch screen (1920x1088) in the screen's 960x544 points: a tap is a touch lifted within 0.8 s
             * that stayed within 20 points; a drag gives a step every 48 points */
            SceTouchData td; memset(&td, 0, sizeof td);
            sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1);
            int down = td.reportNum > 0;
            float x = down ? td.report[0].x * 0.5f : 0, y = down ? td.report[0].y * 0.5f : 0;
            if (down && !t_down) { t_x0 = t_sx = x; t_y0 = t_sy = y; t_time = 0; t_moved = 0; }
            if (down) {
                t_time += dt;
                if (fabsf(x - t_x0) > 20 || fabsf(y - t_y0) > 20) t_moved = 1;
                in.touch = 1; in.hx = x; in.hy = y;
                if (y - t_sy > 48) { in.swipe_y = 1; t_sy = y; } else if (t_sy - y > 48) { in.swipe_y = -1; t_sy = y; }
                if (x - t_sx > 48) { in.swipe_x = 1; t_sx = x; } else if (t_sx - x > 48) { in.swipe_x = -1; t_sx = x; }
            } else if (t_down && !t_moved && t_time < 0.8f) { in.tap = 1; in.tx = t_x0; in.ty = t_y0; in.any = 1; }
            t_down = down;
        }
        uint32_t touch_btn = in.touch ? SCE_CTRL_CROSS : 0;   /* a finger held skips the films, as a button held does */

        /* autoplay drives the menus */
        if (autoplay && mode == MODE_FRONT) {
            memset(&in, 0, sizeof in); in.btn = -1;
            auto_t += dt;
            if (front_screen() == FE_SPLASH) { if (auto_t > 2.0f) { in.any = 1; auto_t = 0; } }
            else if (auto_screen < (int)(sizeof AUTO_SCREENS / sizeof AUTO_SCREENS[0])) {
                if (auto_t > 6.0f) {
                    auto_t = 0; auto_screen++;
                    if (auto_screen < (int)(sizeof AUTO_SCREENS / sizeof AUTO_SCREENS[0])) {
                        front_enter(AUTO_SCREENS[auto_screen]);
                        logm("autoplay: menu screen %d", AUTO_SCREENS[auto_screen]);
                        int nv, nd; float ld; snd_stats(&nv, &nd, &ld);
                        logm("  sound: %d voices, %d decoding, mixer load %.0f%%", nv, nd, ld * 100);
                    }
                } else if (front_screen() == FE_MAIN && ((int)(auto_t * 2) % 2) && (R.frame % 30) == 0) in.down = 1;
            } else if (auto_level < FE_SETS[auto_set].n) {
                while (auto_level < FE_SETS[auto_set].n && !FE_SETS[auto_set].lv[auto_level].installed) auto_level++;
                if (auto_level == 1 && !auto_intro && auto_set == story_set_index()) {   /* the story: the Tutorial, the intro, Level 1 */
                    auto_intro = 1;
                    if (begin_intro()) { mode = MODE_INTRO; auto_t = 0; last = now_us(); continue; }
                }
                if (auto_level < FE_SETS[auto_set].n) {
                    begin_level(auto_set, auto_level, 1); mode = MODE_RACE; auto_t = 0;
                    last = now_us();
                    continue;
                }
#ifdef NR_PC
            } else if (getenv("AUTOPLAY_ALL") && auto_set + 1 < FE_NSETS) {   /* (the PC test machine: every level set) */
                auto_set++; auto_level = 0; logm("autoplay: level set %d", auto_set);
#endif
            } else if (!auto_outro) {                     /* after the story: the ending, then the credits */
                auto_outro = 1;
                if (begin_outro()) { mode = MODE_OUTRO; auto_t = 0; last = now_us(); continue; }
            } else if (!auto_credits2) {                  /* then the credits again as the main menu starts them, driven hard */
                static float menu_wait; menu_wait += dt;  /* (4 s in the menu first, its music playing, as a player would) */
                if (menu_wait > 4.0f) {
                    auto_credits2 = 1; credits_stress = 1;
                    snd_peak_sec(NULL, NULL);             /* (the first reading then counts from here, not from boot) */
                    logm("autoplay: the credits from the main menu, driven with boost, hops and wings");
                    begin_level(-1, 0, 1); mode = MODE_RACE; auto_t = 0; last = now_us(); continue;
                }
            } else if (!auto_cmd) {                       /* last: the Commander along Level 1 (its sparks: ShockNearbyLogic) */
                auto_cmd = 1; credits_stress = 0;
                int cv = -1, ss = story_set_index();
                for (int k = 0; k < FE_NVEH; k++) if (!strcmp(FE_VEH[k].key, "commander")) cv = k;
                if (cv >= 0 && ss >= 0 && FE_SETS[ss].n > 1 && FE_SETS[ss].lv[1].installed) {
                    SAVE.vehicle = cv; auto_cmd = 2;      /* (the autoplay's own save) */
                    logm("autoplay: the Commander along %s (its sparks)", FE_SETS[ss].lv[1].xml);
                    begin_level(ss, 1, 1); mode = MODE_RACE; auto_t = 0; last = now_us(); continue;
                }
            } else break;
        }

        log_buffered = mode == MODE_RACE || mode == MODE_OUTRO || mode == MODE_INTRO;
        uint64_t c0 = now_us();
        gfx_hook_additive = mode != MODE_FRONT ? draw_particles_hook : NULL;   /* the race's particles and tracks' glow only */
        gfx_hook_decal = mode == MODE_RACE ? draw_decal_hook : NULL;            /* the tracks' tread */
        if (mode != MODE_RACE) trk_nq = 0;
        if (mode == MODE_INTRO) {
            if (intro_frame(dt, autoplay ? 0 : pad.buttons | touch_btn, autoplay ? 0 : pressed | (in.tap ? SCE_CTRL_CROSS : 0))) {   /* Finish 0x72ab30 -> Story Levels, level 1 */
                end_cinema(); snd_post("Stop_Music", 0);
                logm("intro: over");
                int st = story_set_index();
                if (FE_SETS[st].n > 1) { begin_level(st, 1, 0); mode = MODE_RACE; }
                else { mode = MODE_FRONT; front_enter(FE_MAIN); }
                if (autoplay) auto_level = 1;
                last = now_us();
            }
        } else if (mode == MODE_OUTRO) {
            if (outro_frame(dt, pad.buttons | touch_btn, pressed | (in.tap ? SCE_CTRL_CROSS : 0))) {          /* -> CreditsLevel2: Made From Scratch */
                end_cinema();
                ach_unlock("Made From Scratch");
                begin_level(-1, 0, 0); mode = MODE_RACE; last = now_us();
            }
        } else if (mode == MODE_FRONT) {
            int set = 0, lvl = 0;
            int res = front_update(dt, &in, &set, &lvl);
            if (res == FE_EXIT) break;
            v3 eye, fwd; float fade;
            front_camera(dt, &eye, &fwd, &fade);
            float lis[3] = { eye.x, eye.y, eye.z }, lf[3] = { fwd.x, fwd.y, fwd.z }, lu[3] = { 0, 0, 1 };
            snd_set_listener(lis, lf, lu);
            gfx_scene_scale = 1;
            if (front_screen() != FE_SPLASH && menu_loaded) gfx_draw_frame(&MENU, eye, fwd, v3make(0, 0, 1), 45, NULL, 0, NULL, 0, NULL);   /* menu_camera: 45 degrees */
            else { glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); }
            hud_begin();
            if (front_screen() != FE_SPLASH && fade > 0) hud_rect(0, 0, 960, 544, (unsigned)(fminf(1, fade) * 255));
            front_draw();
            hud_end();
            vglSwapBuffers(GL_FALSE);
            if (res == FE_PLAY && lvl == FE_LEVEL_INTRO) {                 /* "Play Tutorial?" No: the intro */
                if (begin_intro()) mode = MODE_INTRO;
                else { begin_level(set, 1, 0); mode = MODE_RACE; }
                last = now_us();
            } else if (res == FE_PLAY) { begin_level(set, lvl, 0); mode = MODE_RACE; last = now_us(); }
        } else {
            /* ---------------------------------------------------------- race */
            R.t += dt; if (R.msg_t > 0) R.msg_t -= dt; if (R.shake > 0) R.shake -= dt;
            if (R.rs == RS_COUNT) countdown_step(dt);
            if (autoplay) bench_step(dt);                     /* (Level 5's start; Level 1 too with the switch file "bench": it holds the race ~20 s) */
#ifdef NR_PC
            if (nrpc_warp >= 0 && R.rs == RS_RACE) {     /* (the PC test machine: to checkpoint N, or past the last one to the finish) */
                if (nrpc_warp < G.ncp) { if (G.cp_has_R[nrpc_warp]) respawn_oriented(G.cp[nrpc_warp], G.cpR[nrpc_warp]); else respawn_at(G.cp[nrpc_warp]); }
                else if (G.nfinish) {                    /* right on the finish point, facing as the car does */
                    float f3[3] = { R.car.rot[0][1], R.car.rot[1][1], 0 }, p3[3] = { G.finish[0][0], G.finish[0][1], G.finish[0][2] + 1 };
                    nr_car_reset(&R.car, p3, f3); cam_reset();
                }
                nrpc_warp = -1;
            }
            if (nrpc_wreck && R.rs == RS_RACE) { nrpc_wreck = 0; explode("test: wrecked by the script", 2); }
            if (nrpc_aim >= 0 && R.rs == RS_RACE && nrpc_aim < OBJS.n && OBJS.o[nrpc_aim].kind == OK_BAFSAW) {
                /* (the test machine: the car sent straight at a sliding saw, to meet it at the middle of its slot) */
                nr_obj *o = &OBJS.o[nrpc_aim];
                float w = o->f[2], x = ceilf(w * o->t / 3.14159265f) * 3.14159265f / w - o->t;
                while (x < 0.5f) x += 3.14159265f / w;
                float fw[3] = { -o->f[7] * nrpc_aim_sign, o->f[6] * nrpc_aim_sign, 0 }, l = sqrtf(fw[0] * fw[0] + fw[1] * fw[1]);
                fw[0] /= l; fw[1] /= l;
                float dist = nrpc_aim_speed * x, p3[3] = { o->f[3] - fw[0] * dist, o->f[4] - fw[1] * dist, o->f[5] + 0.7f };
                nr_car_reset(&R.car, p3, fw); cam_reset();
                for (int k = 0; k < 3; k++) R.car.vel[k] = fw[k] * nrpc_aim_speed;
                logm("  test: aimed at saw %d, meeting it in %.2f s from %.0f units at %.0f", nrpc_aim, x, dist, nrpc_aim_speed);
                nrpc_aim = -1;
            }
#endif
            if (R.rs == RS_COUNT && R.cd >= 0) {             /* GO: the clock past 0 (StartRace 0x715080) */
                R.rs = RS_RACE; R.race_t = 0;
                if (WARPM.nch) cam_reset();                 /* (the countdown's camera ends where the chase camera snaps to) */
                gh_start(); gh_rec_go(); gh_labels_fade(1);  /* StartRace 0x715080: the replays and the recording begin at GO */
                rec_car_appears();
            }
            int go_menu = 0, restart = 0, next = 0;
            if (autoplay) {
                if (bench_mode < 0) auto_t += dt;
                float auto_secs = 50;
#ifdef NR_PC
                if (getenv("AUTOPLAY_SECS")) auto_secs = (float)atof(getenv("AUTOPLAY_SECS"));   /* (the PC test machine) */
#endif
                if (chase_on) auto_secs = ch_length(&CH) + 45;            /* (to the finish: the replay's time and some) */
                if (credits_stress && R.credits) auto_secs = 150;
                if (auto_cmd == 2) auto_secs = fminf(auto_secs, 45);
#ifdef NR_PC
                if (audit_on && audit_n) auto_secs = audit_n * 0.06f + 8;
#endif
                if ((R.rs == RS_FINISH && R.t > 4) || (R.rs != RS_FINISH && auto_t > auto_secs)) {
                    if (R.rs != RS_FINISH) { logm("RESULT %s not finished after %.0f s, checkpoints %d/%d, wrecks %d%s", R.lv->xml, auto_secs, R.cp_count, G.ncp, R.wrecks, chase_on ? ", physics drive" : ""); fx_log(); }
                    if (chase_on) logm("CHASE %s assists %d resyncs %d replay %.1f s", R.lv->xml, CH.assists, CH.resyncs, ch_length(&CH));
                    int nv, nd; float ld; snd_stats(&nv, &nd, &ld);
                    logm("  sound: %d voices, %d decoding, mixer load %.0f%%", nv, nd, ld * 100);
                    auto_level++;
                    car_sounds_off(); stop_emitters(); level_sounds_exit(); snd_post("Stop_CarEngine", OBJ_CAR);
                    mode = MODE_FRONT; front_enter(FE_LEVELS);
                    continue;
                }
            }
            uint32_t m_pause = act_mask(ACT_PAUSE);
            if (R.credits && !autoplay) {                   /* the credits drive's skip_intro_text (0x7346b0): hold the pad's Y */
                static float cr_hold; static int cr_armed;
                if (R.rs == RS_COUNT && R.t < 0.1f) { cr_hold = 0; cr_armed = 0; }
                cin_skip_update(dt, pressed != 0);
                if ((R.rs == RS_RACE || R.rs == RS_COUNT) && cinema_skip(&cr_hold, &cr_armed, (pad.buttons | touch_btn) & ~m_pause, dt)) {
                    car_sounds_off(); R.rs = RS_FINISH; R.t = 2.5f; R.done_time = R.race_t;    /* (into the fade out) */
                    warp_out_portal();
                }
            }
            if (R.rs != RS_PAUSE) ann_update(dt);
            if (credits_stress && R.credits) {           /* (autoplay: the sound, second by second) */
                static float sec_t; sec_t += dt;
                if (sec_t >= 1.0f) {
                    sec_t = 0; float pk; int lt, nv_, nd_; float ld_;
                    snd_peak_sec(&pk, &lt); snd_stats(&nv_, &nd_, &ld_);
                    logm("  sound %.0f s: %d voices %d decoding, load %.0f%% peak %.0f%% late %d%s%s", R.race_t, nv_, nd_, ld_ * 100, pk * 100, lt,
                         R.boosting ? ", boosting" : "", R.wings ? ", wings" : "");
                }
            }
            if (R.rs == RS_PAUSE) {
                int pa = pause_update(dt, &in, (pressed & m_pause) != 0);
                if (pa != PA_NONE && pa != PA_QUIT_APP) snd_post("Resume_Master_Audio_Bus", 0);
                if (pa == PA_RESUME) { R.rs = R.rs_before_pause; PM.state = PM_NONE; }
                else if (pa == PA_RESTART) restart = 1;
                else if (pa == PA_MENU) go_menu = 1;
                else if (pa == PA_QUIT_APP) quit_app = 1;
            } else if (R.rs == RS_FINISH && R.credits) {
                if (R.t > 4.0f) {
                    car_sounds_off(); stop_emitters(); level_sounds_exit(); snd_post("Stop_CarEngine", OBJ_CAR); snd_post("Stop_Music", 0);
                    front_menu_level();
                    mode = MODE_FRONT; front_enter(FE_MAIN); last = now_us();
                    continue;
                }
            } else if (R.rs == RS_FINISH && !R.lv->score) {   /* score style 0 (the tutorials): no table; BaseLevel state 5 fades out, then on */
                if (R.t > 2.5f) next = 1;
            } else if (R.rs == RS_FINISH) {
                if (R.fin_stage == 0) {                   /* state 3: 1.7 s, then ShowResults -> name entry */
                    if (R.t > 1.7f) { R.fin_stage = 1; R.name_col = 0; ann_queue("Play_NewHighScore"); }
                } else if (R.fin_stage == 1) {            /* name entry (0x6e5770): left/right between the letters and Accept,
                                                           * up/down the letter ("^" / "v"), Cross on a letter: the next one */
                    for (int c = 0; c < 3; c++) {          /* touch: "^", the letter, "v"; Accept */
                        float x = 480 + (c - 1) * 35.36f;
                        if (ui_hit(&in, x, 245, 34, 36)) { R.name_col = c; in.up = 1; }
                        else if (ui_hit(&in, x, 284, 34, 36)) { if (R.name_col != c) snd_post("Play_MenuMoveLowShort", 0); R.name_col = c; }
                        else if (ui_hit(&in, x, 323, 34, 36)) { R.name_col = c; in.down = 1; }
                    }
                    if (ui_hit(&in, 844.4f, 499.1f, 176.8f, 40)) { R.name_col = 3; in.accept = 1; }
                    int done_ = (in.accept && R.name_col == 3) || in.start;      /* MenuStart accepts */
                    if (autoplay && R.t > 2.6f) done_ = 1;
                    if ((in.left || in.back) && R.name_col > 0) { R.name_col--; snd_post("Play_MenuMoveLowShort", 0); }   /* (MenuBack: left) */
                    else if (in.right && R.name_col < 3) { R.name_col++; snd_post("Play_MenuMoveLowShort", 0); }
                    else if (R.name_col < 3 && (in.up || in.down)) {      /* 0x6f5300: A..Z and _ */
                        char *c = &SAVE.initials[R.name_col];
                        if (!((*c >= 'A' && *c <= 'Z') || *c == '_')) *c = 'A';
                        else if (in.down) *c = *c == 'Z' ? '_' : *c == '_' ? 'A' : *c + 1;
                        else *c = *c == 'A' ? '_' : *c == '_' ? 'Z' : *c - 1;
                        snd_post("Play_MenuMoveLowShort", 0);
                    } else if (in.accept && R.name_col < 3) { R.name_col++; snd_post("Play_MenuMoveLowShort", 0); }
                    if (done_) { snd_post("Play_MenuMoveLowShort", 0); results_open(); }
                } else if (PM.state == PM_DLG) {          /* Quit? / Restart Current Level? / Select Ghosts to Challenge */
                    int pa = pm_dialog_result(ui_dlg_update(&PM.d, &in));
                    if (pa == PA_MENU) go_menu = 1; else if (pa == PA_QUIT_APP) quit_app = 1;
                    else if (pa == PA_RESTART) restart = 1; else if (pa == PA_NEXT) next = 1;
                } else {                                  /* results (0x6e6290): Quit, Continue, Restart; L/R the table */
                    int rr = res_update(dt, &in);
                    PM.from_results = 1;
                    if (rr == RES_QUIT) pm_dialog(DK_QUIT);
                    else if (rr == RES_RESTART) pm_dialog(DK_RESTART);
                    else if (rr == RES_CONTINUE) {
                        if (pm_ghost_step() && arcade_has_next()) { PM.after = PA_NEXT; pm_dialog(DK_GHOSTS); }
                        else next = 1;
                    }
                }
            } else if (!autoplay && (pressed & m_pause) && !R.stunt_over) {        /* no pausing after the stunt clock ran out */
                R.rs_before_pause = R.rs; R.rs = RS_PAUSE; R.pause_sel = 0;
                PM.state = PM_MENU; PM.sel = 0; PM.from_results = 0;
                snd_post("Pause_Master_Audio_Bus", 0);
            }
            if (R.rs == RS_RACE && !autoplay && !R.stunt_over && R.alive_t > 0.5f && (pressed & act_mask(ACT_RESET))) {
                /* CarResetHard (0x708f44, the pad's Back): the car blows itself up - Die and "Car Death" cause 1 - and
                 * comes back at the checkpoint as after any death (CarResetSoft, the warp, is bound to nothing) */
                SAVE.hard_resets++; ach_unlock("Self-Termination");
                explode("self-termination", 1);
            }
            if (R.rs != RS_PAUSE && R.rs != RS_DEAD && !autoplay && (pressed & act_mask(ACT_VIEW))) cam_switch_view();

            /* car input (CarLogic's inputs: CarGas, CarBrake, CarTurn, CarPitch, CarRollX/Y, CarJump,
             * CarBoost, CarWings) */
            nr_car_input ci; memset(&ci, 0, sizeof ci);
            int racing = R.rs == RS_RACE, wings_pressed = 0, roll_held = 0;
            float cam_tx = 0, cam_ty = 0;                    /* the Advanced scheme's camera turn (right stick) */
            if (racing && chase_on && !R.stunt_over) {
                float rp[3], rR[3][3], rv[3];
#ifdef NR_PC
                {   static FILE *tr; static float next_t;        /* (CHASE_TRACE: where the car went, 4 times a second) */
                    if (getenv("CHASE_TRACE")) {
                        if (!tr) { tr = fopen(getenv("CHASE_TRACE"), "w"); if (tr) for (int k = 0; k < G.ncp; k++) fprintf(tr, "cp,%d,%.1f,%.1f,%.1f\n", k, G.cp[k][0], G.cp[k][1], G.cp[k][2]); }
                        if (R.race_t < next_t - 1) next_t = 0;
                        if (tr && R.race_t >= next_t) { next_t = R.race_t + 0.25f;
                            fprintf(tr, "t,%s,%.2f,%.1f,%.1f,%.1f,%.1f,%d,%d,%.0f,%d\n", R.lv->xml, R.race_t, R.car.pos[0], R.car.pos[1], R.car.pos[2], ch_time(&CH), R.car.grounded, R.wings, nr_car_speed(&R.car), R.cp_count); fflush(tr); }
                    }
                }
#endif
                CH.heat = infinite_heat ? 0 : R.heat / HEAT_MAX;
                if (ch_drive(&CH, &R.car, R.wings, dt, &ci, &wings_pressed, rp, rR, rv)) {
                    logm("  assist %d (%s) at %.1f s, replay %.1f s, at %.0f %.0f %.0f", CH.assists, CH.why, R.race_t, ch_time(&CH), rp[0], rp[1], rp[2]);
                    nr_car_reset_rot(&R.car, rp, rR); memcpy(R.car.vel, rv, 12);
                    if (R.wings) { R.wings = 0; layer_event("Wings_Off"); }
                    cam_reset();
                }
            }
            else if (racing && autoplay && !R.stunt_over && !pilot_on && pilot_coast) ci.throttle = 1;     /* (the replay's end: straight on) */
            else if (racing && autoplay && !R.stunt_over && !pilot_on
#ifdef NR_PC
                     && !getenv("AUTOPLAY_IDLE")
#endif
                     ) nr_ai_drive_world(&R.ai, &R.car, &COLL, PATH, L.pathlen, L.d.npath, &ci);
            else if (racing && !R.stunt_over) {          /* (time up in stunt mode: CarLogic state 10, no input, it coasts) */
                float sx = SAVE.swap_sticks ? rx : lx, sy = SAVE.swap_sticks ? ry : ly;      /* steering stick */
                float tx = SAVE.swap_sticks ? lx : rx, ty = SAVE.swap_sticks ? ly : ry;      /* trick stick */
                /* Advanced Controls (ControlScheme_TriggerPedals_RollMode): the Roll button held makes the steering stick
                 * the trick stick, the other stick turns the camera. On the brake's button it rolls in the air only */
                int adv = SAVE.adv_controls, shared = SAVE.bind[ACT_TRICK] == SAVE.bind[ACT_BRAKE];
                int roll = adv && (pad.buttons & act_mask(ACT_TRICK)) && (!shared || R.car.grounded < 2);
                roll_held = roll;
                ci.steer = -sx;
                ci.throttle = (pad.buttons & act_mask(ACT_GAS)) ? 1.0f : 0.0f;
                ci.brake = (pad.buttons & act_mask(ACT_BRAKE)) && !(roll && shared) ? 1.0f : 0.0f;
                ci.jump = (pressed & act_mask(ACT_JUMP)) != 0;
                ci.boost = (pad.buttons & act_mask(ACT_BOOST)) != 0;
                if (!adv) {
                    ci.pitch = SAVE.invert_y ? -sy : sy;      /* pull back: nose up */
                    ci.roll_x = tx; ci.roll_y = SAVE.invert_y ? ty : -ty;
                } else if (roll) { ci.roll_x = sx; ci.roll_y = SAVE.invert_y ? sy : -sy; }
                else ci.pitch = SAVE.invert_y ? -sy : sy;
                if (adv) { cam_tx = tx; cam_ty = -ty; }
                {   /* the d-pad does the tricks too (each direction no action is set to): left / right roll, up / down flip,
                     * as the trick stick pushed all the way */
                    uint32_t bound = 0;
                    for (int a = 0; a < ACT_N; a++) bound |= act_mask(a);
                    uint32_t dp = pad.buttons & ~bound;
                    float dx = ((dp & SCE_CTRL_RIGHT) ? 1.0f : 0.0f) - ((dp & SCE_CTRL_LEFT) ? 1.0f : 0.0f);
                    float dy = ((dp & SCE_CTRL_DOWN) ? 1.0f : 0.0f) - ((dp & SCE_CTRL_UP) ? 1.0f : 0.0f);
                    if (dx != 0) ci.roll_x = dx;
                    if (dy != 0) ci.roll_y = SAVE.invert_y ? dy : -dy;
                }
                /* Controls: "Steering Stick Tricks In The Air" (on by default; not on the PC, where it only leans the car a
                 * little): off the ground a moment (not just over a crest) with the wings in, the steering stick flips and
                 * rolls the car as the trick stick does, unless the trick stick or the d-pad is already at it */
                if (SAVE.air_tricks && !roll && !R.wings && R.car.grounded == 0 && R.car.air_time > 0.2f
                    && fabsf(ci.roll_x) <= 0.01f && fabsf(ci.roll_y) <= 0.01f && sx * sx + sy * sy > 0.3f * 0.3f) {
                    ci.roll_x = sx; ci.roll_y = SAVE.invert_y ? sy : -sy;
                }
                wings_pressed = (pressed & act_mask(ACT_WINGS)) != 0;
            } else if (R.rs == RS_FINISH) {     /* the FinishCar coasts, its BodyRigid Drag 3, 2.2, 0.005 (the car's: 1, 0.02, 0.005) */
                float sp = nr_car_speed(&R.car), kd = sp > 1e-3f ? fmaxf(0, sp - (2.0f + 2.18f * sp) * dt) / sp : 0;
                for (int k = 0; k < 3; k++) R.car.vel[k] *= kd;
            }
            if (credits_stress && racing && autoplay && R.credits) {   /* boost 2 s in 5, a hop every 3 s, the wings just after */
                float ph = fmodf(R.race_t, 5.0f);
                ci.boost = ph > 1.0f && ph < 3.2f;
                if (fmodf(R.race_t, 3.0f) < dt) ci.jump = 1;
                if (fmodf(R.race_t + 2.55f, 3.0f) < dt) wings_pressed = 1;
            }
            /* CarRollMode (0x70907c): the trick stick moved, or the Advanced scheme's Roll button held */
            R.roll_mode = racing && (roll_held || fabsf(ci.roll_x) > 0.01f || fabsf(ci.roll_y) > 0.01f);
            /* the wings (0x708e70 / 0x706880): toggled, not more often than every 0.3 s; only in the
             * air (two wheels down at most); not when too hot; they fold when the car lands or jumps */
            if (R.wings_cool > 0) R.wings_cool -= dt;
            if (wings_pressed && R.wings_cool <= 0) {
                R.wings_cool = 0.3f;
                if (R.lv && !R.lv->wings && !R.credits) {          /* level set wings_enabled 0 (0x6f9050): refused */
                    snprintf(R.msg, sizeof R.msg, "Wings Disabled"); R.msg_t = 0.7f;
                    snd_post("Play_TutorialDisplayDown", 0);
                }
                else if (R.wings) { R.wings = 0; snd_post("Play_WingIn", OBJ_CAR); layer_event("Wings_Off"); }
                else if (R.car.grounded > 2) { snprintf(R.msg, sizeof R.msg, "In Air Only"); R.msg_t = 0.7f; }
                else if (!infinite_heat && R.heat > HEAT_MAX * 0.9f) { snprintf(R.msg, sizeof R.msg, "Too Hot"); R.msg_t = 0.7f; }
                else { R.wings = 1; snd_post("Play_WingOut", OBJ_CAR); layer_event("Wings_On"); ach_unlock("Wings Deployed"); }
            }
            if (R.wings && (R.car.grounded > 2 || (!infinite_heat && R.heat > HEAT_MAX * 0.98f))) {
                R.wings = 0; snd_post("Play_WingIn", OBJ_CAR); layer_event("Wings_Off");
            }
            ci.wings = R.wings;
            if (R.heat >= HEAT_MAX * 0.999f && !infinite_heat) ci.boost = 0;

            /* the level's objects: their logic, then their collision near the car */
            memset(&R.hit, 0, sizeof R.hit);
            if (R.rs != RS_PAUSE) {
                beat_t += dt;
                float bk = 1.0f - fmodf(beat_t, beat_period);         /* LightPulse: 1 on the beat, falling over 1 s */
                gfx_beat = bk < 0 ? 0 : bk > 1 ? 1 : bk;
                gfx_ab = fmodf(beat_t, beat_period);
            }
            if (objs_on && R.rs != RS_PAUSE) {
                nr_objs_frame of; memset(&of, 0, sizeof of);
                memcpy(of.car_pos, R.car.pos, 12); memcpy(of.car_vel, R.car.vel, 12); memcpy(of.car_rot, R.car.rot, 36);
                for (int k = 0; k < 3; k++) of.car_half[k] = (carp.hull_max[k] - carp.hull_min[k]) * 0.5f;
                of.car_active = R.rs == RS_RACE || R.rs == RS_COUNT || R.rs == RS_FINISH;
                v3 vf_ = len(R.view_fwd) > 0.5f ? R.view_fwd : v3make(R.car.rot[0][1], R.car.rot[1][1], R.car.rot[2][1]);
                v3 vu_ = len(R.view_up) > 0.5f ? R.view_up : v3make(0, 0, 1);
                v3 vr_ = norm(cross(vf_, vu_)); vu_ = cross(vr_, vf_);
                for (int k = 0; k < 3; k++) { of.cam_rot[k][0] = (&vr_.x)[k]; of.cam_rot[k][1] = (&vu_.x)[k]; of.cam_rot[k][2] = -(&vf_.x)[k]; }
                of.cam_pos[0] = R.eye.x; of.cam_pos[1] = R.eye.y; of.cam_pos[2] = R.eye.z;
                of.beat = gfx_beat;
                nr_objs_update(&OBJS, &of, R.rs == RS_COUNT && R.cd < CD_OBJS ? 0.0f : dt);   /* (they start 0.12 s in: 4.5 s at GO) */
                R.cool_rate = of.cool; R.car_cooler = of.car_cooler;
                if (of.cool_glow && R.rs == RS_RACE) {      /* CarLogic's cooling glow: Play_CoolDown as it starts */
                    if (R.cool_t <= 0) snd_post("Play_CoolDown", OBJ_CAR);
                    R.cool_t = 1.5f;
                }
                if (of.finished) R.finish_obj = 1;
                if (R.rs == RS_RACE) for (int k = 0; k < 3; k++) R.car.pos[k] += of.car_push[k];   /* out of a CORE drone */
                if (OBJS.respawn_set) {                      /* TriggerRespawnLocation */
                    OBJS.respawn_set = 0; R.resp_kind = 2;
                    memcpy(R.resp_pos, OBJS.respawn_pos, 12); memcpy(R.resp_R, OBJS.respawn_R, 36);
                }
                if (OBJS.tutorial) { tutorial_show(OBJS.tutorial); OBJS.tutorial = NULL; }
                if (R.rs == RS_RACE) {
                    if (of.heat > 0 && !infinite_heat) R.heat += of.heat * (HEAT_MAX / 50.0f);   /* lasers: 3000 a second */
#ifdef NR_PC
                    if (haz_on && of.heat > 0) {             /* (the test machine: a laser on the car) */
                        static float l0 = -9, l1 = -9, lh;
                        if (R.race_t - l1 > 0.3f) { if (l1 >= 0 && lh > 0) logm("HAZLASER %s %.2f-%.2f s, heat %.0f, %s", R.lv->xml, l0, l1, lh, pilot_on ? "replay" : "driven"); l0 = R.race_t; lh = 0; }
                        l1 = R.race_t; lh += of.heat;
                    }
#endif
                    for (int k = 0; k < 3; k++) R.car.vel[k] = (R.car.vel[k] + of.car_dv[k]) * of.car_vscale;
                }
                nr_objs_collision(&OBJS, R.car.pos, 8.0f + nr_car_speed(&R.car) * 0.1f);
#ifdef NR_PC
                if (haz_on && R.rs == RS_RACE) haz_frame();
                if (R.rs == RS_RACE && getenv("SHOTRT")) {     /* (pictures at these race times) */
                    extern char shot_req[64]; static float last_rt = -1;
                    const char *q = getenv("SHOTRT");
                    while (*q) {
                        float ts = (float)atof(q);
                        if (last_rt < ts && R.race_t >= ts) snprintf(shot_req, 64, "%s_rt%05.2f", R.lv->xml, ts);
                        q = strchr(q, ','); if (!q) break; q++;
                    }
                    last_rt = R.race_t;
                }
#endif
            }

            if (R.rs != RS_PAUSE && R.rs != RS_DEAD && !(pilot_on && R.rs == RS_RACE)) {
                float pre_v[3], pre_w[3]; memcpy(pre_v, R.car.vel, 12); memcpy(pre_w, R.car.angvel, 12);
                /* the wings level out against the camera and turn about its axes */
                if (len(R.view_up) > 0.5f) {
                    v3 vr = norm(cross(R.view_fwd, R.view_up)), vu = norm(cross(vr, R.view_fwd));
                    R.car.cam_right[0] = vr.x; R.car.cam_right[1] = vr.y; R.car.cam_right[2] = vr.z;
                    R.car.cam_up[0] = vu.x; R.car.cam_up[1] = vu.y; R.car.cam_up[2] = vu.z;
                }
                uint64_t pt0 = sceKernelGetProcessTimeWide();
                nr_car_frame(&R.car, &carp, &COLL, &ci, dt);
                if (R.car.jumped) {
                    snd_post("Play_Jump", OBJ_CAR); layer_event("Jump");
                    if (R.wings) { R.wings = 0; snd_post("Play_WingIn", OBJ_CAR); layer_event("Wings_Off"); }
                }
                R.phys_acc += dt;
                int steps = 0;
                while (R.phys_acc >= NR_CAR_DT && steps < 12) {
                    nr_car_step(&R.car, &carp, &COLL, NR_CAR_DT);
                    if (racing && autoplay && !chase_on && !pilot_coast
#ifdef NR_PC
                        && !getenv("AUTOPLAY_IDLE")
#endif
                        ) {
                        const char *why = NULL;
                        if (nr_car_watchdog(&R.car, &R.ai, PATH, L.pathlen, L.d.npath, COLL.minz, NR_CAR_DT, &R.bad_time, &why, autoplay))
                            logm("  back on the track (%s)", why);
                    }
                    R.phys_acc -= NR_CAR_DT; steps++;
                }
                if (steps == 12) R.phys_acc = 0;
                /* CarLogic's collision rules (0x709290) over this frame's contacts */
                if (objs_on && racing) {
                    nr_objs_car_contacts(&OBJS, &R.car, R.wings, &R.hit);
                    if (R.hit.restore_vel) { memcpy(R.car.vel, pre_v, 12); memcpy(R.car.angvel, pre_w, 12); }   /* through a streetlight */
                    for (int k = 0; k < 3; k++) R.car.vel[k] += R.hit.dv[k];
                    if (R.hit.fold_wings && R.wings) { R.wings = 0; snd_post("Play_WingIn", OBJ_CAR); layer_event("Wings_Off"); }
                }
                phys_us += (float)(sceKernelGetProcessTimeWide() - pt0);
            }
            if (R.rs != RS_PAUSE) { update_gibs(dt); if (R.cool_t > 0) R.cool_t -= dt; tutorial_update(dt); }
            if (R.rs != RS_PAUSE) trk_update(dt, R.rs == RS_RACE);   /* the tyre tracks (CarLogic 0x7065d0) */
#ifdef NR_PC
            if (getenv("POSDBG") && R.race_t > atof(getenv("POSDBG")) && (R.frame % 3) == 0)
                logm("    pos %.2f s: %.1f %.1f %.1f vel %.1f %.1f %.1f wheels %d rs %d pilot %d", R.race_t, R.car.pos[0], R.car.pos[1], R.car.pos[2], R.car.vel[0], R.car.vel[1], R.car.vel[2], R.car.grounded, R.rs, pilot_on);
#endif
            /* the ReplayManager (0x6ff320): this frame's poses, then its 0.06 s ticks (paused: held) */
            if (R.rec_car >= 0 && R.rs != RS_DEAD) {
                if (R.wings != R.rec_wings) { gh_rec_wings(R.rec_car, R.wings); R.rec_wings = R.wings; }
                gh_rec_pose(R.rec_car, R.car.rot, R.car.pos);
            }
            for (int i = 0; i < 16; i++) if (R.rec_gib[i] >= 0) {
                if (i >= ngibs || GIBS[i].life <= 0) { gh_rec_remove(R.rec_gib[i]); R.rec_gib[i] = -1; continue; }
                float gp[3] = { GIBS[i].pos.x, GIBS[i].pos.y, GIBS[i].pos.z };
                gh_rec_pose(R.rec_gib[i], GIBS[i].rot, gp);
            }
            gh_update(R.rs == RS_PAUSE ? 0 : dt);
            if (pilot_on && (R.rs == RS_RACE || R.rs == RS_DEAD) && dt > 0) pilot_drive(dt);
#ifdef NR_PC
            if (audit_on && pilot_on == 1 && R.rs == RS_RACE) audit_frame();
            if (audit_on && R.rs == RS_FINISH) { audit_flush(0); audit_flush(1); }
#endif
            if (R.rs == RS_DEAD) {
                R.dead_t += dt; R.boom_t += dt;
                if (R.stunt_over && R.dead_t > 1.0f) stunt_results();
                else if (R.lv && R.lv->sd == 3 && !R.credits && !pilot_on) {
                    /* ChallengeLevel: no respawn (CarLogic+0x564 = 0). After 0.5 s the level restarts (message 0x23 ->
                     * RestartSdLevelMessage) and its Exit keeps the dead attempt as a self replay (re_N 12.1) */
                    if (R.dead_t > 0.5f) {
                        int kept = gh_self_add();
                        logm("  challenge: restart after the wreck (attempt kept: %s, %d kept)", kept >= 0 ? "yes" : "too short", gh_self_count());
                        car_sounds_off(); start_race_ex(1);
                    }
                }
                else if (R.dead_t > 2.0f && !pilot_on) {                              /* CarLogic state 7: 2 s (the replays: 34 ticks) */
                    respawn_after_death();
                    if (chase_on) ch_resync(&CH, R.car.pos);
                }
            }

            /* ---- heat, boost, jets, the car's sounds */
            v3 cpos = v3make(R.car.pos[0], R.car.pos[1], R.car.pos[2]);
            v3 cfw = v3make(R.car.rot[0][1], R.car.rot[1][1], R.car.rot[2][1]);
            v3 cup = v3make(R.car.rot[0][2], R.car.rot[1][2], R.car.rot[2][2]);
            v3 crt = v3make(R.car.rot[0][0], R.car.rot[1][0], R.car.rot[2][0]);
            float spd = nr_car_speed(&R.car), vf = nr_car_forward_speed(&R.car);
            if (R.rs == RS_RACE) {
                int boost = ci.boost || R.wings;          /* CarLogic: the jets run whenever the wings are out */
                if (boost != R.boosting) {
                    if (boost) { snd_post("Play_Boost", OBJ_CAR); layer_event("Boost_On"); }
                    else { snd_post("Stop_Boost", OBJ_CAR); layer_event("Boost_Off"); }
                    R.boosting = boost;
                }
                /* the jets (0x703400): out in roll mode (Play_CarJetEject / Retract), each one firing while the stick
                 * leans its way past 0.05 (Play_CarJet while any fires): roll right -> the left jet, roll left -> the
                 * right one, pitch -> the front or the back one */
                int jets = R.roll_mode;
                if (jets != R.jets) { snd_post(jets ? "Play_CarJetEject" : "Play_CarJetRetract", OBJ_CAR); R.jets = jets; }
                int thrust = 0;
                if (jets) {
                    float lr = ci.roll_x, lp = -ci.roll_y;      /* (as the car leans them, nr_car.c) */
                    if (lr < -0.05f) thrust |= 8; else if (lr > 0.05f) thrust |= 4;
                    if (lp < -0.05f) thrust |= 1; else if (lp > 0.05f) thrust |= 2;
                }
                if ((thrust != 0) != (R.jet_thrust != 0)) snd_post(thrust ? "Play_CarJet" : "Stop_CarJet", OBJ_CAR);
                R.jet_thrust = thrust;
                /* heat: fast at first, slower near the top (the game's three rates) */
                if (boost && !infinite_heat) {
                    /* CarLogic 0x703af0: 6 a second, 2.75 above 90%, 1.125 above 99%, scaled by Boost
                     * Force / 30, of a maximum of 50 (here 100) */
                    float hr = R.heat > HEAT_MAX * 0.99f ? 1.125f : R.heat > HEAT_MAX * 0.9f ? 2.75f : 6.0f;
                    R.heat += hr * (carp.boost_force / 30.0f) * (HEAT_MAX / 50.0f) * dt;
                } else if (!boost) {
                    R.heat -= 2.0f * (HEAT_MAX / 50.0f) * dt;      /* 0x708d00: cools 2 a second */
                }
                /* CooldownPadLogic: 0.5 x the car's speed a second on the pad; CarCooler: 6 x the boost's
                 * heating rate, so boosting never overheats (PC units, max 50) */
                R.heat -= R.cool_rate * (HEAT_MAX / 50.0f) * dt;
                if (R.car_cooler) R.heat -= (carp.boost_force / 30.0f) * 6.0f * (HEAT_MAX / 50.0f) * dt;
                if (R.heat < 0) R.heat = 0;
                /* CarLogic 0x70ac1f: below z = -60 it nearly fell; below -70 it is lost; back above -40, it recovered */
                if (cpos.z > -40.0f) { if (R.nearly_fell) { R.nearly_fell = 0; ach_unlock("Down, Not Out"); } }
                else if (cpos.z < -60.0f) {
                    R.nearly_fell = 1;
                    if (cpos.z < -70.0f && !pilot_on) explode("fell out of the level", 5);
                }
                if (R.heat >= HEAT_MAX && !pilot_on) {
                    R.heat = HEAT_MAX;
                    SAVE.overheats++; if (SAVE.overheats >= 3) ach_unlock("Toasty");
                    explode("overheated", 4);                    /* ("Overheat" the first two times: the announcer) */
                }
                int alarm = R.heat > HEAT_MAX * 0.75f;
                if (alarm != R.alarm) { layer_event(alarm ? "Alarm_On" : "Alarm_Off"); R.alarm = alarm; }
                /* impacts, as CarLogic judges them: a crash kills above 60 (20 with the wings out, the
                 * saws' own Death Speed) unless the car lands on its wheels; deadly surfaces kill at once */
                if (pilot_on) R.hit.death = 0;               /* (the test drive crashes where its replay does) */
                if (R.hit.death == 2) explode("deadly surface", 2);
                else if (R.hit.death == 3) {
                    logm("  crash at %.1f %.1f %.1f, surface normal %.2f %.2f %.2f, speed %.0f, wings %d, wheels down %d", R.hit.death_p[0], R.hit.death_p[1], R.hit.death_p[2],
                         R.hit.death_n[0], R.hit.death_n[1], R.hit.death_n[2], spd, R.wings, R.car.grounded);
                    explode("high-speed crash", 3);
                }
                else if (R.hit.crash_sound == 2) snd_post("Play_Rand_MediumCrash", OBJ_CAR);
                else if (R.hit.crash_sound == 1) snd_post("Play_Rand_SmallCrash", OBJ_CAR);
                if (R.hit.shake > 0.003f && R.hit.shake > R.shake_amp * expf(-6.0f * R.shake_t)) { R.shake_amp = R.hit.shake; R.shake_t = 0; }
                int scraping = R.hit.scraping;
                if (scraping != R.scrape) { snd_post(scraping ? "Play_Rand_Scrape" : "Stop_Rand_Scrape", OBJ_CAR); R.scrape = scraping; }
                /* tyre skid: sideways sliding on the ground */
                v3 vel = v3make(R.car.vel[0], R.car.vel[1], R.car.vel[2]);
                int skid = R.car.grounded >= 2 && fabsf(dot(vel, crt)) > 6.0f && spd > 10;
                if (skid != R.skid) { snd_post(skid ? "Play_Rand_TireSkid" : "Stop_Rand_TireSkid", OBJ_CAR); R.skid = skid; }
                /* driving on walls / upside down */
                /* tricks (TrickSystem2): turns in the air, wheelies, wall and ceiling rides, grinds;
                 * scored on landing, and they cool the car */
                {
                    nr_trick_in ti; memset(&ti, 0, sizeof ti);
                    for (int w = 0; w < 4; w++) { ti.contact[w] = R.car.wheel_contact[w]; memcpy(ti.wheel_n[w], R.car.wheel_n[w], 12); }
                    memcpy(ti.rot, R.car.rot, sizeof ti.rot); memcpy(ti.angvel, R.car.angvel, 12);
                    ti.speed_mph = nr_car_mph(&R.car); ti.body_speed = spd; ti.body_contacts = R.car.body_touch;
                    nr_trick_award aw;
                    if (nr_tricks_update(&R.tricks, &ti, dt, (unsigned)rand() * 2654435761u, &aw)) {
                        char pb[24]; fmt_points(pb, sizeof pb, aw.points);
                        snprintf(R.trick_head, sizeof R.trick_head, "%s x%d", pb, aw.kinds);
                        snprintf(R.trick, sizeof R.trick, "%s", aw.text); R.trick_t = 2.6f;
                        /* Trick Complete (0x6fb7f0): the run's stats first, then the lifetime ones */
                        int lifetime_before = SAVE.points;
                        R.score += aw.score; R.tricks_level++; if (aw.score > R.biggest) R.biggest = aw.score;
                        if (!infinite_heat) { R.heat -= aw.heat_relief * HEAT_MAX; if (R.heat < 0) R.heat = 0; }
                        if (!strcmp(aw.event, "Play_Rand_TrickAwesomePhrases")) ann_phrase(trick_phrase(aw.score));
                        else ann_queue(aw.event);
                        if (aw.barrel_roll) { ach_unlock("Do A Barrel Roll!"); SAVE.barrel_rolls++; }
                        SAVE.wall_dist += aw.wall_mi; SAVE.ceil_dist += aw.ceil_mi;     /* LifetimeData (miles) */
                        if (R.tricks_level == 3) ach_unlock("Hat Trick");
                        if (SAVE.tricks + 1 >= 100) ach_unlock("Tricks Make You Cool");
                        if (aw.score > 9000) ach_unlock("Over 9000");
                        if (R.score >= 100000 && R.lv && R.lv->score && !(R.lv->score & 1)) ach_unlock("100 Grand");   /* stunt mode */
                        if (lifetime_before >= 1000000) ach_unlock("Million Point Mark");
                        SAVE.tricks++; SAVE.points += aw.score; if (aw.score > SAVE.biggest) SAVE.biggest = aw.score;
                    }
                    if (R.tricks.wheelie[0] > 5) ach_unlock("Wheelie of Fortune");
                    if (R.tricks.wall_dur > 3) ach_unlock("Wall Ride");
                    if (R.tricks.ceil_dur > 1.5f) ach_unlock("Gravity? No Care.");
                }
                if (R.trick_t > 0) R.trick_t -= dt;
                /* boost rings */
                for (int i = 0; i < G.nring; i++) {
                    ring_t *g = &G.ring[i];
                    if (g->cool > 0) { g->cool -= dt; continue; }
                    float dx = cpos.x - g->p[0], dy = cpos.y - g->p[1], dz = cpos.z - g->p[2];
                    float along = dx * g->ax[0] + dy * g->ax[1] + dz * g->ax[2];
                    float d2 = dx * dx + dy * dy + dz * dz - along * along;
                    if (fabsf(along) < 3 && d2 < g->radius * g->radius) {
                        float dirs = (vel.x * g->ax[0] + vel.y * g->ax[1] + vel.z * g->ax[2]) >= 0 ? 1.0f : -1.0f;
                        for (int k = 0; k < 3; k++) R.car.vel[k] += g->ax[k] * dirs * g->strength * 0.25f;
                        R.heat -= g->heal; if (R.heat < 0) R.heat = 0;
                        snd_set_obj_pos(OBJ_RING, g->p); snd_post("Play_RingRipple", OBJ_RING);
                        g->cool = 1.0f;
                    }
                }
            }
            {   /* engine pitch (CarLogic 0x703ff0): revs build while a pedal is held and fall
                 * back otherwise, rising further while the car is in the air */
                int pedal = ci.throttle > 0 || ci.brake > 0 || (R.rs == RS_COUNT && (pad.buttons & (act_mask(ACT_GAS) | act_mask(ACT_BRAKE))));
                if (pedal) R.rev += 4.2f * dt;
                R.rev = fminf(16.6667f, fmaxf(0, R.rev - 3.0f * dt));
                float mult = 1;
                if (!R.car.grounded) { R.air_rev += pedal ? dt : -0.35f * dt; mult = fminf(R.air_rev + 1, 5); }
                else R.air_rev = 0;
                float v = (fabsf(vf) * 0.4f + R.rev * 0.6f) * mult;
                R.rpm = fmaxf(0.01f, fminf(50.0f, fmaxf(0.125f, v)) * 0.02f);
            }
            if ((R.frame & 1) == 0) {
                snd_set_param("CarEngineRPM", R.rpm);
                snd_set_param("Heat_Level", R.heat / HEAT_MAX);
                float cp_[3] = { cpos.x, cpos.y, cpos.z };
                snd_set_obj_pos(OBJ_CAR, cp_);
                snd_set_obj_pos(OBJ_SPARK, cp_);
                float st = R.rs == RS_RACE ? (spd - 100.0f) / 50.0f : 0; st = st < 0 ? 0 : st > 1 ? 1 : st;
                snd_set_obj_param(OBJ_SPARK, "CMD_Volume", st);
            }
            update_emitters(cpos);
            if (objs_on) update_obj_sounds(cpos);
            if (R.rs != RS_PAUSE) shock_update(dt, R.rs == RS_RACE);

            /* ---- checkpoints and finish */
            /* the race clock (BaseLevel +0x48) runs on while the car is wrecked: a crash costs its 2 s (the
             * shipped replays' lengths match their times only so); the stunt clock as well (re_I 4) */
            if (R.rs == RS_DEAD) { R.race_t += dt; if (R.stunt && !R.stunt_over) R.stunt_left -= dt; }
            if (R.rs == RS_RACE) {
                R.race_t += dt; R.alive_t += dt;
                if (R.stunt && !R.stunt_over) {          /* StuntLevel::PostUpdate (0x724130) */
                    static const char *NUM[10] = { "Play_10", "Play_9", "Play_8", "Play_7", "Play_6", "Play_5", "Play_4", "Play_3", "Play_2", "Play_1" };
                    R.stunt_left -= dt;
                    for (int k = 0; k < 10; k++) if (!(R.stunt_voice >> k & 1) && R.stunt_left < 11 - k) { snd_post(NUM[k], 0); R.stunt_voice |= 1 << k; }
                    {   static const struct { float total, at; const char *ev; } W[3] = {
                            { 300, 301, "Play_FiveMinutesLeft" }, { 60, 61, "Play_OneMinuteLeft" }, { 30, 31, "Play_ThirtySecondsLeft" } };
                        for (int k = 0; k < 3; k++) if (!(R.stunt_voice >> (10 + k) & 1) && 300.0f > W[k].total && R.stunt_left < W[k].at) {
                            snd_post(W[k].ev, 0); R.stunt_voice |= 1 << (10 + k);
                        }
                    }
                    if (R.stunt_left <= 0.8f) {             /* time up: the car coasts to a stop, then the results */
                        R.stunt_over = 1; R.coast_t = R.settle_t = 0;
                        if (objs_on) nr_objs_level_finished(&OBJS);
                        if (R.wings) { R.wings = 0; snd_post("Play_WingIn", OBJ_CAR); layer_event("Wings_Off"); }
                        logm("  stunt clock out: score %d", R.score);
                    }
                }
                if (R.stunt_over) {                       /* CarSettled (0x6ffdc0) */
                    float w2 = R.car.angvel[0] * R.car.angvel[0] + R.car.angvel[1] * R.car.angvel[1] + R.car.angvel[2] * R.car.angvel[2];
                    R.coast_t += dt;
                    if (spd * spd < 100 && w2 < 3) R.settle_t += dt; else R.settle_t = 0;
                    if (R.settle_t > 1.5f || R.coast_t > 30) stunt_results();
                }
                for (int i = 0; i < G.ncp; i++) {
                    if (R.cp_hit[i]) continue;
                    float dx = cpos.x - G.cp[i][0], dy = cpos.y - G.cp[i][1], dz = cpos.z - G.cp[i][2];
                    int pass;
                    if (G.cp_has_R[i]) {
                        /* CheckpointLogic (0x728070): the car crossing the gate's plane (its forward axis changes sign
                         * within 20 units) inside its frame, 9.5 x 8.25 units either side of the middle times its scale */
                        const float (*M)[3] = G.cpR[i];
                        float lx = dx * M[0][0] + dy * M[1][0] + dz * M[2][0];
                        float ly = dx * M[0][1] + dy * M[1][1] + dz * M[2][1];
                        float lz = dx * M[0][2] + dy * M[1][2] + dz * M[2][2];
                        pass = fabsf(ly) <= 20 && !(R.cp_ly[i] * ly > 0) &&
                               fabsf(lx / G.cp_scl[i][0]) <= 9.5f && fabsf(lz / G.cp_scl[i][2]) <= 8.25f;
                        R.cp_ly[i] = ly;
                    } else pass = dx * dx + dy * dy < 24 * 24 && fabsf(dz) < 20;
                    if (pass) {
                        R.cp_hit[i] = 1; R.cp_count++; R.last_cp = i;
                        ann_queue("Play_NitronicRushCheckpoint");          /* (CheckpointLogic 0x727e8f: the announcer) */
                        snprintf(R.msg, sizeof R.msg, "Checkpoint"); R.msg_t = 1.2f;     /* (the PC: no count) */
                        R.resp_kind = 1;
                        if (objs_on) {             /* the gate's five ghosts rise */
                            float Rc[3][3];
                            if (G.cp_has_R[i]) memcpy(Rc, G.cpR[i], 36);
                            else {
                                v3 f_ = norm(v3make(cfw.x, cfw.y, 0)), u_ = v3make(0, 0, 1), r_ = cross(f_, u_);
                                float c9[3][3] = { { r_.x, f_.x, u_.x }, { r_.y, f_.y, u_.y }, { r_.z, f_.z, u_.z } }; memcpy(Rc, c9, 36);
                            }
                            nr_objs_checkpoint_passed(&OBJS, G.cp[i], Rc);
                        }
                        /* CheckpointLogic (0x727c10): the heat back to nothing, the car's cooling glow (and with it
                         * Play_CoolDown) */
                        snd_set_param("Heat_Level", R.heat / HEAT_MAX); R.heat = 0;     /* (0x727ff1: the RTPC before the reset) */
                        if (R.cool_t <= 0) snd_post("Play_CoolDown", OBJ_CAR);
                        R.cool_t = 1.5f;
                        logm("  checkpoint %d/%d at %.2f s", R.cp_count, G.ncp, R.race_t);
                    }
                }
                int fin = 0;
                if (G.nfbox) for (int i = 0; i < G.nfbox; i++) {
                    /* LevelSwitch (0x6a8040): l = R^T (car - pos) / scale; in when lx^2 + (lz - 2)^2 < 81, |ly| < 19, lz > 0 */
                    const float *b = G.fbox[i];
                    float d3[3] = { cpos.x - b[0], cpos.y - b[1], cpos.z - b[2] }, l[3];
                    for (int k = 0; k < 3; k++) l[k] = (d3[0] * b[3 + k] + d3[1] * b[6 + k] + d3[2] * b[9 + k]) / (fabsf(b[12 + k]) > 1e-4f ? b[12 + k] : 1);
                    if (l[0] * l[0] + (l[2] - 2) * (l[2] - 2) < 81 && fabsf(l[1]) < 19 && l[2] > 0) fin = 1;
                }
                else for (int i = 0; i < G.nfinish; i++) {
                    float dx = cpos.x - G.finish[i][0], dy = cpos.y - G.finish[i][1], dz = cpos.z - G.finish[i][2];
                    if (dx * dx + dy * dy < 22 * 22 && fabsf(dz) < 25) fin = 1;
                }
                if (R.finish_obj) fin = 1;                   /* EnumLogic type 6 (EndTunnel) */
                if (!G.nfinish && !(objs_on && nr_objs_count(&OBJS, OK_FINISH)) && L.d.npath > 2 && L.pathlen[R.ai.idx] > L.pathlen[L.d.npath - 1] - 20) fin = 1;
                if (R.stunt || R.rs != RS_RACE) fin = 0;     /* stunt levels end on the clock */
                if (fin && R.credits) {                      /* BaseLevel::Finish: state 5, the screen fades, then the menu */
                    R.rs = RS_FINISH; R.t = 0; R.done_time = R.race_t; warp_out_portal();
                    fin = 0;
                }
                if (fin) {
                    R.rs = RS_FINISH; R.t = 0; R.done_time = R.race_t; R.result_sel = 0;
                    warp_out_portal();                       /* the car leaves for the end tunnel's portal */
                    car_sounds_off();
                    snd_post("Play_WarpOut", 0);                /* CarFinish 0x705da0: the car leaves for its stand-in */
                    ann_queue("Play_Finish");
                    save_level *sl = save_level_get(&SAVE, R.lv->xml, 1);
                    run_finished(R.race_t, 1);
                    R.new_best = sl && (sl->best <= 0 || R.race_t < sl->best);
                    if (sl) { if (R.new_best) sl->best = R.race_t; sl->done = 1; sl->unlocked = 1; }
                    if (R.new_best && sl && sl->best != R.race_t) {}
                    fe_set *fs = &FE_SETS[R.set];
                    if (R.level + 1 < fs->n) {
                        save_level *nx = save_level_get(&SAVE, fs->lv[R.level + 1].xml, 1);
                        if (nx && !nx->unlocked && !fs->lv[R.level + 1].unlocked) { nx->unlocked = 1; R.unlocked_next = 1; ui_popup(1, fs->lv[R.level + 1].name); }
                    }
                    if (objs_on) nr_objs_level_finished(&OBJS);  /* TriggerAtFinish: Homage, Great Scott!, Outta Time */
                    if (R.lv->sd == 2) { ann_queue("Play_TutorialComplete"); ach_unlock("Initiate"); }
                    else if (R.lv->score && R.lv->score != 6 && !R.wrecks) ach_unlock("Still Alive");
                    if (R.score > SAVE.best_score) SAVE.best_score = R.score;
                    save_write(&SAVE, SAVE_PATH);
                    logm("RESULT %s finished in %.2f s, checkpoints %d/%d, wrecks %d%s", R.lv->xml, R.race_t, R.cp_count, G.ncp, R.wrecks, chase_on ? ", physics drive" : ""); fx_log();
                    if (chase_on) logm("CHASE %s assists %d resyncs %d replay %.1f s", R.lv->xml, CH.assists, CH.resyncs, ch_length(&CH));
                }
            }

            /* ---- leaving the race */
            if (restart) { car_sounds_off(); start_race(); }
            if (next && R.story && R.lv && R.lv->sd == 2 && !autoplay) {   /* TutorialLevel 0x70d920: in Story mode, the intro */
                car_sounds_off(); stop_emitters();
                if (begin_intro()) { mode = MODE_INTRO; last = now_us(); continue; }
            }
            if (next) {
                fe_set *fs = &FE_SETS[R.set];
                if (R.story && R.level + 1 < fs->n && fs->lv[R.level + 1].installed) {
                    car_sounds_off(); stop_emitters();
                    begin_level(R.set, R.level + 1, 0); last = now_us();
                    continue;
                }
                if (R.story && R.level + 1 >= fs->n && begin_outro()) { mode = MODE_OUTRO; last = now_us(); continue; }   /* the last story level */
                /* Arcade: Continue goes on to the set's next level (0x6f8c30); stunt levels back to the menu */
                if (!R.story && !R.stunt && !autoplay && R.level + 1 < fs->n && fs->lv[R.level + 1].installed) {
                    save_level *nx = save_level_get(&SAVE, fs->lv[R.level + 1].xml, 0);
                    if (fs->lv[R.level + 1].unlocked || (nx && nx->unlocked)) {
                        car_sounds_off(); stop_emitters();
                        begin_level(R.set, R.level + 1, 0); last = now_us();
                        continue;
                    }
                }
                go_menu = 2;
            }
            if (go_menu) {
                if (autoplay) auto_level++;                  /* (a tutorial's own end: on to the next level) */
                car_sounds_off(); stop_emitters(); level_sounds_exit();
                snd_post("Stop_CarEngine", OBJ_CAR);
                front_menu_level();
                mode = MODE_FRONT;
                front_enter(go_menu == 2 && !R.story ? FE_LEVELS : FE_MAIN);
                save_write(&SAVE, SAVE_PATH);
                last = now_us();
                continue;
            }

            /* ---- camera */
            float M[4][4];
            float Mi[4][4] = { { crt.x, cfw.x, cup.x, cpos.x }, { crt.y, cfw.y, cup.y, cpos.y }, { crt.z, cfw.z, cup.z, cpos.z }, { 0, 0, 0, 1 } };
            memcpy(M, Mi, sizeof M);
            if (R.rs == RS_DEAD) {                    /* the death camera: stays, follows the pieces */
                v3 Mc = R.boom; float spread = 0;
                if (ngibs) {
                    Mc = v3make(0, 0, 0);
                    for (int g = 0; g < ngibs; g++) Mc = add(Mc, GIBS[g].pos);
                    Mc = mul(Mc, 1.0f / ngibs);
                    v3 S = v3make(0, 0, 0);
                    for (int g = 0; g < ngibs; g++) { v3 dd = sub(GIBS[g].pos, Mc); S = add(S, v3make(fabsf(dd.x), fabsf(dd.y), fabsf(dd.z))); }
                    spread = len(mul(S, 1.0f / ngibs));
                }
                cam_track(&CAM, Mc, spread, dt);
            } else if (R.rs == RS_FINISH) {                 /* the finish camera (mode 6): stays, follows the car */
                cam_track(&CAM, cpos, 0, dt);
            } else if (R.rs == RS_COUNT && WARPM.nch) {     /* the countdown's: round the stand-in (0x712dc3) */
                cam_intro(&CAM, cpos, R.car.rot, R.cd, &COLL);
            } else if (R.rs != RS_PAUSE) cam_chase_step(&CAM, &R.car, &COLL, dt, R.wings ? ci.pitch : 0, part_tmax[0] > 0 ? part_t[0] / part_tmax[0] : (float)R.wings, R.credits, cam_tx, cam_ty);
            if (CAM.fade < 1) CAM.fade = fminf(1, CAM.fade + 3.0f * dt);          /* Fade Speed 3: from black */
            if (R.rs == RS_COUNT) CAM.fade = fminf(CAM.fade, fminf(1, fmaxf(0, R.cd - CD_T0)));   /* the countdown's: over its first second */
            v3 eye = CAM.pos, fwd = mul(CAM.z, -1), upv = CAM.y;
            if (R.rs == RS_FINISH) {
                frc_update(dt);
                if (frc_active()) {
                    eye = FRC.pos; fwd = norm(sub(FRC.look, FRC.pos));
                    v3 rr = cross(fwd, v3make(0, 0, 1)); if (len(rr) < 1e-3f) rr = v3make(1, 0, 0);
                    upv = norm(cross(norm(rr), fwd));
                }
            }
            R.eye = eye;
            R.fov = frc_active() ? FRC.fov : CAM.fov;
            R.view_fwd = fwd; R.view_up = upv;
            /* camera shake (ChaseCamera: x 50/s damped 6, y 70/s damped 7) */
            if (R.shake > 0) { R.shake_amp = fmaxf(R.shake_amp, fminf(0.1f, R.shake * 0.3f)); R.shake_t = 0; R.shake = 0; }
            if (R.shake_amp > 0.0005f) {
                R.shake_t += dt;
                float sx = R.shake_amp * expf(-6.0f * R.shake_t) * sinf(50.0f * R.shake_t);
                float sy = R.shake_amp * expf(-7.0f * R.shake_t) * sinf(70.0f * R.shake_t);
                v3 rr = norm(cross(fwd, upv)), uu = cross(rr, fwd);
                fwd = norm(add(fwd, add(mul(rr, sx), mul(uu, sy))));
                if (R.shake_t > 1.2f) R.shake_amp = 0;
            }
            float lis[3] = { eye.x, eye.y, eye.z }, lf[3] = { fwd.x, fwd.y, fwd.z }, lu[3] = { upv.x, upv.y, upv.z };
            snd_set_listener(lis, lf, lu);

            /* ---- draw */
            static gfx_obj objs[MAX_DRAWOBJ]; int nobj = 0; memset(objs, 0, sizeof objs);
            gfx_sprite spr[24]; int nspr = 0;
            /* the warp-in through the countdown and the warp-out from the finish: the stand-in, not the car */
            int in_count = R.rs == RS_COUNT || (R.rs == RS_PAUSE && R.rs_before_pause == RS_COUNT);
            int in_fin = R.rs == RS_FINISH || (R.rs == RS_PAUSE && R.rs_before_pause == RS_FINISH);
            int warp = WARPM.nch && (in_count || in_fin);
            if (R.rs != RS_DEAD && !frc_active() && CAR.nch) {
                if (warp) {
                    float wp = in_count ? WARP_RATE * (R.cd - CD_T0) / CD_RATE : 0.85f - WARP_RATE * R.t;
                    warp_obj(&objs[nobj++], M, wp);
                }
                else { objs[nobj].model = &CAR; memcpy(objs[nobj].m, M, sizeof M); nobj++; }   /* (drawn in the bumper view too, as the PC) */
            }
            /* moving parts: wings out while flying, boosters out while boosting, jets with the jets */
            {
                float tgt[3] = { R.wings ? part_tmax[0] : 0, R.boosting ? part_tmax[1] : 0, R.jets ? part_tmax[2] : 0 };
                for (int g = 0; g < 3; g++) {
                    if (part_t[g] < tgt[g]) part_t[g] = fminf(tgt[g], part_t[g] + dt);
                    else if (part_t[g] > tgt[g]) part_t[g] = fmaxf(tgt[g], part_t[g] - dt);
                }
            }
            if (R.rs != RS_DEAD && !frc_active() && ncp && !warp) {
                pose_parts(M);
                /* the wing tips' sparkle trail, while the wings are out */
                int wing_force = 0;
#ifdef NR_PC
                wing_force = getenv("WINGFORCE") != NULL;      /* (the test machine: the wing tips trail on the ground too) */
#endif
                if (particles_ok && R.rs == RS_RACE && (wing_force || (R.wings && part_tmax[0] > 0 && part_t[0] >= 0.9f * part_tmax[0]))) {
#ifdef NR_PC
                    if (getenv("TRKLOG") && nce && !CE[0].st.primed) logm("  wing tips: trailing (%d emitters, %d particles)", nce, particles_count());
#endif
                    for (int i = 0; i < nce; i++) {
                        float M3[3][4]; for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) M3[r][c] = PW[CE[i].part][r][c];
                        unsigned m0 = particles_made;
                        particles_emit(&CE[i].e, &CE[i].st, M3, dt, 1.0f);
                        fx_wing += particles_made - m0;
                        if (test_shots && fx_wing - (particles_made - m0) < 120 && fx_wing >= 120 && !test_shot_req[0])
                            snprintf(test_shot_req, sizeof test_shot_req, "%s_wings", R.lv->xml);   /* (the first 120) */
                    }
                } else if (R.rs != RS_PAUSE) emitters_reset();
                for (int i = 0; i < ncp && nobj < MAX_DRAWOBJ; i++) if (CP[i].model >= 0) {
                    objs[nobj].model = &CPM[CP[i].model]; memcpy(objs[nobj].m, PW[i], sizeof PW[i]); nobj++;
                }
                for (int i = 0; i < ncf && nobj < MAX_DRAWOBJ && FLAME.nch; i++) {
                    int g = CP[CF[i].part].group;
                    int on = (g == 1 && R.boosting && part_t[1] > 0.3f) || (g == 2 && R.jets && CP[CF[i].part].jet >= 0 && (R.jet_thrust >> CP[CF[i].part].jet & 1) && part_t[2] >= part_tmax[2] * 0.9f);
                    if (!on) continue;
                    float fl = 0.85f + 0.3f * sinf(R.t * 40 + i * 2.1f);
                    float sc[3] = { CF[i].scl[0], CF[i].scl[1], CF[i].scl[2] * fl };
                    float L[4][4]; m4trs(L, CF[i].pos, CF[i].ori, sc);
                    m4mul(objs[nobj].m, PW[CF[i].part], L); objs[nobj].model = &FLAME; nobj++;
                }
            }
            if (R.rs != RS_DEAD && R.boosting && nspr < 24) {
                v3 bp = add(cpos, mul(cfw, -1.9f));
                spr[nspr].pos = bp; spr[nspr].size = 2.2f; spr[nspr].r = 1.0f; spr[nspr].g = 0.45f; spr[nspr].b = 0.15f; spr[nspr].tex = 0; spr[nspr].angle = 0; nspr++;
            }
            nobj = gh_draw(objs, nobj, MAX_DRAWOBJ);
            /* the tunnels' portals (ChildObjectLogic's Portal of tunnel2 / EndTunnel2: traffic.fx, scrolling blue) */
            for (int i = 0; PORTALM.nch && R.lv && i < nportals && nobj < MAX_DRAWOBJ; i++) if (!strcmp(PORTALS[i].lv, R.lv->xml)) {
                gfx_obj *g = &objs[nobj++]; memset(g, 0, sizeof *g); g->model = &PORTALM;
                for (int r = 0; r < 3; r++) { for (int c = 0; c < 3; c++) g->m[r][c] = PORTALS[i].R[r][c] * PORTALS[i].s[c]; g->m[r][3] = PORTALS[i].p[r]; }
                g->m[3][3] = 1;
            }
            nobj = add_level_objects(objs, nobj, eye, fwd);
            if (R.rs != RS_DEAD && !frc_active()) nobj = shock_draw(objs, nobj, MAX_DRAWOBJ, eye);
            /* sparks where the body scrapes (CarLogic's sparks emitter, rate = sliding speed / 3) */
            if (particles_ok && R.rs != RS_PAUSE) {
                if (R.rs == RS_RACE) {
                    unsigned m0 = particles_made, had = fx_sparks; car_sparks(); fx_sparks += particles_made - m0;
                    if (test_shots && had < 40 && fx_sparks >= 40) test_spark_t = R.race_t + 0.15f;   /* (a moment later: they start under the car) */
                }
#ifdef NR_PC
                if (getenv("TRKLOG") && R.rs == RS_RACE) {   /* (the scrapes: their stretches) */
                    static int on; static float t0, mx; static int nc;
                    if (R.hit.spark_rate > 0) { if (!on) { on = 1; t0 = R.race_t; mx = 0; nc = 0; } mx = fmaxf(mx, R.hit.spark_rate); nc = R.car.ncontact > nc ? R.car.ncontact : nc; }
                    else if (on) { on = 0; logm("  scrape %.2f-%.2f s: rate up to %.1f, %d contacts, speed %.0f", t0, R.race_t, mx, nc, nr_car_speed(&R.car)); }
                }
#endif
                if (objs_on) run_emitters(eye, fwd, dt);
                particles_update(dt);
            } else if (R.hit.spark_rate >= 2.0f && R.rs == RS_RACE) {
                v3 sp = v3make(R.hit.spark[0], R.hit.spark[1], R.hit.spark[2]);
                for (int i = 0; i < 3 && nspr < 24; i++) {
                    float a = R.t * 37.0f + i * 2.1f;
                    spr[nspr].pos = add(sp, v3make(0.25f * sinf(a), 0.25f * cosf(a * 1.3f), 0.2f * sinf(a * 0.7f)));
                    spr[nspr].size = 0.35f + 0.25f * (i & 1); spr[nspr].angle = a;
                    spr[nspr].r = 1.6f; spr[nspr].g = 0.9f; spr[nspr].b = 0.35f; spr[nspr].tex = 0; nspr++;
                }
            }
            for (int i = 0; i < ngibs && nobj < MAX_DRAWOBJ; i++) {
                gib_t *g = &GIBS[i];
                if (g->life <= 0) continue;
                float G4[4][4] = { { g->rot[0][0], g->rot[0][1], g->rot[0][2], g->pos.x }, { g->rot[1][0], g->rot[1][1], g->rot[1][2], g->pos.y },
                                   { g->rot[2][0], g->rot[2][1], g->rot[2][2], g->pos.z }, { 0, 0, 0, 1 } };
                objs[nobj].model = &GIBM[g->model].m; memcpy(objs[nobj].m, G4, sizeof G4); nobj++;
            }
            if (R.boom_t > 0 && R.boom_t < 1.6f) {
                float k = R.boom_t / 1.6f;
                for (int i = 0; i < 10 && nspr < 24; i++) {
                    float a = i * 0.628f, rr = 1.5f + 9.0f * k * (0.6f + 0.4f * sinf(i * 1.7f));
                    spr[nspr].pos = add(R.boom, v3make(cosf(a) * rr * 0.6f, sinf(a) * rr * 0.6f, rr * 0.4f * sinf(i * 2.3f) + 1));
                    spr[nspr].size = 4 + 10 * k; spr[nspr].angle = i;
                    /* the game's explosion is a blue-white flash (splosion: Color1 0.2 0.6 1.0) */
                    spr[nspr].r = 0.5f * (1 - k) * (1 - k); spr[nspr].g = 0.9f * (1 - k); spr[nspr].b = 1.6f * (1 - k);
                    spr[nspr].tex = 0; nspr++;
                }
            }
            trk_build(eye);
            /* (SpeedFov: the same smoothed speed factor drives the FOV and the speed blur; credits: TurnOffBlur) */
            R.blur = R.credits || R.rs == RS_DEAD ? 0 : CAM.s;
            (void)spd;
            gfx_post post; memset(&post, 0, sizeof post);
            post.blur = SAVE.motion_blur ? R.blur : 0;      /* Options: Toggle Motion Blur */
            dyn_res_step(dt, SAVE.dyn_res && bench_mode < 0);
            /* autoplay: what each part of the picture costs (BENCH lines in the log). On Campaign_Level1
             * and Campaign_Level5, 10 s in, the race holds still ("Measuring performance" on screen),
             * vsync goes off and the same view is drawn 70 frames in each of these ways */
            if (bench_mode >= 0) {
                GSW.no_bloom = 0; GSW.no_wide = 0; GSW.no_post = bench_mode == 7;
                if (bench_mode == 8) post.blur = 0;
                gfx_scene_scale = bench_mode == 6 ? 0.7f : 1.0f;
                if (bench_mode == 1) nobj = R.rs != RS_DEAD && CAR.nch ? 1 : 0;         /* the car only */
                if (bench_mode == 4 || bench_mode == 5) {                                /* one model left out */
                    int k = 0;
                    for (int i = 0; i < nobj; i++) if (!objs[i].model || !strstr(objs[i].model->name, bench_mode == 4 ? "AirMine" : "core_building")) objs[k++] = objs[i];
                    nobj = k;
                }
                gfx_dbg = bench_mode == 2 ? 8 : bench_mode == 3 ? 16 : bench_mode == 9 ? 1 : bench_mode == 10 ? 4 : 0;
            }
            if (R.boom_t > 0 && R.boom_t < 0.25f) post.flash = 0.35f * (1 - R.boom_t / 0.25f);
            gfx_time = R.t;
            uint64_t g0 = now_us();
            T_logic += g0 - c0;
#ifdef NR_PC
            if (getenv("OBJSTAT") && R.rs == RS_RACE && R.race_t > atof(getenv("OBJSTAT"))) {   /* (the test machine: the objects' triangles, once) */
                static int done; if (!done) { done = 1;
                    struct { const level_rt *m; int n, tris; } A[256]; int na = 0, total = 0;
                    for (int i = 0; i < nobj; i++) {
                        const level_rt *m = objs[i].model; if (!m) continue;
                        int t = 0; for (uint32_t c = 0; c < m->nch; c++) t += m->ch[c].ni / 3;
                        int k; for (k = 0; k < na && A[k].m != m; k++) {}
                        if (k == na && na < 256) { A[na].m = m; A[na].n = 0; A[na].tris = 0; na++; }
                        if (k < na) { A[k].n++; A[k].tris += t; } total += t;
                    }
                    for (int i = 0; i < na; i++) for (int j = i + 1; j < na; j++) if (A[j].tris > A[i].tris) { __typeof__(A[0]) x = A[i]; A[i] = A[j]; A[j] = x; }
                    logm("OBJSTAT %d objects, %d triangles (before the view test)", nobj, total);
                    for (int i = 0; i < na && i < 25; i++) logm("OBJSTAT   %-32s x%-3d %6d tris (%d chunks each)", A[i].m->name, A[i].n, A[i].tris, (int)A[i].m->nch);
                }
            }
#endif
            if (!nodraw) gfx_draw_frame(&L, eye, fwd, upv, R.fov, objs, nobj, spr, nspr, &post);
            uint64_t g1 = now_us();
            T_draw += g1 - g0;
            /* (no glFinish probe here any more: draining the GPU once a second made the autoplay stutter, and the
             * dynamic resolution took those frames for slow ones; the bench measures the GPU instead) */

            if (!nodraw) {
            hud_begin();
            if (CAM.fade < 1 && R.rs != RS_DEAD) hud_rect(0, 0, 960, 544, (unsigned)((1 - CAM.fade) * 255));   /* the camera's fade from black */
            if (frc_active() && FRC.fade < 1) hud_rect(0, 0, 960, 544, (unsigned)((1 - FRC.fade) * 255));
            gh_draw_labels(R.rs == RS_PAUSE ? 0 : dt);
            if (!(R.rs == RS_FINISH && (R.fin_stage >= 1 || !R.lv->score))) draw_race_hud();
            if (bench_mode >= 0) hud_text(HUD_FONT_MAIN, 480, 70, 22, 0xFFFFFFC0, HUD_CENTER, "Measuring performance (automatic test only)");
            else if (R.rs == RS_PAUSE) pause_draw();
            if (R.credits && R.rs != RS_FINISH) cin_skip_draw();
            if (R.rs == RS_FINISH && R.credits) {
                float k = R.t - 2.5f; k = k < 0 ? 0 : k > 1 ? 1 : k;
                if (k > 0) hud_rect(0, 0, 960, 544, (unsigned)(k * 255));
            } else if (R.rs == RS_FINISH) { draw_finish(); if (R.fin_stage == 2 && PM.state == PM_DLG) ui_dlg_draw(&PM.d); }
            ui_popup_update(dt);
            ui_popup_draw();
            hud_end();
            } else ui_popup_update(dt);
            if (test_shots) {   /* (the warp-in during the countdown, the warp-out after the finish line) */
                static float last_cd = 99, last_ft = 99;
                if (test_spark_t >= 0 && R.rs == RS_RACE && R.race_t >= test_spark_t && !test_shot_req[0]) {
                    snprintf(test_shot_req, sizeof test_shot_req, "%s_sparks", R.lv->xml); test_spark_t = -1; }
                if (R.rs == RS_COUNT) { const float at[] = { -5.0f, -3.5f, -2.0f, -0.5f };
                    for (int i = 0; i < 4; i++) if (last_cd < at[i] && R.cd >= at[i]) snprintf(test_shot_req, sizeof test_shot_req, "%s_in%d", R.lv->xml, i);
                    last_cd = R.cd; } else last_cd = 99;
                if (R.rs == RS_FINISH && !R.credits) { const float at[] = { 0.3f, 1.0f, 1.7f, 2.4f, 3.1f };
                    for (int i = 0; i < 5; i++) if (last_ft < at[i] && R.t >= at[i]) snprintf(test_shot_req, sizeof test_shot_req, "%s_out%d", R.lv->xml, i);
                    last_ft = R.t; } else last_ft = -1;
                test_shot_take();
            }
            uint64_t s0 = now_us();
            T_hud += s0 - g1;
            vglSwapBuffers(GL_FALSE);
            T_swap += now_us() - s0;
            T_n++;
            {   /* a frame that took long: what it was doing (logged at most twice a second) */
                static uint64_t last_slow;
                uint64_t e = now_us();
                if (e - c0 > 30000 && e - last_slow > 500000) {
                    last_slow = e;
                    logm("  slow frame %.0f ms at %.1f s: logic %.1f, draw %.1f [sky+cull %.1f (clear %.1f sky %.1f) level %.1f objects %.1f glow %.1f glow objects %.1f particles %.1f screen %.1f], hud %.1f, swap %.1f (state %d, objects %d drawn %d culled, particles %d, draws %d, tris %d)",
                         (e - c0) / 1000.0f, R.race_t, (g0 - c0) / 1000.0f, (g1 - g0) / 1000.0f,
                         gfx_prof[0], gfx_prof[7], gfx_prof_sky, gfx_prof[1], gfx_prof[2], gfx_prof[3], gfx_prof[4], gfx_prof[5], gfx_prof[6],
                         (s0 - g1) / 1000.0f, (e - s0) / 1000.0f, R.rs, gfx_objs_drawn, gfx_objs_culled, particles_count(), gfx_draws, gfx_tris);
                }
            }
            if (quit_app) { logm("Quit Game"); break; }   /* the pause menu's / results' Quit Game */
        }

        float cpu = (now_us() - c0) / 1000.0f;
        float ms_frame = dt * 1000.0f;
        win_frames++; win_cpu += cpu;
        if (ms_frame > win_worst) win_worst = ms_frame;
        if (ms_frame > 17.2f) win_over++;                  /* (with the cap off: frames that would miss the 60 fps display) */
        if (tn - win_start > 5000000) {
            float secs = (tn - win_start) / 1e6f;
            int nv = 0, nd = 0, late = 0; float ld = 0, pk = 0; snd_stats(&nv, &nd, &ld); snd_peak(&pk, &late);
            if (mode == MODE_RACE)
                logm("%s fps %.1f worst %.1f ms over16.7 %d%s frame %.2f ms = logic %.2f (phys %.2f) + draw %.2f + hud %.2f + gpu wait %.2f | 3d %.1f/%.1f ms | objects %d (+%d culled) draws %d tris %d | race %.1f s cp %d/%d speed %.0f heat %.0f blur %.2f | sound %d voices %d decoding load %.0f%% peak %.0f%% late %d",
                     R.lv ? R.lv->xml : "?", win_frames / secs, win_worst, win_over, vsync_on ? "" : " (no cap)", win_cpu / win_frames,
                     T_n ? T_logic / 1000.0f / T_n : 0, phys_us / 1000.0f / win_frames, T_n ? T_draw / 1000.0f / T_n : 0,
                     T_n ? T_hud / 1000.0f / T_n : 0, T_n ? T_swap / 1000.0f / T_n : 0, win_gpu_n ? win_gpu_sum / win_gpu_n : 0, win_gpu,
                     gfx_objs_drawn, gfx_objs_culled, gfx_draws, gfx_tris, R.race_t, R.cp_count, G.ncp, nr_car_speed(&R.car), R.heat, R.blur, nv, nd, ld * 100, pk * 100, late);
            else
                logm("%s %d fps %.1f worst %.1f ms | sound %d voices %d decoding load %.0f%% peak %.0f%% late %d", mode == MODE_OUTRO ? "ending" : mode == MODE_INTRO ? "intro" : "menu screen", front_screen(), win_frames / secs, win_worst, nv, nd, ld * 100, pk * 100, late);
            {   char pb[200]; snd_prof(pb, sizeof pb);
#ifdef NR_PC
                late = 1;                                 /* (the PC test machine: always) */
#endif
                if (late || pk > 0.8f) logm("    mixer: %s", pb);
                if (SHK.on && mode == MODE_RACE) { logm("    sparks: %d bolts, at most %d at once", SHK.made, SHK.most); SHK.made = SHK.most = 0; }
                if (late && !autoplay) snd_debug_dump(snd_log_line);   /* (a gap: what was playing; autoplay dumps anyway) */
            }
            if (mode == MODE_RACE && (gh_count() || gh_rec_on())) { char gs[400]; gh_stats(gs, sizeof gs); logm("    %s", gs); }
            if ((autoplay && (R.frame % 4) == 0)
#ifdef NR_PC
                 || getenv("SND_DUMP")
#endif
                ) snd_debug_dump(snd_log_line);
            win_start = tn; win_frames = 0; win_over = 0; win_worst = 0; win_cpu = 0; phys_us = 0; win_gpu = win_gpu_sum = 0; win_gpu_n = 0;
            T_all_busy += T_logic + T_draw + T_hud; T_all_swap += T_swap;
            T_logic = T_draw = T_hud = T_swap = 0; T_n = 0;
        }
    }
    logm(autoplay ? "autoplay finished" : "leaving");
    (void)quit_app;
    save_write(&SAVE, SAVE_PATH);
    snd_shutdown();
    io_shutdown(); g_log = 0;
    sceKernelExitProcess(0);
    return 0;
}
