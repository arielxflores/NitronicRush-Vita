/* Background file writing: see io.h. */
#include "io.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../src/tinf/tinf.h"
#include "../src/libdeflate/libdeflate.h"
#ifdef NR_PC
#include <sys/stat.h>
#else
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/stat.h>
#endif

enum { J_WRITE, J_RENAME, J_REMOVE, J_MKDIR, J_READ };
typedef struct { int type; char path[200], path2[200]; void *data; size_t n; uint32_t off, raw; int slot; } io_job;
/* the reads' results (state 0 free, 1 asked, 2 done) */
#define NREAD 8
static struct { volatile int state; void *data; uint32_t n; } RD[NREAD];
static void *read_entry(const char *path, uint32_t off, uint32_t size, uint32_t raw, uint32_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *b = malloc(size + 1);
    int ok = b && fseek(f, off, SEEK_SET) == 0 && fread(b, 1, size, f) == size;
    fclose(f);
    if (!ok) { free(b); return NULL; }
    if (!raw) { *n = size; return b; }
    uint8_t *r = malloc(raw + 1); unsigned int len = raw;
    /* (libdeflate as nr_pak.c, with a decompressor of this thread's own; tinf if it can't be made or refuses the entry) */
    static struct libdeflate_decompressor *dd; static int dd_failed;
    if (!dd && !dd_failed) { dd = libdeflate_alloc_decompressor(); dd_failed = !dd; }
    size_t got = 0;
    const int ok2 = r && dd && libdeflate_deflate_decompress(dd, b, size, r, raw, &got) == LIBDEFLATE_SUCCESS && got == raw;
    if (!ok2 && (!r || tinf_uncompress(r, &len, b, size) != TINF_OK || len != raw)) { free(r); free(b); return NULL; }
    free(b); *n = raw;
    return r;
}
#ifdef __vita__
/* (the start of the linker's .vitapad, vita/textpad.ld: read-only, so the code segment stays read-only) */
__attribute__((section(".nrtextpad"), used)) const uint32_t nr_textpad = 0x4e525041;
#endif
static FILE *LOG;
void (*io_note)(const char *line);                       /* (the thread's own lines for the log, as its other lines) */
/* The log is written over the last one in place, never cut short or made anew: on the Vita the first write of a run
 * that has to find new room on the memory card held up every file read and write for 14-25 s (boot.log: the game's
 * start waited 24.5 s for it from Nov 3 on; the other ports saw the same). After this run's text comes LOG_END (written
 * again after each piece, over the last one); the older text after it is blanked when the thread has nothing else to
 * do. Only a run that logs more than the last one did makes the file longer. */
static const char LOG_END[] = "=== the end of this run's log ===\n";
static long log_pos, log_old, log_blank;                 /* (this run's text's end; the file's length at the start; blanked to) */
static void log_open(const char *path)
{
    log_pos = log_old = log_blank = 0;
    LOG = fopen(path, "r+b");
    if (LOG && fseek(LOG, 0, SEEK_END) == 0) { log_old = ftell(LOG); if (log_old < 0) log_old = 0; }
    if (!LOG) LOG = fopen(path, "wb");
}
static void log_put(const char *s, size_t n)
{
    if (!LOG) return;
    if (fseek(LOG, log_pos, SEEK_SET) == 0 && fwrite(s, 1, n, LOG) == n) log_pos += (long)n;
    fwrite(LOG_END, 1, sizeof LOG_END - 1, LOG);
    fflush(LOG);
}
static int log_blank_step(void)                          /* one piece of the old text blanked (0: nothing left) */
{
    static char sp[16384];
    const long from = log_pos + (long)(sizeof LOG_END - 1);
    if (log_blank < from) log_blank = from;
    if (!LOG || log_blank >= log_old) return 0;
    if (sp[0] != ' ') for (size_t i = 0; i < sizeof sp; i++) sp[i] = i % 128 == 127 ? '\n' : ' ';
    long n = log_old - log_blank; if (n > (long)sizeof sp) n = (long)sizeof sp;
    if (fseek(LOG, log_blank, SEEK_SET) == 0) fwrite(sp, 1, (size_t)n, LOG);
    fflush(LOG);
    log_blank += n;
    return 1;
}

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
    case J_READ: {
        uint32_t n = 0;
        void *d = read_entry(j->path, j->off, (uint32_t)j->n, j->raw, &n);
        RD[j->slot].data = d; RD[j->slot].n = n;
        __sync_synchronize();
        RD[j->slot].state = 2;
        return; }
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
void io_init(const char *log_path) { log_open(log_path); while (log_blank_step()) {} }
void io_log(const char *s, size_t n) { log_put(s, n); }
/* (IOREAD_DELAY=n: a read is done only on the n-th time it is asked about, as the Vita's thread would be later) */
static io_job LATE[8]; static int late_n[8];
static void run_now(io_job *j)
{
    if (j->type == J_READ && getenv("IOREAD_DELAY")) { LATE[j->slot] = *j; late_n[j->slot] = atoi(getenv("IOREAD_DELAY")); return; }
    do_job(j);
}
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
static void slow_note(const char *what, const char *path, uint64_t us)
{
    static int notes;
    if (us < 150000 || !io_note || notes >= 40) return;
    notes++;
    char b[300]; snprintf(b, sizeof b, "io: %s %s took %.2f s", what, path, us / 1e6); io_note(b);
}

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
            const uint64_t t = sceKernelGetProcessTimeWide();
            log_put(out, n);
            const uint64_t dt = sceKernelGetProcessTimeWide() - t;
            static int first = 1;
            if (first && io_note) {                      /* (how long the first piece took: see log_open) */
                first = 0;
                char b[200]; snprintf(b, sizeof b, "log: the first write took %.3f s (over the last run's log in place, %ld bytes long)", dt / 1e6, log_old);
                io_note(b);
            } else slow_note("writing the log", "", dt);
        }
        for (;;) {
            io_job j;
            lock();
            int have = jhead != jtail;
            if (have) { j = JOBS[jhead]; jhead = (jhead + 1) % NJOB; busy = 1; }
            unlock();
            if (!have) break;
            const uint64_t t = sceKernelGetProcessTimeWide();
            const int ty = j.type; char pth[200]; snprintf(pth, sizeof pth, "%s", j.path);
            do_job(&j);
            slow_note(ty == J_WRITE ? "writing" : ty == J_READ ? "reading from" : ty == J_RENAME ? "renaming" : ty == J_REMOVE ? "removing" : "making", pth,
                      sceKernelGetProcessTimeWide() - t);
        }
        lock(); int idle = lhead == ltail && jhead == jtail; if (idle) busy = 0; unlock();
        if (quit && idle) break;
        /* nothing to do: some of the last run's old text blanked (a few pieces at a time, then back to waiting) */
        for (int k = 0; idle && k < 4; k++) {
            const uint64_t t = sceKernelGetProcessTimeWide();
            if (!log_blank_step()) break;
            slow_note("blanking the old log", "", sceKernelGetProcessTimeWide() - t);
            lock(); idle = lhead == ltail && jhead == jtail; unlock();
        }
    }
    return sceKernelExitDeleteThread(0);
}
void io_init(const char *log_path)
{
    log_open(log_path);
    lock_sema = sceKernelCreateSema("nr_io_lock", 0, 1, 1, NULL);
    work_sema = sceKernelCreateSema("nr_io_work", 0, 0, 1 << 20, NULL);
    thread = sceKernelCreateThread("nr_io", io_thread, 0x10000100 + 24, 0x8000, 0, 0x20000 /* core 1 */, NULL);
    if (lock_sema < 0 || work_sema < 0 || thread < 0 || sceKernelStartThread(thread, 0, NULL) < 0) thread = -1;
}
void io_log(const char *s, size_t n)
{
    if (thread < 0) { log_put(s, n); return; }
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

int io_read_start(const char *pack_path, uint32_t off, uint32_t size, uint32_t raw)
{
    int slot = -1;
    for (int i = 0; i < NREAD; i++) if (RD[i].state == 0) { slot = i; break; }
    if (slot < 0) return -1;
    RD[slot].state = 1; RD[slot].data = NULL; RD[slot].n = 0;
    io_job j; memset(&j, 0, sizeof j);
    j.type = J_READ; snprintf(j.path, sizeof j.path, "%s", pack_path); j.off = off; j.n = size; j.raw = raw; j.slot = slot;
    SUBMIT(&j);
    return slot;
}
int io_read_poll(int t, void **data, uint32_t *n)
{
#ifdef NR_PC
    if (t >= 0 && t < NREAD && RD[t].state == 1 && late_n[t] > 0 && --late_n[t] == 0) do_job(&LATE[t]);
#endif
    if (t < 0 || t >= NREAD || RD[t].state != 2) return 0;
    __sync_synchronize();
    *data = RD[t].data; if (n) *n = RD[t].n;
    RD[t].data = NULL; RD[t].state = 0;
    return 1;
}
