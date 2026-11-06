/* SPDX-License-Identifier: GPL-3.0-or-later
 * HackRF One back-end (libhackrf). libhackrf pulls 8-bit I/Q from a callback, fed here
 * from a ring buffer: write() converts and blocks while the ring is full, the radio starts
 * once the ring has been filled the first time. */
#include <hackrf.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sdr.h"

struct sdr_dev {
    hackrf_device *dev;
    char serial[64];                /* as hackrf_info prints it */
    int8_t *ring;
    size_t size, rd, wr, used;      /* bytes */
    int started, stopping;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    uint64_t consumed;              /* samples */
    int64_t at_ns;
    uint64_t underruns;
    uint64_t shortfalls;            /* reported by the firmware: its FIFO ran dry (USB too slow) */
    int64_t m0_at;                  /* last read of the M0 state; -1: not supported */
    int64_t m0_period;              /* ns between two reads (args "m0_poll=S", default 1 s; 0: never) */
    int rt;                         /* args "rt=N": SCHED_FIFO priority of the USB thread (default 60, 0: none) */
    int rt_done;
    pthread_t writer;               /* the radio thread: libhackrf calls tx_cb from it for the first transfers */
    int have_writer;
    int64_t cb_log_ns;              /* args "cb_log_ms=N": log a USB callback later than N ms after the previous one */
    int64_t cb_last_ns;
};

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int tx_cb(hackrf_transfer *t)
{
    sdr_dev *d = t->tx_ctx;
    if (!d->rt_done && !(d->have_writer && pthread_equal(pthread_self(), d->writer))) {
        /* runs in the libusb event thread of libhackrf: a late transfer empties the firmware FIFO */
        d->rt_done = 1;
        if (d->rt > 0) {
            struct sched_param sp = {.sched_priority = d->rt};
            int e = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
            fprintf(stderr, "hackrf: USB thread SCHED_FIFO %d: %s\n", d->rt, e ? strerror(e) : "ok");
        }
    }
    size_t want = (size_t)t->valid_length, got = 0;
    pthread_mutex_lock(&d->lock);
    while (got < want && d->used) {
        size_t chunk = want - got;
        if (chunk > d->used)
            chunk = d->used;
        if (chunk > d->size - d->rd)
            chunk = d->size - d->rd;
        memcpy(t->buffer + got, d->ring + d->rd, chunk);
        d->rd = (d->rd + chunk) % d->size;
        d->used -= chunk;
        got += chunk;
    }
    if (got < want) {
        memset(t->buffer + got, 0, want - got);
        if (!d->stopping)
            d->underruns++;
    }
    d->consumed += want / 2;
    int64_t now = d->at_ns = now_ns();
    pthread_cond_signal(&d->cond);
    pthread_mutex_unlock(&d->lock);
    if (d->cb_log_ns > 0 && !(d->have_writer && pthread_equal(pthread_self(), d->writer))) {
        if (d->cb_last_ns && now - d->cb_last_ns > d->cb_log_ns)
            fprintf(stderr, "hackrf: USB callback %.1f ms after the previous one\n", (now - d->cb_last_ns) / 1e6);
        d->cb_last_ns = now;
    }
    return 0;
}

static void hk_close(sdr_dev *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->lock);
    d->stopping = 1;
    pthread_cond_broadcast(&d->cond);
    pthread_mutex_unlock(&d->lock);
    if (d->dev) {
        if (d->started)
            hackrf_stop_tx(d->dev);
        hackrf_close(d->dev);
    }
    hackrf_exit();
    free(d->ring);
    free(d);
}

static const char *arg_value(const char *args, const char *key, char *buf, size_t len)
{
    if (!args)
        return NULL;
    size_t kl = strlen(key);
    for (const char *p = args; *p;) {
        while (*p == ' ' || *p == ',')
            p++;
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            p += kl + 1;
            size_t n = strcspn(p, ", ");
            if (n >= len)
                n = len - 1;
            memcpy(buf, p, n);
            buf[n] = 0;
            return buf;
        }
        p += strcspn(p, ", ");
    }
    return NULL;
}

/* args "if=2400e6": force the intermediate frequency of the MAX2837 (2150-2750 MHz) below 2150 MHz,
 * where the RFFC5072 mixes it down (LO = IF + RF); default: the firmware's choice (about 2350 MHz) */
static int set_freq(hackrf_device *dev, double freq, const char *args)
{
    char v[32];
    if (freq < 2150e6 && arg_value(args, "if", v, sizeof v)) {
        double ifq = strtod(v, NULL);
        /* like the firmware: the mixer LO on a whole MHz (a fractional one adds spurs), the
         * remainder on the IF */
        double lo = round((ifq + freq) / 1e6) * 1e6;
        ifq = lo - freq;
        if (ifq >= 2150e6 && ifq <= 2750e6)
            return hackrf_set_freq_explicit(dev, (uint64_t)llround(ifq), (uint64_t)llround(lo),
                                            RF_PATH_FILTER_LOW_PASS);
    }
    return hackrf_set_freq(dev, (uint64_t)llround(freq));
}

static sdr_dev *hk_open(const sdr_params *p, char *err, int errlen)
{
    int r = hackrf_init();
    if (r != HACKRF_SUCCESS) {
        snprintf(err, errlen, "hackrf_init: %s", hackrf_error_name(r));
        return NULL;
    }
    sdr_dev *d = calloc(1, sizeof *d);
    pthread_mutex_init(&d->lock, NULL);
    pthread_cond_init(&d->cond, NULL);
    char serial[64];
    const char *s = arg_value(p->args, "serial", serial, sizeof serial);
    r = hackrf_open_by_serial(s, &d->dev);
    if (r != HACKRF_SUCCESS) {
        snprintf(err, errlen, "no HackRF%s%s: %s", s ? " with serial " : "", s ? s : "",
                 hackrf_error_name(r));
        d->dev = NULL;
        hk_close(d);
        return NULL;
    }
    read_partid_serialno_t ps;
    if (hackrf_board_partid_serialno_read(d->dev, &ps) == HACKRF_SUCCESS)
        snprintf(d->serial, sizeof d->serial, "%08x%08x%08x%08x", ps.serial_no[0], ps.serial_no[1],
                 ps.serial_no[2], ps.serial_no[3]);
    double corr = 1.0 / (1.0 + p->ppm * 1e-6);   /* crystal error moves rate and LO alike */
    double bw = p->bandwidth > 0 ? p->bandwidth : p->rate;   /* the supervisor computes it from the band edges */
    if ((r = hackrf_set_sample_rate(d->dev, p->rate * corr)) != HACKRF_SUCCESS ||
        (r = hackrf_set_baseband_filter_bandwidth(
             d->dev, hackrf_compute_baseband_filter_bw((uint32_t)bw))) != HACKRF_SUCCESS ||
        (r = set_freq(d->dev, p->freq_hz * corr, p->args)) != HACKRF_SUCCESS ||
        (r = hackrf_set_txvga_gain(d->dev, (uint32_t)(p->gain < 0 ? 0 : p->gain > 47 ? 47 : p->gain))) !=
            HACKRF_SUCCESS ||
        (r = hackrf_set_amp_enable(d->dev, p->amp ? 1 : 0)) != HACKRF_SUCCESS) {
        snprintf(err, errlen, "HackRF setup: %s", hackrf_error_name(r));
        hk_close(d);
        return NULL;
    }
    char v[32];
    d->m0_period = arg_value(p->args, "m0_poll", v, sizeof v) ? (int64_t)(strtod(v, NULL) * 1e9) : 1000000000LL;
    /* the libusb thread above our own real-time threads (radio 50, carriers 40): never delayed by them */
    d->rt = arg_value(p->args, "rt", v, sizeof v) ? atoi(v) : 60;
    d->cb_log_ns = arg_value(p->args, "cb_log_ms", v, sizeof v) ? (int64_t)(strtod(v, NULL) * 1e6) : 0;
    int ms = p->buffer_ms > 0 ? p->buffer_ms : 40;
    size_t bytes = (size_t)(p->rate * 2 * ms / 1000.0);
    bytes = (bytes + 262143) / 262144 * 262144;
    if (bytes < 4 * 262144)
        bytes = 4 * 262144;
    d->size = bytes;
    d->ring = malloc(bytes);
    if (!d->ring) {
        snprintf(err, errlen, "out of memory");
        hk_close(d);
        return NULL;
    }
    return d;
}

static int hk_start(sdr_dev *d)
{
    (void)d;
    return 0;   /* the radio starts on its own once the ring has been filled */
}

static int hk_write(sdr_dev *d, const float complex *const *bufs, int n, float scale)
{
    const float *in = (const float *)bufs[0];
    float k = scale * 127.0f;
    size_t bytes = (size_t)n * 2, done = 0;
    while (done < bytes) {
        pthread_mutex_lock(&d->lock);
        while (d->used == d->size && !d->stopping) {
            if (!d->started) {
                /* libhackrf fills the first transfers by calling tx_cb from here: unlock */
                d->started = 1;
                d->writer = pthread_self();
                d->have_writer = 1;
                pthread_mutex_unlock(&d->lock);
                int r = hackrf_start_tx(d->dev, tx_cb, d);
                if (r != HACKRF_SUCCESS) {
                    fprintf(stderr, "hackrf_start_tx: %s\n", hackrf_error_name(r));
                    return -1;
                }
                pthread_mutex_lock(&d->lock);
                continue;
            }
            pthread_cond_wait(&d->cond, &d->lock);
        }
        if (d->stopping) {
            pthread_mutex_unlock(&d->lock);
            return -1;
        }
        size_t chunk = bytes - done, space = d->size - d->used;
        if (chunk > space)
            chunk = space;
        if (chunk > d->size - d->wr)
            chunk = d->size - d->wr;
        size_t wr = d->wr;
        pthread_mutex_unlock(&d->lock);

        /* only this thread writes [wr, wr+chunk), the callback never reads past 'used' */
        int8_t *o = d->ring + wr;
        const float *src = in + done;
        for (size_t i = 0; i < chunk; i++) {
            float v = src[i] * k;
            v = v > 127.0f ? 127.0f : v < -127.0f ? -127.0f : v;
            o[i] = (int8_t)lrintf(v);
        }

        pthread_mutex_lock(&d->lock);
        d->wr = (d->wr + chunk) % d->size;
        d->used += chunk;
        pthread_mutex_unlock(&d->lock);
        done += chunk;
    }
    return 0;
}

/* the firmware counts the times its sample FIFO ran dry: gaps on air the host does not see (USB
 * transfers too late although our ring was full); read at most once a second */
static void read_m0(sdr_dev *d)
{
    int64_t now = now_ns();
    if (d->m0_at < 0 || d->m0_period <= 0 || now - d->m0_at < d->m0_period || !d->started)
        return;
    d->m0_at = now;
    hackrf_m0_state st;
    if (hackrf_get_m0_state(d->dev, &st) != HACKRF_SUCCESS) {
        d->m0_at = -1;
        return;
    }
    pthread_mutex_lock(&d->lock);
    uint64_t prev = d->shortfalls;
    d->shortfalls = st.num_shortfalls;
    pthread_mutex_unlock(&d->lock);
    if (d->cb_log_ns > 0 && st.num_shortfalls > prev)
        fprintf(stderr, "hackrf: firmware FIFO ran dry %llu time(s), longest %u bytes\n",
                (unsigned long long)(st.num_shortfalls - prev), (unsigned)st.longest_shortfall);
}

static void hk_stats(sdr_dev *d, sdr_stats *s)
{
    read_m0(d);
    pthread_mutex_lock(&d->lock);
    s->samples = d->consumed;
    s->at_ns = d->at_ns;
    s->underruns = d->underruns;
    s->late = d->shortfalls;
    s->fill = (double)d->used / (double)d->size;
    pthread_mutex_unlock(&d->lock);
    snprintf(s->serial, sizeof s->serial, "%s", d->serial);
}

const sdr_driver dvbtx_sdr_driver = {
    .abi = SDR_ABI,
    .name = "hackrf",
    .open = hk_open,
    .start = hk_start,
    .write = hk_write,
    .get_stats = hk_stats,
    .close = hk_close,
};
