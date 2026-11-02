/* SPDX-License-Identifier: GPL-3.0-or-later
 * Tests of dvbt2_group (several DVB-T2 carriers synthesised at the radio rate by one IFFT).
 * Derived from GNU Radio gr-dtv (Copyright Free Software Foundation, Inc. / Ron Economos).
 *
 * usage: dvbt2_group_test MODE in.ts nframes k out_prefix base member...
 *   base:   comma list for all members, e.g. fft=32768,ext=1,gi=1/128,pp=7,ldata=59
 *   member: comma list overriding base: off=<bins>,gain=<lin>,bits=8,cr=5/6,rot=1,short=0,fec=,ti=,l1=
 *   k may be written k:threads (dvbt2_group_new threads argument)
 *   MODE single: k must be 1, one member: group output == dvbt2_samples() output (in process)
 *        interp: one member: writes <prefix>_native.cf32 (dvbt2_samples) and <prefix>_group.cf32
 *        sum:    group of all members == sum of each member synthesised alone by its own group
 *        bench:  CPU time of the group (no output)
 * Member i reads the TS from packet 10000 * i (each member has its own stream). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../engine/src/dvbt2.h"

#define MAXM 16

typedef struct src { FILE *f; } src;

static void pull(void *ctx, uint8_t *pkt)
{
    src *s = ctx;
    if (fread(pkt, 1, 188, s->f) != 188) {
        rewind(s->f);
        if (fread(pkt, 1, 188, s->f) != 188)
            exit(1);
    }
}

static src *src_open(const char *path, int member)
{
    src *s = calloc(1, sizeof *s);
    s->f = fopen(path, "rb");
    if (!s->f) {
        perror(path);
        exit(1);
    }
    fseek(s->f, 0, SEEK_END);
    long n = ftell(s->f) / 188;
    fseek(s->f, (long)(10000L * member % n) * 188, SEEK_SET);
    return s;
}

static void frac(const char *v, int *a, int *b)
{
    if (sscanf(v, "%d/%d", a, b) != 2) {
        fprintf(stderr, "bad fraction %s\n", v);
        exit(1);
    }
}

static void parse(char *list, dvbt2_cfg *c, int *off, float *gain)
{
    for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
        char *eq = strchr(tok, '=');
        if (!eq) {
            fprintf(stderr, "bad option %s\n", tok);
            exit(1);
        }
        *eq = 0;
        const char *k = tok, *v = eq + 1;
        long x = strtol(v, NULL, 0);
        if (!strcmp(k, "bw")) c->bw_hz = atof(v);
        else if (!strcmp(k, "fft")) c->fft = (int)x;
        else if (!strcmp(k, "ext")) c->extended = (int)x;
        else if (!strcmp(k, "gi")) frac(v, &c->gi_num, &c->gi_den);
        else if (!strcmp(k, "pp")) c->pilot = (int)x;
        else if (!strcmp(k, "bits")) c->bits = (int)x;
        else if (!strcmp(k, "rot")) c->rotation = (int)x;
        else if (!strcmp(k, "cr")) frac(v, &c->cr_num, &c->cr_den);
        else if (!strcmp(k, "short")) c->short_frame = (int)x;
        else if (!strcmp(k, "fec")) c->fec_blocks = (int)x;
        else if (!strcmp(k, "ldata")) c->data_symbols = (int)x;
        else if (!strcmp(k, "ti")) c->ti_blocks = (int)x;
        else if (!strcmp(k, "l1")) c->l1_bits = (int)x;
        else if (!strcmp(k, "cell")) c->cell_id = (int)x;
        else if (!strcmp(k, "off")) *off = (int)x;
        else if (!strcmp(k, "gain")) *gain = (float)atof(v);
        else {
            fprintf(stderr, "unknown option %s\n", k);
            exit(1);
        }
    }
}

static double cpu_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

static double wall_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

int main(int argc, char **argv)
{
    if (argc < 8) {
        fprintf(stderr, "usage: %s single|interp|sum|bench in.ts nframes k out_prefix base member...\n", argv[0]);
        return 1;
    }
    const char *mode = argv[1], *ts = argv[2], *prefix = argv[5];
    int nframes = atoi(argv[3]), k = atoi(argv[4]), nm = argc - 7, threads = 0;
    const char *colon = strchr(argv[4], ':');
    if (colon)
        threads = atoi(colon + 1);
    if (nm > MAXM)
        nm = MAXM;
    dvbt2_cfg cfg[MAXM];
    int off[MAXM];
    float gain[MAXM];
    for (int i = 0; i < nm; i++) {
        dvbt2_cfg c = {
            .bw_hz = 8e6, .fft = 32768, .extended = 1, .gi_num = 1, .gi_den = 128, .pilot = 7,
            .bits = 8, .rotation = 1, .cr_num = 3, .cr_den = 5, .l1_bits = 6,
            .network_id = 0x3085, .t2_system_id = 0x8001,
        };
        char *b = strdup(argv[6]), *m = strdup(argv[7 + i]);
        off[i] = 0;
        gain[i] = 1.0f;
        parse(b, &c, &off[i], &gain[i]);
        parse(m, &c, &off[i], &gain[i]);
        const char *e = dvbt2_cfg_check(&c);
        if (e) {
            fprintf(stderr, "member %d: %s\n", i, e);
            return 2;
        }
        cfg[i] = c;
    }
    dvbt2_timing_t tm;
    dvbt2_timing(&cfg[0], &tm);
    long total = (long)nframes * tm.frame_samples * k;
    printf("k=%d members=%d frame_samples(radio)=%ld rate=%.3f\n", k, nm, (long)tm.frame_samples * k,
           tm.sample_rate * k);
    fflush(stdout);

    double t0 = cpu_now();
    dvbt2_group *g = dvbt2_group_new(k, threads);
    for (int i = 0; i < nm; i++) {
        int r = dvbt2_group_add(g, &cfg[i], off[i], gain[i], pull, src_open(ts, i));
        if (r < 0) {
            fprintf(stderr, "member %d: %s\n", i, dvbt2_group_error(r));
            return 2;
        }
    }
    double t1 = cpu_now(), tinit = t1 - t0;
    enum { CH = 65536 };
    float complex *buf = malloc(sizeof(float complex) * CH), *ref = malloc(sizeof(float complex) * CH);
    float complex *acc = malloc(sizeof(float complex) * CH);

    if (!strcmp(mode, "bench")) {
        double pw = 0;
        long done = 0;
        t1 = cpu_now();
        double w0 = wall_now();
        while (done < total) {
            int n = dvbt2_group_samples(g, buf, (int)(total - done > CH ? CH : total - done));
            for (int i = 0; i < n; i += 97)
                pw += crealf(buf[i] * conjf(buf[i]));
            done += n;
        }
        double cpu = cpu_now() - t1, dur = (double)total / (tm.sample_rate * k);
        double wall = wall_now() - w0;
        printf("init=%.3f s cpu=%.3f s wall=%.3f s signal=%.3f s cpu/realtime=%.3f wall/realtime=%.3f "
               "mean_power~%.3f\n", tinit, cpu, wall, dur, cpu / dur, wall / dur, pw / (double)(total / 97));
        dvbt2_group_free(g);
        return 0;
    }

    if (!strcmp(mode, "interp")) {
        char p1[512], p2[512];
        snprintf(p1, sizeof p1, "%s_group.cf32", prefix);
        snprintf(p2, sizeof p2, "%s_native.cf32", prefix);
        FILE *fg = fopen(p1, "wb"), *fn = fopen(p2, "wb");
        long done = 0;
        while (done < total) {
            int n = dvbt2_group_samples(g, buf, (int)(total - done > CH ? CH : total - done));
            fwrite(buf, sizeof(float complex), (size_t)n, fg);
            done += n;
        }
        dvbt2 *t = dvbt2_new(&cfg[0], pull, src_open(ts, 0));
        long nat = (long)nframes * tm.frame_samples;
        done = 0;
        while (done < nat) {
            int n = dvbt2_samples(t, buf, (int)(nat - done > CH ? CH : nat - done));
            fwrite(buf, sizeof(float complex), (size_t)n, fn);
            done += n;
        }
        fclose(fg);
        fclose(fn);
        printf("wrote %s (%ld) and %s (%ld)\n", p1, total, p2, nat);
        dvbt2_free(t);
        dvbt2_group_free(g);
        return 0;
    }

    /* single / sum: compare in process */
    dvbt2 *one = NULL;
    dvbt2_group *alone[MAXM] = { 0 };
    if (!strcmp(mode, "single")) {
        if (k != 1 || nm != 1 || off[0] != 0 || gain[0] != 1.0f) {
            fprintf(stderr, "single: k=1, one member, off=0, gain=1\n");
            return 1;
        }
        one = dvbt2_new(&cfg[0], pull, src_open(ts, 0));
    } else if (!strcmp(mode, "sum")) {
        for (int i = 0; i < nm; i++) {
            alone[i] = dvbt2_group_new(k, 0);
            int r = dvbt2_group_add(alone[i], &cfg[i], off[i], gain[i], pull, src_open(ts, i));
            if (r < 0) {
                fprintf(stderr, "alone %d: %s\n", i, dvbt2_group_error(r));
                return 2;
            }
        }
    } else {
        fprintf(stderr, "unknown mode %s\n", mode);
        return 1;
    }
    long done = 0, ndiff = 0, first = -1;
    double es = 0, ps = 0, emax = 0;
    while (done < total) {
        int n = dvbt2_group_samples(g, buf, (int)(total - done > CH ? CH : total - done));
        if (one) {
            int got = 0;
            while (got < n)
                got += dvbt2_samples(one, ref + got, n - got);
        } else {
            memset(acc, 0, sizeof(float complex) * (size_t)n);
            for (int i = 0; i < nm; i++) {
                int got = 0;
                while (got < n) {
                    int m = dvbt2_group_samples(alone[i], ref, n - got);
                    for (int j = 0; j < m; j++)
                        acc[got + j] += ref[j];
                    got += m;
                }
            }
            memcpy(ref, acc, sizeof(float complex) * (size_t)n);
        }
        for (int i = 0; i < n; i++) {
            float complex d = buf[i] - ref[i];
            double e = crealf(d * conjf(d));
            if (memcmp(&buf[i], &ref[i], sizeof(float complex))) {
                if (first < 0)
                    first = done + i;
                ndiff++;
            }
            es += e;
            ps += crealf(ref[i] * conjf(ref[i]));
            if (e > emax)
                emax = e;
        }
        done += n;
    }
    double rms = sqrt(ps / (double)total);
    printf("%s: %ld samples, %ld not bit-identical (first %ld), max error %.3e x RMS, signal/error %.1f dB, "
           "mean power %.4f\n", mode, total, ndiff, first, sqrt(emax) / rms,
           es > 0 ? 10 * log10(ps / es) : 999.0, ps / (double)total);
    dvbt2_free(one);
    for (int i = 0; i < nm; i++)
        dvbt2_group_free(alone[i]);
    dvbt2_group_free(g);
    return 0;
}
