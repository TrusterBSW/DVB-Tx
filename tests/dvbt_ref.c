/* Test: modulate a TS file with the dvbtx DVB-T coder at the elementary rate (cf32 out). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../engine/src/dvbt.h"
#include "../engine/src/ofdm.h"

static FILE *tsf;
static void pull(void *ctx, uint8_t *pkt)
{
    (void)ctx;
    if (fread(pkt, 1, 188, tsf) != 188) { rewind(tsf); if (fread(pkt, 1, 188, tsf) != 188) exit(1); }
}

int main(int argc, char **argv)
{
    if (argc < 9) { fprintf(stderr, "usage: in.ts out.cf32 fft bits k n gi nsym\n"); return 1; }
    tsf = fopen(argv[1], "rb");
    FILE *o = fopen(argv[2], "wb");
    dvbt_cfg c = { atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), atoi(argv[6]), atoi(argv[7]), 8e6, 0 };
    dvbt *t = dvbt_new(&c, pull, NULL);
    if (!t) { fprintf(stderr, "bad cfg %s\n", dvbt_cfg_check(&c)); return 1; }
    int nc = dvbt_ncarriers(&c);
    ofdm_group *g = ofdm_group_new(c.fft, c.fft / c.gi, 1);
    ofdm_group_add(g, nc, 0, 1.0f);
    float complex *car = malloc(sizeof(float complex) * nc), *out = malloc(sizeof(float complex) * ofdm_group_len(g));
    int nsym = atoi(argv[8]);
    for (int s = 0; s < nsym; s++) {
        dvbt_symbol(t, car);
        const float complex *cp[1] = { car };
        ofdm_group_synth(g, cp, out);
        fwrite(out, sizeof(float complex), ofdm_group_len(g), o);
    }
    fprintf(stderr, "ts rate %.1f\n", dvbt_ts_rate(&c));
    fclose(o);
    return 0;
}
