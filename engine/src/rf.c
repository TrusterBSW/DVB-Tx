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

#include "dvbt.h"
#include "ofdm.h"
#include "sdr.h"
#include "ts.h"
#include "util.h"

#ifndef PLUGIN_DIR
#define PLUGIN_DIR "/usr/lib/dvbtx"
#endif

#define MAX_RADIOS 16
#define MAX_CARRIERS 128
#define RING_SYMS 32

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
    int ncar;
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
    carrier *mem[64];
    int nmem;

    _Atomic int64_t t0;           /* CLOCK_MONOTONIC ns at which sample 0 is (virtually) on air */
    int64_t win_start, win_min, track_start;
    int tracking;
    uint64_t written;
    _Atomic uint64_t underruns_seen;
    sdr_stats last;

    pthread_t th;
    int started;
};

static radio *g_radios[MAX_RADIOS];
static int g_nradios;
static carrier *g_carriers[MAX_CARRIERS];
static int g_ncarriers;
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
            snprintf(path, sizeof path, "%s/dvbtx-sdr-%s.so", exe, name);
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
    int64_t raw = st.at_ns - (int64_t)((double)st.samples * 1e9 / r->p.rate);
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

/* ---- carrier threads ---- */

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
    while (g_running) {
        pthread_mutex_lock(&c->lock);
        while (g_running && c->wr - c->rd >= RING_SYMS)
            pthread_cond_wait(&c->cond, &c->lock);
        pthread_mutex_unlock(&c->lock);
        if (!g_running)
            break;
        dvbt_symbol(c->mod, c->ring + (size_t)(c->wr % RING_SYMS) * c->ncar);
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
        if (c->out_is_udp)
            sendto(c->out_fd, buf, sizeof buf, 0, (struct sockaddr *)&c->out_sa, sizeof c->out_sa);
        else if (c->out_fd >= 0 && write(c->out_fd, buf, sizeof buf) < 0 && errno != EAGAIN)
            break;
    }
    return NULL;
}

/* ---- radio thread ---- */

static void *radio_thread(void *arg)
{
    radio *r = arg;
    rt_priority(50, "the radio threads");
    float complex *out = xcalloc(r->symlen, sizeof *out);
    const float complex *cars[64];
    const float complex *bufs[1] = { out };
    int64_t next_track = 0;
    while (g_running) {
        for (int i = 0; i < r->nmem; i++) {
            carrier *c = r->mem[i];
            pthread_mutex_lock(&c->lock);
            while (g_running && c->wr == c->rd)
                pthread_cond_wait(&c->cond, &c->lock);
            pthread_mutex_unlock(&c->lock);
            cars[i] = c->ring + (size_t)(c->rd % RING_SYMS) * c->ncar;
        }
        if (!g_running)
            break;
        ofdm_group_synth(r->grp, cars, out);
        for (int i = 0; i < r->nmem; i++) {
            carrier *c = r->mem[i];
            pthread_mutex_lock(&c->lock);
            c->rd++;
            pthread_cond_broadcast(&c->cond);
            pthread_mutex_unlock(&c->lock);
        }
        if (r->drv->write(r->dev, bufs, r->symlen, r->scale)) {
            LOGE("radio %d: write failed", r->idx);
            break;
        }
        r->written += (uint64_t)r->symlen;
        int64_t now = mono_ns();
        if (now >= next_track) {
            radio_track(r);
            next_track = now + 50000000;
        }
    }
    free(out);
    g_running = 0;
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
    c->out_fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (c->out_fd < 0) {
        snprintf(err, errlen, "carrier %d: cannot open %s: %s", c->idx, path, strerror(errno));
        return -1;
    }
    return 0;
bad:
    snprintf(err, errlen, "carrier %d: bad output '%s'", c->idx, out);
    return -1;
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
        c->offset_hz = conf_num(s, "offset", 0);
        c->gain_db = conf_num(s, "gain_db", 0);
        double ts_rate;
        mux_clock_fn clk;
        if (!strcmp(c->standard, "dvbt")) {
            dvbt_cfg tc = { 0 };
            const char *mode = conf_str(s, "mode", "8k");
            tc.fft = !strcmp(mode, "2k") ? 2048 : 8192;
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
        if (pi < 0 || pi >= ninputs || bi >= ninputs) {
            snprintf(err, errlen, "service %d: bad input index", s->index);
            return -1;
        }
        sc.primary = inputs[pi];
        sc.backup = bi >= 0 ? inputs[bi] : NULL;
        sc.switch_timeout_ms = (int)conf_int(s, "switch_timeout_ms", 3000);
        sc.return_ms = (int)conf_int(s, "return_ms", 10000);
        sc.max_late_ms = (int)conf_int(s, "max_late_ms", 600);
        sc.early_ms = (int)conf_int(s, "early_ms", 0);
        sc.pcr_max_ms = (int)conf_int(s, "pcr_max_ms", 35);
        sc.pmt_period_ms = (int)conf_int(s, "pmt_period_ms", 100);
        if (mux_add_service(g_carriers[ci]->mux, &sc) < 0) {
            snprintf(err, errlen, "service %d: too many services on carrier %d", s->index, ci);
            return -1;
        }
    }
    for (int i = 0; i < g_ncarriers; i++)
        mux_start(g_carriers[i]->mux);

    /* OFDM groups: one per radio, all its carriers share mode, guard interval and bandwidth */
    for (int ri = 0; ri < g_nradios; ri++) {
        radio *r = g_radios[ri];
        dvbt_cfg *ref = NULL;
        double ptot = 0;
        for (int i = 0; i < g_ncarriers; i++) {
            carrier *c = g_carriers[i];
            if (c->radio != r)
                continue;
            if (!ref)
                ref = &c->tc;
            else if (c->tc.fft != ref->fft || c->tc.gi != ref->gi || c->tc.bw_hz != ref->bw_hz) {
                snprintf(err, errlen, "radio %d: carriers %d and %d differ in mode/guard/bandwidth; "
                         "one radio needs identical OFDM parameters", ri, r->mem[0]->idx, c->idx);
                return -1;
            }
            if (r->nmem >= 64) {
                snprintf(err, errlen, "radio %d: too many carriers", ri);
                return -1;
            }
            r->mem[r->nmem++] = c;
            ptot += dvbt_mean_power(&c->tc) * pow(10.0, c->gain_db / 10.0);
        }
        if (!r->nmem) {
            snprintf(err, errlen, "radio %d has no carrier", ri);
            return -1;
        }
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
            float g = (float)(pow(10.0, c->gain_db / 20.0) / sqrt(ptot));
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
            LOGE("radio %d (%s): %s", ri, r->driver, err);
            g_running = 0;
            return -1;
        }
        /* before the radio reports anything: on air after its buffer and our symbol ring */
        int64_t lead = (int64_t)r->p.buffer_ms * 1000000 + (int64_t)(RING_SYMS * r->symlen / r->p.rate * 1e9);
        atomic_store(&r->t0, mono_ns() + lead);
        LOGI("radio %d: %s at %.3f MHz, %.6f MS/s, %d carrier(s), FFT %d", ri, r->driver, r->p.freq_hz / 1e6,
             r->p.rate / 1e6, r->nmem, r->nfft);
    }
    for (int i = 0; i < g_ncarriers; i++) {
        carrier *c = g_carriers[i];
        void *(*fn)(void *) = c->radio ? dvbt_thread : ts_thread;
        if (pthread_create(&c->th, NULL, fn, c))
            return -1;
        c->started = 1;
    }
    for (int ri = 0; ri < g_nradios; ri++) {
        radio *r = g_radios[ri];
        if (pthread_create(&r->th, NULL, radio_thread, r))
            return -1;
        r->started = 1;
    }
    return 0;
}

void rf_stop(void)
{
    g_running = 0;
    for (int i = 0; i < g_ncarriers; i++) {
        carrier *c = g_carriers[i];
        pthread_mutex_lock(&c->lock);
        pthread_cond_broadcast(&c->cond);
        pthread_mutex_unlock(&c->lock);
    }
    for (int ri = 0; ri < g_nradios; ri++) {
        radio *r = g_radios[ri];
        if (r->dev)
            r->drv->close(r->dev);     /* unblocks a pending write */
        r->dev = NULL;
        if (r->started)
            pthread_join(r->th, NULL);
    }
    for (int i = 0; i < g_ncarriers; i++)
        if (g_carriers[i]->started)
            pthread_join(g_carriers[i]->th, NULL);
}

int rf_running(void) { return g_running; }

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
    static double prev_cpu[MAX_RADIOS + MAX_CARRIERS];
    int n = snprintf(buf, len, "\"radios\":[");
    for (int ri = 0; ri < g_nradios && n < len - 512; ri++) {
        radio *r = g_radios[ri];
        double cpu = r->started ? thread_cpu(r->th) : 0;
        double load = dt > 0 ? (cpu - prev_cpu[ri]) / dt * 100 : 0;
        prev_cpu[ri] = cpu;
        n += snprintf(buf + n, len - n,
                      "%s{\"index\":%d,\"driver\":\"%s\",\"freq\":%.0f,\"rate\":%.3f,\"carriers\":%d,"
                      "\"underruns\":%llu,\"late\":%llu,\"fill\":%.3f,\"cpu\":%.1f,\"samples\":%llu}",
                      ri ? "," : "", ri, r->driver, r->p.freq_hz, r->p.rate, r->nmem,
                      (unsigned long long)r->last.underruns, (unsigned long long)r->last.late, r->last.fill, load,
                      (unsigned long long)r->last.samples);
    }
    n += snprintf(buf + n, len - n, "],\"carriers\":[");
    for (int i = 0; i < g_ncarriers && n < len - 4096; i++) {
        carrier *c = g_carriers[i];
        double cpu = c->started ? thread_cpu(c->th) : 0;
        double load = dt > 0 ? (cpu - prev_cpu[MAX_RADIOS + i]) / dt * 100 : 0;
        prev_cpu[MAX_RADIOS + i] = cpu;
        if (i)
            buf[n++] = ',';
        int m = mux_stats_json(c->mux, buf + n, len - n - 64, dt);
        if (m > 0 && buf[n + m - 1] == '}') {
            n += m - 1;
            n += snprintf(buf + n, len - n, ",\"standard\":\"%s\",\"radio\":%d,\"offset\":%.0f,\"cpu\":%.1f}",
                          c->standard, c->radio ? c->radio->idx : -1, c->offset_hz, load);
        }
    }
    n += snprintf(buf + n, len - n, "]");
    return n;
}
