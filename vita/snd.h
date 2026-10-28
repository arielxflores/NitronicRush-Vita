/* Sound engine: plays the game's own Wwise events, sounds and interactive music from the
 * data made by converter/nr_audio.py (snd/sound.nsb + snd/<media>.ogg inside the packs).
 * All calls are from the game thread; mixing runs on its own thread (Vita) or inside
 * snd_render (PC tools). */
#ifndef NR_SND_H
#define NR_SND_H
#include <stdint.h>
#include <stddef.h>
#include "../src/nr_pak.h"

#define SND_RATE 48000
#define SND_MAX_OBJ 256          /* game objects: 0 = no position (2D), others carry a position */

int      snd_init(nr_pak *pak, int start_thread);   /* 0 = ok */
void     snd_shutdown(void);
uint32_t snd_hash(const char *name);                /* Wwise FNV-1 id of a name */
int      snd_post(const char *event, int obj);      /* 0 if the event is unknown */
int      snd_post_id(uint32_t id, int obj);
void     snd_set_param(const char *name, float v);  /* global game parameter (RTPC) */
void     snd_set_obj_param(int obj, const char *name, float v);
void     snd_set_obj_pos(int obj, const float p[3]);
void     snd_set_listener(const float p[3], const float fwd[3], const float up[3]);
void     snd_stop_all(void);
void     snd_stop_obj(int obj, float fade_ms);        /* everything playing on one object */
void     snd_clear_obj(int obj);                     /* forget an object's position and parameters */
void     snd_set_state(const char *group, const char *state);   /* a Wwise state (Crash_States, Announcer_Crash) */
const char *snd_event_name(uint32_t id);
/* stats for the log: voices playing, voices decoding, mixer load (0..1) */
void     snd_stats(int *voices, int *decoding, float *load);
void     snd_peak(float *peak, int *late);
void     snd_peak_sec(float *peak, int *late);   /* the same, kept apart (the credits' second-by-second log) */
void     snd_prof(char *b, size_t n);   /* the mixer's time in opening, seeking and decoding streams since the last call */   /* the mixer's worst buffer (x its time slot) and late buffers since the last call */
void     snd_cores(int *mixer, int *loader);   /* the cores the mixer and the file loader run on (-1: not running) */
/* PC tools: render frames of stereo 16-bit audio (no thread) */
void     snd_render(int16_t *out, int frames);
void     snd_debug_dump(void (*out)(const char *));
#endif
