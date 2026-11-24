/* The level collision's sphere queries timed alone (the PC test machine):
 *   collbench DATA LEVEL QUERIES [OUT]
 * QUERIES: a file of float x, y, z, r (SPHREC=file in a game run saves them: the barrels' and the gibs' queries); each
 * is run again here (20 times over) and timed; OUT gets every result (hits, push, normal) to compare two builds. */
#include "nr_pak.h"
#include "nr_coll.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "collbench DATA LEVEL QUERIES [OUT]\n"); return 1; }
    char p[512]; nr_pak pak; snprintf(p, sizeof p, "%s/nitronic.idx", argv[1]);
    if (nr_pak_open(&pak, p)) { fprintf(stderr, "no packs at %s\n", p); return 1; }
    snprintf(p, sizeof p, "levels/%s.nrc", argv[2]);
    uint32_t sz; uint8_t *b = nr_pak_read(&pak, p, &sz);
    nr_coll c; if (!b || nr_coll_parse(b, sz, &c)) { fprintf(stderr, "no collision %s\n", p); return 1; }
    FILE *f = fopen(argv[3], "rb"); if (!f) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f) / 16; fseek(f, 0, SEEK_SET);
    float (*q)[4] = malloc(16 * n); if (fread(q, 16, n, f) != (size_t)n) return 1; fclose(f);
    FILE *o = argc > 4 ? fopen(argv[4], "wb") : NULL;
    c.no_dyn = 1;
    long hits = 0; double best = 1e30;
    int reps = getenv("REPS") ? atoi(getenv("REPS")) : 20;
    for (int rep = 0; rep < reps; rep++) {
        double t0 = now_s();
        for (long i = 0; i < n; i++) {
            float push[3], nn[3];
            int h = nr_coll_sphere(&c, q[i], q[i][3], push, nn);
            if (rep == 0) { hits += h; if (o) { fwrite(&h, 4, 1, o); fwrite(push, 4, 3, o); fwrite(nn, 4, 3, o); } }
        }
        double t = now_s() - t0; if (t < best) best = t;
    }
    printf("%s: %ld queries, %ld contacts, %.1f ns a query (best of %d)\n", argv[2], n, hits, best * 1e9 / n, reps);
    if (o) fclose(o);
    return 0;
}
