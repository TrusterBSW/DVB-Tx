/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ofdm.h"

#include <fftw3.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define MAX_MEMBERS 64

struct ofdm_group {
    int nfft, ncp, n;
    struct { int ncar, bin_off; float gain; double ph, dph; } m[MAX_MEMBERS];
    fftwf_complex *buf;
    fftwf_plan plan;
};

static pthread_mutex_t plan_lock = PTHREAD_MUTEX_INITIALIZER;  /* FFTW planner is not thread-safe */

ofdm_group *ofdm_group_new(int nfft, int ncp, int threads)
{
    ofdm_group *g = xcalloc(1, sizeof *g);
    g->nfft = nfft;
    g->ncp = ncp;
    g->buf = fftwf_alloc_complex(nfft);
    pthread_mutex_lock(&plan_lock);
    (void)threads;
    g->plan = fftwf_plan_dft_1d(nfft, g->buf, g->buf, FFTW_BACKWARD, FFTW_MEASURE);
    pthread_mutex_unlock(&plan_lock);
    return g;
}

void ofdm_group_free(ofdm_group *g)
{
    if (!g)
        return;
    pthread_mutex_lock(&plan_lock);
    fftwf_destroy_plan(g->plan);
    pthread_mutex_unlock(&plan_lock);
    fftwf_free(g->buf);
    free(g);
}

int ofdm_group_add(ofdm_group *g, int ncar, int bin_off, float gain)
{
    if (g->n >= MAX_MEMBERS)
        return -1;
    int half = ncar / 2;
    if (bin_off - half <= -g->nfft / 2 || bin_off + half >= g->nfft / 2)
        return -1;
    for (int i = 0; i < g->n; i++) {   /* refuse overlapping carriers */
        int lo = g->m[i].bin_off - g->m[i].ncar / 2, hi = g->m[i].bin_off + g->m[i].ncar / 2;
        if (bin_off + half >= lo && bin_off - half <= hi)
            return -2;
    }
    g->m[g->n].ncar = ncar;
    g->m[g->n].bin_off = bin_off;
    g->m[g->n].gain = gain;
    /* a member off DC follows a continuous oscillator e^{j 2 pi bin_off n / nfft}: each symbol is
     * nfft + ncp samples long, so its phase advances by bin_off * ncp / nfft cycles per symbol (a
     * fraction unless the offset is a multiple of nfft / ncp bins). Without it the receiver sees a
     * phase step between symbols, takes it for a frequency error and degrades the signal (DVB-T
     * 8k at +3579 bins of 16384, GI 512: C/N 9.6 dB on a link that gives 30 dB) */
    g->m[g->n].ph = 0;
    g->m[g->n].dph = fmod((double)bin_off * g->ncp / g->nfft, 1.0);
    return g->n++;
}

int ofdm_group_len(const ofdm_group *g) { return g->nfft + g->ncp; }

void ofdm_group_synth(ofdm_group *g, const float complex *const *cars, float complex *out)
{
    float complex *b = (float complex *)g->buf;
    int N = g->nfft;
    memset(b, 0, sizeof(float complex) * N);
    for (int i = 0; i < g->n; i++) {
        const float complex *c = cars[i];
        int ncar = g->m[i].ncar, half = ncar / 2;
        float complex gain = g->m[i].gain * cexpf(2.0f * (float)M_PI * (float)g->m[i].ph * I);
        g->m[i].ph = fmod(g->m[i].ph + g->m[i].dph, 1.0);
        int start = g->m[i].bin_off - half;            /* bin of carrier 0, may be negative */
        int k = 0;
        for (; k < ncar && start + k < 0; k++)
            b[start + k + N] = c[k] * gain;
        for (; k < ncar; k++)
            b[start + k] = c[k] * gain;
    }
    fftwf_execute(g->plan);
    memcpy(out, b + N - g->ncp, sizeof(float complex) * g->ncp);
    memcpy(out + g->ncp, b, sizeof(float complex) * N);
}
