/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fcbank.h"

#include <fftw3.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    fftwf_plan fwd;              /* in place, on any job's x[i] */
    int binoff;
    double ph, dph;              /* continuity phase of the frequency shift (cycles) */
    double fph, dfph;            /* residual offset (off - bin), applied to the symbols (cycles) */
    symblk_fn get;
    void *ctx;
    sym_fn get1;                 /* fcbank_add: one symbol at a time */
    void *ctx1;
    volatile int muted;          /* off the air: its symbols are pulled (and dropped), not filtered */
} member;

/* one block: the symbol windows of its members, then (worker) their spectra and the output */
typedef struct job {
    fftwf_complex *x[MAX_MEMBERS];   /* window of L symbols, then its FFT */
    float complex rot[MAX_MEMBERS];
    unsigned char live[MAX_MEMBERS];
    int any;                     /* a member on the air */
    fftwf_complex *Y;            /* N bins, then N samples */
    int state;                   /* JOB_FREE, JOB_READY (to compute), JOB_BUSY, JOB_DONE */
} job;

enum { JOB_FREE, JOB_READY, JOB_BUSY, JOB_DONE };
#define MAX_WORKERS 8

struct fcbank {
    double fs;
    long a0, b0;
    int N, H, V;
    member m[MAX_MEMBERS];
    int nm;
    fftwf_plan inv;              /* in place, on any job's Y */
    const float complex *fifo;   /* H valid samples of the current block */
    float complex *zeros;        /* the block of a bank whose members are all off the air */
    int fpos;
    /* blocks: job[seq % njobs]; the caller fills them in order, the workers compute them */
    job *jobs;
    int njobs, nworkers, started;
    long filled, taken, used;    /* blocks filled, taken by a worker, given out (current one included) */
    pthread_t th[MAX_WORKERS];
    int running;                 /* worker threads created (read by the statistics) */
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int stop;
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

/* FFTW_MEASURE is quick for 7-smooth sizes, can take minutes with larger factors (and, time-limited,
 * finds nothing better than FFTW_ESTIMATE for them) */
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
    k->zeros = xcalloc(k->H, sizeof(float complex));
    k->fpos = k->H;
    k->njobs = 1;
    k->jobs = xcalloc(1, sizeof(job));
    k->jobs[0].Y = fftwf_alloc_complex(k->N);
    pthread_mutex_init(&k->lock, NULL);
    pthread_cond_init(&k->cond, NULL);
    pthread_mutex_lock(&plan_lock);
    k->inv = fftwf_plan_dft_1d(k->N, k->jobs[0].Y, k->jobs[0].Y, FFTW_BACKWARD, plan_flags(k->N));
    pthread_mutex_unlock(&plan_lock);
    return k;
}

int fcbank_members(const fcbank *k) { return k->nm; }

double fcbank_offset(const fcbank *k, int i)
{
    return (double)k->m[i].binoff * k->fs / k->N + k->m[i].dfph * k->m[i].rs;
}

static double response(int pulse, double f, double rs, double alpha, double top)   /* unit in the pass band */
{
    double x = fabs(f) / rs, lo = (1 - alpha) / 2, hi = (1 + alpha) / 2;
    if (pulse == FC_FLAT) {
        lo = alpha / 2;
        hi = top;
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

int fcbank_add_blk(fcbank *k, double rs, int pulse, double alpha, double offset_hz, float gain, symblk_fn get,
                   void *ctx)
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
    if (pulse == FC_FLAT) {
        /* the taper above the occupied band may end before Rs / 2 when the radio band is short */
        double room = (k->fs / 2 - fabs(edge)) / rs - 1.0 / m->L;
        if (room < ext)
            ext = room;
        if (ext <= alpha / 2 + 0.01)
            return -1;
    } else if (fabs(edge) + rs * ext >= k->fs / 2) {
        return -1;
    }
    /* bins of the member: f = k' Rs / L for |f| < ext Rs, weighted by the exact response (a time-domain
     * window to the overlap was tried: 76 dB of MER instead of 114, the wrap-around of the RRC tails is
     * negligible) */
    int half = (int)ceil(ext * m->L) + 1;          /* beyond L/2: images of the symbol spectrum */
    m->src = xcalloc(2 * half + 1, sizeof(int));
    m->dst = xcalloc(2 * half + 1, sizeof(int));
    m->w = xcalloc(2 * half + 1, sizeof(float));
    double sumw2 = 0;
    for (int kk = -half; kk <= half; kk++) {
        double w = response(pulse, (double)kk * rs / m->L, rs, alpha, ext);
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
    k->jobs[0].x[k->nm] = fftwf_alloc_complex(m->L);
    pthread_mutex_lock(&plan_lock);
    m->fwd = fftwf_plan_dft_1d(m->L, k->jobs[0].x[k->nm], k->jobs[0].x[k->nm], FFTW_FORWARD, plan_flags(m->L));
    pthread_mutex_unlock(&plan_lock);
    /* phase continuity: block j multiplied by e^{j 2 pi binoff H (j+1) / N} */
    m->dph = fmod((double)m->binoff * k->H / k->N, 1.0);
    m->ph = m->dph;
    return k->nm++;
}

/* fcbank_add: a source of one symbol at a time */
static void one_by_one(void *ctx, float complex *v, int n)
{
    member *m = ctx;
    for (int i = 0; i < n; i++)
        m->get1(m->ctx1, &v[i]);
}

int fcbank_add(fcbank *k, double rs, int pulse, double alpha, double offset_hz, float gain, sym_fn get, void *ctx)
{
    if (k->nm >= MAX_MEMBERS)
        return -1;
    int i = fcbank_add_blk(k, rs, pulse, alpha, offset_hz, gain, one_by_one, &k->m[k->nm]);
    if (i >= 0) {
        k->m[i].get1 = get;
        k->m[i].ctx1 = ctx;
    }
    return i;
}

void fcbank_threads(fcbank *k, int n)
{
    if (k->started || n < 0)
        return;
    k->nworkers = n > MAX_WORKERS ? MAX_WORKERS : n;
}

void fcbank_sizes(const fcbank *k, int i, int *N, int *H, int *L, int *h)
{
    *N = k->N;
    *H = k->H;
    *L = k->m[i].L;
    *h = k->m[i].h;
}

/* the sequential part of a block: the members' new symbols (and their phases) into job j */
static void fill(fcbank *k, job *j)
{
    j->any = 0;
    for (int i = 0; i < k->nm; i++) {
        member *m = &k->m[i];
        if (m->muted) {
            /* keep its symbol clock and phases; the window restarts from silence */
            m->get(m->ctx, m->win, m->h);
            memset(m->win, 0, sizeof(float complex) * m->L);
            m->fph = fmod(m->fph + m->dfph * m->h, 1.0);
            m->ph = fmod(m->ph + m->dph, 1.0);
            j->live[i] = 0;
            continue;
        }
        /* slide the symbol window by h and pull h new symbols */
        memmove(m->win, m->win + m->h, sizeof(float complex) * (m->L - m->h));
        m->get(m->ctx, m->win + m->L - m->h, m->h);
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
        memcpy(j->x[i], m->win, sizeof(float complex) * m->L);
        j->rot[i] = cexpf(2.0f * (float)M_PI * (float)m->ph * I);
        m->ph = fmod(m->ph + m->dph, 1.0);
        j->live[i] = 1;
        j->any = 1;
    }
}

/* the FFTs of a block (any thread) */
static void compute(fcbank *k, job *j)
{
    if (!j->any)
        return;
    float complex *Y = (float complex *)j->Y;
    memset(Y, 0, sizeof(fftwf_complex) * k->N);
    for (int i = 0; i < k->nm; i++) {
        if (!j->live[i])
            continue;
        member *m = &k->m[i];
        fftwf_execute_dft(m->fwd, j->x[i], j->x[i]);
        const float complex *X = (const float complex *)j->x[i];
        float complex rot = j->rot[i];
        for (int b = 0; b < m->nbins; b++)
            Y[m->dst[b]] += X[m->src[b]] * (m->w[b] * rot);
    }
    fftwf_execute_dft(k->inv, j->Y, j->Y);
}

static void *worker(void *arg)
{
    fcbank *k = arg;
    pthread_mutex_lock(&k->lock);
    for (;;) {
        while (!k->stop && k->taken == k->filled)
            pthread_cond_wait(&k->cond, &k->lock);
        if (k->stop)
            break;
        job *j = &k->jobs[k->taken++ % k->njobs];
        j->state = JOB_BUSY;
        pthread_mutex_unlock(&k->lock);
        compute(k, j);
        pthread_mutex_lock(&k->lock);
        j->state = JOB_DONE;
        pthread_cond_broadcast(&k->cond);
    }
    pthread_mutex_unlock(&k->lock);
    return NULL;
}

/* the job buffers of the workers (job 0 holds the ones the plans were made on) */
static void start_workers(fcbank *k)
{
    k->started = 1;
    if (k->nworkers <= 0)
        return;
    /* enough blocks in flight for every worker, and for the caller to fill the next ones meanwhile */
    int nj = 2 * k->nworkers + 1;
    job *jobs = xcalloc(nj, sizeof(job));
    jobs[0] = k->jobs[0];
    for (int q = 1; q < nj; q++) {
        jobs[q].Y = fftwf_alloc_complex(k->N);
        for (int i = 0; i < k->nm; i++)
            jobs[q].x[i] = fftwf_alloc_complex(k->m[i].L);
    }
    free(k->jobs);
    k->jobs = jobs;
    k->njobs = nj;
    for (int w = 0; w < k->nworkers; w++) {
        if (pthread_create(&k->th[w], NULL, worker, k)) {
            k->nworkers = w;
            break;
        }
        __atomic_store_n(&k->running, w + 1, __ATOMIC_RELEASE);
    }
}

double fcbank_workers_cpu(const fcbank *k)
{
    double t = 0;
    int n = __atomic_load_n(&k->running, __ATOMIC_ACQUIRE);
    for (int w = 0; w < n; w++) {
        clockid_t cid;
        struct timespec ts;
        if (!pthread_getcpuclockid(k->th[w], &cid) && !clock_gettime(cid, &ts))
            t += ts.tv_sec + ts.tv_nsec / 1e9;
    }
    return t;
}

static void next_block(fcbank *k)
{
    if (!k->started)
        start_workers(k);
    if (k->nworkers <= 0) {
        job *j = &k->jobs[0];
        fill(k, j);
        compute(k, j);
        k->fifo = j->any ? (const float complex *)j->Y + k->V / 2 : k->zeros;
        k->fpos = 0;
        return;
    }
    pthread_mutex_lock(&k->lock);
    if (k->used > 0)                                   /* the block just given out */
        k->jobs[(k->used - 1) % k->njobs].state = JOB_FREE;
    /* fill the free blocks ahead (the symbol pulls may wait on the members' sources) */
    while (k->filled < k->used + k->njobs) {
        job *j = &k->jobs[k->filled % k->njobs];
        pthread_mutex_unlock(&k->lock);
        fill(k, j);
        pthread_mutex_lock(&k->lock);
        j->state = JOB_READY;
        k->filled++;
        pthread_cond_broadcast(&k->cond);
    }
    job *j = &k->jobs[k->used % k->njobs];
    while (j->state != JOB_DONE)
        pthread_cond_wait(&k->cond, &k->lock);
    k->used++;
    pthread_mutex_unlock(&k->lock);
    k->fifo = j->any ? (const float complex *)j->Y + k->V / 2 : k->zeros;
    k->fpos = 0;
}

void fcbank_mute(fcbank *k, int i, int muted)
{
    if (i >= 0 && i < k->nm)
        k->m[i].muted = muted;
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
    pthread_mutex_lock(&k->lock);
    k->stop = 1;
    pthread_cond_broadcast(&k->cond);
    pthread_mutex_unlock(&k->lock);
    for (int w = 0; w < k->nworkers; w++)
        pthread_join(k->th[w], NULL);
    pthread_mutex_lock(&plan_lock);
    for (int i = 0; i < k->nm; i++)
        fftwf_destroy_plan(k->m[i].fwd);
    fftwf_destroy_plan(k->inv);
    pthread_mutex_unlock(&plan_lock);
    for (int q = 0; q < k->njobs; q++) {
        for (int i = 0; i < k->nm; i++)
            fftwf_free(k->jobs[q].x[i]);
        fftwf_free(k->jobs[q].Y);
    }
    for (int i = 0; i < k->nm; i++) {
        free(k->m[i].src);
        free(k->m[i].dst);
        free(k->m[i].w);
        free(k->m[i].win);
    }
    pthread_mutex_destroy(&k->lock);
    pthread_cond_destroy(&k->cond);
    free(k->jobs);
    free(k->zeros);
    free(k);
}
