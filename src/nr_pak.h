/* Data packs made by converter/nr_pack.py (NRPK, or NRP2 whose entries may be deflated). nr_pak_open takes either one .pak, or a .idx file that
 * lists several packs (one name per line, same folder); files are then looked up across them. */
#ifndef NR_PAK_H
#define NR_PAK_H
#include <stdint.h>
#include <stdio.h>
#define NR_PAK_MAX_FILES 160
#define NR_PAK_OPEN_MAX 8      /* pack files kept open at once (the rest are opened when needed) */
typedef struct { char name[56]; uint32_t off, size, raw; } nr_pak_entry;   /* raw != 0: deflated, raw = unpacked size */
typedef struct {
    FILE *f[NR_PAK_MAX_FILES]; int nf;
    uint32_t last_use[NR_PAK_MAX_FILES], clock;
    char path[NR_PAK_MAX_FILES][160];
    uint32_t count; nr_pak_entry *ents; uint8_t *file_of;
} nr_pak;

int   nr_pak_open(nr_pak *p, const char *path);          /* 0 = ok */
/* malloc'd copy of a file inside the pak (caller frees), NULL if missing */
void *nr_pak_read(nr_pak *p, const char *name, uint32_t *size);
void  nr_pak_close(nr_pak *p);
/* index of the entry called name (or -1); its pack file path, offset and size go to the outputs */
int   nr_pak_find(nr_pak *p, const char *name, const char **pack_path, uint32_t *off, uint32_t *size);
#endif
