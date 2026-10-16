/* RF smoke test: N DVB-T carriers (same TS file looped) on one HackRF through the OFDM group. */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../engine/src/dvbt.h"
#include "../engine/src/ofdm.h"
#include "../engine/src/sdr.h"

static FILE *tsf;
static void pull(void *ctx, uint8_t *pkt)
{
    (void)ctx;
    if (fread(pkt, 1, 188, tsf) != 188) { rewind(tsf); if (fread(pkt, 1, 188, tsf) != 188) exit(1); }
}

int main(int argc, char **argv)
{
    if (argc < 7) { fprintf(stderr, "usage: plugin.so in.ts lo_hz os gain_db off_hz [off_hz...]\n"); return 1; }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    const sdr_driver *drv = dlsym(h, "dvbtx_sdr_driver");
    tsf = fopen(argv[2], "rb");
    double lo = atof(argv[3]); int os = atoi(argv[4]); double gain = atof(argv[5]);
    int nc = argc - 6;
    dvbt_cfg c = { 8192, 6, 7, 8, 32, 8e6, 0 };
    double spacing = dvbt_sample_rate(&c) / c.fft;
    int nfft = c.fft * os;
    ofdm_group *g = ofdm_group_new(nfft, nfft / c.gi, 1);
    dvbt *mods[8]; float complex *cars[8];
    double P = dvbt_mean_power(&c) * nc;
    for (int i = 0; i < nc; i++) {
        double off = atof(argv[6 + i]);
        int bins = (int)lround(off / spacing);
        if (fabs(bins * spacing - off) > 1) { fprintf(stderr, "offset %g not a multiple of %g\n", off, spacing); return 1; }
        mods[i] = dvbt_new(&c, pull, NULL);
        cars[i] = malloc(sizeof(float complex) * dvbt_ncarriers(&c));
        if (ofdm_group_add(g, dvbt_ncarriers(&c), bins, (float)(1.0 / sqrt(P))) < 0) { fprintf(stderr, "bad placement\n"); return 1; }
    }
    sdr_params p = { .freq_hz = lo, .rate = spacing * nfft, .gain = gain, .buffer_ms = 50 };
    char err[256];
    sdr_dev *d = drv->open(&p, err, sizeof err);
    if (!d) { fprintf(stderr, "%s\n", err); return 1; }
    fprintf(stderr, "rate %.3f MS/s, ts rate %.1f bit/s per carrier\n", p.rate / 1e6, dvbt_ts_rate(&c));
    int len = ofdm_group_len(g);
    float complex *out = malloc(sizeof(float complex) * len);
    float scale = powf(10.0f, -13.0f / 20.0f);
    for (long s = 0;; s++) {
        for (int i = 0; i < nc; i++) dvbt_symbol(mods[i], cars[i]);
        ofdm_group_synth(g, (const float complex *const *)cars, out);
        const float complex *b[1] = { out };
        if (drv->write(d, b, len, scale)) break;
        if (s % 5000 == 0) { sdr_stats st; drv->get_stats(d, &st); fprintf(stderr, "sym %ld underruns %lu fill %.2f\n", s, (unsigned long)st.underruns, st.fill); }
    }
    return 0;
}
