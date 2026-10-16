/* Benchmark: DVB-T coding of one 8k 64-QAM 7/8 carrier (null packets), symbols per second. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../engine/src/dvbt.h"
#include "../engine/src/outer.h"
static void pull(void *ctx, uint8_t *p) { (void)ctx; memset(p, 0xFF, 188); p[0] = 0x47; p[1] = 0x1F; p[2] = 0xFF; p[3] = 0x10; for (int i = 4; i < 188; i++) p[i] = (uint8_t)(i * 37); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
int main(void)
{
    dvbt_cfg c = { 8192, 6, 7, 8, 32, 8e6, 0 };
    dvbt *t = dvbt_new(&c, pull, NULL);
    static float complex car[6817];
    int n = 4000;
    double t0 = now();
    for (int i = 0; i < n; i++) dvbt_symbol(t, car);
    double dt = now() - t0;
    double need = dvbt_sample_rate(&c) / (8192 + 256);
    printf("%.0f symbols/s, real time needs %.0f -> %.1f %% of a core\n", n / dt, need, need / (n / dt) * 100);
    outer o; outer_init(&o); uint8_t pk[188], cw[204]; pull(NULL, pk);
    t0 = now(); for (int i = 0; i < 200000; i++) outer_packet(&o, pk, cw); dt = now() - t0;
    printf("outer: %.0f packets/s (needs %.0f) -> %.1f %%\n", 200000 / dt, dvbt_ts_rate(&c) / 1504, dvbt_ts_rate(&c) / 1504 / (200000 / dt) * 100);
    return 0;
}
