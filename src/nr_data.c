#include "nr_data.h"
#include <stdlib.h>
#include <string.h>

int nr_level_parse(const uint8_t *b, uint32_t size, nr_leveldata *l)
{
    memset(l, 0, sizeof *l);
    if (size < 132 || (memcmp(b, "NRL2", 4) && memcmp(b, "NRL3", 4))) return -1;
    uint32_t h[5]; memcpy(h, b + 4, 20);
    if (h[0] != 2 && h[0] != 3) return -2;
    l->version = (int)h[0];
    l->glow_scale = h[0] >= 3 ? 32.0f : 8.0f;
    uint32_t mrec = h[0] >= 3 ? 52 : 16;
    l->ntex = h[1]; l->nmat = h[2]; l->nchunk = h[3]; l->npath = h[4];
    memcpy(l->start, b + 24, 12);
    memcpy(&l->light, b + 36, 96);
    uint32_t o = 132;
    if (o + 64ull * l->ntex + (uint64_t)mrec * l->nmat + 36ull * l->nchunk > size) return -3;
    l->texnames = (char (*)[64])(b + o); o += 64 * l->ntex;
    l->mats = calloc(l->nmat + 1, sizeof(nr_mat));
    for (uint32_t i = 0; i < l->nmat; i++, o += mrec) {
        nr_mat *m = &l->mats[i];
        memcpy(m, b + o, 16);
        if (mrec >= 52) {
            memcpy(&m->anim, b + o + 16, 2); m->fx = b[o + 18];
            memcpy(m->c2, b + o + 20, 16); memcpy(m->p, b + o + 36, 16);
        } else m->anim = -1;
    }
    l->chunks = calloc(l->nchunk + 1, sizeof(nr_chunk));
    for (uint32_t i = 0; i < l->nchunk; i++, o += 36) memcpy(&l->chunks[i], b + o, 36);
    for (uint32_t i = 0; i < l->nchunk; i++) {
        nr_chunk *c = &l->chunks[i];
        uint64_t need = (uint64_t)c->nv * NR_VERTEX_SIZE + c->ni * 2 + (c->ni & 1) * 2;
        if (o + need > size || c->mat >= l->nmat) { nr_level_data_free(l); return -4; }
        c->verts = b + o; o += c->nv * NR_VERTEX_SIZE;
        c->idx = (const uint16_t *)(b + o); o += c->ni * 2 + (c->ni & 1) * 2;
    }
    if (o + 12ull * l->npath > size) { nr_level_data_free(l); return -5; }
    l->path = malloc(12 * (l->npath + 1));
    memcpy(l->path, b + o, 12 * l->npath);
    return 0;
}

void nr_level_data_free(nr_leveldata *l)
{
    free(l->mats); free(l->chunks); free(l->path);
    memset(l, 0, sizeof *l);
}

int nr_tex_parse(const uint8_t *b, uint32_t size, nr_texdata *t)
{
    if (size < 12 || memcmp(b, "NRT1", 4)) return -1;
    memcpy(&t->w, b + 4, 2); memcpy(&t->h, b + 6, 2);
    t->fmt = b[8]; t->nmips = b[9]; memcpy(&t->flags, b + 10, 2);
    t->data = b + 12;
    if (t->fmt != 1 && t->fmt != 5 && t->fmt != 8) return -2;     /* 8 = plain RGBA8 (fonts) */
    uint32_t total = 0;
    for (int i = 0; i < t->nmips; i++) total += nr_tex_mip_size(t, i, NULL, NULL);
    return 12 + total <= size ? 0 : -3;
}

uint32_t nr_tex_mip_size(const nr_texdata *t, int level, int *pw, int *ph)
{
    int w = t->w >> level, h = t->h >> level;
    if (w < 4) w = 4;
    if (h < 4) h = 4;
    if (pw) *pw = w;
    if (ph) *ph = h;
    if (t->fmt == 8) {                                   /* (a side stops at 1 down the mips) */
        int w8 = t->w >> level, h8 = t->h >> level;
        return (uint32_t)(w8 < 1 ? 1 : w8) * (uint32_t)(h8 < 1 ? 1 : h8) * 4;
    }
    return (uint32_t)(w / 4) * (h / 4) * (t->fmt == 1 ? 8 : 16);
}

static void rgb565(uint16_t c, int *o)
{
    int r = c >> 11 & 31, g = c >> 5 & 63, b = c & 31;
    o[0] = r << 3 | r >> 2; o[1] = g << 2 | g >> 4; o[2] = b << 3 | b >> 2;
}

void nr_dxt_decode(const nr_texdata *t, const uint8_t *s, int w, int h, uint8_t *out)
{
    int bs = t->fmt == 1 ? 8 : 16;
    for (int by = 0; by < h / 4; by++) for (int bx = 0; bx < w / 4; bx++, s += bs) {
        const uint8_t *cb = s + bs - 8;
        uint16_t c0 = cb[0] | cb[1] << 8, c1 = cb[2] | cb[3] << 8;
        uint32_t bits = cb[4] | cb[5] << 8 | cb[6] << 16 | (uint32_t)cb[7] << 24;
        int p[4][4]; rgb565(c0, p[0]); rgb565(c1, p[1]);
        for (int k = 0; k < 3; k++) {
            p[2][k] = (2 * p[0][k] + p[1][k]) / 3; p[3][k] = (p[0][k] + 2 * p[1][k]) / 3;
        }
        p[0][3] = p[1][3] = p[2][3] = p[3][3] = 255;
        int a[8] = { 255, 255, 255, 255, 255, 255, 255, 255 };
        uint64_t abits = 0;
        if (bs == 16) {
            a[0] = s[0]; a[1] = s[1];
            for (int k = 1; k < 7; k++) a[k + 1] = ((7 - k) * a[0] + k * a[1]) / 7;
            for (int k = 0; k < 6; k++) abits |= (uint64_t)s[2 + k] << (8 * k);
        }
        for (int i = 0; i < 16; i++) {
            int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
            uint8_t *o = out + (y * w + x) * 4;
            int *c = p[bits >> (2 * i) & 3];
            o[0] = c[0]; o[1] = c[1]; o[2] = c[2];
            o[3] = bs == 16 ? a[abits >> (3 * i) & 7] : 255;
        }
    }
}
