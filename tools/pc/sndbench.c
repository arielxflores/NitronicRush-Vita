/* The sound mixer alone, replaying what a game run sent it: the test machine's measure of the mixer's cost.
 *
 *   NRPC run with SND_REC=rec.bin   (every command the mixer took, with the time of its block)
 *   build/sndbench DATA rec.bin [out.wav]
 *
 * It builds vita/snd.c on its own (no game, no GL) and mixes the run again block by block (snd_bench), the commands
 * arriving in the same blocks as in the game, so the mix is the game's. Per second of sound it prints the voices,
 * those heard and those decoding Vorbis, and the time the mixing took (the file loader's time - on the Vita a thread
 * of its own - left out). The Vita runs the same with the switch file "sndbench" (see docs/testing.md).
 * Under valgrind --tool=callgrind it shows where the mixer's work goes. */
#include "../../vita/snd.c"

static void out_line(const char *s) { puts(s); }

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: sndbench DATA rec.bin [out.wav]\n"); return 1; }
    char idx[512]; snprintf(idx, sizeof idx, "%s/nitronic.idx", argv[1]);
    static nr_pak pk;
    if (nr_pak_open(&pk, idx)) { fprintf(stderr, "no data at %s\n", idx); return 1; }
    if (snd_init(&pk, 0)) { fprintf(stderr, "no sound bank\n"); return 1; }
    return snd_bench(argv[2], argc > 3 ? argv[3] : NULL, out_line) ? 1 : 0;
}
