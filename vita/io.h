/* Background file writing. The log, the save, the score tables and the replays are written by a thread on another
 * core, so the game never waits for the memory card: writing the log out cost a frame every 5 s in a race, saving a
 * replay at the finish line 0.1-0.2 s. Everything is done in the order it was asked for. (The PC test build does it
 * all at once, in the caller.) */
#ifndef NR_IO_H
#define NR_IO_H
#include <stddef.h>
#include <stdint.h>

void io_init(const char *log_path);         /* opens the log (written over the last one in place; see io.c); starts the thread */
void io_log(const char *s, size_t n);        /* appends to the log */
void io_write(const char *path, const void *data, size_t n);   /* a whole file (path.tmp, then renamed); data copied */
void io_write_take(const char *path, void *data, size_t n);    /* the same, taking the malloc'd buffer */
void io_rename(const char *from, const char *to);
void io_remove(const char *path);
void io_mkdir(const char *path);
void io_flush(void);                         /* waits until everything asked for is on the card */
/* a file inside a data pack read (and inflated) by the thread: the level pictures of the level select, so stepping
 * through the levels never waits for the card (a picture took 20-100 ms). A ticket (-1: all 8 in use), then asked each
 * frame: 1 = done (*data malloc'd, the caller frees; NULL if the read failed; the ticket is free again), 0 = not yet.
 * (The PC test build reads it at once.) */
int  io_read_start(const char *pack_path, uint32_t off, uint32_t size, uint32_t raw);
int  io_read_poll(int ticket, void **data, uint32_t *n);
void io_shutdown(void);                      /* flush, close the log */
/* (the Vita: set by the game - the thread's own lines for the log, a write or read of the card that took long) */
extern void (*io_note)(const char *line);
#endif
