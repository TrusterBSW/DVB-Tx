/* fcbank check: two DVB-C-like carriers (random 256-QAM symbols, logged) synthesised by fcbank
 * and by scgen; the symbol files and both outputs (cf32) go to argv[1] for fcbank_check.py. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../engine/src/fcbank.h"
#include "../engine/src/scgen.h"

typedef struct { unsigned x; FILE *log; } src;
static void qam(void *ctx, float complex *s)
{
    src *c = ctx;
    c->x = c->x * 1103515245u + 12345u;
    int i = (c->x >> 8) & 15, q = (c->x >> 16) & 15;
    *s = ((2 * i - 15) + (2 * q - 15) * I) / sqrtf(170.0f);
    if (c->log) fwrite(s, sizeof *s, 1, c->log);
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    double fs = argc > 2 ? atof(argv[2]) : 20e6, rs = argc > 3 ? atof(argv[3]) : 6.875e6;
    double off[2] = { -4e6, 4e6 }, alpha = 0.15, rsk[2] = { rs, rs };
    if (argc > 7) {          /* dir fs rs df rs2 off1 off2 */
        rsk[1] = atof(argv[5]);
        off[0] = atof(argv[6]);
        off[1] = atof(argv[7]);
    }
    char p[512];
    int n = 1 << 20;
    float complex *buf = malloc(sizeof *buf * n);
    for (int pass = 0; pass < 2; pass++) {
        src s[2];
        for (int k = 0; k < 2; k++) {
            snprintf(p, sizeof p, "%s/sym%d_%s.cf32", dir, k, pass ? "sc" : "fc");
            s[k].x = 1 + k;
            s[k].log = fopen(p, "wb");
        }
        if (pass == 0) {
            fcbank *b = fcbank_new(fs, rs, argc > 4 ? atof(argv[4]) : 0), *b2 = NULL;
            if (!b) { fprintf(stderr, "fcbank_new failed\n"); return 1; }
            if (fcbank_add(b, rsk[0], FC_RRC, alpha, off[0], 0.5f, qam, &s[0]) < 0) { fprintf(stderr, "add failed\n"); return 1; }
            if (fcbank_add(b, rsk[1], FC_RRC, alpha, off[1], 0.5f, qam, &s[1]) < 0) {
                b2 = fcbank_new(fs, rsk[1], argc > 4 ? atof(argv[4]) : 0);
                if (!b2 || fcbank_add(b2, rsk[1], FC_RRC, alpha, off[1], 0.5f, qam, &s[1]) < 0) { fprintf(stderr, "add2 failed\n"); return 1; }
                fprintf(stderr, "second carrier in its own bank\n");
            }
            snprintf(p, sizeof p, "%s/offsets.txt", dir);
            FILE *o = fopen(p, "w");
            fprintf(o, "%.6f\n%.6f\n", fcbank_offset(b, 0), b2 ? fcbank_offset(b2, 0) : fcbank_offset(b, 1));
            fclose(o);
            fcbank_block(b, buf, n);
            if (b2) {
                float complex *t = malloc(sizeof *t * n);
                fcbank_block(b2, t, n);
                for (int j = 0; j < n; j++) buf[j] += t[j];
                free(t);
                fcbank_free(b2);
            }
            fcbank_free(b);
        } else {
            scgen *g[2];
            for (int k = 0; k < 2; k++) g[k] = scgen_new(rsk[k], fs, off[k], alpha, 32, 0.5f, qam, &s[k]);
            static float complex t[16384];
            for (int i = 0; i < n; i += 16384) {
                scgen_block(g[0], buf + i, 16384);
                scgen_block(g[1], t, 16384);
                for (int j = 0; j < 16384; j++) buf[i + j] += t[j];
            }
        }
        for (int k = 0; k < 2; k++) fclose(s[k].log);
        snprintf(p, sizeof p, "%s/out_%s.cf32", dir, pass ? "sc" : "fc");
        FILE *f = fopen(p, "wb");
        fwrite(buf, sizeof *buf, n, f);
        fclose(f);
    }
    /* speed: 1 and 4 members, mono-thread */
    for (int nm = 1; nm <= 4; nm *= 4) {
        fcbank *b = fcbank_new(fs, rs, 0);
        src s[4] = { { 1, NULL }, { 2, NULL }, { 3, NULL }, { 4, NULL } };
        for (int k = 0; k < nm; k++) fcbank_add(b, rs, FC_RRC, alpha, -fs * 0.3 + k * fs * 0.6 / 3, 0.25f, qam, &s[k]);
        double t0 = now();
        for (int r = 0; r < 8; r++) fcbank_block(b, buf, n);
        double dt = now() - t0;
        printf("fcbank %d member(s): %.1f MS/s output (one core)\n", nm, 8.0 * n / dt / 1e6);
        fcbank_free(b);
        scgen *g = scgen_new(rs, fs, 1e6, alpha, 32, 1.0f, qam, &s[0]);
        t0 = now();
        for (int r = 0; r < 2; r++) for (int i = 0; i < n; i += 16384) scgen_block(g, buf + i, 16384);
        if (nm == 1) printf("scgen span 32, 1 carrier: %.1f MS/s output (one core)\n", 2.0 * n / (now() - t0) / 1e6);
        scgen_free(g);
    }
    return 0;
}
