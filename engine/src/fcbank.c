/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fcbank.h"

#include <fftw3.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define MAX_MEMBERS 32
#define MIN_OVERLAP_SYMBOLS 256

typedef struct member {
    double rs;
    long a, b;                   /* Rs / Fs = a / b */
    int L, h;                    /* FFT size, new symbols per block */
    int nbins;
    int *src, *dst;              /* used FFT bins and where they go in the output spectrum */
    float *w;                    /* RRC weight * scale */
    float complex *win;          /* last L symbols */
    fftwf_complex *x;
    fftwf_plan fwd;
    int binoff;
    double ph, dph;              /* continuity phase of the frequency shift (cycles) */
    double fph, dfph;            /* residual offset (off - bin), applied to the symbols (cycles) */
    sym_fn get;
    void *ctx;
} member;

struct fcbank {
    double fs;
    long a0, b0;
    int N, H, V;
    member m[MAX_MEMBERS];
    int nm;
    fftwf_complex *Y;
    fftwf_plan inv;
    float complex *fifo;         /* H valid samples of the last block */
    int fpos;
};

static pthread_mutex_t plan_lock = PTHREAD_MUTEX_INITIALIZER;

/* p/q close to x (continued fractions), q <= qmax; 0 if none within the tolerance */
static int rational(double x, long qmax, long *p, long *q)
{
    long h0 = 0, h1 = 1, k0 = 1, k1 = 0;
    double v = x;
    for (int i = 0; i < 40; i++) {
        long a = (long)floor(v);
        long h2 = a * h1 + h0, k2 = a * k1 + k0;
        if (k2 > qmax)
            break;
        h0 = h1;
        h1 = h2;
        k0 = k1;
        k1 = k2;
        if (fabs((double)h1 / (double)k1 - x) <= 1e-11 * x) {
            *p = h1;
            *q = k1;
            return 1;
        }
        double f = v - (double)a;
        if (f < 1e-15)
            break;
        v = 1.0 / f;
    }
    return 0;
}

static int smooth_to(long n, int pmax)
{
    static const int f[] = { 2, 3, 5, 7, 11, 13, 17, 19, 23 };
    for (int i = 0; i < 9 && f[i] <= pmax; i++)
        while (n % f[i] == 0)
            n /= f[i];
    return n == 1;
}

static int smooth(long n) { return smooth_to(n, 23); }     /* FFT sizes accepted (dvbtx.config.SC_PRIMES) */

/* FFTW_MEASURE is quick for 7-smooth sizes, can take minutes with larger factors */
static unsigned plan_flags(long n) { return smooth_to(n, 7) ? FFTW_MEASURE : FFTW_ESTIMATE; }

fcbank *fcbank_new(double fs, double rs, double df)
{
    long a, b;
    if (rs <= 0 || rs >= fs || !rational(rs / fs, 65536, &a, &b) || !smooth(a) || !smooth(b))
        return NULL;
    fcbank *k = xcalloc(1, sizeof *k);
    k->fs = fs;
    k->a0 = a;
    k->b0 = b;
    /* overlap: whole multiple of b, at least MIN_OVERLAP_SYMBOLS symbols */
    long mv = (MIN_OVERLAP_SYMBOLS + a - 1) / a;
    k->V = (int)(b * mv);
    /* N = b * m with m 2^i 3^j 5^k: N and every L = m * a stay FFT-friendly; N ~ 16k or more */
    long m0 = 1, m = 0;
    while (m0 < 3 * mv || b * m0 < 16384)
        m0++;
    for (long c = m0; c < 4 * m0 && df > 0; c++) {
        double bins = (double)b * c * df / fs;      /* df in bins */
        if (smooth_to(c, 5) && c % 2 == 0 && fabs(bins - round(bins)) < 1e-9) {
            m = c;
            break;
        }
    }
    if (!m)
        for (m = m0; !smooth_to(m, 5) || m % 2; m++)
            ;
    k->H = (int)(b * (m - mv));
    k->N = k->H + k->V;
    if (k->N > (1 << 20)) {
        free(k);
        return NULL;
    }
    k->Y = fftwf_alloc_complex(k->N);
    k->fifo = xcalloc(k->H, sizeof(float complex));
    k->fpos = k->H;
    pthread_mutex_lock(&plan_lock);
    k->inv = fftwf_plan_dft_1d(k->N, k->Y, k->Y, FFTW_BACKWARD, plan_flags(k->N));
    pthread_mutex_unlock(&plan_lock);
    return k;
}

int fcbank_members(const fcbank *k) { return k->nm; }

double fcbank_offset(const fcbank *k, int i)
{
    return (double)k->m[i].binoff * k->fs / k->N + k->m[i].dfph * k->m[i].rs;
}

static double response(int pulse, double f, double rs, double alpha)   /* unit in the pass band */
{
    double x = fabs(f) / rs, lo = (1 - alpha) / 2, hi = (1 + alpha) / 2;
    if (pulse == FC_FLAT) {
        lo = alpha / 2;
        hi = 0.5;
        if (x <= lo)
            return 1;
        if (x >= hi)
            return 0;
        return 0.5 + 0.5 * cos(M_PI * (x - lo) / (hi - lo));
    }
    if (x <= lo)
        return 1;
    if (x >= hi)
        return 0;
    return cos(M_PI / (2 * alpha) * (x - lo));
}

int fcbank_add(fcbank *k, double rs, int pulse, double alpha, double offset_hz, float gain, sym_fn get, void *ctx)
{
    long a, b;
    if (k->nm >= MAX_MEMBERS || !rational(rs / k->fs, 65536, &a, &b) || !smooth(a) || !smooth(b))
        return -1;
    if (((long)k->N * a) % b || ((long)k->H * a) % b || (long)k->V * a / b < 64)
        return -1;
    member *m = &k->m[k->nm];
    memset(m, 0, sizeof *m);
    m->rs = rs;
    m->a = a;
    m->b = b;
    m->L = (int)((long)k->N * a / b);
    m->h = (int)((long)k->H * a / b);
    m->get = get;
    m->ctx = ctx;
    m->binoff = (int)lround(offset_hz / k->fs * k->N);
    double edge = (double)m->binoff / k->N * k->fs;
    m->dfph = (offset_hz - edge) / rs;       /* the rest of the offset: |.| < Fs / 2N, rotates the symbols */
    double ext = pulse == FC_FLAT ? 0.5 : (1 + alpha) / 2;           /* extent, in Rs */
    if (fabs(edge) + rs * ext >= k->fs / 2)
        return -1;
    /* bins of the member: f = k' Rs / L for |f| < ext Rs, weighted by the exact response (a time-domain
     * window to the overlap was tried: 76 dB of MER instead of 114, the wrap-around of the RRC tails is
     * negligible) */
    int half = (int)ceil(ext * m->L) + 1;          /* beyond L/2: images of the symbol spectrum */
    m->src = xcalloc(2 * half + 1, sizeof(int));
    m->dst = xcalloc(2 * half + 1, sizeof(int));
    m->w = xcalloc(2 * half + 1, sizeof(float));
    double sumw2 = 0;
    for (int kk = -half; kk <= half; kk++) {
        double w = response(pulse, (double)kk * rs / m->L, rs, alpha);
        if (w <= 0)
            continue;
        m->src[m->nbins] = (kk % m->L + m->L) % m->L;
        m->dst[m->nbins] = ((kk + m->binoff) % k->N + k->N) % k->N;
        m->w[m->nbins] = (float)w;
        sumw2 += w * w;
        m->nbins++;
    }
    /* RRC, unit-power symbols: E|X|^2 = L, unnormalised IFFT: E|y|^2 = sum |Y|^2;
     * flat: a tone of amplitude A gives X = L A, y = L A w */
    double scale = pulse == FC_FLAT ? gain / (double)m->L : gain / sqrt((double)m->L * sumw2);
    for (int i = 0; i < m->nbins; i++)
        m->w[i] = (float)(m->w[i] * scale);
    m->win = xcalloc(m->L, sizeof(float complex));
    m->x = fftwf_alloc_complex(m->L);
    pthread_mutex_lock(&plan_lock);
    m->fwd = fftwf_plan_dft_1d(m->L, m->x, m->x, FFTW_FORWARD, plan_flags(m->L));
    pthread_mutex_unlock(&plan_lock);
    /* phase continuity: block j multiplied by e^{j 2 pi binoff H (j+1) / N} */
    m->dph = fmod((double)m->binoff * k->H / k->N, 1.0);
    m->ph = m->dph;
    return k->nm++;
}

void fcbank_sizes(const fcbank *k, int i, int *N, int *H, int *L, int *h)
{
    *N = k->N;
    *H = k->H;
    *L = k->m[i].L;
    *h = k->m[i].h;
}

static void next_block(fcbank *k)
{
    memset(k->Y, 0, sizeof(fftwf_complex) * k->N);
    float complex *Y = (float complex *)k->Y;
    for (int i = 0; i < k->nm; i++) {
        member *m = &k->m[i];
        /* slide the symbol window by h and pull h new symbols */
        memmove(m->win, m->win + m->h, sizeof(float complex) * (m->L - m->h));
        for (int s = m->L - m->h; s < m->L; s++)
            m->get(m->ctx, &m->win[s]);
        if (m->dfph != 0) {
            /* exact offset: the residual (< half a bin) shifts the symbols; the shaping filter is not
             * shifted with them, an error below -60 dB at the band edges */
            double complex r = cexp(2 * M_PI * m->fph * I), w = cexp(2 * M_PI * m->dfph * I);
            for (int s = m->L - m->h; s < m->L; s++) {
                m->win[s] *= (float complex)r;
                r *= w;
            }
            m->fph = fmod(m->fph + m->dfph * m->h, 1.0);
        }
        memcpy(m->x, m->win, sizeof(float complex) * m->L);
        fftwf_execute(m->fwd);
        const float complex *X = (const float complex *)m->x;
        float complex rot = cexpf(2.0f * (float)M_PI * (float)m->ph * I);
        m->ph = fmod(m->ph + m->dph, 1.0);
        for (int j = 0; j < m->nbins; j++)
            Y[m->dst[j]] += X[m->src[j]] * (m->w[j] * rot);
    }
    fftwf_execute(k->inv);
    memcpy(k->fifo, Y + k->V / 2, sizeof(float complex) * k->H);
    k->fpos = 0;
}

void fcbank_block(fcbank *k, float complex *out, int n)
{
    while (n > 0) {
        if (k->fpos == k->H)
            next_block(k);
        int c = k->H - k->fpos;
        if (c > n)
            c = n;
        memcpy(out, k->fifo + k->fpos, sizeof(float complex) * c);
        k->fpos += c;
        out += c;
        n -= c;
    }
}

void fcbank_free(fcbank *k)
{
    if (!k)
        return;
    pthread_mutex_lock(&plan_lock);
    for (int i = 0; i < k->nm; i++) {
        fftwf_destroy_plan(k->m[i].fwd);
        fftwf_free(k->m[i].x);
    }
    fftwf_destroy_plan(k->inv);
    pthread_mutex_unlock(&plan_lock);
    for (int i = 0; i < k->nm; i++) {
        free(k->m[i].src);
        free(k->m[i].dst);
        free(k->m[i].w);
        free(k->m[i].win);
    }
    fftwf_free(k->Y);
    free(k->fifo);
    free(k);
}
