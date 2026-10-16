/* Benchmark: DVB-S2 symbol generation and the scgen shaping/resampling filter. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../engine/src/dvbs2.h"
#include "../engine/src/scgen.h"
static void pull(void *ctx, uint8_t *p) { (void)ctx; memset(p, 0x5A, 188); p[0] = 0x47; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void qpsk(void *ctx, float complex *s) { (void)ctx; static unsigned x = 1; x = x * 1103515245u + 12345u; *s = ((x >> 16) & 1 ? 0.7f : -0.7f) + ((x >> 17) & 1 ? 0.7f : -0.7f) * I; }
int main(void)
{
    dvbs2_cfg c = { 3, 3, 4, 0, 1, 0.2, 0 };
    dvbs2 *s = dvbs2_new(&c, pull, NULL);
    int n = 4000000; float complex v;
    double t0 = now();
    for (int i = 0; i < n; i++) dvbs2_symbol(s, &v);
    double dt = now() - t0;
    printf("DVB-S2 8PSK 3/4: %.1f Msym/s (one core)\n", n / dt / 1e6);
    scgen *g = scgen_new(10e6, 24.9e6, 6.3e6, 0.2, 16, 1.0f, qpsk, NULL);
    static float complex buf[16384];
    t0 = now();
    for (int i = 0; i < 1000; i++) scgen_block(g, buf, 16384);
    dt = now() - t0;
    printf("scgen span 16: %.1f MS/s output (one core)\n", 16384.0 * 1000 / dt / 1e6);
    return 0;
}
