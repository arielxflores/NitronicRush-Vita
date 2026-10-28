#include "nr_pak.h"
#include "tinf/tinf.h"
#include <stdlib.h>
#include <string.h>

static int add_pack(nr_pak *p, const char *path)
{
    if (p->nf >= NR_PAK_MAX_FILES) return -4;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char magic[4]; uint32_t n;
    if (fread(magic, 1, 4, f) != 4 || (memcmp(magic, "NRPK", 4) && memcmp(magic, "NRP2", 4)) || fread(&n, 4, 1, f) != 1 || n > 100000) { fclose(f); return -2; }
    int v2 = !memcmp(magic, "NRP2", 4);
    p->ents = realloc(p->ents, sizeof(nr_pak_entry) * (p->count + n));
    p->file_of = realloc(p->file_of, p->count + n);
    for (uint32_t i = 0; i < n; i++) {
        nr_pak_entry *e = &p->ents[p->count + i];
        if (fread(e->name, 1, 56, f) != 56 || fread(&e->off, 4, 1, f) != 1 || fread(&e->size, 4, 1, f) != 1) { fclose(f); return -3; }
        e->raw = 0;
        if (v2 && fread(&e->raw, 4, 1, f) != 1) { fclose(f); return -3; }
    }
    memset(p->file_of + p->count, p->nf, n);
    p->count += n;
    snprintf(p->path[p->nf], sizeof p->path[0], "%s", path);
    fclose(f);                       /* reopened on demand: there can be more packs than open files */
    p->f[p->nf++] = NULL;
    return 0;
}

static FILE *pack_file(nr_pak *p, int i)
{
    p->last_use[i] = ++p->clock;
    if (p->f[i]) return p->f[i];
    int open = 0, lru = -1;
    for (int k = 0; k < p->nf; k++) if (p->f[k]) { open++; if (lru < 0 || p->last_use[k] < p->last_use[lru]) lru = k; }
    if (open >= NR_PAK_OPEN_MAX && lru >= 0) { fclose(p->f[lru]); p->f[lru] = NULL; }
    p->f[i] = fopen(p->path[i], "rb");
    return p->f[i];
}

int nr_pak_open(nr_pak *p, const char *path)
{
    memset(p, 0, sizeof *p);
    size_t l = strlen(path);
    if (l > 4 && !strcmp(path + l - 4, ".idx")) {
        FILE *ix = fopen(path, "r");
        if (!ix) return -1;
        char dir[256]; snprintf(dir, sizeof dir, "%s", path);
        char *slash = strrchr(dir, '/'); if (slash) slash[1] = 0; else dir[0] = 0;
        char line[128]; int r = 0;
        while (fgets(line, sizeof line, ix)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!line[0]) continue;
            char full[400]; snprintf(full, sizeof full, "%s%s", dir, line);
            if ((r = add_pack(p, full)) != 0) break;
        }
        fclose(ix);
        if (r || !p->nf) { nr_pak_close(p); return r ? r : -5; }
        return 0;
    }
    int r = add_pack(p, path);
    if (r) nr_pak_close(p);
    return r;
}

void *nr_pak_read(nr_pak *p, const char *name, uint32_t *size)
{
    for (uint32_t i = 0; i < p->count; i++) {
        nr_pak_entry *e = &p->ents[i];
        if (strncmp(e->name, name, sizeof e->name)) continue;
        FILE *f = pack_file(p, p->file_of[i]);
        if (!f) return NULL;
        uint8_t *b = malloc(e->size + 1);
        if (!b) return NULL;
        fseek(f, e->off, SEEK_SET);
        if (fread(b, 1, e->size, f) != e->size) { free(b); return NULL; }
        b[e->size] = 0;
        if (e->raw) {                                    /* deflated entry */
            uint8_t *r = malloc(e->raw + 1);
            unsigned int len = e->raw;
            if (!r || tinf_uncompress(r, &len, b, e->size) != TINF_OK || len != e->raw) { free(r); free(b); return NULL; }
            free(b); b = r; b[e->raw] = 0;
            if (size) *size = e->raw;
            return b;
        }
        if (size) *size = e->size;
        return b;
    }
    return NULL;
}

int nr_pak_find(nr_pak *p, const char *name, const char **pack_path, uint32_t *off, uint32_t *size)
{
    for (uint32_t i = 0; i < p->count; i++) {
        nr_pak_entry *e = &p->ents[i];
        if (strncmp(e->name, name, sizeof e->name)) continue;
        if (pack_path) *pack_path = p->path[p->file_of[i]];
        if (off) *off = e->off;
        if (size) *size = e->size;
        return (int)i;
    }
    return -1;
}

void nr_pak_close(nr_pak *p)
{
    for (int i = 0; i < p->nf; i++) if (p->f[i]) fclose(p->f[i]);
    free(p->ents); free(p->file_of);
    memset(p, 0, sizeof *p);
}
