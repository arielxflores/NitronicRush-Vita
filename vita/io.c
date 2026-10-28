/* Background file writing: see io.h. */
#include "io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NR_PC
#include <sys/stat.h>
#else
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/stat.h>
#endif

enum { J_WRITE, J_RENAME, J_REMOVE, J_MKDIR };
typedef struct { int type; char path[200], path2[200]; void *data; size_t n; } io_job;
static FILE *LOG;

static void do_job(io_job *j)
{
    switch (j->type) {
    case J_WRITE: {
        char tmp[210]; snprintf(tmp, sizeof tmp, "%s.tmp", j->path);
        FILE *f = fopen(tmp, "wb");
        int ok = f && fwrite(j->data, 1, j->n, f) == j->n;
        if (f) ok &= fclose(f) == 0;
        if (ok) { remove(j->path); rename(tmp, j->path); } else remove(tmp);
        break; }
    case J_RENAME: rename(j->path, j->path2); break;
    case J_REMOVE: remove(j->path); break;
    case J_MKDIR:
#ifdef NR_PC
        mkdir(j->path, 0777);
#else
        sceIoMkdir(j->path, 0777);
#endif
        break;
    }
    free(j->data); j->data = NULL;
}

#ifdef NR_PC
/* ---- the PC test build: at once */
void io_init(const char *log_path) { LOG = fopen(log_path, "w"); }
void io_log(const char *s, size_t n) { if (LOG) { fwrite(s, 1, n, LOG); fflush(LOG); } }
static void run_now(io_job *j) { do_job(j); }
void io_flush(void) {}
void io_shutdown(void) { if (LOG) fclose(LOG); LOG = NULL; }
#define SUBMIT(j) run_now(j)
#else
/* ---- the Vita: a ring of jobs and a ring of log text, drained by a thread on core 1 */
#define NJOB 96
#define LOGRING (128 * 1024)
static io_job JOBS[NJOB]; static volatile int jhead, jtail;        /* jhead: next to run, jtail: next free */
static char RING[LOGRING]; static volatile int lhead, ltail;        /* bytes lhead..ltail are waiting */
static volatile int busy, quit, dropped;
static SceUID lock_sema = -1, work_sema = -1, thread = -1;
static void lock(void) { sceKernelWaitSema(lock_sema, 1, NULL); }
static void unlock(void) { sceKernelSignalSema(lock_sema, 1); }

static int io_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    static char out[32768];
    for (;;) {
        SceUInt to = 200000;
        sceKernelWaitSema(work_sema, 1, &to);
        for (;;) {                                       /* the log first (the newest lines matter after a crash) */
            lock();
            int n = 0;
            while (lhead != ltail && n < (int)sizeof out) { out[n++] = RING[lhead]; lhead = (lhead + 1) % LOGRING; }
            if (n) busy = 1;
            unlock();
            if (!n) break;
            if (LOG) { fwrite(out, 1, n, LOG); fflush(LOG); }
        }
        for (;;) {
            io_job j;
            lock();
            int have = jhead != jtail;
            if (have) { j = JOBS[jhead]; jhead = (jhead + 1) % NJOB; busy = 1; }
            unlock();
            if (!have) break;
            do_job(&j);
        }
        lock(); if (lhead == ltail && jhead == jtail) busy = 0; unlock();
        if (quit && !busy) break;
    }
    return sceKernelExitDeleteThread(0);
}
void io_init(const char *log_path)
{
    LOG = fopen(log_path, "w");
    lock_sema = sceKernelCreateSema("nr_io_lock", 0, 1, 1, NULL);
    work_sema = sceKernelCreateSema("nr_io_work", 0, 0, 1 << 20, NULL);
    thread = sceKernelCreateThread("nr_io", io_thread, 0x10000100 + 24, 0x8000, 0, 0x20000 /* core 1 */, NULL);
    if (lock_sema < 0 || work_sema < 0 || thread < 0 || sceKernelStartThread(thread, 0, NULL) < 0) thread = -1;
}
void io_log(const char *s, size_t n)
{
    if (thread < 0) { if (LOG) { fwrite(s, 1, n, LOG); fflush(LOG); } return; }
    lock();
    int used = (ltail - lhead + LOGRING) % LOGRING;
    if (used + (int)n < LOGRING - 1) for (size_t i = 0; i < n; i++) { RING[ltail] = s[i]; ltail = (ltail + 1) % LOGRING; }
    else dropped++;
    unlock();
    sceKernelSignalSema(work_sema, 1);
}
static void submit(io_job *j)
{
    if (thread < 0) { do_job(j); return; }
    for (;;) {
        lock();
        int next = (jtail + 1) % NJOB, ok = next != jhead;
        if (ok) { JOBS[jtail] = *j; jtail = next; }
        unlock();
        sceKernelSignalSema(work_sema, 1);
        if (ok) return;
        sceKernelDelayThread(2000);                      /* (full: a moment for the thread) */
    }
}
void io_flush(void)
{
    if (thread < 0) return;
    for (int k = 0; k < 5000; k++) {                     /* (at most 10 s) */
        lock(); int idle = lhead == ltail && jhead == jtail && !busy; unlock();
        if (idle) return;
        sceKernelSignalSema(work_sema, 1);
        sceKernelDelayThread(2000);
    }
}
void io_shutdown(void)
{
    io_flush();
    if (thread >= 0) { quit = 1; sceKernelSignalSema(work_sema, 1); sceKernelWaitThreadEnd(thread, NULL, NULL); thread = -1; }
    if (LOG) fclose(LOG); LOG = NULL;
}
#define SUBMIT(j) submit(j)
#endif

static void job(int type, const char *a, const char *b, void *data, size_t n)
{
    io_job j; memset(&j, 0, sizeof j);
    j.type = type; snprintf(j.path, sizeof j.path, "%s", a ? a : ""); snprintf(j.path2, sizeof j.path2, "%s", b ? b : "");
    j.data = data; j.n = n;
    SUBMIT(&j);
}
void io_write(const char *path, const void *data, size_t n)
{
    void *c = malloc(n ? n : 1);
    if (!c) return;
    memcpy(c, data, n);
    job(J_WRITE, path, NULL, c, n);
}
void io_write_take(const char *path, void *data, size_t n) { job(J_WRITE, path, NULL, data, n); }
void io_rename(const char *from, const char *to) { job(J_RENAME, from, to, NULL, 0); }
void io_remove(const char *path) { job(J_REMOVE, path, NULL, NULL, 0); }
void io_mkdir(const char *path) { job(J_MKDIR, path, NULL, NULL, 0); }
