/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "scgen.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define NPHASE 1024
#define MAX_SPAN 32

struct scgen {
    double step;                 /* symbols per output sample */
    double frac;                 /* position within the newest symbol interval, [0,1) */
    int span;
    float *h;                    /* [NPHASE][2*span]: taps oldest-first, each duplicated (re, im) */
    float complex win[MAX_SPAN]; /* last span symbols, oldest first */
    double ph, dph;              /* NCO phase (cycles) */
    float complex rot, w;
    int renorm;
    float gain;
    sym_fn get;
    void *ctx;
};

static double rrc(double t, double a)
{
    if (fabs(t) < 1e-9)
        return 1 - a + 4 * a / M_PI;
    if (a > 0 && fabs(fabs(4 * a * t) - 1) < 1e-9)
        return a / sqrt(2) * ((1 + 2 / M_PI) * sin(M_PI / (4 * a)) + (1 - 2 / M_PI) * cos(M_PI / (4 * a)));
    return (sin(M_PI * t * (1 - a)) + 4 * a * t * cos(M_PI * t * (1 + a))) / (M_PI * t * (1 - 16 * a * a * t * t));
}

static void push(scgen *g)
{
    memmove(g->win, g->win + 1, sizeof(float complex) * (g->span - 1));
    g->get(g->ctx, &g->win[g->span - 1]);
}

static inline float complex filt(const scgen *g)
{
    int p = (int)(g->frac * NPHASE);
    const float *h = g->h + (size_t)p * 2 * g->span;
    const float *x = (const float *)g->win;
    float re = 0, im = 0;
    for (int i = 0; i < 2 * g->span; i += 2) {
        re += x[i] * h[i];
        im += x[i + 1] * h[i + 1];
    }
    return re + im * I;
}

/* Kaiser-windowed sinc, cut-off half way between the occupied band edge and the first image */
static double bessel_i0(double x)
{
    double s = 1, t = 1;
    for (int k = 1; k < 40; k++) {
        t *= (x / (2 * k)) * (x / (2 * k));
        s += t;
    }
    return s;
}

static double lowpass(double t, double occupied, double span)
{
    double fc = (occupied / 2 + 0.5) / 2;            /* cycles per input sample */
    double sinc = fabs(t) < 1e-12 ? 2 * fc : sin(2 * M_PI * fc * t) / (M_PI * t);
    double half = span / 2, beta = 7.0, r = t / half;
    if (fabs(r) >= 1)
        return 0;
    return sinc * bessel_i0(beta * sqrt(1 - r * r)) / bessel_i0(beta);
}

scgen *scgen_new(double rs, double fs, double off, double rolloff, int span, float gain, sym_fn get, void *ctx)
{
    return scgen_new2(rs, fs, off, PULSE_RRC, rolloff, span, gain, get, ctx);
}

scgen *scgen_new2(double rs, double fs, double off, int pulse, double rolloff, int span, float gain, sym_fn get,
                  void *ctx)
{
    if (span > MAX_SPAN)
        span = MAX_SPAN;
    if (span < 4)
        span = 4;
    scgen *g = xcalloc(1, sizeof *g);
    g->step = rs / fs;
    g->span = span;
    g->get = get;
    g->ctx = ctx;
    g->gain = gain;
    g->h = xcalloc((size_t)NPHASE * 2 * span, sizeof(float));
    /* window index i (oldest first) holds s_{k-span+1+i}; output time t = k + frac - span/2 */
    for (int p = 0; p < NPHASE; p++) {
        double frac = (double)p / NPHASE;
        for (int i = 0; i < span; i++) {
            double tau = frac + (span - 1 - i) - span / 2.0;
            float v = (float)(pulse == PULSE_LOWPASS ? lowpass(tau, rolloff, span) : rrc(tau, rolloff));
            g->h[(size_t)p * 2 * span + 2 * i] = v;
            g->h[(size_t)p * 2 * span + 2 * i + 1] = v;
        }
    }
    if (pulse == PULSE_LOWPASS) {
        /* interpolation of an already band-limited signal: unit gain in the passband (DC) */
        for (int p = 0; p < NPHASE; p++) {
            float *hp = g->h + (size_t)p * 2 * span;
            double sum = 0;
            for (int i = 0; i < span; i++)
                sum += hp[2 * i];
            for (int i = 0; i < 2 * span; i++)
                hp[i] = (float)(hp[i] / sum);
        }
        g->dph = off / fs;
        g->rot = 1.0f;
        g->w = cexpf(2.0f * (float)M_PI * (float)g->dph * I);
        return g;
    }
    /* normalise to unit output power: unit-power random QPSK through the filter */
    sym_fn real_get = g->get;
    g->get = NULL;
    double acc = 0;
    unsigned seed = 12345;
    int n = 40000;
    for (int k = 0; k < n; k++) {
        if (g->frac + g->step >= 1.0 || k == 0) {
            memmove(g->win, g->win + 1, sizeof(float complex) * (span - 1));
            seed = seed * 1103515245u + 12345u;
            g->win[span - 1] = ((seed >> 16) & 1 ? 0.70710678f : -0.70710678f) +
                               ((seed >> 17) & 1 ? 0.70710678f : -0.70710678f) * I;
        }
        g->frac += g->step;
        if (g->frac >= 1.0)
            g->frac -= 1.0;
        float complex y = filt(g);
        if (k > 4 * span / g->step)
            acc += crealf(y * conjf(y));
    }
    double pw = acc / (n - (int)(4 * span / g->step) - 1);
    float sc = pw > 0 ? (float)(1.0 / sqrt(pw)) : 1.0f;
    for (size_t i = 0; i < (size_t)NPHASE * 2 * span; i++)
        g->h[i] *= sc;
    memset(g->win, 0, sizeof g->win);
    g->frac = 0;
    g->get = real_get;
    g->dph = off / fs;
    g->rot = 1.0f;
    g->w = cexpf(2.0f * (float)M_PI * (float)g->dph * I);
    return g;
}

void scgen_free(scgen *g)
{
    if (!g)
        return;
    free(g->h);
    free(g);
}

void scgen_block(scgen *g, float complex *out, int n)
{
    float gain = g->gain;
    for (int i = 0; i < n; i++) {
        g->frac += g->step;
        while (g->frac >= 1.0) {
            g->frac -= 1.0;
            push(g);
        }
        out[i] = filt(g) * g->rot * gain;
        g->rot *= g->w;
        if (++g->renorm == 4096) {     /* exact phase every 4096 samples: no drift of the rotator */
            g->renorm = 0;
            g->ph = fmod(g->ph + g->dph * 4096, 1.0);
            g->rot = cexpf(2.0f * (float)M_PI * (float)g->ph * I);
        }
    }
}
