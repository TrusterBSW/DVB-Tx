/* DVB-S2 coder regression / speed check (BBFRAME, BCH, LDPC, interleaver, mapping, PL framing).
 * Build (same flags as engine/Makefile):
 *   gcc -O3 -march=x86-64-v2 -mtune=generic -std=gnu11 -fcx-limited-range -pthread \
 *       tests/dvbs2_coder_check.c engine/src/dvbs2.c engine/src/dvb_fec.c engine/src/util.c -lm -o dvbs2_coder_check
 * dvbs2_coder_check write out.bin : symbols of several MODCODs (fixed pseudo-random TS), 6 PLFRAMEs each
 * dvbs2_coder_check time          : PLFRAMEs/s and Msym/s per MODCOD (one core)
 * Compare two builds with cmp on the written files. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../engine/src/dvbs2.h"

static uint32_t rng;
static void pull(void *ctx, uint8_t *p)
{
    (void)ctx;
    p[0] = 0x47;
    for (int i = 1; i < 188; i++) {
        rng = rng * 1664525u + 1013904223u;
        p[i] = (uint8_t)(rng >> 24);
    }
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static const int mc[][3] = { { 2, 1, 4 }, { 2, 9, 10 }, { 3, 3, 5 }, { 3, 9, 10 }, { 4, 2, 3 }, { 5, 9, 10 },
                             { 3, 3, 4 }, { 5, 3, 4 } };

int main(int argc, char **argv)
{
    if (argc < 2 || (!strcmp(argv[1], "write") && argc < 3)) {
        fprintf(stderr, "usage: write out.bin | time\n");
        return 1;
    }
    int wr = !strcmp(argv[1], "write");
    FILE *o = wr ? fopen(argv[2], "wb") : NULL;
    if (wr && !o)
        return 1;
    for (unsigned m = 0; m < sizeof mc / sizeof mc[0]; m++)
        for (int sf = 0; sf < 2; sf++)
            for (int pil = 0; pil < 2; pil++) {
                dvbs2_cfg c = { mc[m][0], mc[m][1], mc[m][2], sf, pil, 0.35, wr ? (int)(m * 1001 + sf * 7 + pil) : 0 };
                if (dvbs2_cfg_check(&c))
                    continue;
                rng = 12345 + m;
                dvbs2 *s = dvbs2_new(&c, pull, NULL);
                int nsym = (sf ? 16200 : 64800) / c.bits;
                int flen = 90 + nsym + (pil ? ((nsym / 90 - 1) / 16) * 36 : 0);
                float complex v;
                if (wr) {
                    long n = (long)flen * 6;
                    for (long i = 0; i < n; i++) {
                        dvbs2_symbol(s, &v);
                        fwrite(&v, sizeof v, 1, o);
                    }
                } else {
                    long n = 0;
                    double t0 = now(), dt;
                    do {
                        for (int i = 0; i < flen * 20; i++)
                            dvbs2_symbol(s, &v);
                        n += flen * 20;
                        dt = now() - t0;
                    } while (dt < 0.5);
                    printf("bits %d %d/%-2d %s pilots %d: %8.0f frames/s %7.2f Msym/s\n", c.bits, c.cr_num, c.cr_den,
                           sf ? "short " : "normal", pil, n / flen / dt, n / dt / 1e6);
                }
                dvbs2_free(s);
            }
    if (o)
        fclose(o);
    return 0;
}
