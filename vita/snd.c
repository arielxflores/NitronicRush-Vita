/* Sound engine for the Vita port: a small player for the game's own Wwise content.
 *
 * converter/nr_audio.py flattens Main.bnk into snd/sound.nsb: the actor-mixer and music
 * hierarchies (nodes), events and their actions, RTPC curves, attenuations and the media
 * table. Every Wwise Vorbis stream is converted to plain Ogg Vorbis (snd/<id>.ogg) and decoded
 * here with stb_vorbis. What is implemented, matching how the game's bank uses Wwise:
 *   - events: Play, Stop, Pause, Resume, SetVolume/SetPitch/SetLPF (+reset), with delays,
 *     fade times and fade curves; targets can be sounds, containers, music or busses
 *   - sounds with loops, random/sequence containers (step and continuous, avoid-repeat,
 *     weights), layer containers with per-child crossfade curves
 *   - interactive music: playlist containers (sequence/random groups, loop counts), segments
 *     with entry/exit cues (pre-entry and post-exit overlap), tracks with clips and trims;
 *     the layer events (Set_Volume_*_On/Off/Jump) move track volumes with fades
 *   - busses with volume RTPCs (Master/Music/SFX/Announcer/Ambient sliders)
 *   - game parameters (global or per object) driving volume, pitch and low-pass curves
 *   - 3D: distance attenuation curves (volume + low-pass), stereo panning, and the per-object
 *     Rel_Distance parameter (approach speed, for the doppler pitch curves) worked out here
 * Voices that are inaudible (e.g. music layers turned down to -96 dB) are not decoded; their
 * position keeps running and they seek back in sync when they come up.
 *
 * From Init.bnk (the mixer busses; converter/nr_audio.py keeps only their volume curves), done here by hand:
 *   - the Tutorial bus' Wwise Meter (RMS, attack 0.1 s, release 1 s, -48..-12 dB) writes the side-chain game
 *     parameter 0x3F2FEE5D, which the Music and "No Music Sub" busses' volume curves follow: the music and the
 *     effects duck under the tutorial's voice (the "Standard Side Chain" bus has a meter too, but nothing plays on it)
 *   - the states: Crash_States = Crashed puts a low-pass of 49 on the Music and No Music Sub busses,
 *     Announcer_Crash = Crashed one of 39 on the Announcer bus, with the state groups' transition times
 *   - the four objects with a playback limit of one instance (the car's jet sounds: the oldest goes; the
 *     engine's layer: a new one is not started while one plays)
 * (Not done: the busses' dynamics - the Master's compressor (-14 dB RMS, 3:1) and +3 dB, the Announcer's
 * compressor (-20 dB, 3:1) and limiter (-6 dBFS) - the Music bus' FutzBox in tunnels and the SFX / Ambient EQs.
 * nr_audio.py normalised the Music bus' volume curve (the bank's tops out at -35.5 dB), so the PC's levels going
 * into them are unknown; the two compressors only make sense together. The tutorial's voice lines, the Tutorial
 * bus' only sounds, are at -96.3 dB in the bank, so on the PC its meter never ducks anything either.)
 */
#include "snd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define STB_VORBIS_NO_PUSHDATA_API
#include "third_party/stb_vorbis.c"

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/audioout.h>
static uint64_t snd_us(void) { return sceKernelGetProcessTimeWide(); }
#else
#include <time.h>
static uint64_t snd_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000000ull + ts.tv_nsec / 1000; }
#endif

/* ------------------------------------------------------------------ bank data (sound.nsb) */
enum { T_SOUND = 1, T_RANSEQ, T_LAYER, T_MIXER, T_BUS, T_MRANSEQ, T_SEGMENT, T_TRACK, T_SWITCH };
enum { A_PLAY = 1, A_STOP, A_PAUSE, A_RESUME, A_SETVOL, A_RESETVOL, A_SETPITCH, A_RESETPITCH, A_SETLPF, A_RESETLPF, A_SETSTATE, A_OTHER };

typedef struct {
    uint32_t id; uint8_t type, flags; int16_t parent, bus, atten;
    float vol, pitch, lpf, rv0, rv1, rp0, rp1, rl0, rl1;
    int16_t loop; uint16_t rtpc_n; uint32_t rtpc_first;
    uint32_t child_first, child_n;
    int32_t xi[4]; float xf[4];
} N_Node;
typedef struct { uint16_t gp; uint8_t target, pad; int32_t curve; } N_Rtpc;
typedef struct { uint8_t scaling, pad; uint16_t n; uint32_t first; } N_Curve;
typedef struct { float x, y; uint32_t interp; } N_Point;
typedef struct { uint32_t id, first, n; } N_Event;
typedef struct { uint8_t kind, scope, curve, meaning; int32_t target; float delay, fade, value, vmin, vmax; uint32_t raw; } N_Action;
typedef struct { uint32_t id, rate; uint8_t ch, streamed; uint16_t missing; uint32_t samples, loop_start, loop_end; } N_Media;
typedef struct { int32_t media; float play_at, begin_trim, end_trim, src_dur; } N_Clip;
typedef struct { int32_t seg; uint16_t n, pad; int8_t rstype, shuffle; int16_t loop; uint32_t weight; uint16_t avoid, pad2; uint32_t first; } N_MItem;
typedef struct { int32_t node; uint32_t weight; } N_RsItem;
typedef struct { int32_t child, gp, curve; uint32_t lr_first, lr_n; } N_Assoc;
typedef struct { uint32_t id; float def; } N_GParam;
typedef struct { int32_t vol, lpf; float maxd; } N_Atten;

static struct {
    uint8_t *raw;
    N_Node *nodes; int32_t *children; N_Rtpc *rtpcs; N_Curve *curves; N_Point *points;
    N_Event *events; N_Action *actions; N_Media *media; N_Clip *clips; N_MItem *mitems;
    N_RsItem *rsitems; N_Assoc *assocs; N_GParam *gparams; N_Atten *attens;
    uint32_t n_nodes, n_children, n_rtpcs, n_curves, n_points, n_events, n_actions, n_media,
             n_clips, n_mitems, n_rsitems, n_assocs, n_gparams, n_attens;
    char *names; uint32_t names_len;
} B;

/* media location in the packs */
typedef struct {
    const char *pack; uint32_t off, size;
    uint8_t *mem;                 /* the Ogg file in memory, or */
    int16_t *pcm; uint32_t pcm_frames; int pcm_ch;   /* short files: decoded once */
    uint32_t bytes, last_use; int users;
    volatile int state;           /* M_NONE, M_LOADING (queued for the media thread), M_READY, M_FAILED */
    stb_vorbis *parked;           /* a long file's decoder, opened by the media thread (its set-up takes
                                   * the Vita 10-30 ms) and not used yet: the first voice takes it */
    volatile int repark;          /* a voice took the parked decoder: the media thread opens the next one (set by
                                   * the mixer, cleared by the media thread when the new one is in place) */
} MediaLoc;
enum { M_NONE, M_LOADING, M_READY, M_FAILED };
static MediaLoc *g_mloc;

/* ------------------------------------------------------------------ helpers */
uint32_t snd_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) { unsigned char c = (unsigned char)*s; if (c >= 'A' && c <= 'Z') c += 32; h = (h * 16777619u) ^ c; }
    return h;
}
static float db2lin(float db) { return db <= -96.0f ? 0.0f : powf(10.0f, db * 0.05f); }
static float frand(void) { static uint32_t s = 0x1234567; s = s * 1664525u + 1013904223u; return (s >> 8) * (1.0f / 16777216.0f); }

static float shape(int type, float t)
{
    if (t <= 0) return 0; if (t >= 1) return 1;
    switch (type) {
    case 0: return 1 - (1 - t) * (1 - t) * (1 - t);          /* Log3 */
    case 1: return sinf(t * 1.5707963f);                      /* Sine */
    case 2: return 1 - (1 - t) * (1 - t);                     /* Log1 */
    case 3: return 0.5f + asinf(2 * t - 1) * 0.31830989f;     /* InvSCurve */
    case 5: return 0.5f - 0.5f * cosf(t * 3.14159265f);       /* SCurve */
    case 6: return t * t;                                     /* Exp1 */
    case 7: return 1 - cosf(t * 1.5707963f);                  /* SineRecip */
    case 8: return t * t * t;                                 /* Exp3 */
    case 9: return 0;                                         /* Constant */
    default: return t;                                        /* Linear */
    }
}
static float curve_eval(int c, float x)
{
    if (c < 0 || (uint32_t)c >= B.n_curves) return 0;
    const N_Curve *cv = &B.curves[c]; const N_Point *p = &B.points[cv->first];
    if (!cv->n) return 0;
    if (x <= p[0].x) return p[0].y;
    for (uint32_t i = 0; i + 1 < cv->n; i++) {
        if (x <= p[i + 1].x) {
            float w = p[i + 1].x - p[i].x, t = w > 0 ? (x - p[i].x) / w : 1;
            float k = shape(p[i].interp, t);
            if (cv->scaling == 2) {            /* dB curves interpolate in amplitude, like Wwise */
                float a = db2lin(p[i].y), b = db2lin(p[i + 1].y), l = a + (b - a) * k;
                return l > 0.0000153f ? 20.0f * log10f(l) : -96.3f;
            }
            return p[i].y + (p[i + 1].y - p[i].y) * k;
        }
    }
    return p[cv->n - 1].y;
}

/* ------------------------------------------------------------------ runtime state */
#define MAXV 64
#define MAXP 24
#define MAXD 64
#define BLOCK 256

typedef struct {       /* a value that moves between two levels over time */
    float from, to; int64_t t0, dur; uint8_t curve, active;
} Ramp;
static float ramp_at(const Ramp *r, int64_t now)
{
    if (!r->dur || now >= r->t0 + r->dur) return r->to;
    if (now <= r->t0) return r->from;
    return r->from + (r->to - r->from) * shape(r->curve, (float)(now - r->t0) / (float)r->dur);
}
static void ramp_set(Ramp *r, float from, float to, int64_t now, float ms, int curve)
{
    r->from = from; r->to = to; r->t0 = now; r->dur = (int64_t)(ms * (SND_RATE / 1000.0f)); r->curve = (uint8_t)curve; r->active = 1;
}

typedef struct {
    int active;
    int node, media, obj, player, root;
    int64_t start, end;        /* global sample times; end < 0 = when the source ends */
    double pos;                /* source frame (fractional) */
    uint32_t src_begin, src_end;
    int loops_left;            /* 0 = forever */
    float rnd_vol, rnd_pitch, rnd_lpf;
    Ramp fade; int stopping; int paused; int64_t pause_t;
    /* decoding */
    stb_vorbis *vb; const int16_t *pcm; int opened; int ch; float *buf; int buf_frames; int64_t buf_start; /* source frame of buf[0] */
    int virt;                  /* decoder position not in sync with pos */
    int fresh;                 /* the decoder is at the file's start, nothing decoded yet */
    float lp[2]; float gl, gr; /* filter state, last gains */
    int is3d, atten;
    int noise; uint32_t nseed; float nf1, nf2;    /* synthesised woosh (SoundSeed plug-in sources) */
    int route;                 /* R_* bits: the busses it goes through that have state or effects here */
    int quiet;                 /* samples it has been silent while its decoder was kept in step */
#ifdef NR_PC
    float dbg[2][2]; int dbg_n; float dbg_max; int64_t dbg_at;   /* (the test machine, SND_CLICKLOG: its own output's jumps) */
#endif
} Voice;
enum { R_MUSIC = 1, R_NOMUSIC = 2, R_ANN = 4, R_TUT = 8 };

typedef struct {               /* music playlist or continuous container in progress */
    int active, kind;          /* 1 music, 2 continuous container */
    int node, obj, root;
    Ramp fade; int stopping; int paused; int64_t pause_t;
    /* music */
    int stack_item[8], stack_child[8], stack_loops[8], sp;
    int cur_seg; int64_t cur_zero; int64_t next_due;
    /* container */
    int pick_i, loops_done, item_voice_count; int64_t next_start; int cur_child;
} Player;

typedef struct { int64_t t; int action; int obj; } Delayed;

typedef struct {
    float pos[3]; int has_pos;
    float prev_dist; int have_prev; float rel;
    float pv[16]; uint16_t pv_set;      /* per-object game parameter values (first 16 parameters) */
} Obj;

static Voice   V[MAXV];
static Player  P[MAXP];
static Delayed D[MAXD];
static Obj     O[SND_MAX_OBJ];
static Ramp   *g_vol_ovr, *g_pitch_ovr, *g_lpf_ovr;    /* per node SetVolume etc. (absolute values) */
static float  *g_gp;                                    /* game parameter values */
static uint8_t *g_gp_set;
static int    *g_seq_pos;                               /* per node playlist position */
static int    *g_mseq;                                  /* per music playlist item step position */
static int    *g_hist;                                  /* per node avoid-repeat history (4 slots) */
static int    *g_bus_paused;
static Ramp   *g_bus_fade;
static int64_t g_now;
static float   g_lis_pos[3], g_lis_fwd[3] = {0, 1, 0}, g_lis_up[3] = {0, 0, 1};
static int     g_ready;
static nr_pak *g_pak;
static float   g_load, g_peak; static int g_stat_voices, g_stat_dec, g_late;
static float   g_peak1; static int g_late1;     /* the same, for snd_peak_sec's own reader */
/* where the mixer's time goes, for the log: opening Vorbis streams, seeking in them, decoding */
static uint64_t pf_open, pf_seek, pf_dec; static uint32_t pf_open_n, pf_seek_n, pf_open_max, pf_seek_max, pf_dec_max;
static uint64_t snd_us(void);

/* the Init.bnk busses done here (see the top) */
static int g_bus_music = -1, g_bus_nomusic = -1, g_bus_ann = -1, g_bus_tut = -1, g_gp_side = -1;
static Ramp g_crash_lpf, g_anncrash_lpf;                 /* the Crashed states' low-pass (0 = Normal) */
static int g_crash_on, g_anncrash_on;
static float g_meter = -96;                              /* the Tutorial bus' meter (dB) */

/* ------------------------------------------------------------------ command queue (game -> mixer) */
enum { C_POST = 1, C_PARAM, C_OBJPARAM, C_OBJPOS, C_LISTENER, C_STOPALL, C_STOPOBJ, C_OBJCLEAR, C_STATE };
typedef struct { int type; uint32_t id; int obj; float v[9]; } Cmd;
#define QN 512
static Cmd Q[QN];
static volatile uint32_t q_head, q_tail;
static void push_cmd(const Cmd *c)
{
    uint32_t h = __atomic_load_n(&q_head, __ATOMIC_RELAXED);
    uint32_t t = __atomic_load_n(&q_tail, __ATOMIC_ACQUIRE);
    if (h - t >= QN) return;                    /* full: drop */
    Q[h % QN] = *c;
    __atomic_store_n(&q_head, h + 1, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------ lookup */
static int find_event(uint32_t id)
{
    int lo = 0, hi = (int)B.n_events - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (B.events[m].id == id) return m; if (B.events[m].id < id) lo = m + 1; else hi = m - 1; }
    return -1;
}
static int find_gp(uint32_t id)
{
    for (uint32_t i = 0; i < B.n_gparams; i++) if (B.gparams[i].id == id) return (int)i;
    return -1;
}
/* the SoundSeed Woosh plug-ins (Main.txt "Source plug-ins"; their FxCustom RTPCs on CarEngineRPM, 0 -> 1): the
 * plug-in's own Volume (dB) and its parameter 50 (taken as the speed: the deflector's frequency scales with it) */
typedef struct { uint32_t id; float db0, db1, s0, s1; } WooshDef;
static const WooshDef WOOSH[] = {
    { 264797718u, -19.543f, 0.0f, 0.5728f, 1.4774f },     /* Woosh Rib (the tunnels' ribs) */
    { 564028017u, -9.6447f, 6.0914f, 0.6985f, 1.0f },     /* Woosh Standard */
    { 51043018u, -40.102f, -20.051f, 1.0297f, 2.1810f },  /* Woosh Checkpoint */
    { 544918316u, 0, 0, 1, 1 },                           /* FanWhoosh (no RTPCs) */
};
static const WooshDef *woosh_def(uint32_t id)
{
    for (unsigned i = 0; i < sizeof WOOSH / sizeof WOOSH[0]; i++) if (WOOSH[i].id == id) return &WOOSH[i];
    return &WOOSH[3];
}
static int g_gp_rpm = -1;                                   /* CarEngineRPM's index */
const char *snd_event_name(uint32_t id)
{
    static char out[96];
    char key[16]; snprintf(key, sizeof key, "%u ", id);
    const char *p = B.names;
    while (p && p < B.names + B.names_len) {
        const char *e = memchr(p, '\n', B.names + B.names_len - p); if (!e) e = B.names + B.names_len;
        size_t kl = strlen(key);
        if ((size_t)(e - p) > kl && !memcmp(p, key, kl)) { size_t n = e - p - kl; if (n > 95) n = 95; memcpy(out, p + kl, n); out[n] = 0; return out; }
        p = e + 1;
    }
    snprintf(out, sizeof out, "%u", id); return out;
}

/* ------------------------------------------------------------------ media / decoding
 * The mixer thread never waits for the memory card: a media thread reads each file into memory
 * when a voice first needs it, and decodes the short ones (sound effects, voices) to PCM once,
 * so they cost nothing to play again. Until its file is ready a voice runs silently and joins
 * in sync. Least recently used files are dropped when the memory budget is reached. */
static uint32_t g_use_clock;
static volatile int g_mem_bytes;
#define MEM_MAX (56u << 20)
#define PCM_FILE_MAX (112u << 10)
#define RQN 256
static int RQ[RQN]; static volatile uint32_t rq_head, rq_tail;
#ifdef __vita__
static SceUID g_media_sema = -1;
#endif

static void media_load(int i)          /* media thread */
{
    MediaLoc *m = &g_mloc[i];
    FILE *f = fopen(m->pack, "rb");
    uint8_t *d = f ? malloc(m->size) : NULL;
    int ok = d && fseek(f, m->off, SEEK_SET) == 0 && fread(d, 1, m->size, f) == m->size;
    if (f) fclose(f);
    if (!ok) { free(d); __atomic_store_n(&m->state, M_FAILED, __ATOMIC_RELEASE); return; }
    m->mem = NULL; m->pcm = NULL; m->bytes = m->size;
    if (m->size <= PCM_FILE_MAX) {
        int ch = 0, rate = 0; short *out = NULL;
        int n = stb_vorbis_decode_memory(d, (int)m->size, &ch, &rate, &out);
        if (n > 0 && out && ch > 0) {
            if (ch > 2) { for (int k = 0; k < n; k++) { out[k * 2] = out[k * ch]; out[k * 2 + 1] = out[k * ch + 1]; } ch = 2; }
            m->pcm = out; m->pcm_frames = (uint32_t)n; m->pcm_ch = ch; m->bytes = (uint32_t)n * ch * 2;
            free(d); d = NULL;
        } else free(out);
    }
    m->mem = d;
    if (d) { int err = 0; m->parked = stb_vorbis_open_memory(d, (int)m->size, &err, NULL); }
    __atomic_add_fetch(&g_mem_bytes, (int)m->bytes, __ATOMIC_RELAXED);
    __atomic_store_n(&m->state, M_READY, __ATOMIC_RELEASE);
}
static void media_repark(int i)       /* media thread: the next voice's decoder ready before it is wanted */
{
    MediaLoc *m = &g_mloc[i];
    if (__atomic_load_n(&m->state, __ATOMIC_ACQUIRE) == M_READY && m->mem && !__atomic_load_n(&m->parked, __ATOMIC_ACQUIRE)) {
        int err = 0;
        stb_vorbis *p = stb_vorbis_open_memory(m->mem, (int)m->size, &err, NULL);
        stb_vorbis *none = NULL;
        if (p && !__atomic_compare_exchange_n(&m->parked, &none, p, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) stb_vorbis_close(p);
    }
    __atomic_store_n(&m->repark, 0, __ATOMIC_RELEASE);
}
static void media_work(void)
{
    while (rq_tail != __atomic_load_n(&rq_head, __ATOMIC_ACQUIRE)) {
        int i = RQ[rq_tail % RQN];
        if (i < 0) { media_repark(i & 0x7fffffff); __atomic_store_n(&rq_tail, rq_tail + 1, __ATOMIC_RELEASE); continue; }
        media_load(i);
        __atomic_store_n(&rq_tail, rq_tail + 1, __ATOMIC_RELEASE);
    }
}
static void media_request(int i)       /* mixer thread */
{
    MediaLoc *m = &g_mloc[i];
    if (m->state != M_NONE) return;
    uint32_t h = rq_head;
    if (h - __atomic_load_n(&rq_tail, __ATOMIC_ACQUIRE) >= RQN) return;
    m->state = M_LOADING;
    RQ[h % RQN] = i;
    __atomic_store_n(&rq_head, h + 1, __ATOMIC_RELEASE);
#ifdef __vita__
    if (g_media_sema >= 0) sceKernelSignalSema(g_media_sema, 1); else media_work();
#else
    media_work();
#endif
}
static void media_request_repark(int i)    /* mixer thread */
{
    MediaLoc *m = &g_mloc[i];
    if (m->pcm || !m->mem || m->repark) return;
    uint32_t h = rq_head;
    if (h - __atomic_load_n(&rq_tail, __ATOMIC_ACQUIRE) >= RQN) return;
    __atomic_store_n(&m->repark, 1, __ATOMIC_RELEASE);
    RQ[h % RQN] = (int)((unsigned)i | 0x80000000u);
    __atomic_store_n(&rq_head, h + 1, __ATOMIC_RELEASE);
#ifdef __vita__
    if (g_media_sema >= 0) sceKernelSignalSema(g_media_sema, 1); else media_work();
#else
    media_work();
#endif
}
static void media_trim(void)           /* mixer thread */
{
    while (__atomic_load_n(&g_mem_bytes, __ATOMIC_RELAXED) > (int)MEM_MAX) {
        int best = -1; uint32_t bt = 0xFFFFFFFF;
        for (uint32_t i = 0; i < B.n_media; i++)          /* (not one used in the last moments: a voice may be about to open it) */
            if (g_mloc[i].state == M_READY && !g_mloc[i].users && !__atomic_load_n(&g_mloc[i].repark, __ATOMIC_ACQUIRE) &&
                g_mloc[i].last_use < bt && g_use_clock - g_mloc[i].last_use > 64) { bt = g_mloc[i].last_use; best = (int)i; }
        if (best < 0) break;
        MediaLoc *m = &g_mloc[best];
        stb_vorbis *pk = __atomic_exchange_n(&m->parked, NULL, __ATOMIC_ACQ_REL);
        if (pk) stb_vorbis_close(pk);
        free(m->mem); free(m->pcm); m->mem = NULL; m->pcm = NULL;
        __atomic_sub_fetch(&g_mem_bytes, (int)m->bytes, __ATOMIC_RELAXED);
        m->state = M_NONE;
    }
}

/* 0: playing from memory; 1: not loaded yet; -1: missing */
static int voice_open(Voice *v)
{
    MediaLoc *m = &g_mloc[v->media];
    if (!m->pack || B.media[v->media].missing) return -1;
    int st = __atomic_load_n(&m->state, __ATOMIC_ACQUIRE);
    if (st == M_FAILED) return -1;
    if (st != M_READY) {
        /* waiting counts as using it: the memory trim must not drop a file a voice is waiting for
         * (a new file has the oldest use time, so it was dropped as soon as it arrived when the
         * cache was full - the ending's music never played) */
        m->last_use = ++g_use_clock;
        media_request(v->media); return 1;
    }
    m->last_use = ++g_use_clock; m->users++;
    if (m->pcm) { v->pcm = m->pcm; v->ch = m->pcm_ch; }
    else {
        v->vb = __atomic_exchange_n(&m->parked, NULL, __ATOMIC_ACQ_REL);
        if (v->vb) media_request_repark(v->media);     /* (the media thread opens the next voice's now) */
        else if (__atomic_load_n(&m->repark, __ATOMIC_ACQUIRE)) { m->users--; return 1; }   /* (that one is on its way: wait, silent) */
        else {
            int err = 0;
            uint64_t t0 = snd_us();
            v->vb = stb_vorbis_open_memory(m->mem, (int)m->size, &err, NULL);
            uint32_t dt_ = (uint32_t)(snd_us() - t0); pf_open += dt_; pf_open_n++; if (dt_ > pf_open_max) pf_open_max = dt_;
            media_request_repark(v->media);
        }
        if (!v->vb) { m->users--; return -1; }
        v->fresh = 1;
        v->ch = stb_vorbis_get_info(v->vb).channels; if (v->ch > 2) v->ch = 2;
    }
    v->opened = 1;
    if (!v->buf) v->buf = malloc(sizeof(float) * 2 * 1100);
    v->buf_frames = 0; v->buf_start = 0; v->virt = 1;
    return 0;
}
static void voice_close(Voice *v)
{
    if (v->opened) {
        if (v->vb) stb_vorbis_close(v->vb);
        if (v->media >= 0) g_mloc[v->media].users--;
    }
    v->vb = NULL; v->pcm = NULL; v->opened = 0;
}
static void voice_kill(Voice *v)
{
    voice_close(v);
    free(v->buf); v->buf = NULL;
    v->active = 0;
}

/* make buf hold source frames [f, f + n) as far as possible (decodes forward, seeks if needed).
 * Returns the frames available, or -1 while the file is still loading */
static int voice_fill(Voice *v, int64_t f)
{
    if (!v->opened) { int r = voice_open(v); if (r > 0) return -1; if (r < 0) return 0; }
    if (f < 0) f = 0;
    if (v->pcm) {
        const MediaLoc *m = &g_mloc[v->media];
        int64_t n = (int64_t)m->pcm_frames - f; if (n > 1024) n = 1024;
        v->buf_start = f; v->buf_frames = n > 0 ? (int)n : 0; v->virt = 0;
        const int16_t *src = v->pcm + f * v->ch; float *dst = v->buf;
        for (int k = 0; k < v->buf_frames * v->ch; k++) dst[k] = src[k] * (1.0f / 32768.0f);
        return v->buf_frames;
    }
    if (!v->virt && f > v->buf_start + v->buf_frames && f - (v->buf_start + v->buf_frames) <= 16384) {
        /* a little past what was decoded (a sound played faster than its rate steps over the buffer's last frame):
         * the decoder stands right there, so decode on to f and drop the frames in between. A seek (which bisects the
         * file) here cost the Vita 2-5 ms every 5 ms block for a pitched-up loop - the credits' crackle */
        int64_t left = f - (v->buf_start + v->buf_frames);
        uint64_t t0 = snd_us();
        while (left > 0) {
            int want = left > 1024 ? 1024 : (int)left;
            int got = stb_vorbis_get_samples_float_interleaved(v->vb, v->ch, v->buf, want * v->ch);
            if (got <= 0) return 0;
            left -= got;
        }
        pf_dec += (uint32_t)(snd_us() - t0);
        v->buf_start = f; v->buf_frames = 0; v->fresh = 0;
    }
    if (v->virt || f < v->buf_start || f > v->buf_start + v->buf_frames) {
        unsigned target = (unsigned)f;
        uint64_t t0 = snd_us();
        int ok = 1;
        if (v->fresh && f < 4096) {
            /* a new decoder stands at the start: decoding up to f costs less than a seek (which
             * bisects the file, decoding pages as it goes) */
            int64_t left = f;
            while (left > 0) {
                int want = left > 1024 ? 1024 : (int)left;
                int got = stb_vorbis_get_samples_float_interleaved(v->vb, v->ch, v->buf, want * v->ch);
                if (got <= 0) { ok = 0; break; }
                left -= got;
            }
            uint32_t dt_ = (uint32_t)(snd_us() - t0); pf_dec += dt_;
        } else {
#ifdef NR_PC
            if (getenv("SND_SEEKLOG")) fprintf(stderr, "SEEK %.3f node %d media %d (%u) to %u from buf %lld+%d virt %d quiet %d pos %.0f\n", g_now / (double)SND_RATE,
                                                v->node, v->media, B.media[v->media].id, target, (long long)v->buf_start, v->buf_frames, v->virt, v->quiet, v->pos);
#endif
            ok = stb_vorbis_seek(v->vb, target);
            uint32_t dt_ = (uint32_t)(snd_us() - t0); pf_seek += dt_; pf_seek_n++; if (dt_ > pf_seek_max) pf_seek_max = dt_;
        }
        v->fresh = 0;
        if (!ok) return 0;
        v->buf_start = target; v->buf_frames = 0; v->virt = 0;
    }
    v->fresh = 0;
    /* drop consumed frames, keep from f */
    int keep = (int)(v->buf_start + v->buf_frames - f);
    if (keep < 0) keep = 0;
    if (keep && f > v->buf_start) memmove(v->buf, v->buf + (f - v->buf_start) * v->ch, sizeof(float) * keep * v->ch);
    v->buf_start = f; v->buf_frames = keep;
    uint64_t t0 = snd_us();
    while (v->buf_frames < 1024) {
        int got = stb_vorbis_get_samples_float_interleaved(v->vb, v->ch, v->buf + v->buf_frames * v->ch, (1024 - v->buf_frames) * v->ch);
        if (got <= 0) break;
        v->buf_frames += got;
    }
    uint32_t dt_ = (uint32_t)(snd_us() - t0); pf_dec += dt_; if (dt_ > pf_dec_max) pf_dec_max = dt_;
    return v->buf_frames;
}

/* ------------------------------------------------------------------ parameters */
static float gp_value(int gp, int obj)
{
    if (gp < 0 || (uint32_t)gp >= B.n_gparams) return 0;
    if (obj > 0 && obj < SND_MAX_OBJ && gp < 16 && (O[obj].pv_set >> gp & 1)) return O[obj].pv[gp];
    return g_gp[gp];
}
/* an RTPC curve's output held to the range Wwise gives the property: voice volume -96.3..0 dB, pitch
 * -2400..+2400 cents, low-pass 0..100. (Every curve in the game's bank stays inside these but one: the
 * CoolDown hiss's Heat_Level curves run to +63.8 dB and +4800 cents at Heat_Level 0, which played it as
 * a full-scale burst of static at every checkpoint.) */
static float rtpc_clamp(int target, float y)
{
    if (target == 0) return y > 0 ? 0 : y < -96.3f ? -96.3f : y;
    if (target == 1) return y > 2400 ? 2400 : y < -2400 ? -2400 : y;
    return y < 0 ? 0 : y > 100 ? 100 : y;
}
static void rtpc_sum(const N_Node *n, int obj, float *vol, float *pitch, float *lpf)
{
    for (uint32_t i = 0; i < n->rtpc_n; i++) {
        const N_Rtpc *r = &B.rtpcs[n->rtpc_first + i];
        float y = rtpc_clamp(r->target, curve_eval(r->curve, gp_value(r->gp, obj)));
        if (r->target == 0) *vol += y; else if (r->target == 1) *pitch += y; else *lpf += y;
    }
}
static float node_vol(int k, int64_t now) { return g_vol_ovr[k].active ? ramp_at(&g_vol_ovr[k], now) : B.nodes[k].vol; }
static float node_pitch(int k, int64_t now) { return g_pitch_ovr[k].active ? ramp_at(&g_pitch_ovr[k], now) : B.nodes[k].pitch; }
static float node_lpf(int k, int64_t now) { return g_lpf_ovr[k].active ? ramp_at(&g_lpf_ovr[k], now) : B.nodes[k].lpf; }

static int has_ancestor(int k, int a)
{
    for (int d = 0; k >= 0 && d < 32; d++, k = B.nodes[k].parent) if (k == a) return 1;
    return 0;
}
static int bus_has(int bus, int a)
{
    for (int d = 0; bus >= 0 && d < 16; d++, bus = B.nodes[bus].parent) if (bus == a) return 1;
    return 0;
}

/* ------------------------------------------------------------------ voices */
static Voice *voice_alloc(void)
{
    for (int i = 0; i < MAXV; i++) if (!V[i].active) { memset(&V[i], 0, sizeof V[i]); return &V[i]; }
    /* steal the quietest stopping or oldest non-music voice */
    int best = -1;
    for (int i = 0; i < MAXV; i++) if (V[i].stopping) { best = i; break; }
    if (best < 0) for (int i = 0; i < MAXV; i++) if (V[i].player < 0 || P[V[i].player].kind != 1) { best = i; break; }
    if (best < 0) return NULL;
    voice_kill(&V[best]); memset(&V[best], 0, sizeof V[best]);
    return &V[best];
}

/* the busses with state or effects here that a node's sound goes through (R_*) */
static int voice_route(int k)
{
    int r = 0;
    for (int b = B.nodes[k].bus, d = 0; b >= 0 && d < 16; d++, b = B.nodes[b].parent)
        r |= (b == g_bus_music ? R_MUSIC : 0) | (b == g_bus_nomusic ? R_NOMUSIC : 0) | (b == g_bus_ann ? R_ANN : 0) | (b == g_bus_tut ? R_TUT : 0);
    return r;
}
static void start_sound(int k, int obj, int player, int root, int64_t when, float fade_ms, int curve)
{
    const N_Node *n = &B.nodes[k];
    int m = n->xi[0];
    if (m < 0 || (uint32_t)m >= B.n_media || B.media[m].missing == 1) return;
    Voice *v = voice_alloc(); if (!v) return;
    if (B.media[m].missing == 2) { v->noise = 1; v->nseed = 0x9E3779B9u ^ (uint32_t)k; }
    v->active = 1; v->node = k; v->media = m; v->obj = obj; v->player = player; v->root = root;
    v->start = when; v->end = -1; v->pos = 0;
    v->src_begin = 0; v->src_end = B.media[m].samples;
    v->loops_left = n->loop;                 /* Wwise: 0 = infinite, 1 = once */
    float rv = 0, rp = 0, rl = 0;
    for (int c = k, d = 0; c >= 0 && d < 32; d++, c = B.nodes[c].parent) {
        const N_Node *q = &B.nodes[c];
        if (q->rv1 > q->rv0) rv += q->rv0 + (q->rv1 - q->rv0) * frand();
        if (q->rp1 > q->rp0) rp += q->rp0 + (q->rp1 - q->rp0) * frand();
        if (q->rl1 > q->rl0) rl += q->rl0 + (q->rl1 - q->rl0) * frand();
    }
    v->rnd_vol = rv; v->rnd_pitch = rp; v->rnd_lpf = rl;
    v->is3d = 0; v->atten = -1;
    for (int c = k, d = 0; c >= 0 && d < 32; d++, c = B.nodes[c].parent)
        if (B.nodes[c].flags & 1) { v->is3d = 1; v->atten = B.nodes[c].atten; break; }
    v->route = voice_route(k);
    v->fade.to = 1; v->fade.from = 1; v->fade.dur = 0;
    if (fade_ms > 0) ramp_set(&v->fade, 0, 1, when, fade_ms, curve);
    v->virt = 1;
}

static void play_node(int k, int obj, int player, int root, int64_t when, float fade_ms, int curve);

static int pick_child(int k)
{
    const N_Node *n = &B.nodes[k];
    int cnt = n->xi[1];
    if (cnt <= 0) return -1;
    const N_RsItem *it = &B.rsitems[n->xi[0]];
    if ((n->xi[3] & 1) == 1) {                 /* sequence */
        int i = g_seq_pos[k] % cnt; g_seq_pos[k] = i + 1;
        return it[i].node;
    }
    int avoid = (n->xi[3] >> 8) & 0xFF; if (avoid > 4) avoid = 4; if (avoid >= cnt) avoid = cnt - 1;
    int *h = &g_hist[k * 4];
    for (int tries = 0; tries < 16; tries++) {
        uint32_t tot = 0;
        for (int i = 0; i < cnt; i++) tot += (n->flags & 8) ? it[i].weight : 1;
        float r = frand() * (float)tot; int pick = cnt - 1;
        for (int i = 0; i < cnt; i++) { float w = (n->flags & 8) ? (float)it[i].weight : 1.0f; if (r < w) { pick = i; break; } r -= w; }
        int bad = 0;
        for (int a = 0; a < avoid; a++) if (h[a] == pick + 1) bad = 1;
        if (bad && tries < 15) continue;
        for (int a = 3; a > 0; a--) h[a] = h[a - 1];
        h[0] = pick + 1;
        return it[pick].node;
    }
    return it[0].node;
}

static Player *player_alloc(void)
{
    for (int i = 0; i < MAXP; i++) if (!P[i].active) { memset(&P[i], 0, sizeof P[i]); return &P[i]; }
    return NULL;
}

/* ---- music */
static int music_next_segment(Player *p)
{
    for (int guard = 0; guard < 64; guard++) {
        if (p->sp <= 0) return -1;
        int top = p->sp - 1;
        const N_MItem *it = &B.mitems[p->stack_item[top]];
        if (it->seg >= 0 && it->n == 0) {
            if (it->loop == 0 || p->stack_loops[top] < it->loop) { p->stack_loops[top]++; return it->seg; }
            p->sp--; continue;
        }
        if (it->n == 0) { p->sp--; continue; }
        int step = (it->rstype == 1 || it->rstype == 3), rnd = (it->rstype == 2 || it->rstype == 3);
        if (p->stack_child[top] >= (step ? 1 : (int)it->n)) {
            p->stack_loops[top]++;
            if (it->loop != 0 && p->stack_loops[top] >= it->loop) { p->sp--; continue; }
            p->stack_child[top] = 0;
        }
        int ci;
        if (rnd) ci = (int)(frand() * it->n) % it->n;
        else if (step) { int mi = p->stack_item[top]; ci = g_mseq[mi] % it->n; g_mseq[mi] = ci + 1; }
        else ci = p->stack_child[top];
        p->stack_child[top]++;
        if (p->sp >= 8) return -1;
        p->stack_item[p->sp] = (int)it->first + ci; p->stack_child[p->sp] = 0; p->stack_loops[p->sp] = 0; p->sp++;
    }
    return -1;
}

static void music_start_segment(Player *p, int seg, int64_t zero)
{
    const N_Node *s = &B.nodes[seg];
    for (uint32_t c = 0; c < s->child_n; c++) {
        int tk = B.children[s->child_first + c];
        const N_Node *t = &B.nodes[tk];
        if (t->type != T_TRACK) continue;
        for (int ci = 0; ci < t->xi[1]; ci++) {
            const N_Clip *cl = &B.clips[t->xi[0] + ci];
            if (cl->media < 0 || B.media[cl->media].missing) continue;
            float a = cl->play_at + cl->begin_trim, e = cl->play_at + cl->src_dur + cl->end_trim;
            if (e <= a) continue;
            int64_t vs = zero + (int64_t)(a * (SND_RATE / 1000.0f)), ve = zero + (int64_t)(e * (SND_RATE / 1000.0f));
            if (ve <= g_now) continue;
            Voice *v = voice_alloc(); if (!v) return;
            const N_Media *m = &B.media[cl->media];
            v->active = 1; v->node = tk; v->media = cl->media; v->obj = p->obj; v->player = (int)(p - P); v->root = p->root;
            v->start = vs; v->end = ve;
            v->src_begin = (uint32_t)(cl->begin_trim * m->rate / 1000.0f);
            v->src_end = m->samples;
            v->pos = v->src_begin;
            if (vs < g_now) { v->pos += (double)(g_now - vs) * m->rate / SND_RATE; v->start = g_now; }
            v->loops_left = t->loop;
            v->fade.from = v->fade.to = 1;
            v->is3d = 0; v->atten = -1; v->virt = 1;
            v->route = voice_route(tk);
        }
    }
}

static void music_update(Player *p)
{
    if (p->paused || p->stopping) return;
    int64_t look = SND_RATE / 4;
    while (p->cur_seg >= 0 && g_now + look >= p->next_due) {
        const N_Node *cs = &B.nodes[p->cur_seg];
        int nx = music_next_segment(p);
        if (nx < 0) { p->cur_seg = -1; break; }
        const N_Node *ns = &B.nodes[nx];
        int64_t exit_t = p->cur_zero + (int64_t)(cs->xf[2] * (SND_RATE / 1000.0f));
        int64_t zero = exit_t - (int64_t)(ns->xf[1] * (SND_RATE / 1000.0f));
        music_start_segment(p, nx, zero);
        p->cur_seg = nx; p->cur_zero = zero;
        p->next_due = zero + (int64_t)(ns->xf[2] * (SND_RATE / 1000.0f));
        if (ns->xf[2] <= ns->xf[1] + 1) break;   /* zero-length segment: avoid spinning */
    }
}

static void play_music(int k, int obj, int root, int64_t when, float fade_ms, int curve)
{
    const N_Node *n = &B.nodes[k];
    Player *p = player_alloc(); if (!p) return;
    p->active = 1; p->kind = 1; p->node = k; p->obj = obj; p->root = root;
    p->fade.from = p->fade.to = 1;
    if (fade_ms > 0) ramp_set(&p->fade, 0, 1, when, fade_ms, curve);
    if (n->type == T_SEGMENT) {
        /* a lone segment: play it once */
        p->sp = 0; p->cur_seg = k; p->cur_zero = when - (int64_t)(n->xf[1] * (SND_RATE / 1000.0f));
        music_start_segment(p, k, p->cur_zero);
        p->next_due = INT64_MAX; p->cur_seg = -1;
        return;
    }
    if (n->type == T_TRACK) { return; }
    /* playlist: root items in sequence */
    p->sp = 0;
    for (int i = n->xi[1] - 1; i >= 0 && p->sp < 8; i--) {
        p->stack_item[p->sp] = n->xi[0] + i; p->stack_child[p->sp] = 0; p->stack_loops[p->sp] = 0; p->sp++;
    }
    int seg = music_next_segment(p);
    if (seg < 0) { p->active = 0; return; }
    const N_Node *s = &B.nodes[seg];
    p->cur_seg = seg; p->cur_zero = when - (int64_t)(s->xf[1] * (SND_RATE / 1000.0f));
    music_start_segment(p, seg, p->cur_zero);
    p->next_due = p->cur_zero + (int64_t)(s->xf[2] * (SND_RATE / 1000.0f));
}

/* ---- continuous containers */
static int player_voice_count(int pi)
{
    int c = 0;
    for (int i = 0; i < MAXV; i++) if (V[i].active && V[i].player == pi) c++;
    return c;
}
static void container_update(Player *p)
{
    if (p->paused || p->stopping) return;
    int pi = (int)(p - P);
    const N_Node *n = &B.nodes[p->node];
    if (g_now < p->next_start) return;
    int xf_old = -1; float xf_ms = 0;
    if (player_voice_count(pi)) {
        int tmode = (n->xi[3] >> 4) & 0xF; float tt = n->xf[0];
        if (tmode != 1 /* crossfade */ || tt <= 0) return;
        /* crossfade: start the next one when the newest is tt ms from its end, fading in while that one fades out.
         * (Only the newest counts: going by the one that ends first started another every block until the old one
         * ended - the Commander's sparks (two players of a 1 s crossfade list) filled all 64 voices) */
        int64_t endt = INT64_MAX, newest = INT64_MIN;
        for (int i = 0; i < MAXV; i++) if (V[i].active && V[i].player == pi && !V[i].stopping) {
            const N_Media *m = &B.media[V[i].media];
            if (V[i].loops_left == 0) return;
            double rest = (V[i].src_end - V[i].pos) * SND_RATE / (double)m->rate;
            int64_t e = (V[i].start > g_now ? V[i].start : g_now) + (int64_t)rest;
            if (V[i].start > newest) { newest = V[i].start; endt = e; xf_old = i; }
        }
        if (xf_old >= 0 && endt - g_now > (int64_t)(tt * SND_RATE / 1000)) return;
        if (xf_old >= 0) { xf_ms = (float)((endt - g_now) * 1000.0 / SND_RATE); if (xf_ms < 5) xf_ms = 5; }
    }
    int cnt = n->xi[1];
    if (p->pick_i >= cnt) {
        p->loops_done++;
        if (n->xi[2] != 0 && p->loops_done >= n->xi[2]) { if (!player_voice_count(pi)) p->active = 0; return; }
        p->pick_i = 0;
    }
    p->pick_i++;
    int c = pick_child(p->node);
    if (c >= 0) play_node(c, p->obj, pi, p->root, g_now, xf_ms, 4);
    if (xf_old >= 0) { V[xf_old].stopping = 1; ramp_set(&V[xf_old].fade, ramp_at(&V[xf_old].fade, g_now), 0, g_now, xf_ms, 4); }
    int tmode = (n->xi[3] >> 4) & 0xF;
    p->next_start = g_now + (tmode == 2 ? (int64_t)(n->xf[0] * SND_RATE / 1000) : 1);
}

static int limit_ok(int k);
static void play_node(int k, int obj, int player, int root, int64_t when, float fade_ms, int curve)
{
    if (k < 0 || (uint32_t)k >= B.n_nodes) return;
    if (!limit_ok(k)) return;
    const N_Node *n = &B.nodes[k];
    switch (n->type) {
    case T_SOUND: start_sound(k, obj, player, root, when, fade_ms, curve); break;
    case T_RANSEQ:
        if ((n->flags & 2) && player < 0) {           /* continuous: a player walks the list */
            if ((n->flags & 4)) g_seq_pos[k] = 0;
            Player *p = player_alloc(); if (!p) return;
            p->active = 1; p->kind = 2; p->node = k; p->obj = obj; p->root = root;
            p->fade.from = p->fade.to = 1;
            if (fade_ms > 0) ramp_set(&p->fade, 0, 1, when, fade_ms, curve);
            p->next_start = when;
            container_update(p);
        } else {
            int c = pick_child(k);
            if (c >= 0) play_node(c, obj, player, root, when, fade_ms, curve);
        }
        break;
    case T_LAYER: case T_MIXER: case T_SWITCH:
        for (uint32_t i = 0; i < n->child_n; i++) play_node(B.children[n->child_first + i], obj, player, root, when, fade_ms, curve);
        break;
    case T_MRANSEQ: case T_SEGMENT: play_music(k, obj, root, when, fade_ms, curve); break;
    default: break;
    }
}

/* ------------------------------------------------------------------ actions */
static int voice_matches(const Voice *v, const N_Action *a, int tgt, int obj)
{
    if ((a->scope & 0xFF) == 0x03 && obj && v->obj != obj) return 0;
    if (B.nodes[tgt].type == T_BUS) return bus_has(B.nodes[v->node].bus, tgt);
    return v->root == tgt || has_ancestor(v->node, tgt);
}
static int player_matches(const Player *p, const N_Action *a, int tgt, int obj)
{
    if ((a->scope & 0xFF) == 0x03 && obj && p->obj != obj) return 0;
    if (B.nodes[tgt].type == T_BUS) return bus_has(B.nodes[p->node].bus, tgt);
    return p->root == tgt || has_ancestor(p->node, tgt);
}

static void do_action(int ai, int obj)
{
    const N_Action *a = &B.actions[ai];
    int tgt = a->target;
    float fade = a->fade;
    switch (a->kind) {
    case A_PLAY:
        if (tgt >= 0) play_node(tgt, obj, -1, tgt, g_now, fade, a->curve);
        break;
    case A_STOP:
        for (int i = 0; i < MAXP; i++) if (P[i].active && (tgt < 0 || player_matches(&P[i], a, tgt, obj))) {
            P[i].stopping = 1;
            ramp_set(&P[i].fade, ramp_at(&P[i].fade, g_now), 0, g_now, fade, a->curve);
        }
        for (int i = 0; i < MAXV; i++) if (V[i].active && (tgt < 0 || voice_matches(&V[i], a, tgt, obj))) {
            V[i].stopping = 1;
            ramp_set(&V[i].fade, ramp_at(&V[i].fade, g_now), 0, g_now, fade > 0 ? fade : 5, a->curve);
        }
        break;
    case A_PAUSE: case A_RESUME: {
        int pause = a->kind == A_PAUSE;
        for (int i = 0; i < MAXV; i++) if (V[i].active && (tgt < 0 || voice_matches(&V[i], a, tgt, obj))) {
            if (pause && !V[i].paused) { V[i].paused = 1; V[i].pause_t = g_now; }
            else if (!pause && V[i].paused) {
                int64_t d = g_now - V[i].pause_t; V[i].paused = 0;
                if (V[i].start > V[i].pause_t) V[i].start += d;
                if (V[i].end >= 0) V[i].end += d;
            }
        }
        for (int i = 0; i < MAXP; i++) if (P[i].active && (tgt < 0 || player_matches(&P[i], a, tgt, obj))) {
            if (pause && !P[i].paused) { P[i].paused = 1; P[i].pause_t = g_now; }
            else if (!pause && P[i].paused) {
                int64_t d = g_now - P[i].pause_t; P[i].paused = 0;
                P[i].cur_zero += d; P[i].next_due += d; P[i].next_start += d;
            }
        }
        if (tgt >= 0 && B.nodes[tgt].type == T_BUS && !pause && fade > 0) ramp_set(&g_bus_fade[tgt], 0, 1, g_now, fade, a->curve);
        break; }
    case A_SETVOL: case A_SETPITCH: case A_SETLPF: {
        if (tgt < 0) break;
        Ramp *r = a->kind == A_SETVOL ? &g_vol_ovr[tgt] : a->kind == A_SETPITCH ? &g_pitch_ovr[tgt] : &g_lpf_ovr[tgt];
        float cur = a->kind == A_SETVOL ? node_vol(tgt, g_now) : a->kind == A_SETPITCH ? node_pitch(tgt, g_now) : node_lpf(tgt, g_now);
        float v = a->value;
        if (a->vmax > a->vmin) v += a->vmin + (a->vmax - a->vmin) * frand();
        if (a->meaning == 2) v += cur;                 /* offset */
        else if (a->meaning == 0) v += (a->kind == A_SETVOL ? B.nodes[tgt].vol : a->kind == A_SETPITCH ? B.nodes[tgt].pitch : B.nodes[tgt].lpf);
        ramp_set(r, cur, v, g_now, fade, a->curve);
        break; }
    case A_RESETVOL: case A_RESETPITCH: case A_RESETLPF: {
        if (tgt < 0) break;
        Ramp *r = a->kind == A_RESETVOL ? &g_vol_ovr[tgt] : a->kind == A_RESETPITCH ? &g_pitch_ovr[tgt] : &g_lpf_ovr[tgt];
        if (!r->active) break;
        float base = a->kind == A_RESETVOL ? B.nodes[tgt].vol : a->kind == A_RESETPITCH ? B.nodes[tgt].pitch : B.nodes[tgt].lpf;
        ramp_set(r, ramp_at(r, g_now), base, g_now, fade, a->curve);
        break; }
    default: break;
    }
}

static void post_event(uint32_t id, int obj)
{
    int e = find_event(id);
    if (e < 0) return;
    const N_Event *ev = &B.events[e];
    for (uint32_t i = 0; i < ev->n; i++) {
        int ai = (int)(ev->first + i);
        float dl = B.actions[ai].delay;
        if (dl > 0) {
            for (int d = 0; d < MAXD; d++) if (!D[d].t) { D[d].t = g_now + (int64_t)(dl * SND_RATE / 1000); D[d].action = ai; D[d].obj = obj; break; }
        } else do_action(ai, obj);
    }
}

/* ------------------------------------------------------------------ mixing */
static float voice_gain(Voice *v, float *pan_l, float *pan_r, float *step, float *lpf_out)
{
    float vol = v->rnd_vol, pitch = v->rnd_pitch, lpf = v->rnd_lpf;
    int k = v->node;
    for (int c = k, d = 0; c >= 0 && d < 32; d++, c = B.nodes[c].parent) {
        const N_Node *n = &B.nodes[c];
        vol += node_vol(c, g_now); pitch += node_pitch(c, g_now); lpf += node_lpf(c, g_now);
        rtpc_sum(n, v->obj, &vol, &pitch, &lpf);
        if (n->type == T_LAYER) {                   /* crossfade curves of the layer this child is in */
            for (int i = 0; i < n->xi[1]; i++) {
                const N_Assoc *as = &B.assocs[n->xi[0] + i];
                int child_on_path = 0;
                for (int q = k, dd = 0; q >= 0 && dd < 32; dd++, q = B.nodes[q].parent) if (q == as->child) { child_on_path = 1; break; }
                if (!child_on_path) continue;
                if (as->curve >= 0) vol += curve_eval(as->curve, gp_value(as->gp, v->obj));
                for (uint32_t r = 0; r < as->lr_n; r++) {
                    const N_Rtpc *rt = &B.rtpcs[as->lr_first + r];
                    float y = rtpc_clamp(rt->target, curve_eval(rt->curve, gp_value(rt->gp, v->obj)));
                    if (rt->target == 0) vol += y; else if (rt->target == 1) pitch += y; else lpf += y;
                }
            }
        }
    }
    float busmul = 1;
    for (int b = B.nodes[k].bus, d = 0; b >= 0 && d < 16; d++, b = B.nodes[b].parent) {
        const N_Node *n = &B.nodes[b];
        vol += node_vol(b, g_now);
        rtpc_sum(n, 0, &vol, &pitch, &lpf);
        if (g_bus_fade[b].active) busmul *= ramp_at(&g_bus_fade[b], g_now);
    }
    /* the Crashed states' low-pass on the busses (a voice under both the Music and the Announcer bus gets both) */
    if (v->route & (R_MUSIC | R_NOMUSIC)) lpf += ramp_at(&g_crash_lpf, g_now);
    if (v->route & R_ANN) lpf += ramp_at(&g_anncrash_lpf, g_now);
    float pl = 1, pr = 1;
    if (v->is3d && v->obj > 0 && v->obj < SND_MAX_OBJ && O[v->obj].has_pos) {
        float dx = O[v->obj].pos[0] - g_lis_pos[0], dy = O[v->obj].pos[1] - g_lis_pos[1], dz = O[v->obj].pos[2] - g_lis_pos[2];
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        if (v->atten >= 0) {
            const N_Atten *at = &B.attens[v->atten];
            if (at->vol >= 0) vol += curve_eval(at->vol, dist);
            if (at->lpf >= 0) lpf += curve_eval(at->lpf, dist);
        }
        if (dist > 0.5f) {
            float rx = g_lis_fwd[1] * g_lis_up[2] - g_lis_fwd[2] * g_lis_up[1];
            float ry = g_lis_fwd[2] * g_lis_up[0] - g_lis_fwd[0] * g_lis_up[2];
            float rz = g_lis_fwd[0] * g_lis_up[1] - g_lis_fwd[1] * g_lis_up[0];
            float pan = (dx * rx + dy * ry + dz * rz) / dist;
            float spread = dist < 4 ? 1 - dist / 4 : 0;
            pan *= 0.85f * (1 - spread);
            pl = pan > 0 ? 1 - pan : 1; pr = pan < 0 ? 1 + pan : 1;
        }
    }
    *pan_l = pl; *pan_r = pr;
    const N_Media *m = &B.media[v->media];
    if (pitch > 2400) pitch = 2400; else if (pitch < -2400) pitch = -2400;
    if (vol > 12) vol = 12;
    *step = (float)m->rate / SND_RATE * powf(2.0f, pitch / 1200.0f);
    if (lpf < 0) lpf = 0; if (lpf > 100) lpf = 100;
    *lpf_out = lpf;
    float g = db2lin(vol) * ramp_at(&v->fade, g_now) * busmul;
    if (v->player >= 0) g *= ramp_at(&P[v->player].fade, g_now);
    return g;
}

static float mixbuf[BLOCK * 2];
static float mixann[BLOCK * 2], mixtut[BLOCK * 2];       /* the Announcer bus (its own voices), the Tutorial bus */

#ifdef NR_PC
static int g_clicklog = -1;
#endif
static void mix_voice(Voice *v)
{
#ifdef NR_PC
    if (g_clicklog < 0) g_clicklog = getenv("SND_CLICKLOG") != NULL;
#endif
    if (v->paused) return;
    if (v->start >= g_now + BLOCK) return;
    int off = v->start > g_now ? (int)(v->start - g_now) : 0;
    int frames = BLOCK - off;
    if (v->end >= 0 && v->end < g_now + BLOCK) frames = (int)(v->end - g_now) - off;
    if (frames <= 0) { if (v->end >= 0 && v->end <= g_now + BLOCK) v->active = 0; return; }
    float pl, pr, step, lpf;
    float g = voice_gain(v, &pl, &pr, &step, &lpf);
    float gl = g * pl, gr = g * pr;
    int audible = (gl > 0.0003f || gr > 0.0003f || v->gl > 0.0003f || v->gr > 0.0003f);
    /* a stream that just fell silent (a music layer switched off) keeps decoding for 2 s: switched back on soon
     * (boosting on and off), it needs no seek - each one stalled the Vita's mixer some 5 ms */
    if (audible) v->quiet = 0;
    else if ((v->route & R_MUSIC) && v->vb && v->opened && !v->virt && v->quiet < 2 * SND_RATE) { v->quiet += frames; audible = 1; }
    double pos = v->pos;
    uint32_t loop_s = v->src_begin, loop_e = v->src_end;
    const N_Media *m = &B.media[v->media];
    if (m->loop_end > m->loop_start && m->loop_end <= m->samples && v->loops_left != 1) { loop_s = m->loop_start; loop_e = m->loop_end; }
    float *dst = (v->route & R_TUT) ? mixtut : (v->route & R_ANN) ? mixann : mixbuf;
    if (v->noise) {
        /* SoundSeed Woosh (the bank's source plug-ins, made here): noise through a resonant band-pass round the plug-ins'
         * 440 Hz deflector, its centre falling as the thing goes by (about 0.7 octave each side, as the Rel_Distance pitch
         * curve), swelling and fading. Each plug-in's own gain and speed follow CarEngineRPM as its RTPCs say (the
         * checkpoint's woosh 20-40 dB down). (Before: wide noise, two thirds of it above 2 kHz - static, not a woosh) */
        float *out = dst + off * 2;
        double total = (double)v->src_end;
        float dgl = (gl - v->gl) / frames, dgr = (gr - v->gr) / frames, cl = v->gl, cr = v->gr;
        const WooshDef *wd = woosh_def(B.media[v->media].id);
        float rpm = g_gp_rpm >= 0 ? g_gp[g_gp_rpm] : 1.0f; rpm = rpm < 0 ? 0 : rpm > 1 ? 1 : rpm;
        float pg = db2lin(wd->db0 + (wd->db1 - wd->db0) * rpm) * 0.075f, spd = wd->s0 + (wd->s1 - wd->s0) * rpm;
        float tm = (float)((pos + step * frames * 0.5) / total); if (tm > 1) tm = 1;
        float fc = 440.0f * spd * exp2f((0.5f - tm) * 1.4f); if (fc > 6000) fc = 6000;
        const float Q = 3.0f, q = 1.0f / Q;
        float f1 = 2.0f * sinf(3.14159265f * fc / SND_RATE), norm = 1.732f / sqrtf(3.14159265f * fc / (Q * SND_RATE));
        for (int i = 0; i < frames; i++) {
            if (pos >= total) { voice_kill(v); return; }
            float t = (float)(pos / total);
            float env = sinf(t * 3.14159f); env *= env;
            v->nseed = v->nseed * 1664525u + 1013904223u;
            float n = ((v->nseed >> 9) * (1.0f / 4194304.0f)) - 1.0f;
            v->nf1 += f1 * v->nf2;                            /* state-variable filter: nf1 low, nf2 band */
            float hp = n - v->nf1 - q * v->nf2;
            v->nf2 += f1 * hp;
            float s = v->nf2 * norm * pg * env;
            out[i * 2] += s * cl; out[i * 2 + 1] += s * cr;
            cl += dgl; cr += dgr;
            pos += step;
        }
        v->pos = pos; v->gl = gl; v->gr = gr;
        return;
    }
    if (!audible) {
        /* advance without decoding */
        pos += step * frames;
        while (pos >= loop_e) {
            if (v->loops_left == 1) { voice_kill(v); return; }
            if (v->loops_left > 1) v->loops_left--;
            pos = loop_s + (pos - loop_e);
            if (loop_e <= loop_s) { voice_kill(v); return; }
        }
        v->pos = pos; v->virt = 1; v->gl = gl; v->gr = gr;
        return;
    }
    g_stat_dec++;
    /* low-pass: Wwise LPF 0..100 -> one-pole coefficient */
    float a = lpf <= 0.5f ? 1.0f : expf(-lpf * 0.045f);
    float dgl = (gl - v->gl) / frames, dgr = (gr - v->gr) / frames;
    float cl = v->gl, cr = v->gr;
    float *out = dst + off * 2;
    int i = 0;
    while (i < frames) {
        int64_t fi = (int64_t)pos;
        if (fi >= loop_e) {
            if (v->loops_left == 1) { voice_kill(v); return; }
            if (v->loops_left > 1) v->loops_left--;
            pos = loop_s + (pos - loop_e); if (loop_e <= loop_s) { voice_kill(v); return; }
            continue;
        }
        if (fi < v->buf_start || fi + 1 >= v->buf_start + v->buf_frames) {
            int got = voice_fill(v, fi);
            if (got < 0) {                            /* its file is still loading: run silently */
                pos += step * (frames - i);
                while (pos >= loop_e) {
                    if (v->loops_left == 1) { voice_kill(v); return; }
                    if (v->loops_left > 1) v->loops_left--;
                    pos = loop_s + (pos - loop_e);
                    if (loop_e <= loop_s) { voice_kill(v); return; }
                }
                v->pos = pos; v->virt = 1; v->gl = gl; v->gr = gr;
                return;
            }
            if (got < 2) {
                if (!v->opened) { voice_kill(v); return; }
                pos = loop_e; v->virt = 1;            /* stream ended early: treat as the loop end */
                continue;
            }
        }
        const float *b = v->buf;
        for (; i < frames; i++) {
            int64_t f0 = (int64_t)pos;
            if (f0 >= loop_e || f0 + 1 >= v->buf_start + v->buf_frames) break;
            float t = (float)(pos - (double)f0);
            int o = (int)(f0 - v->buf_start) * v->ch;
            float l, r;
            if (v->ch == 2) { l = b[o] + (b[o + 2] - b[o]) * t; r = b[o + 1] + (b[o + 3] - b[o + 1]) * t; }
            else { l = r = b[o] + (b[o + 1] - b[o]) * t; }
            if (a < 1.0f) { v->lp[0] += (l - v->lp[0]) * a; v->lp[1] += (r - v->lp[1]) * a; l = v->lp[0]; r = v->lp[1]; }
            out[i * 2] += l * cl; out[i * 2 + 1] += r * cr;
#ifdef NR_PC
            if (g_clicklog) {
                float x[2] = { l * cl, r * cr };
                for (int c = 0; c < 2; c++) {
                    if (v->dbg_n >= 2) { float d = fabsf(x[c] - 2 * v->dbg[c][0] + v->dbg[c][1]); if (d > v->dbg_max) { v->dbg_max = d; v->dbg_at = g_now + off + i; } }
                    v->dbg[c][1] = v->dbg[c][0]; v->dbg[c][0] = x[c];
                }
                v->dbg_n++;
            }
#endif
            cl += dgl; cr += dgr;
            pos += step;
        }
    }
    v->pos = pos; v->gl = gl; v->gr = gr;
#ifdef NR_PC
    if (g_clicklog && v->dbg_max > 0.12f) {
        fprintf(stderr, "VCLICK %.3f node %d media %d obj %d jump %.2f gain %.2f/%.2f pos %.0f/%u%s\n", v->dbg_at / (double)SND_RATE, v->node, v->media, v->obj,
                v->dbg_max, gl, gr, pos, loop_e, v->fresh ? " fresh" : "");
    }
    v->dbg_max = 0;
#endif
}

/* the Init.bnk busses' meter and effects, once per block (see the top) */
static void bus_effects(void)
{
    const float bt = (float)BLOCK / SND_RATE;
    /* the Tutorial bus' meter -> the side-chain parameter (RMS; attack 0.1 s, release 1 s; held to -48..-12 dB) */
    float ss = 0;
    for (int i = 0; i < BLOCK * 2; i++) ss += mixtut[i] * mixtut[i];
    float db = ss > 1e-12f ? 10.0f * log10f(ss / (BLOCK * 2)) : -96.0f;
    g_meter = db + (g_meter - db) * expf(-bt / (db > g_meter ? 0.1f : 1.0f));
    if (g_gp_side >= 0) g_gp[g_gp_side] = g_meter < -48 ? -48 : g_meter > -12 ? -12 : g_meter;
    /* the Announcer bus (its own voices and the Tutorial bus'). Its Dialog_Leveler_Soft compressor (-20 dB RMS, 3:1)
     * and ML1 limiter (-6 dBFS) are not done, as the Master bus' compressor is not (see the top): alone they would put
     * the voices about 3 dB further under the music than the PC has them */
    for (int i = 0; i < BLOCK * 2; i++) mixbuf[i] += mixann[i] + mixtut[i];
}
/* the objects with a playback limit of one (Main.bnk): 0 = the new one is not started */
static int limit_ok(int k)
{
    static const struct { uint32_t id; uint8_t keep_old; } L1[] = {
        { 117550239u, 0 }, { 186813217u, 0 }, { 466890807u, 0 },      /* Play_CarJetEject, Play_CarJetRetract, Play_CarJet: the oldest goes */
        { 233954249u, 1 } };                                          /* Play_CarEngine's layer: the new one is refused */
    uint32_t id = B.nodes[k].id;
    for (unsigned i = 0; i < sizeof L1 / sizeof L1[0]; i++) if (L1[i].id == id) {
        for (int j = 0; j < MAXV; j++) {
            Voice *v = &V[j];
            if (!v->active || v->stopping || !(v->node == k || has_ancestor(v->node, k))) continue;
            if (L1[i].keep_old) return 0;
            v->stopping = 1; ramp_set(&v->fade, ramp_at(&v->fade, g_now), 0, g_now, 10, 4);
        }
        return 1;
    }
    return 1;
}
/* a state group's new state (the groups whose states change something here) */
static void set_state(uint32_t group, uint32_t state)
{
    int crashed = state == 4265657233u;                   /* Crashed */
    if (group == 1272941803u && crashed != g_crash_on) {        /* Crash_States: -> Crashed 1.25 s, Crashed -> Normal 1 s (else 0.25 s) */
        float ms = crashed ? 1250 : state == 1160234136u ? 1000 : 250;
        ramp_set(&g_crash_lpf, ramp_at(&g_crash_lpf, g_now), crashed ? 49.0f : 0.0f, g_now, ms, 4); g_crash_on = crashed;
    } else if (group == 879486522u && crashed != g_anncrash_on) {   /* Announcer_Crash: -> Crashed 0.1 s, Crashed -> Normal 1 s */
        float ms = crashed ? 100 : state == 1160234136u ? 1000 : 100;
        ramp_set(&g_anncrash_lpf, ramp_at(&g_anncrash_lpf, g_now), crashed ? 39.0f : 0.0f, g_now, ms, 4); g_anncrash_on = crashed;
    }
}

static void process_commands(void)
{
    uint32_t t = __atomic_load_n(&q_tail, __ATOMIC_RELAXED);
    uint32_t h = __atomic_load_n(&q_head, __ATOMIC_ACQUIRE);
    while (t != h) {
        Cmd *c = &Q[t % QN];
        switch (c->type) {
        case C_POST: post_event(c->id, c->obj); break;
        case C_PARAM: { int g = find_gp(c->id); if (g >= 0) { g_gp[g] = c->v[0]; g_gp_set[g] = 1; } break; }
        case C_OBJPARAM: { int g = find_gp(c->id); if (g >= 0 && g < 16 && c->obj > 0 && c->obj < SND_MAX_OBJ) { O[c->obj].pv[g] = c->v[0]; O[c->obj].pv_set |= 1u << g; } break; }
        case C_OBJPOS:
            if (c->obj > 0 && c->obj < SND_MAX_OBJ) { memcpy(O[c->obj].pos, c->v, 12); O[c->obj].has_pos = 1; }
            break;
        case C_LISTENER: memcpy(g_lis_pos, c->v, 12); memcpy(g_lis_fwd, c->v + 3, 12); memcpy(g_lis_up, c->v + 6, 12); break;
        case C_STOPOBJ:
            for (int i = 0; i < MAXP; i++) if (P[i].active && P[i].obj == c->obj) { P[i].stopping = 1; ramp_set(&P[i].fade, ramp_at(&P[i].fade, g_now), 0, g_now, c->v[0], 4); }
            for (int i = 0; i < MAXV; i++) if (V[i].active && V[i].obj == c->obj) { V[i].stopping = 1; ramp_set(&V[i].fade, ramp_at(&V[i].fade, g_now), 0, g_now, c->v[0] > 5 ? c->v[0] : 5, 4); }
            break;
        case C_OBJCLEAR:
            if (c->obj > 0 && c->obj < SND_MAX_OBJ) memset(&O[c->obj], 0, sizeof O[0]);
            break;
        case C_STATE: set_state(c->id, (uint32_t)c->obj); break;
        case C_STOPALL:
            for (int i = 0; i < MAXV; i++) if (V[i].active) voice_kill(&V[i]);
            for (int i = 0; i < MAXP; i++) P[i].active = 0;
            for (int i = 0; i < MAXD; i++) D[i].t = 0;
            break;
        }
        t++;
    }
    __atomic_store_n(&q_tail, t, __ATOMIC_RELEASE);
}

static void update_objects(void)
{
    /* Rel_Distance: how fast an object closes on the listener (game units/s, + = away) */
    int rel = find_gp(snd_hash("Rel_Distance"));
    if (rel < 0 || rel >= 16) return;
    for (int i = 1; i < SND_MAX_OBJ; i++) {
        Obj *o = &O[i];
        if (!o->has_pos) continue;
        float dx = o->pos[0] - g_lis_pos[0], dy = o->pos[1] - g_lis_pos[1], dz = o->pos[2] - g_lis_pos[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (o->have_prev) {
            float vel = (d - o->prev_dist) * (SND_RATE / (float)BLOCK);
            o->rel += (vel - o->rel) * 0.2f;
            float r = o->rel * 0.5f; if (r > 15) r = 15; if (r < -15) r = -15;
            o->pv[rel] = r; o->pv_set |= 1u << rel;
        }
        o->prev_dist = d; o->have_prev = 1;
    }
}

static void mix_block(void)
{
    process_commands();
    media_trim();
    update_objects();
    for (int d = 0; d < MAXD; d++) if (D[d].t && D[d].t <= g_now) { int ai = D[d].action, ob = D[d].obj; D[d].t = 0; do_action(ai, ob); }
    for (int i = 0; i < MAXP; i++) {
        Player *p = &P[i];
        if (!p->active) continue;
        if (p->kind == 1) music_update(p); else container_update(p);
        if (p->stopping && ramp_at(&p->fade, g_now) <= 0.0001f && g_now >= p->fade.t0 + p->fade.dur) {
            for (int v = 0; v < MAXV; v++) if (V[v].active && V[v].player == i) voice_kill(&V[v]);
            p->active = 0;
        } else if (p->kind == 1 && p->cur_seg < 0 && !player_voice_count(i)) p->active = 0;
        else if (p->kind == 2 && !p->stopping && p->loops_done > 0 && B.nodes[p->node].xi[2] != 0 && p->loops_done >= B.nodes[p->node].xi[2] && !player_voice_count(i)) p->active = 0;
    }
    memset(mixbuf, 0, sizeof mixbuf); memset(mixann, 0, sizeof mixann); memset(mixtut, 0, sizeof mixtut);
    int nv = 0;
    for (int i = 0; i < MAXV; i++) {
        Voice *v = &V[i];
        if (!v->active) continue;
        if (v->stopping && g_now >= v->fade.t0 + v->fade.dur && ramp_at(&v->fade, g_now) <= 0.0001f) { voice_kill(v); continue; }
        mix_voice(v);
        if (v->active) nv++;
        else voice_kill(v);
    }
    bus_effects();
    g_stat_voices = nv;
    g_now += BLOCK;
}

static void render(int16_t *out, int frames)
{
    while (frames > 0) {
        g_stat_dec = 0;
        mix_block();
        int n = frames < BLOCK ? frames : BLOCK;
        for (int i = 0; i < n * 2; i++) {
            float x = mixbuf[i] * 0.5f, ax = fabsf(x);      /* 6 dB of headroom: the bank's sounds are mastered hot */
            if (ax > 0.75f) x = copysignf(0.75f + 0.25f * tanhf((ax - 0.75f) * 4.0f), x);   /* soft limit */
            out[i] = (int16_t)(x * 32767.0f);
        }
        out += n * 2; frames -= n;
    }
}
void snd_render(int16_t *out, int frames) { if (g_ready) render(out, frames); else memset(out, 0, frames * 4); }

/* ------------------------------------------------------------------ threads (Vita) */
#ifdef __vita__
/* The mixer renders 21 ms buffers into a ring of five and an output thread hands them to the
 * audio port, so the mixer can run up to about 85 ms ahead: a slow moment (a stream starting, a
 * seek) uses up that lead instead of leaving a gap. The output thread lives on the mixer's core
 * at a higher priority; it only waits on the port. */
static volatile int g_run, g_mix_alive;
static SceUID g_thread = -1, g_out_thread = -1, g_sem_free = -1, g_sem_full = -1;
#define GRAIN 1024
#define NRING 5
static int16_t g_ring[NRING][GRAIN * 2];
static int g_port = -1;
static int out_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int r = 0, prev = -1; SceUInt64 t_play = 0;
    while (g_run) {
        SceUInt to = 100000;
        if (sceKernelWaitSema(g_sem_full, 1, &to) < 0) continue;
        SceUInt64 t = sceKernelGetProcessTimeWide();
        /* the buffer before ran out before this one was ready: a gap you can hear */
        if (prev >= 0 && t - t_play > (SceUInt64)(GRAIN * 1000000ull / SND_RATE) + 2000) { g_late++; g_late1++; }
        sceAudioOutOutput(g_port, g_ring[r]);           /* returns when the one before has played */
        t_play = sceKernelGetProcessTimeWide();
        if (prev >= 0) sceKernelSignalSema(g_sem_free, 1);
        prev = r; r = (r + 1) % NRING;
    }
    return 0;
}
static int g_mix_core = -1, g_media_core = -1;
static int snd_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    g_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, GRAIN, SND_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (g_port < 0) return 0;
    int vols[2] = {SCE_AUDIO_OUT_MAX_VOL, SCE_AUDIO_OUT_MAX_VOL};
    sceAudioOutSetVolume(g_port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vols);
    g_sem_free = sceKernelCreateSema("nr_snd_free", 0, NRING, NRING, NULL);
    g_sem_full = sceKernelCreateSema("nr_snd_full", 0, 0, NRING, NULL);
    g_out_thread = sceKernelCreateThread("nr_sound_out", out_thread, 0x10000100 - 20, 0x4000, 0, 0x10000 << (g_mix_core < 0 ? 2 : g_mix_core), NULL);
    if (g_out_thread >= 0) sceKernelStartThread(g_out_thread, 0, NULL);
    g_mix_alive = 1;
    int w = 0;
    while (g_run) {
        SceUInt to = 100000;
        if (sceKernelWaitSema(g_sem_free, 1, &to) < 0) continue;
        SceUInt64 t0 = sceKernelGetProcessTimeWide();
        render(g_ring[w], GRAIN);
        SceUInt64 t1 = sceKernelGetProcessTimeWide();
        float load = (float)(t1 - t0) / (GRAIN * 1e6f / SND_RATE);
        g_load = g_load * 0.97f + load * 0.03f;
        if (load > g_peak) g_peak = load;
        if (load > g_peak1) g_peak1 = load;
        w = (w + 1) % NRING;
        sceKernelSignalSema(g_sem_full, 1);
    }
    if (g_out_thread >= 0) { sceKernelWaitThreadEnd(g_out_thread, NULL, NULL); sceKernelDeleteThread(g_out_thread); g_out_thread = -1; }
    sceAudioOutOutput(g_port, NULL);
    sceAudioOutReleasePort(g_port);
    return 0;
}
#endif

#ifdef __vita__
static volatile int g_media_alive;
static int media_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    g_media_alive = 1;
    while (g_run) {
        sceKernelWaitSema(g_media_sema, 1, NULL);
        media_work();
    }
    return 0;
}
static SceUID g_media_thread = -1;
#endif
#ifndef __vita__
static int g_mix_core = -1, g_media_core = -1;
#endif
void snd_cores(int *mixer, int *loader) { if (mixer) *mixer = g_mix_core; if (loader) *loader = g_media_core; }

/* ------------------------------------------------------------------ public */
int snd_init(nr_pak *pak, int start_thread)
{
    uint32_t size = 0;
    uint8_t *d = nr_pak_read(pak, "snd/sound.nsb", &size);
    if (!d || size < 64 || memcmp(d, "NSB1", 4)) { free(d); return -1; }
    uint32_t c[14]; memcpy(c, d + 8, sizeof c);
    B.n_nodes = c[0]; B.n_children = c[1]; B.n_rtpcs = c[2]; B.n_curves = c[3]; B.n_points = c[4]; B.n_events = c[5];
    B.n_actions = c[6]; B.n_media = c[7]; B.n_clips = c[8]; B.n_mitems = c[9]; B.n_rsitems = c[10]; B.n_assocs = c[11];
    B.n_gparams = c[12]; B.n_attens = c[13];
    uint8_t *p = d + 64;
#define TAKE(dst, n, T) do { dst = (T *)p; p += (size_t)(n) * sizeof(T); } while (0)
    TAKE(B.nodes, B.n_nodes, N_Node); TAKE(B.children, B.n_children, int32_t); TAKE(B.rtpcs, B.n_rtpcs, N_Rtpc);
    TAKE(B.curves, B.n_curves, N_Curve); TAKE(B.points, B.n_points, N_Point); TAKE(B.events, B.n_events, N_Event);
    TAKE(B.actions, B.n_actions, N_Action); TAKE(B.media, B.n_media, N_Media); TAKE(B.clips, B.n_clips, N_Clip);
    TAKE(B.mitems, B.n_mitems, N_MItem); TAKE(B.rsitems, B.n_rsitems, N_RsItem); TAKE(B.assocs, B.n_assocs, N_Assoc);
    TAKE(B.gparams, B.n_gparams, N_GParam); TAKE(B.attens, B.n_attens, N_Atten);
#undef TAKE
    if (p + 4 > d + size) { free(d); return -2; }
    memcpy(&B.names_len, p, 4); B.names = (char *)p + 4;
    if (B.names + B.names_len > (char *)d + size) { free(d); return -3; }
    B.raw = d;
    /* events sorted by id for lookup */
    for (uint32_t i = 1; i < B.n_events; i++) {
        N_Event e = B.events[i]; int j = (int)i - 1;
        while (j >= 0 && B.events[j].id > e.id) { B.events[j + 1] = B.events[j]; j--; }
        B.events[j + 1] = e;
    }
    g_vol_ovr = calloc(B.n_nodes, sizeof(Ramp)); g_pitch_ovr = calloc(B.n_nodes, sizeof(Ramp)); g_lpf_ovr = calloc(B.n_nodes, sizeof(Ramp));
    g_bus_fade = calloc(B.n_nodes, sizeof(Ramp));
    g_seq_pos = calloc(B.n_nodes, sizeof(int)); g_mseq = calloc(B.n_mitems + 1, sizeof(int)); g_hist = calloc(B.n_nodes * 4, sizeof(int));
    g_bus_paused = calloc(B.n_nodes, sizeof(int));
    g_gp = calloc(B.n_gparams + 1, sizeof(float)); g_gp_set = calloc(B.n_gparams + 1, 1);
    for (uint32_t i = 0; i < B.n_gparams; i++) g_gp[i] = B.gparams[i].def;
    g_gp_rpm = find_gp(1419219678u);                        /* CarEngineRPM (the wooshes' own RTPCs) */
    for (uint32_t k = 0; k < B.n_nodes; k++) if (B.nodes[k].type == T_BUS) {
        uint32_t id = B.nodes[k].id;
        if (id == 302710993u) g_bus_music = (int)k;            /* Music */
        else if (id == 929985075u) g_bus_nomusic = (int)k;    /* No Music Sub Audio Bus (SFX, Ambient) */
        else if (id == 592732956u) g_bus_ann = (int)k;        /* Announcer */
        else if (id == 266828571u) g_bus_tut = (int)k;        /* Tutorial (under the Announcer) */
    }
    g_gp_side = find_gp(1060105821u);                         /* (0x3F2FEE5D, the meters' game parameter) */
#ifdef NR_PC
    if (getenv("SND_CURVES")) {                        /* (the test machine: the volume and pitch curves' ranges) */
        for (uint32_t k = 0; k < B.n_nodes; k++) for (uint32_t r = 0; r < B.nodes[k].rtpc_n; r++) {
            const N_Rtpc *rt = &B.rtpcs[B.nodes[k].rtpc_first + r];
            if (rt->curve < 0) continue;
            const N_Curve *cv = &B.curves[rt->curve]; float lo = 1e9f, hi = -1e9f, x0 = 1e9f, x1 = -1e9f;
            for (uint32_t q = 0; q < cv->n; q++) { const N_Point *pp = &B.points[cv->first + q]; lo = fminf(lo, pp->y); hi = fmaxf(hi, pp->y); x0 = fminf(x0, pp->x); x1 = fmaxf(x1, pp->x); }
            fprintf(stderr, "curve node %u type %d gp %u target %d scaling %d x %.3f..%.3f y %.2f..%.2f n %u\n", B.nodes[k].id, B.nodes[k].type,
                    rt->gp < B.n_gparams ? B.gparams[rt->gp].id : 0, rt->target, cv->scaling, x0, x1, lo, hi, cv->n);
        }
    }
#endif
    g_mloc = calloc(B.n_media, sizeof(MediaLoc));
    for (uint32_t i = 0; i < B.n_media; i++) {
        char nm[64]; snprintf(nm, sizeof nm, "snd/%u.ogg", B.media[i].id);
        const char *pk; uint32_t off, sz;
        if (nr_pak_find(pak, nm, &pk, &off, &sz) >= 0) { g_mloc[i].pack = pk; g_mloc[i].off = off; g_mloc[i].size = sz; }
    }
    g_pak = pak;
    g_ready = 1;
#ifdef __vita__
    if (start_thread) {
        g_run = 1;
        /* the mixer has core 2 to itself (the game is on core 0; core 1 carries the system's display
         * queue, and on core 1 the frame rate dropped). On core 3 - the system's core, which CapUnlocker
         * lets games use - the mixer was held up for 100 ms and more now and then (v28's log): a gap.
         * The file loader goes there instead (or on core 1 without CapUnlocker): it only reads and
         * unpacks files ahead, nothing waits on it to the millisecond. Each thread starts on its core;
         * its threads only wait, never spin. */
        g_media_sema = sceKernelCreateSema("nr_media", 0, 0, RQN, NULL);
        g_mix_core = 2;
        g_thread = sceKernelCreateThread("nr_sound", snd_thread, 0x10000100 - 16, 0x40000, 0, 0x40000 /* core 2 */, NULL);
        if (g_thread >= 0) sceKernelStartThread(g_thread, 0, NULL); else g_mix_core = -1;
        g_media_core = 3;
        g_media_thread = sceKernelCreateThread("nr_media", media_thread, 0x10000100 + 16, 0x10000, 0, 0x80000 /* core 3 */, NULL);
        if (g_media_sema >= 0 && g_media_thread >= 0) {
            sceKernelStartThread(g_media_thread, 0, NULL);
            for (int i = 0; i < 20 && !g_media_alive; i++) sceKernelDelayThread(5000);
            /* not running after 100 ms: core 3 is not ours after all */
            if (!g_media_alive) { g_media_core = 1; sceKernelChangeThreadCpuAffinityMask(g_media_thread, 0x20000); }
        } else g_media_sema = -1;
    }
#else
    (void)start_thread;
#endif
    return 0;
}

void snd_shutdown(void)
{
#ifdef __vita__
    if (g_thread >= 0) { g_run = 0; sceKernelWaitThreadEnd(g_thread, NULL, NULL); sceKernelDeleteThread(g_thread); g_thread = -1; }
    if (g_media_thread >= 0) { sceKernelSignalSema(g_media_sema, 1); sceKernelWaitThreadEnd(g_media_thread, NULL, NULL); sceKernelDeleteThread(g_media_thread); g_media_thread = -1; }
#endif
    for (int i = 0; i < MAXV; i++) if (V[i].active) voice_kill(&V[i]);
    g_ready = 0;
}

int snd_post_id(uint32_t id, int obj)
{
    if (!g_ready || find_event(id) < 0) return 0;
#ifdef NR_PC
    {   static int evlog = -1; if (evlog < 0) evlog = getenv("SND_EVLOG") != NULL;   /* (the test machine: every event posted) */
        if (evlog) fprintf(stderr, "EV %.3f %s obj %d\n", g_now / (double)SND_RATE, snd_event_name(id), obj); }
#endif
    Cmd c = {C_POST, id, obj, {0}}; push_cmd(&c); return 1;
}
int snd_post(const char *event, int obj) { return snd_post_id(snd_hash(event), obj); }
void snd_set_param(const char *name, float v) { if (!g_ready) return; Cmd c = {C_PARAM, snd_hash(name), 0, {v}}; push_cmd(&c); }
void snd_set_obj_param(int obj, const char *name, float v) { if (!g_ready) return; Cmd c = {C_OBJPARAM, snd_hash(name), obj, {v}}; push_cmd(&c); }
void snd_set_obj_pos(int obj, const float p[3]) { if (!g_ready) return; Cmd c = {C_OBJPOS, 0, obj, {p[0], p[1], p[2]}}; push_cmd(&c); }
void snd_set_listener(const float p[3], const float f[3], const float u[3])
{
    if (!g_ready) return;
    Cmd c = {C_LISTENER, 0, 0, {p[0], p[1], p[2], f[0], f[1], f[2], u[0], u[1], u[2]}}; push_cmd(&c);
}
void snd_stop_obj(int obj, float fade_ms) { if (!g_ready) return; Cmd c = {C_STOPOBJ, 0, obj, {fade_ms}}; push_cmd(&c); }
void snd_clear_obj(int obj) { if (!g_ready) return; Cmd c = {C_OBJCLEAR, 0, obj, {0}}; push_cmd(&c); }
void snd_stop_all(void) { if (!g_ready) return; Cmd c = {C_STOPALL, 0, 0, {0}}; push_cmd(&c); }
void snd_set_state(const char *group, const char *state)
{
    if (!g_ready) return;
#ifdef NR_PC
    if (getenv("SND_EVLOG")) fprintf(stderr, "EV %.3f STATE %s %s\n", g_now / (double)SND_RATE, group, state);
#endif
    Cmd c = {C_STATE, snd_hash(group), (int)snd_hash(state), {0}}; push_cmd(&c);
}
void snd_stats(int *voices, int *decoding, float *load)
{
    if (voices) *voices = g_stat_voices;
    if (decoding) *decoding = g_stat_dec;
    if (load) *load = g_load;
}
void snd_peak(float *peak, int *late) { if (peak) *peak = g_peak; if (late) *late = g_late; g_peak = 0; g_late = 0; }
void snd_peak_sec(float *peak, int *late) { if (peak) *peak = g_peak1; if (late) *late = g_late1; g_peak1 = 0; g_late1 = 0; }
void snd_prof(char *b, size_t n)
{
    snprintf(b, n, "opened %u streams (%.1f ms, slowest %.1f) | seeks %u (%.1f ms, slowest %.1f) | decoding %.1f ms (slowest fill %.1f)",
             pf_open_n, pf_open / 1000.0f, pf_open_max / 1000.0f, pf_seek_n, pf_seek / 1000.0f, pf_seek_max / 1000.0f, pf_dec / 1000.0f, pf_dec_max / 1000.0f);
    pf_open = pf_seek = pf_dec = 0; pf_open_n = pf_seek_n = pf_open_max = pf_seek_max = pf_dec_max = 0;
}

/* debug listing of what is playing (PC tools / boot log) */
void snd_debug_dump(void (*out)(const char *))
{
    {   char l0[96]; snprintf(l0, sizeof l0, "media cache %.1f MB, load queue %u", __atomic_load_n(&g_mem_bytes, __ATOMIC_RELAXED) / 1048576.0f, rq_head - rq_tail); out(l0); }
    char line[200];
    for (int i = 0; i < MAXV; i++) {
        Voice *v = &V[i];
        if (!v->active) continue;
        float pl, pr, st, lp;
        float g = voice_gain(v, &pl, &pr, &st, &lp);
        snprintf(line, sizeof line, "v%-2d node %u (type %d) media %u pos %.0f/%u start %+.2fs end %.2fs gain %.1f dB%s%s player %d",
                 i, B.nodes[v->node].id, B.nodes[v->node].type, B.media[v->media].id, v->pos, B.media[v->media].samples,
                 (v->start - g_now) / (float)SND_RATE, v->end < 0 ? -1.0f : (v->end - g_now) / (float)SND_RATE,
                 g > 0 ? 20 * log10f(g) : -999.0f, v->virt ? " virt" : "", v->paused ? " paused" : "", v->player);
        if (!v->opened && v->media >= 0) { size_t ln_ = strlen(line); snprintf(line + ln_, sizeof line - ln_, " (file %s)", g_mloc[v->media].state == M_LOADING ? "loading" : g_mloc[v->media].state == M_READY ? "ready" : g_mloc[v->media].state == M_FAILED ? "failed" : "not requested"); }
        out(line);
#ifdef NR_PC
        if (getenv("SND_GAINDBG") && g > 2) {           /* (the test machine: where a loud voice's gain comes from) */
            for (int c = v->node, d = 0; c >= 0 && d < 32; d++, c = B.nodes[c].parent) {
                const N_Node *n = &B.nodes[c];
                snprintf(line, sizeof line, "    node %u type %d vol %.1f rnd %.1f", n->id, n->type, node_vol(c, g_now), d == 0 ? v->rnd_vol : 0.0f); out(line);
                for (uint32_t r = 0; r < n->rtpc_n; r++) {
                    const N_Rtpc *rt = &B.rtpcs[n->rtpc_first + r];
                    snprintf(line, sizeof line, "      rtpc gp %d (id %u) value %.3f curve %d -> %.2f (target %d, scaling %d)", rt->gp, rt->gp >= 0 ? B.gparams[rt->gp].id : 0, gp_value(rt->gp, v->obj), rt->curve,
                             curve_eval(rt->curve, gp_value(rt->gp, v->obj)), rt->target, rt->curve >= 0 ? B.curves[rt->curve].scaling : -1); out(line);
                    if (rt->curve >= 0) for (uint32_t q = 0; q < B.curves[rt->curve].n; q++) {
                        const N_Point *pp = &B.points[B.curves[rt->curve].first + q];
                        snprintf(line, sizeof line, "        point %.3f -> %.3f (shape %d)", pp->x, pp->y, pp->interp); out(line);
                    }
                }
            }
            for (int b = B.nodes[v->node].bus, d = 0; b >= 0 && d < 16; d++, b = B.nodes[b].parent) {
                const N_Node *n = &B.nodes[b];
                snprintf(line, sizeof line, "    bus %u vol %.1f", n->id, node_vol(b, g_now)); out(line);
                for (uint32_t r = 0; r < n->rtpc_n; r++) {
                    const N_Rtpc *rt = &B.rtpcs[n->rtpc_first + r];
                    snprintf(line, sizeof line, "      rtpc gp %d value %.3f -> %.2f (target %d)", rt->gp, gp_value(rt->gp, 0), curve_eval(rt->curve, gp_value(rt->gp, 0)), rt->target); out(line);
                }
            }
        }
#endif
    }
}
