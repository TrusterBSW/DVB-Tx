/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "rf.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "dvbc.h"
#include "dvbs.h"
#include "dvbs2.h"
#include "dvbt.h"
#include "dvbt2.h"
#include "ofdm.h"
#include "fcbank.h"
#include "scgen.h"
#include "sdr.h"
#include "ts.h"
#include "util.h"

#ifndef PLUGIN_DIR
#define PLUGIN_DIR "/usr/lib/dvbtx"
#endif

#define MAX_RADIOS 16
#define MAX_CARRIERS 128
#define RING_SYMS 32
#define SYMBLK 4096               /* symbols per ring slot of a filter-bank member */
#define MAX_BANKS 64

typedef struct radio radio;

typedef struct carrier {
    int idx;
    char standard[16];
    mux *mux;
    radio *radio;
    double pkt_ns;
    double offset_hz;
    double gain_db;

    /* dvbt */
    dvbt_cfg tc;
    dvbt *mod;
    /* single carrier (dvbc) */
    void *src;                    /* dvbc / dvbs symbol source */
    sym_fn symf;
    scgen *sg;
    fcbank *fb;                   /* this pseudo-carrier runs a filter bank (members in other carriers) */
    int in_bank, held, sym_pos;   /* member of a filter bank: the ring holds symbols */
    void *symctx;
    char synth[8];                /* auto, fft, fir */
    double symbol_rate, rolloff;
    int bits, cr_k, cr_n;
    dvbs2_cfg s2;
    /* dvbt2: native-rate samples resampled like a single carrier */
    dvbt2_cfg t2c;
    dvbt2 *t2;
    float complex *t2buf;
    int t2n, t2pos;
    dvbt2_group *t2g;             /* this pseudo-carrier synthesises DVB-T2 carriers at k x their rate */
    int in_group;                 /* member of such a group (no thread of its own) */
    struct carrier *group;        /* that group (its pseudo-carrier) and the member number in it */
    int member;
    int ncar;                     /* elements per ring slot: OFDM carriers, or samples of a block */
    float complex *ring;
    uint64_t wr, rd;
    pthread_mutex_t lock;
    pthread_cond_t cond;

    /* ts output */
    int out_fd;
    struct sockaddr_in out_sa;
    int out_is_udp;
    int64_t t0;

    pthread_t th;
    int started;
    volatile int muted;           /* stopped from the web page: off the air, not coded nor synthesised */
    double owed;                  /* muted: packets its multiplex owes for the air time skipped */
    struct carrier *bank;         /* in_bank: the bank's pseudo-carrier and the member number in it */
    int bank_idx;
} carrier;

struct radio {
    int idx;
    char driver[32];
    const sdr_driver *drv;
    sdr_dev *dev;
    sdr_params p;
    char args[256], clock[32];
    double backoff_db;
    float scale;

    ofdm_group *grp;
    int nfft, ncp, symlen;
    carrier *mem[64];             /* DVB-T carriers sharing the OFDM synthesis */
    int nmem;
    carrier *sc[64];              /* single-carrier modulations added after it (and filter banks) */
    int nsc;
    int ncarriers;
    int blklen;                   /* samples per output block */

    _Atomic int64_t t0;           /* CLOCK_MONOTONIC ns at which sample 0 is (virtually) on air */
    int64_t win_start, win_min, track_start;
    int tracking;
    uint64_t written;
    _Atomic uint64_t underruns_seen;
    sdr_stats last;

    pthread_t th;
    int started;
    /* RADIO_OFF: not found at the start, RADIO_LOST: stopped since (unplugged, write error): its carriers
     * are off the air, the others go on */
    _Atomic int state;
    char err[160];
    uint64_t sample_base;         /* samples written before the device was last opened (RADIO_IDLE in between) */
};
/* RADIO_IDLE: all its carriers stopped, the device closed until one starts again */
enum { RADIO_ON, RADIO_OFF, RADIO_LOST, RADIO_IDLE };

static radio *g_radios[MAX_RADIOS];
static int g_nradios;
static carrier *g_carriers[MAX_CARRIERS];
static int g_ncarriers;
static carrier *g_banks[MAX_BANKS];   /* pseudo-carriers synthesising filter banks */
static int g_nbanks;
static volatile int g_running;

/* ---- plugins ---- */

const sdr_driver *sdr_load(const char *name, char *err, int errlen)
{
    char path[PATH_MAX];
    const char *dir = getenv("DVBTX_PLUGIN_DIR");
    void *h = NULL;
    if (dir) {
        snprintf(path, sizeof path, "%s/dvbtx-sdr-%s.so", dir, name);
        h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    }
    if (!h) {
        char exe[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) {
            exe[n] = 0;
            char *slash = strrchr(exe, '/');
            if (slash)
                *slash = 0;
            snprintf(path, sizeof path, "%.4000s/dvbtx-sdr-%.64s.so", exe, name);
            h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        }
    }
    if (!h) {
        snprintf(path, sizeof path, "%s/dvbtx-sdr-%s.so", PLUGIN_DIR, name);
        h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    }
    if (!h) {
        snprintf(err, errlen, "radio driver '%s': %s", name, dlerror());
        return NULL;
    }
    const sdr_driver *d = dlsym(h, "dvbtx_sdr_driver");
    if (!d || d->abi != SDR_ABI) {
        snprintf(err, errlen, "radio driver '%s': incompatible plugin", name);
        return NULL;
    }
    return d;
}

/* ---- clocks ---- */

static int64_t radio_clock(void *ctx, uint64_t slot)
{
    carrier *c = ctx;
    return atomic_load_explicit(&c->radio->t0, memory_order_relaxed) + (int64_t)((double)slot * c->pkt_ns);
}

static int64_t ts_clock(void *ctx, uint64_t slot)
{
    carrier *c = ctx;
    return c->t0 + (int64_t)((double)slot * c->pkt_ns);
}

static void radio_track(radio *r)
{
    sdr_stats st;
    r->drv->get_stats(r->dev, &st);
    r->last = st;
    if (!st.samples || !st.at_ns)
        return;
    int64_t raw = st.at_ns - (int64_t)((double)(st.samples + r->sample_base) * 1e9 / r->p.rate);
    if (!r->win_start) {
        r->win_start = st.at_ns;
        r->win_min = raw;
        r->track_start = st.at_ns;
    }
    if (raw < r->win_min)
        r->win_min = raw;
    if (st.at_ns - r->win_start < 1000000000LL)
        return;
    int64_t t0 = atomic_load(&r->t0);
    int64_t d = r->win_min - t0;
    if (!r->tracking || st.at_ns - r->track_start < 4000000000LL) {
        /* start-up: follow the measurement directly */
        if (r->tracking && llabs(d) > 2000000)
            LOGD("radio %d: clock step %.2f ms", r->idx, d / 1e6);
        t0 = r->win_min;
        r->tracking = 1;
    } else {
        int64_t lim = 50000;               /* 50 ppm per 1 s window */
        int64_t step = d / 8;
        if (step > lim)
            step = lim;
        if (step < -lim)
            step = -lim;
        t0 += step;
    }
    atomic_store(&r->t0, t0);
    r->win_start = st.at_ns;
    r->win_min = raw;
}

/* DVB-T2 samples at the elementary rate, one at a time for scgen (PULSE_LOWPASS) */
#define T2BUF 40000
static void t2_sample(void *ctx, float complex *v)
{
    carrier *c = ctx;
    if (c->t2pos == c->t2n) {
        c->t2n = dvbt2_samples(c->t2, c->t2buf, T2BUF);
        c->t2pos = 0;
    }
    *v = c->t2buf[c->t2pos++];
}

/* symbols of a filter-bank member, from its ring (called by the bank thread) */
static void bank_sym(void *ctx, float complex *v)
{
    carrier *c = ctx;
    if (c->sym_pos == SYMBLK) {
        pthread_mutex_lock(&c->lock);
        if (c->held) {
            c->rd++;
            pthread_cond_broadcast(&c->cond);
        }
        while (g_running && c->wr == c->rd)
            pthread_cond_wait(&c->cond, &c->lock);
        c->held = 1;
        pthread_mutex_unlock(&c->lock);
        c->sym_pos = 0;
    }
    *v = c->ring[(size_t)(c->rd % RING_SYMS) * SYMBLK + c->sym_pos++];
    if (c->muted)
        *v = 0;
}

/* ---- carrier threads ---- */

/* A muted carrier skips its coding and synthesis but still pulls (and drops) the packets its
 * multiplex would have sent during that air time, so that the mux clock (PCR, CC, inputs, backup
 * clips) goes on and the carrier comes back seamlessly. */
static void skip_air(carrier *c, float complex *slot, double seconds)
{
    uint8_t pkt[TS_PKT];
    memset(slot, 0, sizeof(float complex) * (size_t)c->ncar);
    c->owed += seconds * 1e9 / c->pkt_ns;
    while (c->owed >= 1) {
        mux_pull(c->mux, pkt);
        c->owed -= 1;
    }
}

static void rt_priority(int prio, const char *what)
{
    static int warned;
    struct sched_param sp = { .sched_priority = prio };
    int r = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (r && !warned) {
        warned = 1;
        LOGW("no real-time priority for %s (%s): give the service LimitRTPRIO or CAP_SYS_NICE", what, strerror(r));
    }
}

static void *dvbt_thread(void *arg)
{
    carrier *c = arg;
    rt_priority(40, "the carrier threads");
    if (!c->mod) {                 /* single carrier: blocks of radio samples, or of symbols for a bank */
        while (g_running) {
            pthread_mutex_lock(&c->lock);
            while (g_running && c->wr - c->rd >= RING_SYMS)
                pthread_cond_wait(&c->cond, &c->lock);
            pthread_mutex_unlock(&c->lock);
            if (!g_running)
                break;
            float complex *slot = c->ring + (size_t)(c->wr % RING_SYMS) * c->ncar;
            if (c->fb)
                fcbank_block(c->fb, slot, c->ncar);
            else if (c->t2g)
                for (int k = 0; k < c->ncar;)
                    k += dvbt2_group_samples(c->t2g, slot + k, c->ncar - k);
            else if (c->in_bank && c->muted)
                skip_air(c, slot, c->ncar / c->symbol_rate);
            else if (c->in_bank)
                for (int k = 0; k < c->ncar; k++)
                    c->symf(c->symctx, &slot[k]);
            else if (c->muted)
                skip_air(c, slot, c->ncar / c->radio->p.rate);
            else
                scgen_block(c->sg, slot, c->ncar);
            pthread_mutex_lock(&c->lock);
            c->wr++;
            pthread_cond_broadcast(&c->cond);
            pthread_mutex_unlock(&c->lock);
        }
        return NULL;
    }
    while (g_running) {
        pthread_mutex_lock(&c->lock);
        while (g_running && c->wr - c->rd >= RING_SYMS)
            pthread_cond_wait(&c->cond, &c->lock);
        pthread_mutex_unlock(&c->lock);
        if (!g_running)
            break;
        float complex *slot = c->ring + (size_t)(c->wr % RING_SYMS) * c->ncar;
        if (c->muted)
            skip_air(c, slot, c->radio->blklen / c->radio->p.rate);
        else
            dvbt_symbol(c->mod, slot);
        pthread_mutex_lock(&c->lock);
        c->wr++;
        pthread_cond_broadcast(&c->cond);
        pthread_mutex_unlock(&c->lock);
    }
    return NULL;
}

static void *ts_thread(void *arg)
{
    carrier *c = arg;
    uint8_t buf[7 * TS_PKT];
    uint64_t slot = 0;
    c->t0 = mono_ns() + 50000000;
    while (g_running) {
        int64_t due = c->t0 + (int64_t)((double)slot * c->pkt_ns) - 20000000;
        int64_t now = mono_ns();
        if (due > now) {
            struct timespec ts = { 0, (long)((due - now) > 2000000 ? 2000000 : (due - now)) };
            nanosleep(&ts, NULL);
            continue;
        }
        for (int i = 0; i < 7; i++)
            mux_pull(c->mux, buf + i * TS_PKT);
        slot += 7;
        if (c->muted)
            continue;
        if (c->out_is_udp)
            sendto(c->out_fd, buf, sizeof buf, 0, (struct sockaddr *)&c->out_sa, sizeof c->out_sa);
        else if (c->out_fd >= 0 && write(c->out_fd, buf, sizeof buf) < 0 && errno != EAGAIN)
            break;
    }
    return NULL;
}

/* ---- radio thread ---- */

static int radio_needed(radio *r)
{
    for (int i = 0; i < g_ncarriers; i++)
        if (g_carriers[i]->radio == r && !g_carriers[i]->muted)
            return 1;
    return 0;
}

/* all the carriers of r stopped: the device closed (nothing on the air, its carriers wait on their
 * rings) until one starts again, then opened again with the clock carried on from the samples written */
static void radio_idle(radio *r)
{
    char err[256];
    r->drv->close(r->dev);
    r->dev = NULL;
    atomic_store(&r->state, RADIO_IDLE);
    memset(&r->last, 0, sizeof r->last);
    LOGI("radio %d (%s): all its carriers stopped, radio off", r->idx, r->driver);
    while (g_running && !radio_needed(r)) {
        struct timespec ts = { 0, 100000000 };
        nanosleep(&ts, NULL);
    }
    if (!g_running)
        return;
    r->dev = r->drv->open(&r->p, err, sizeof err);
    if (!r->dev) {
        snprintf(r->err, sizeof r->err, "%.159s", err);
        atomic_store(&r->state, RADIO_LOST);
        LOGE("radio %d (%s): %s: its carriers are off the air", r->idx, r->driver, err);
        return;
    }
    r->sample_base = r->written;
    r->win_start = 0;
    r->tracking = 0;
    int64_t lead = (int64_t)r->p.buffer_ms * 1000000 + (int64_t)(RING_SYMS * r->blklen / r->p.rate * 1e9);
    atomic_store(&r->t0, mono_ns() + lead - (int64_t)((double)r->written * 1e9 / r->p.rate));
    atomic_store(&r->state, RADIO_ON);
    LOGI("radio %d (%s): a carrier started again, radio on", r->idx, r->driver);
}

static void *radio_thread(void *arg)
{
    radio *r = arg;
    rt_priority(50, "the radio threads");
    float complex *out = xcalloc(r->blklen, sizeof *out);
    int zn = 0;
    for (int i = 0; i < r->nmem; i++)
        zn = r->mem[i]->ncar > zn ? r->mem[i]->ncar : zn;
    float complex *zeros = xcalloc(zn ? zn : 1, sizeof *zeros);   /* the symbols of a stopped DVB-T carrier */
    const float complex *cars[64];
    const float complex *bufs[1] = { out };
    int64_t next_track = 0;
    while (g_running) {
        if (!radio_needed(r)) {
            radio_idle(r);
            if (!g_running || atomic_load(&r->state) != RADIO_ON)
                break;
            next_track = 0;
        }
        carrier *all[128];
        int nall = 0;
        for (int i = 0; i < r->nmem; i++)
            all[nall++] = r->mem[i];
        for (int i = 0; i < r->nsc; i++)
            all[nall++] = r->sc[i];
        for (int i = 0; i < nall; i++) {
            carrier *c = all[i];
            pthread_mutex_lock(&c->lock);
            while (g_running && c->wr == c->rd)
                pthread_cond_wait(&c->cond, &c->lock);
            pthread_mutex_unlock(&c->lock);
        }
        if (!g_running)
            break;
        if (r->nmem) {
            for (int i = 0; i < r->nmem; i++)
                cars[i] = r->mem[i]->muted ? zeros
                                           : r->mem[i]->ring + (size_t)(r->mem[i]->rd % RING_SYMS) * r->mem[i]->ncar;
            ofdm_group_synth(r->grp, cars, out);
        } else {
            memset(out, 0, sizeof(float complex) * r->blklen);
        }
        for (int i = 0; i < r->nsc; i++) {
            if (r->sc[i]->muted)
                continue;
            const float complex *b = r->sc[i]->ring + (size_t)(r->sc[i]->rd % RING_SYMS) * r->sc[i]->ncar;
            for (int k = 0; k < r->blklen; k++)
                out[k] += b[k];
        }
        for (int i = 0; i < nall; i++) {
            carrier *c = all[i];
            pthread_mutex_lock(&c->lock);
            c->rd++;
            pthread_cond_broadcast(&c->cond);
            pthread_mutex_unlock(&c->lock);
        }
        if (r->drv->write(r->dev, bufs, r->blklen, r->scale)) {
            if (g_running)
                rf_radio_lost(r->idx, "write failed (unplugged?)");
            break;
        }
        r->written += (uint64_t)r->blklen;
        int64_t now = mono_ns();
        if (now >= next_track) {
            radio_track(r);
            next_track = now + 50000000;
        }
    }
    free(out);
    free(zeros);
    return NULL;
}

/* ---- setup ---- */

static int parse_code_rate(const char *s, int *k, int *n)
{
    return sscanf(s, "%d/%d", k, n) == 2 ? 0 : -1;
}

static int open_output(carrier *c, const char *out, char *err, int errlen)
{
    c->out_fd = -1;
    if (!strncmp(out, "udp://", 6)) {
        const char *a = out + 6, *colon = strrchr(a, ':');
        char host[128];
        if (!colon)
            goto bad;
        snprintf(host, sizeof host, "%.*s", (int)(colon - a), a);
        c->out_sa.sin_family = AF_INET;
        c->out_sa.sin_port = htons((uint16_t)atoi(colon + 1));
        if (inet_pton(AF_INET, host, &c->out_sa.sin_addr) != 1)
            goto bad;
        c->out_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        int ttl = 4, snd = 8 << 20;
        setsockopt(c->out_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
        setsockopt(c->out_fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof snd);
        c->out_is_udp = 1;
        return 0;
    }
    const char *path = !strncmp(out, "file://", 7) ? out + 7 : out;
    c->out_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);   /* a FIFO ignores O_TRUNC */
    if (c->out_fd < 0) {
        snprintf(err, errlen, "carrier %d: cannot open %s: %s", c->idx, path, strerror(errno));
        return -1;
    }
    return 0;
bad:
    snprintf(err, errlen, "carrier %d: bad output '%s'", c->idx, out);
    return -1;
}

static carrier *pseudo_carrier(radio *r, const char *what)
{
    carrier *bc = xcalloc(1, sizeof *bc);
    bc->idx = -1;
    bc->radio = r;
    snprintf(bc->standard, sizeof bc->standard, "%s", what);
    pthread_mutex_init(&bc->lock, NULL);
    pthread_cond_init(&bc->cond, NULL);
    bc->ncar = r->blklen;
    bc->ring = xcalloc((size_t)RING_SYMS * bc->ncar, sizeof(float complex));
    return bc;
}

/* DVB-T2 carriers of a radio whose rate is k times theirs: synthesised together, one IFFT of k x FFT points
 * per OFDM symbol (no resampling); the others go to the filter banks */
static int t2_groups(radio *r, carrier **sc, int nsc, double wtot, char *err, int errlen)
{
    int n = 0;
    for (int i = 0; i < nsc; i++)
        n += !strcmp(sc[i]->standard, "dvbt2") && strcmp(sc[i]->synth, "fir");
    carrier *grp[8];
    dvbt2_timing_t tim[8];
    int ng = 0, nmem[8] = { 0 };
    for (int i = 0; i < nsc; i++) {
        carrier *c = sc[i];
        dvbt2_timing_t t;
        if (strcmp(c->standard, "dvbt2") || !strcmp(c->synth, "fir") || dvbt2_timing(&c->t2c, &t))
            continue;
        double kf = r->p.rate / t.sample_rate, bf = c->offset_hz / (r->p.rate / ((double)t.fft * lround(kf)));
        int k = (int)lround(kf), bins = (int)lround(bf);
        if (fabs(kf - k) > 1e-9 * kf || fabs(bf - bins) > 1e-4) {
            LOGI("carrier %d: radio rate %.3f not a multiple of %.3f or offset %.1f off the subcarrier grid: "
                 "DVB-T2 resampled", c->idx, r->p.rate, t.sample_rate, c->offset_hz);
            continue;
        }
        int g = 0;
        while (g < ng && !dvbt2_timing_equal(&tim[g], &t))
            g++;
        if (g == ng) {
            if (ng == 8 || g_nbanks >= MAX_BANKS)
                continue;
            dvbt2_group *t2g = dvbt2_group_new(k, n > 1 ? (n < 4 ? n : 4) : 0);
            if (!t2g)
                continue;
            grp[ng] = pseudo_carrier(r, "t2group");
            grp[ng]->t2g = t2g;
            tim[ng++] = t;
        }
        float gain = (float)sqrt(pow(10.0, c->gain_db / 10.0) / wtot);
        int rc = dvbt2_group_add(grp[g]->t2g, &c->t2c, bins, gain, mux_pull, c->mux);
        if (rc < 0) {
            LOGI("carrier %d: DVB-T2 group: %s, resampled instead", c->idx, dvbt2_group_error(rc));
            continue;
        }
        LOGD("radio %d: DVB-T2 group %d member %d: carrier %d at %d bins (FFT %d x %d)", r->idx, g, rc, c->idx, bins,
             t.fft, k);
        c->in_group = 1;
        c->group = grp[g];
        c->member = rc;
        nmem[g]++;
        r->ncarriers++;
    }
    for (int g = 0; g < ng; g++) {
        if (!nmem[g]) {
            dvbt2_group_free(grp[g]->t2g);
            continue;
        }
        if (g_nbanks >= MAX_BANKS) {
            snprintf(err, errlen, "radio %d: too many synthesis groups", r->idx);
            return -1;
        }
        g_banks[g_nbanks++] = grp[g];
        r->sc[r->nsc++] = grp[g];
    }
    return 0;
}

/* a radio's parameters from its [radio N] section */
static void radio_params(radio *r, const conf_sec *s)
{
    r->idx = s->index;
    snprintf(r->driver, sizeof r->driver, "%s", conf_str(s, "driver", "hackrf"));
    snprintf(r->args, sizeof r->args, "%s", conf_str(s, "args", ""));
    snprintf(r->clock, sizeof r->clock, "%s", conf_str(s, "clock", "internal"));
    r->p.freq_hz = conf_num(s, "freq", 0);
    r->p.rate = conf_num(s, "rate", 0);
    r->p.bandwidth = conf_num(s, "bandwidth", 0);
    r->p.gain = conf_num(s, "gain", 0);
    r->p.amp = (int)conf_int(s, "amp", 0);
    r->p.ppm = conf_num(s, "ppm", 0);
    r->p.channels = 1;
    r->p.args = r->args;
    r->p.clock = r->clock;
    r->p.buffer_ms = (int)conf_int(s, "buffer_ms", 50);
    r->backoff_db = conf_num(s, "backoff_db", 13);
}

int rf_setup(const conf *cf, input **inputs, int ninputs, char *err, int errlen)
{
    /* radios */
    for (int ri = 0; ri < cf->nsec; ri++) {
        const conf_sec *s = &cf->sec[ri];
        if (strcmp(s->kind, "radio"))
            continue;
        if (g_nradios >= MAX_RADIOS || s->index != g_nradios) {
            snprintf(err, errlen, "radios must be numbered 0..%d in order", MAX_RADIOS - 1);
            return -1;
        }
        radio *r = xcalloc(1, sizeof *r);
        radio_params(r, s);
        if (r->p.freq_hz <= 0 || r->p.rate <= 0) {
            snprintf(err, errlen, "radio %d: freq and rate are required", r->idx);
            return -1;
        }
        g_radios[g_nradios++] = r;
    }

    /* carriers */
    for (int ci = 0; ci < cf->nsec; ci++) {
        const conf_sec *s = &cf->sec[ci];
        if (strcmp(s->kind, "carrier"))
            continue;
        if (g_ncarriers >= MAX_CARRIERS || s->index != g_ncarriers) {
            snprintf(err, errlen, "carriers must be numbered 0..n in order");
            return -1;
        }
        carrier *c = xcalloc(1, sizeof *c);
        c->idx = s->index;
        pthread_mutex_init(&c->lock, NULL);
        pthread_cond_init(&c->cond, NULL);
        snprintf(c->standard, sizeof c->standard, "%s", conf_str(s, "standard", "dvbt"));
        snprintf(c->synth, sizeof c->synth, "%s", conf_str(s, "synth", "auto"));
        c->offset_hz = conf_num(s, "offset", 0);
        c->gain_db = conf_num(s, "gain_db", 0);
        c->muted = !conf_int(s, "on", 1);
        double ts_rate;
        mux_clock_fn clk;
        if (!strcmp(c->standard, "dvbt")) {
            dvbt_cfg tc = { 0 };
            const char *mode = conf_str(s, "mode", "8k");
            tc.fft = !strcmp(mode, "2k") ? 2048 : !strcmp(mode, "8k") ? 8192 : 0;
            if (!tc.fft) {
                snprintf(err, errlen, "carrier %d: DVB-T mode must be 2k or 8k", c->idx);
                return -1;
            }
            const char *cons = conf_str(s, "constellation", "64qam");
            tc.bits = !strcmp(cons, "qpsk") ? 2 : !strcmp(cons, "16qam") ? 4 : !strcmp(cons, "64qam") ? 6 : 0;
            if (parse_code_rate(conf_str(s, "code_rate", "7/8"), &tc.cr_k, &tc.cr_n)) {
                snprintf(err, errlen, "carrier %d: bad code_rate", c->idx);
                return -1;
            }
            tc.gi = (int)conf_int(s, "guard", 32);
            tc.bw_hz = conf_num(s, "bandwidth", 8e6);
            tc.cell_id = (int)conf_int(s, "cell_id", 0);
            const char *e = dvbt_cfg_check(&tc);
            if (e) {
                snprintf(err, errlen, "carrier %d: %s", c->idx, e);
                return -1;
            }
            c->tc = tc;
            ts_rate = dvbt_ts_rate(&tc);
            int ri = (int)conf_int(s, "radio", 0);
            if (ri < 0 || ri >= g_nradios) {
                snprintf(err, errlen, "carrier %d: radio %d does not exist", c->idx, ri);
                return -1;
            }
            c->radio = g_radios[ri];
            clk = radio_clock;
        } else if (!strcmp(c->standard, "dvbc")) {
            const char *cons = conf_str(s, "constellation", "256qam");
            c->bits = !strcmp(cons, "16qam") ? 4 : !strcmp(cons, "32qam") ? 5 : !strcmp(cons, "64qam") ? 6 :
                      !strcmp(cons, "128qam") ? 7 : !strcmp(cons, "256qam") ? 8 : 0;
            if (!c->bits) {
                snprintf(err, errlen, "carrier %d: DVB-C constellation must be 16qam, 32qam, 64qam, 128qam or 256qam", c->idx);
                return -1;
            }
            c->symbol_rate = conf_num(s, "symbol_rate", 6.9e6);
            c->rolloff = conf_num(s, "rolloff", 0.15);
            ts_rate = dvbc_ts_rate(c->symbol_rate, c->bits);
            int ri = (int)conf_int(s, "radio", 0);
            if (ri < 0 || ri >= g_nradios) {
                snprintf(err, errlen, "carrier %d: radio %d does not exist", c->idx, ri);
                return -1;
            }
            c->radio = g_radios[ri];
            clk = radio_clock;
        } else if (!strcmp(c->standard, "dvbs")) {
            if (parse_code_rate(conf_str(s, "code_rate", "3/4"), &c->cr_k, &c->cr_n)) {
                snprintf(err, errlen, "carrier %d: bad code_rate", c->idx);
                return -1;
            }
            c->symbol_rate = conf_num(s, "symbol_rate", 27.5e6);
            c->rolloff = conf_num(s, "rolloff", 0.35);
            ts_rate = dvbs_ts_rate(c->symbol_rate, c->cr_k, c->cr_n);
            int ri = (int)conf_int(s, "radio", 0);
            if (ri < 0 || ri >= g_nradios) {
                snprintf(err, errlen, "carrier %d: radio %d does not exist", c->idx, ri);
                return -1;
            }
            c->radio = g_radios[ri];
            clk = radio_clock;
        } else if (!strcmp(c->standard, "dvbs2")) {
            const char *mod = conf_str(s, "modulation", "8psk");
            c->s2.bits = !strcmp(mod, "qpsk") ? 2 : !strcmp(mod, "8psk") ? 3 : !strcmp(mod, "16apsk") ? 4 :
                         !strcmp(mod, "32apsk") ? 5 : 0;
            if (parse_code_rate(conf_str(s, "code_rate", "3/4"), &c->s2.cr_num, &c->s2.cr_den)) {
                snprintf(err, errlen, "carrier %d: bad code_rate", c->idx);
                return -1;
            }
            c->s2.short_frame = !strcmp(conf_str(s, "frame", "normal"), "short");
            c->s2.pilots = (int)conf_int(s, "pilots", 1);
            c->rolloff = c->s2.rolloff = conf_num(s, "rolloff", 0.35);
            c->s2.gold_code = (int)conf_int(s, "gold_code", 0);
            const char *e = dvbs2_cfg_check(&c->s2);
            if (e) {
                snprintf(err, errlen, "carrier %d: %s", c->idx, e);
                return -1;
            }
            c->symbol_rate = conf_num(s, "symbol_rate", 27.5e6);
            ts_rate = dvbs2_ts_rate(&c->s2, c->symbol_rate);
            int ri = (int)conf_int(s, "radio", 0);
            if (ri < 0 || ri >= g_nradios) {
                snprintf(err, errlen, "carrier %d: radio %d does not exist", c->idx, ri);
                return -1;
            }
            c->radio = g_radios[ri];
            clk = radio_clock;
        } else if (!strcmp(c->standard, "dvbt2")) {
            dvbt2_cfg t = { 0 };
            t.bw_hz = conf_num(s, "bandwidth", 8e6);
            const char *f = conf_str(s, "fft", "32k");
            t.fft = !strcmp(f, "1k") ? 1024 : !strcmp(f, "2k") ? 2048 : !strcmp(f, "4k") ? 4096 : !strcmp(f, "8k") ? 8192 :
                    !strcmp(f, "16k") ? 16384 : !strcmp(f, "32k") ? 32768 : 0;
            if (!t.fft) {
                snprintf(err, errlen, "carrier %d: DVB-T2 fft must be 1k, 2k, 4k, 8k, 16k or 32k", c->idx);
                return -1;
            }
            t.extended = (int)conf_int(s, "extended", 1);
            if (sscanf(conf_str(s, "guard", "1/128"), "%d/%d", &t.gi_num, &t.gi_den) != 2) {
                snprintf(err, errlen, "carrier %d: bad guard", c->idx);
                return -1;
            }
            const char *pp = conf_str(s, "pilot", "PP7");
            t.pilot = atoi(pp + ((pp[0] == 'P' || pp[0] == 'p') ? 2 : 0));
            const char *cons = conf_str(s, "constellation", "256qam");
            t.bits = !strcmp(cons, "qpsk") ? 2 : !strcmp(cons, "16qam") ? 4 : !strcmp(cons, "64qam") ? 6 :
                     !strcmp(cons, "256qam") ? 8 : 0;
            if (!t.bits) {
                snprintf(err, errlen, "carrier %d: DVB-T2 constellation must be qpsk, 16qam, 64qam or 256qam", c->idx);
                return -1;
            }
            t.rotation = (int)conf_int(s, "rotation", 1);
            if (parse_code_rate(conf_str(s, "code_rate", "2/3"), &t.cr_num, &t.cr_den)) {
                snprintf(err, errlen, "carrier %d: bad code_rate", c->idx);
                return -1;
            }
            t.short_frame = !strcmp(conf_str(s, "frame", "normal"), "short");
            t.fec_blocks = (int)conf_int(s, "fec_blocks", 0);
            t.data_symbols = (int)conf_int(s, "data_symbols", 0);
            t.ti_blocks = (int)conf_int(s, "ti_blocks", 0);
            const char *l1 = conf_str(s, "l1_constellation", "64qam");
            t.l1_bits = !strcmp(l1, "bpsk") ? 1 : !strcmp(l1, "qpsk") ? 2 : !strcmp(l1, "16qam") ? 4 :
                        !strcmp(l1, "64qam") ? 6 : 0;
            if (!t.l1_bits) {
                snprintf(err, errlen, "carrier %d: DVB-T2 l1_constellation must be bpsk, qpsk, 16qam or 64qam", c->idx);
                return -1;
            }
            t.cell_id = (int)conf_int(s, "cell_id", 0);
            t.network_id = (int)conf_int(s, "network_id", 0x3085);
            t.t2_system_id = (int)conf_int(s, "t2_system_id", 0x8001);
            t.t2_frames = (int)conf_int(s, "t2_frames", 0);
            t.frequency = (uint32_t)conf_num(s, "frequency", 0);
            const char *e = dvbt2_cfg_check(&t);
            if (e) {
                snprintf(err, errlen, "carrier %d: %s", c->idx, e);
                return -1;
            }
            c->t2c = t;
            ts_rate = dvbt2_ts_rate(&t);
            c->symbol_rate = dvbt2_sample_rate(&t);
            c->rolloff = dvbt2_occupied_bw(&t) / c->symbol_rate;     /* occupied fraction */
            int ri = (int)conf_int(s, "radio", 0);
            if (ri < 0 || ri >= g_nradios) {
                snprintf(err, errlen, "carrier %d: radio %d does not exist", c->idx, ri);
                return -1;
            }
            c->radio = g_radios[ri];
            clk = radio_clock;
        } else if (!strcmp(c->standard, "ts")) {
            ts_rate = conf_num(s, "ts_rate", 0);
            if (ts_rate <= 0) {
                snprintf(err, errlen, "carrier %d: ts_rate required for standard ts", c->idx);
                return -1;
            }
            if (open_output(c, conf_req(s, "output"), err, errlen))
                return -1;
            clk = ts_clock;
        } else {
            snprintf(err, errlen, "carrier %d: standard '%s' is not handled by this engine", c->idx, c->standard);
            return -1;
        }
        c->pkt_ns = TS_PKT * 8 * 1e9 / ts_rate;

        mux_cfg mc = { 0 };
        mc.carrier = c->idx;
        mc.tsid = (int)conf_int(s, "tsid", c->idx + 1);
        mc.ts_rate = ts_rate;
        mc.pat_period_ms = (int)conf_int(s, "pat_period_ms", 100);
        mc.tdt_period_ms = (int)conf_int(s, "tdt_period_ms", 1000);
        mc.tot_period_ms = (int)conf_int(s, "tot_period_ms", 5000);
        mc.nit_pid = (int)conf_int(s, "nit_pid", 16);
        snprintf(mc.tot_desc_path, sizeof mc.tot_desc_path, "%s", conf_str(s, "tot_desc", ""));
        snprintf(mc.monitor, sizeof mc.monitor, "%s", conf_str(s, "monitor", ""));
        c->mux = mux_new(&mc, clk, c);
        for (int t = 0;; t++) {
            const char *v = conf_nth(s, "table", t);
            if (!v)
                break;
            mux_table_cfg tc;
            char path[512];
            if (sscanf(v, "%i,%i,%511[^\n]", &tc.pid, &tc.cycle_ms, path) != 3) {
                snprintf(err, errlen, "carrier %d: bad table '%s' (pid,cycle_ms,path)", c->idx, v);
                return -1;
            }
            snprintf(tc.path, sizeof tc.path, "%s", path);
            mux_add_table(c->mux, &tc);
        }
        g_carriers[g_ncarriers++] = c;
    }

    /* services */
    for (int si = 0; si < cf->nsec; si++) {
        const conf_sec *s = &cf->sec[si];
        if (strcmp(s->kind, "service"))
            continue;
        int ci = (int)conf_int(s, "carrier", 0);
        if (ci < 0 || ci >= g_ncarriers) {
            snprintf(err, errlen, "service %d: carrier %d does not exist", s->index, ci);
            return -1;
        }
        mux_svc_cfg sc = { 0 };
        snprintf(sc.name, sizeof sc.name, "%s", conf_str(s, "name", "service"));
        sc.sid = (int)conf_int(s, "sid", 0);
        sc.pid_base = (int)conf_int(s, "pid_base", 0x100 + s->index * 0x20);
        int pi = (int)conf_int(s, "primary", -1), bi = (int)conf_int(s, "backup", -1);
        if (pi < 0 || pi >= ninputs || bi >= ninputs || !inputs[pi] || (bi >= 0 && !inputs[bi])) {
            snprintf(err, errlen, "service %d: bad input index", s->index);
            return -1;
        }
        sc.primary = inputs[pi];
        sc.backup = bi >= 0 ? inputs[bi] : NULL;
        sc.switch_timeout_ms = (int)conf_int(s, "switch_timeout_ms", 3000);
        sc.return_ms = (int)conf_int(s, "return_ms", 10000);
        sc.max_late_ms = (int)conf_int(s, "max_late_ms", 600);
        sc.early_ms = (int)conf_int(s, "early_ms", 100);      /* the supervisor's default */
        sc.pcr_max_ms = (int)conf_int(s, "pcr_max_ms", 35);
        sc.pmt_period_ms = (int)conf_int(s, "pmt_period_ms", 100);
        sc.priority = (int)conf_int(s, "priority", 5);
        if (sc.priority < 1 || sc.priority > 10)
            sc.priority = 5;
        if (mux_add_service(g_carriers[ci]->mux, &sc) < 0) {
            snprintf(err, errlen, "service %d: too many services on carrier %d", s->index, ci);
            return -1;
        }
    }
    for (int i = 0; i < g_ncarriers; i++)
        mux_start(g_carriers[i]->mux);

    /* per radio: one OFDM synthesis for its DVB-T carriers (same mode, guard interval and bandwidth),
     * plus its single-carrier modulations; each carrier gets its share of the power */
    for (int ri = 0; ri < g_nradios; ri++) {
        radio *r = g_radios[ri];
        dvbt_cfg *ref = NULL;
        double wtot = 0;
        for (int i = 0; i < g_ncarriers; i++) {
            carrier *c = g_carriers[i];
            if (c->radio != r)
                continue;
            if (r->nmem + r->nsc >= 64) {
                snprintf(err, errlen, "radio %d: too many carriers", ri);
                return -1;
            }
            wtot += pow(10.0, c->gain_db / 10.0);
            if (!strcmp(c->standard, "dvbt")) {
                if (!ref)
                    ref = &c->tc;
                else if (c->tc.fft != ref->fft || c->tc.gi != ref->gi || c->tc.bw_hz != ref->bw_hz) {
                    snprintf(err, errlen, "radio %d: DVB-T carriers %d and %d differ in mode/guard/bandwidth; "
                             "one radio needs identical OFDM parameters", ri, r->mem[0]->idx, c->idx);
                    return -1;
                }
                r->mem[r->nmem++] = c;
            } else {
                r->sc[r->nsc++] = c;
            }
        }
        if (!r->nmem && !r->nsc) {
            snprintf(err, errlen, "radio %d has no carrier", ri);
            return -1;
        }
        if (r->nmem) {
            double spacing = dvbt_sample_rate(ref) / ref->fft;
            double nf = r->p.rate / spacing;
            r->nfft = (int)lround(nf);
            if (fabs(nf - r->nfft) > 1e-6 * nf) {
                snprintf(err, errlen, "radio %d: rate %.3f is not a multiple of the subcarrier spacing %.6f Hz "
                         "(use for instance %.3f)", ri, r->p.rate, spacing, ceil(nf / ref->fft) * ref->fft * spacing);
                return -1;
            }
            r->ncp = r->nfft / ref->gi;
            r->symlen = r->nfft + r->ncp;
            r->blklen = r->symlen;
            r->grp = ofdm_group_new(r->nfft, r->ncp, 1);
            for (int i = 0; i < r->nmem; i++) {
                carrier *c = r->mem[i];
                double b = c->offset_hz / spacing;
                int bins = (int)lround(b);
                if (fabs(b - bins) > 1e-3) {
                    snprintf(err, errlen, "carrier %d: offset %.1f Hz is not a multiple of %.6f Hz", c->idx,
                             c->offset_hz, spacing);
                    return -1;
                }
                double w = pow(10.0, c->gain_db / 10.0) / wtot;
                float g = (float)sqrt(w / dvbt_mean_power(&c->tc));
                int rr = ofdm_group_add(r->grp, dvbt_ncarriers(&c->tc), bins, g);
                if (rr < 0) {
                    snprintf(err, errlen, "carrier %d: %s", c->idx,
                             rr == -2 ? "overlaps another carrier" : "outside the radio bandwidth");
                    return -1;
                }
                c->ncar = dvbt_ncarriers(&c->tc);
                c->ring = xcalloc((size_t)RING_SYMS * c->ncar, sizeof(float complex));
                c->mod = dvbt_new(&c->tc, mux_pull, c->mux);
            }
        } else {
            r->blklen = 16384;
        }
        /* single carriers: frequency-domain filter banks at the radio rate when the symbol rate is a
         * simple fraction of it (exact shaping, one IFFT for several carriers), else scgen */
        long df = 0;                       /* gcd of the offsets (Hz): exact bins when possible */
        for (int i = 0; i < r->nsc; i++) {
            long o = labs(lround(r->sc[i]->offset_hz)), a = df;
            while (o) {
                long t = a % o;
                a = o;
                o = t;
            }
            df = a;
        }
        carrier *banks[64], *scl[64];
        int nbanks = 0, nsc = r->nsc;
        memcpy(scl, r->sc, sizeof(carrier *) * nsc);
        r->nsc = 0;
        r->ncarriers = r->nmem;
        if (t2_groups(r, scl, nsc, wtot, err, errlen))
            return -1;
        for (int i = 0; i < nsc; i++) {
            carrier *c = scl[i];
            if (c->in_group)
                continue;
            int is_t2 = !strcmp(c->standard, "dvbt2");
            double half = is_t2 ? c->symbol_rate * c->rolloff / 2 : c->symbol_rate * (1 + c->rolloff) / 2;
            if (fabs(c->offset_hz) + half > r->p.rate / 2) {
                snprintf(err, errlen, "carrier %d: outside the radio bandwidth", c->idx);
                return -1;
            }
            float g = (float)sqrt(pow(10.0, c->gain_db / 10.0) / wtot);
            if (is_t2) {
                c->t2 = dvbt2_new(&c->t2c, mux_pull, c->mux);
                c->t2buf = xcalloc(T2BUF, sizeof(float complex));
                c->src = c->t2;
                c->symf = t2_sample;
            } else if (!strcmp(c->standard, "dvbs2")) {
                c->src = dvbs2_new(&c->s2, mux_pull, c->mux);
                c->symf = dvbs2_symbol;
            } else if (!strcmp(c->standard, "dvbs")) {
                c->src = dvbs_new(c->cr_k, c->cr_n, mux_pull, c->mux);
                c->symf = dvbs_symbol;
            } else {
                c->src = dvbc_new(c->bits, mux_pull, c->mux);
                c->symf = dvbc_symbol;
            }
            if (!c->src) {
                snprintf(err, errlen, "carrier %d: invalid modulation parameters", c->idx);
                return -1;
            }
            c->symctx = is_t2 ? (void *)c : c->src;
            r->ncarriers++;
            int pulse = is_t2 ? FC_FLAT : FC_RRC;
            if (strcmp(c->synth, "fir")) {
                int ok = -1;
                for (int b = 0; b < nbanks && ok < 0; b++)
                    if ((ok = fcbank_add(banks[b]->fb, c->symbol_rate, pulse, c->rolloff, c->offset_hz, g, bank_sym,
                                         c)) >= 0)
                        c->bank = banks[b];
                if (ok < 0 && nbanks < 64 && g_nbanks < MAX_BANKS) {
                    fcbank *fb = fcbank_new(r->p.rate, c->symbol_rate, (double)df);
                    if (fb && (ok = fcbank_add(fb, c->symbol_rate, pulse, c->rolloff, c->offset_hz, g, bank_sym,
                                               c)) >= 0) {
                        carrier *bc = pseudo_carrier(r, "bank");
                        bc->fb = fb;
                        banks[nbanks++] = bc;
                        g_banks[g_nbanks++] = bc;
                        r->sc[r->nsc++] = bc;
                        c->bank = bc;
                    } else {
                        fcbank_free(fb);
                    }
                }
                if (ok >= 0) {
                    c->in_bank = 1;
                    c->bank_idx = ok;
                    c->sym_pos = SYMBLK;
                    c->ncar = SYMBLK;
                    c->ring = xcalloc((size_t)RING_SYMS * c->ncar, sizeof(float complex));
                    continue;
                }
                if (!strcmp(c->synth, "fft")) {
                    snprintf(err, errlen, "carrier %d: symbol rate %.3f is not a simple fraction of the radio rate "
                             "%.3f (synth = fft)", c->idx, c->symbol_rate, r->p.rate);
                    return -1;
                }
                LOGI("carrier %d: no frequency-domain synthesis (rate %.3f / %.3f, offset %.0f), time-domain filter",
                     c->idx, c->symbol_rate, r->p.rate, c->offset_hz);
            }
            if (is_t2)
                c->sg = scgen_new2(c->symbol_rate, r->p.rate, c->offset_hz, PULSE_LOWPASS, c->rolloff, 32, g, t2_sample, c);
            else
                c->sg = scgen_new(c->symbol_rate, r->p.rate, c->offset_hz, c->rolloff, 16, g, c->symf, c->src);
            c->ncar = r->blklen;
            c->ring = xcalloc((size_t)RING_SYMS * c->ncar, sizeof(float complex));
            r->sc[r->nsc++] = c;
        }
        for (int b = 0; b < nbanks; b++)
            for (int i = 0; i < fcbank_members(banks[b]->fb); i++) {
                int N, H, L, h;
                fcbank_sizes(banks[b]->fb, i, &N, &H, &L, &h);
                LOGD("radio %d: filter bank %d (FFT %d, %d new samples) member %d at %.3f Hz (FFT %d, %d new symbols)",
                     ri, b, N, H, i, fcbank_offset(banks[b]->fb, i), L, h);
            }
        r->scale = (float)pow(10.0, -r->backoff_db / 20.0);
        r->drv = sdr_load(r->driver, err, errlen);
        if (!r->drv)
            return -1;
    }
    return 0;
}

int rf_start(void)
{
    char err[256];
    g_running = 1;
    for (int ri = 0; ri < g_nradios; ri++) {
        radio *r = g_radios[ri];
        r->dev = r->drv->open(&r->p, err, sizeof err);
        if (!r->dev) {
            LOGE("radio %d (%s): %s: its carriers are off the air", ri, r->driver, err);
            snprintf(r->err, sizeof r->err, "%.159s", err);
            atomic_store(&r->state, RADIO_OFF);
            continue;
        }
        /* before the radio reports anything: on air after its buffer and our symbol ring */
        int64_t lead = (int64_t)r->p.buffer_ms * 1000000 + (int64_t)(RING_SYMS * r->blklen / r->p.rate * 1e9);
        atomic_store(&r->t0, mono_ns() + lead);
        LOGI("radio %d: %s at %.3f MHz, %.6f MS/s, %d carrier(s), FFT %d", ri, r->driver, r->p.freq_hz / 1e6,
             r->p.rate / 1e6, r->ncarriers, r->nfft);
    }
    for (int i = 0; i < g_ncarriers; i++)        /* stopped in the configuration (on = 0) */
        if (g_carriers[i]->muted)
            rf_carrier_on(i, 0);
    for (int i = 0; i < g_ncarriers; i++) {
        carrier *c = g_carriers[i];
        if (c->in_group || (c->radio && atomic_load(&c->radio->state) != RADIO_ON))
            continue;
        void *(*fn)(void *) = c->radio ? dvbt_thread : ts_thread;
        if (pthread_create(&c->th, NULL, fn, c))
            return -1;
        c->started = 1;
    }
    for (int i = 0; i < g_nbanks; i++) {
        if (atomic_load(&g_banks[i]->radio->state) != RADIO_ON)
            continue;
        if (pthread_create(&g_banks[i]->th, NULL, dvbt_thread, g_banks[i]))
            return -1;
        g_banks[i]->started = 1;
    }
    for (int ri = 0; ri < g_nradios; ri++) {
        radio *r = g_radios[ri];
        if (atomic_load(&r->state) != RADIO_ON)
            continue;
        if (pthread_create(&r->th, NULL, radio_thread, r))
            return -1;
        r->started = 1;
    }
    return 0;
}

/* carrier ci off the air (on = 0) or back (1); its radio and the other carriers go on */
int rf_carrier_on(int ci, int on)
{
    if (ci < 0 || ci >= g_ncarriers)
        return -1;
    carrier *c = g_carriers[ci];
    c->muted = !on;
    if (c->in_group && c->group && c->group->t2g)
        dvbt2_group_mute(c->group->t2g, c->member, !on);
    if (c->in_bank && c->bank && c->bank->fb)
        fcbank_mute(c->bank->fb, c->bank_idx, !on);
    LOGI("carrier %d: %s", ci, on ? "on the air" : "off the air");
    return 0;
}

void rf_radio_lost(int ri, const char *why)
{
    radio *r = g_radios[ri];
    int on = RADIO_ON;
    if (!atomic_compare_exchange_strong(&r->state, &on, RADIO_LOST))
        return;
    snprintf(r->err, sizeof r->err, "%s", why);
    LOGE("radio %d (%s): %s: its carriers are off the air", ri, r->driver, why);
}

int rf_radios_lost(void)
{
    for (int ri = 0; ri < g_nradios; ri++)
        if (atomic_load(&g_radios[ri]->state) == RADIO_LOST)
            return 1;
    return 0;
}

void rf_stop(void)
{
    g_running = 0;
    for (int i = 0; i < g_ncarriers + g_nbanks; i++) {
        carrier *c = i < g_ncarriers ? g_carriers[i] : g_banks[i - g_ncarriers];
        pthread_mutex_lock(&c->lock);
        pthread_cond_broadcast(&c->cond);
        pthread_mutex_unlock(&c->lock);
    }
    /* the radio threads finish their current write (the radio keeps consuming), then the radios close */
    for (int ri = 0; ri < g_nradios; ri++)       /* a lost radio's thread may be stuck in its driver: */
        if (g_radios[ri]->started && atomic_load(&g_radios[ri]->state) != RADIO_LOST)   /* left as it is */
            pthread_join(g_radios[ri]->th, NULL);
    for (int ri = 0; ri < g_nradios; ri++) {
        radio *r = g_radios[ri];
        if (r->dev && atomic_load(&r->state) != RADIO_LOST && atomic_load(&r->state) != RADIO_IDLE)
            r->drv->close(r->dev);
        r->dev = NULL;
    }
    for (int i = 0; i < g_ncarriers; i++)
        if (g_carriers[i]->started)
            pthread_join(g_carriers[i]->th, NULL);
    for (int i = 0; i < g_nbanks; i++)
        if (g_banks[i]->started)
            pthread_join(g_banks[i]->th, NULL);
}

/* a radio whose sample counter has not moved for timeout_ns (unplugged, wedged USB): its index, or -1 */
int rf_stalled(int64_t now, int64_t timeout_ns)
{
    static uint64_t last[MAX_RADIOS];
    static int64_t since[MAX_RADIOS];
    for (int ri = 0; ri < g_nradios; ri++) {
        radio *r = g_radios[ri];
        if (!r->dev || atomic_load(&r->state) != RADIO_ON)
            continue;
        sdr_stats st;
        r->drv->get_stats(r->dev, &st);
        if (st.samples != last[ri] || !since[ri]) {
            last[ri] = st.samples;
            since[ri] = now;
        } else if (now - since[ri] > timeout_ns) {
            return ri;
        }
    }
    return -1;
}

int rf_running(void) { return g_running; }

mux *rf_carrier_mux(int ci) { return ci >= 0 && ci < g_ncarriers ? g_carriers[ci]->mux : NULL; }

void rf_reload_tables(void)
{
    for (int i = 0; i < g_ncarriers; i++)
        mux_reload_tables(g_carriers[i]->mux);
}

static double thread_cpu(pthread_t th)
{
    clockid_t cid;
    struct timespec ts;
    if (pthread_getcpuclockid(th, &cid) || clock_gettime(cid, &ts))
        return 0;
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int rf_stats_json(char *buf, int len, double dt)
{
    static double prev_cpu[MAX_RADIOS + MAX_CARRIERS], prev_bank[MAX_BANKS];
    int n = snprintf(buf, len, "\"radios\":[");
    for (int ri = 0; ri < g_nradios && n < len - 512; ri++) {
        radio *r = g_radios[ri];
        double cpu = r->started && atomic_load(&r->state) == RADIO_ON ? thread_cpu(r->th) : prev_cpu[ri];
        double load = dt > 0 ? (cpu - prev_cpu[ri]) / dt * 100 : 0;
        static const char *const state_name[] = { "on", "off", "lost", "idle" };
        int st = atomic_load(&r->state);
        char esc[sizeof r->err];               /* the driver's message, safe in a JSON string */
        int e = 0;
        for (const char *q = st == RADIO_ON ? "" : r->err; *q && e < (int)sizeof esc - 1; q++)
            esc[e++] = *q == '"' || *q == '\\' || (unsigned char)*q < 0x20 ? ' ' : *q;
        esc[e] = 0;
        prev_cpu[ri] = cpu;
        double bload = 0;                  /* filter-bank threads of this radio */
        int nb = 0;
        for (int i = 0; i < g_nbanks; i++) {
            if (g_banks[i]->radio != r)
                continue;
            double bc = g_banks[i]->started ? thread_cpu(g_banks[i]->th) : 0;
            bload += dt > 0 ? (bc - prev_bank[i]) / dt * 100 : 0;
            prev_bank[i] = bc;
            nb++;
        }
        n += snprintf(buf + n, len - n,
                      "%s{\"index\":%d,\"driver\":\"%s\",\"freq\":%.0f,\"rate\":%.3f,\"carriers\":%d,"
                      "\"underruns\":%llu,\"late\":%llu,\"fill\":%.3f,\"cpu\":%.1f,\"samples\":%llu,"
                      "\"banks\":%d,\"bank_cpu\":%.1f,\"state\":\"%s\",\"error\":\"%s\"}",
                      ri ? "," : "", ri, r->driver, r->p.freq_hz, r->p.rate, r->ncarriers,
                      (unsigned long long)r->last.underruns, (unsigned long long)r->last.late, r->last.fill, load,
                      (unsigned long long)r->last.samples, nb, bload, state_name[st], esc);
    }
    n += snprintf(buf + n, len - n, "],\"carriers\":[");
    for (int i = 0; i < g_ncarriers && n < len - 4096; i++) {
        carrier *c = g_carriers[i];
        double cpu = c->started ? thread_cpu(c->th) : 0;
        if (c->in_group && c->group && c->group->t2g) {
            /* a DVB-T2 group member has no thread: its worker (shared by n members) and its share of the
             * group thread (summing the members, IFFT) */
            pthread_t th;
            int sharing = 1, n = dvbt2_group_members(c->group->t2g);
            cpu = dvbt2_group_worker(c->group->t2g, c->member, &th, &sharing) ? thread_cpu(th) / sharing : 0;
            if (c->group->started)
                cpu += thread_cpu(c->group->th) / (n > 0 ? n : 1);
        }
        double load = dt > 0 ? (cpu - prev_cpu[MAX_RADIOS + i]) / dt * 100 : 0;
        prev_cpu[MAX_RADIOS + i] = cpu;
        if (i)
            buf[n++] = ',';
        int m = mux_stats_json(c->mux, buf + n, len - n - 64, dt);
        if (m > 0 && buf[n + m - 1] == '}') {
            n += m - 1;
            n += snprintf(buf + n, len - n, ",\"standard\":\"%s\",\"radio\":%d,\"offset\":%.0f,\"cpu\":%.1f,\"on\":%d}",
                          c->standard, c->radio ? c->radio->idx : -1, c->offset_hz, load, !c->muted);
        }
    }
    n += snprintf(buf + n, len - n, "]");
    return n;
}

/* "dvbtx-engine --probe engine.ini N": radio N can be opened (it is back), closed at once without
 * transmitting; 0 if so */
int rf_probe(const conf *cf, int index, char *err, int errlen)
{
    const conf_sec *s = conf_find(cf, "radio", index);
    if (!s) {
        snprintf(err, errlen, "no radio %d", index);
        return -1;
    }
    radio r = { 0 };
    radio_params(&r, s);
    const sdr_driver *drv = sdr_load(r.driver, err, errlen);
    if (!drv)
        return -1;
    sdr_dev *d = drv->open(&r.p, err, errlen);
    if (!d)
        return -1;
    drv->close(d);
    return 0;
}
