/* fcbank with worker threads against the single-thread bank: two members, stopped and started
 * again at fixed symbol counts (from the source itself, so at the same block whatever the
 * read-ahead), output hashed; every worker count must give the same hash. In one process FFTW
 * plans the same sizes the same way, so the outputs must be bit-identical.
 *   gcc -O2 -o /tmp/fcbank_threads_check tests/fcbank_threads_check.c engine/src/fcbank.c \
 *       engine/src/util.c -lfftw3f -lm -pthread && /tmp/fcbank_threads_check */
#include <stdint.h>
#include <stdio.h>
#include "../engine/src/fcbank.h"

typedef struct { unsigned x; long n; int sched; } src;
static fcbank *K;

static void blk(void *ctx, float complex *v, int n)
{
    src *s = ctx;
    if (s->sched) {          /* member 0 drives the stops: member 1 at 1 M symbols, 0 at 2.5 M ... */
        static const long at[4] = { 1000000, 2500000, 3000000, 4000000 };
        static const int who[4] = { 1, 0, 1, 0 }, off[4] = { 1, 1, 0, 0 };
        for (int i = 0; i < 4; i++)
            if (s->n < at[i] && s->n + n >= at[i])
                fcbank_mute(K, who[i], off[i]);
    }
    s->n += n;
    for (int i = 0; i < n; i++) {
        s->x = s->x * 1103515245u + 12345u;
        v[i] = ((int)(s->x >> 24) - 128) / 90.0f + ((int)((s->x >> 16) & 255) - 128) / 90.0f * I;
    }
}

static uint64_t run(int workers)
{
    src a = { 1, 0, 1 }, b = { 2, 0, 0 };
    K = fcbank_new(56e6, 20e6, 0);
    if (!K || fcbank_add_blk(K, 20e6, FC_RRC, 0.2, -13e6, 0.5f, blk, &a) < 0 ||
        fcbank_add_blk(K, 20e6, FC_RRC, 0.2, 12.3e6, 0.5f, blk, &b) < 0)
        return 0;
    fcbank_threads(K, workers);
    static float complex out[10007];
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < 600; i++) {
        fcbank_block(K, out, 10007);
        const uint8_t *p = (const uint8_t *)out;
        for (size_t j = 0; j < sizeof out; j++)
            h = (h ^ p[j]) * 1099511628211ULL;
    }
    fcbank_free(K);
    return h;
}

int main(void)
{
    uint64_t ref = run(0);
    int bad = !ref;
    for (int w = 1; w <= 5 && !bad; w++) {
        uint64_t h = run(w);
        printf("%d worker(s): %016llx %s\n", w, (unsigned long long)h, h == ref ? "same" : "DIFFERENT");
        bad |= h != ref;
    }
    printf("fcbank_threads_check: %s\n", bad ? "FAILED" : "OK (bit-identical to the single-thread bank)");
    return bad;
}
