/* SPDX-License-Identifier: GPL-3.0-or-later
 * Test: modulate a TS file with the dvbtx DVB-T2 modulator at the elementary rate (cf32 out).
 * Derived from GNU Radio gr-dtv (Copyright Free Software Foundation, Inc. / Ron Economos).
 *
 * usage: dvbt2_ref in.ts out.cf32|- nframes [key=value ...]
 *   bw=8e6 fft=32768 ext=1 gi=1/128 pp=7 bits=8 rot=1 cr=3/5 short=0 fec=0 ldata=0 ti=0
 *   l1=6 cell=0 net=0x3085 sys=0x8001
 *   out "-": do not write (benchmark); prints rates and the CPU real-time factor. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../engine/src/dvbt2.h"

static FILE *tsf;
static long npkt;

static void pull(void *ctx, uint8_t *pkt)
{
    (void)ctx;
    if (fread(pkt, 1, 188, tsf) != 188) {
        rewind(tsf);
        if (fread(pkt, 1, 188, tsf) != 188)
            exit(1);
    }
    npkt++;
}

static void frac(const char *v, int *a, int *b)
{
    if (sscanf(v, "%d/%d", a, b) != 2) {
        fprintf(stderr, "bad fraction %s\n", v);
        exit(1);
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s in.ts out.cf32|- nframes [key=value ...]\n", argv[0]);
        return 1;
    }
    dvbt2_cfg c = {
        .bw_hz = 8e6, .fft = 32768, .extended = 1, .gi_num = 1, .gi_den = 128, .pilot = 7,
        .bits = 8, .rotation = 1, .cr_num = 3, .cr_den = 5, .short_frame = 0, .l1_bits = 6,
        .cell_id = 0, .network_id = 0x3085, .t2_system_id = 0x8001,
    };
    for (int i = 4; i < argc; i++) {
        char k[32];
        const char *eq = strchr(argv[i], '=');
        if (!eq || eq - argv[i] >= (int)sizeof k) {
            fprintf(stderr, "bad option %s\n", argv[i]);
            return 1;
        }
        memcpy(k, argv[i], (size_t)(eq - argv[i]));
        k[eq - argv[i]] = 0;
        const char *v = eq + 1;
        long x = strtol(v, NULL, 0);
        if (!strcmp(k, "bw")) c.bw_hz = atof(v);
        else if (!strcmp(k, "fft")) c.fft = (int)x;
        else if (!strcmp(k, "ext")) c.extended = (int)x;
        else if (!strcmp(k, "gi")) frac(v, &c.gi_num, &c.gi_den);
        else if (!strcmp(k, "pp")) c.pilot = (int)x;
        else if (!strcmp(k, "bits")) c.bits = (int)x;
        else if (!strcmp(k, "rot")) c.rotation = (int)x;
        else if (!strcmp(k, "cr")) frac(v, &c.cr_num, &c.cr_den);
        else if (!strcmp(k, "short")) c.short_frame = (int)x;
        else if (!strcmp(k, "fec")) c.fec_blocks = (int)x;
        else if (!strcmp(k, "ldata")) c.data_symbols = (int)x;
        else if (!strcmp(k, "ti")) c.ti_blocks = (int)x;
        else if (!strcmp(k, "l1")) c.l1_bits = (int)x;
        else if (!strcmp(k, "cell")) c.cell_id = (int)x;
        else if (!strcmp(k, "net")) c.network_id = (int)x;
        else if (!strcmp(k, "sys")) c.t2_system_id = (int)x;
        else {
            fprintf(stderr, "unknown option %s\n", k);
            return 1;
        }
    }
    const char *err = dvbt2_cfg_check(&c);
    if (err) {
        fprintf(stderr, "invalid configuration: %s\n", err);
        return 2;
    }
    dvbt2_info info;
    dvbt2_get_info(&c, &info);
    printf("ldata=%d fec=%d ti=%d np2=%d frame_samples=%d frame=%.6f s\n", info.data_symbols,
           info.fec_blocks, info.ti_blocks, info.n_p2, info.frame_samples, info.frame_seconds);
    printf("fs=%.6f ts_rate=%.6f occupied_bw=%.3f\n", dvbt2_sample_rate(&c), dvbt2_ts_rate(&c),
           dvbt2_occupied_bw(&c));
    fflush(stdout);
    tsf = fopen(argv[1], "rb");
    if (!tsf) {
        perror(argv[1]);
        return 1;
    }
    int bench = !strcmp(argv[2], "-");
    FILE *o = bench ? NULL : fopen(argv[2], "wb");
    if (!bench && !o) {
        perror(argv[2]);
        return 1;
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t0);
    dvbt2 *t = dvbt2_new(&c, pull, NULL);
    if (!t) {
        fprintf(stderr, "dvbt2_new failed\n");
        return 1;
    }
    struct timespec ti;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ti);
    double init = (double)(ti.tv_sec - t0.tv_sec) + 1e-9 * (double)(ti.tv_nsec - t0.tv_nsec);
    t0 = ti;
    long total = (long)atoi(argv[3]) * info.frame_samples, done = 0;
    float complex buf[8192];
    double pw = 0;
    while (done < total) {
        int want = total - done > 8192 ? 8192 : (int)(total - done);
        int n = dvbt2_samples(t, buf, want);
        for (int i = 0; i < n; i++)
            pw += crealf(buf[i]) * crealf(buf[i]) + cimagf(buf[i]) * cimagf(buf[i]);
        if (o)
            fwrite(buf, sizeof(float complex), (size_t)n, o);
        done += n;
    }
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t1);
    double cpu = (double)(t1.tv_sec - t0.tv_sec) + 1e-9 * (double)(t1.tv_nsec - t0.tv_nsec);
    double dur = (double)total / dvbt2_sample_rate(&c);
    printf("samples=%ld packets=%ld mean_power=%.4f init=%.3f s cpu=%.3f s signal=%.3f s "
           "cpu/realtime=%.3f\n", total, npkt, pw / (double)total, init, cpu, dur, cpu / dur);
    dvbt2_free(t);
    if (o)
        fclose(o);
    return 0;
}
