/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ettus USRP back-end (UHD C API), e.g. B200/B210: up to 61.44 MS/s on one TX channel.
 * write() fills a ring buffer (buffer_ms) drained by a sender thread, so that a late synthesis
 * thread does not underflow the radio. The master clock rate is an integer multiple of the
 * sample rate (args may force master_clock_rate=...). Underflows come from async messages. */
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uhd.h>

#include "sdr.h"

struct sdr_dev {
    uhd_usrp_handle usrp;
    uhd_tx_streamer_handle tx;
    uhd_tx_metadata_handle md;
    size_t max_samps;
    double rate;

    float complex *ring;           /* already scaled samples */
    size_t size, rd, wr, used;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int started;

    uint64_t sent;
    int64_t at_ns;
    volatile uint64_t underruns, late;
    volatile int stop;
    pthread_t async_th, send_th;
    int async_started, send_started;
};

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void last_error(char *err, int errlen, const char *what)
{
    char buf[512] = "";
    uhd_get_last_error(buf, sizeof buf);
    snprintf(err, errlen, "%s: %s", what, buf);
}

static void *async_thread(void *arg)
{
    sdr_dev *d = arg;
    uhd_async_metadata_handle amd;
    if (uhd_async_metadata_make(&amd))
        return NULL;
    while (!d->stop) {
        bool valid = false;
        if (uhd_tx_streamer_recv_async_msg(d->tx, &amd, 0.2, &valid) || !valid)
            continue;
        uhd_async_metadata_event_code_t code;
        uhd_async_metadata_event_code(amd, &code);
        if (code == UHD_ASYNC_METADATA_EVENT_CODE_UNDERFLOW ||
            code == UHD_ASYNC_METADATA_EVENT_CODE_UNDERFLOW_IN_PACKET)
            d->underruns++;
        else if (code == UHD_ASYNC_METADATA_EVENT_CODE_TIME_ERROR || code == UHD_ASYNC_METADATA_EVENT_CODE_SEQ_ERROR ||
                 code == UHD_ASYNC_METADATA_EVENT_CODE_SEQ_ERROR_IN_BURST)
            d->late++;
    }
    uhd_async_metadata_free(&amd);
    return NULL;
}

static void *send_thread(void *arg)
{
    sdr_dev *d = arg;
    struct sched_param sp = { .sched_priority = 60 };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);   /* best effort */
    size_t chunk_max = d->max_samps * 8;
    while (!d->stop) {
        pthread_mutex_lock(&d->lock);
        while (!d->stop && (!d->started || d->used == 0))
            pthread_cond_wait(&d->cond, &d->lock);
        size_t chunk = d->used;
        if (chunk > d->size - d->rd)
            chunk = d->size - d->rd;
        if (chunk > chunk_max)
            chunk = chunk_max;
        size_t rd = d->rd;
        pthread_mutex_unlock(&d->lock);
        if (d->stop)
            break;
        size_t done = 0;
        while (done < chunk && !d->stop) {
            const void *ptr = d->ring + rd + done;
            size_t sent = 0;
            if (uhd_tx_streamer_send(d->tx, &ptr, chunk - done, &d->md, 1.0, &sent))
                break;
            done += sent;
        }
        pthread_mutex_lock(&d->lock);
        d->rd = (d->rd + done) % d->size;
        d->used -= done;
        d->sent += done;
        d->at_ns = now_ns();
        pthread_cond_broadcast(&d->cond);
        pthread_mutex_unlock(&d->lock);
    }
    return NULL;
}

static void ud_close(sdr_dev *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->lock);
    d->stop = 1;
    pthread_cond_broadcast(&d->cond);
    pthread_mutex_unlock(&d->lock);
    if (d->send_started)
        pthread_join(d->send_th, NULL);
    if (d->async_started)
        pthread_join(d->async_th, NULL);
    if (d->md)
        uhd_tx_metadata_free(&d->md);
    if (d->tx)
        uhd_tx_streamer_free(&d->tx);
    if (d->usrp)
        uhd_usrp_free(&d->usrp);
    free(d->ring);
    free(d);
}

static sdr_dev *ud_open(const sdr_params *p, char *err, int errlen)
{
    sdr_dev *d = calloc(1, sizeof *d);
    pthread_mutex_init(&d->lock, NULL);
    pthread_cond_init(&d->cond, NULL);
    char args[512];
    const char *ua = p->args ? p->args : "";
    int n = snprintf(args, sizeof args, "%s", ua);
    if (!strstr(ua, "master_clock_rate")) {
        int k = (int)floor(61.44e6 / p->rate);
        if (k < 1)
            k = 1;
        while (k > 1 && p->rate * k > 56e6)   /* keep the AD9361 within its 56 MHz analog bandwidth */
            k--;
        n += snprintf(args + n, sizeof args - n, "%smaster_clock_rate=%.6f", n ? "," : "", p->rate * k);
    }
    if (!strstr(ua, "num_send_frames"))
        snprintf(args + n, sizeof args - n, "%snum_send_frames=64", n ? "," : "");
    if (uhd_usrp_make(&d->usrp, args)) {
        last_error(err, errlen, "uhd_usrp_make");
        d->usrp = NULL;
        ud_close(d);
        return NULL;
    }
    size_t ch = 0;
    if (p->clock && *p->clock && strcmp(p->clock, "internal"))
        uhd_usrp_set_clock_source(d->usrp, p->clock, 0);
    if (uhd_usrp_set_tx_rate(d->usrp, p->rate, ch)) {
        last_error(err, errlen, "set_tx_rate");
        ud_close(d);
        return NULL;
    }
    double actual = 0;
    uhd_usrp_get_tx_rate(d->usrp, ch, &actual);
    if (fabs(actual - p->rate) > 1e-6 * p->rate) {
        snprintf(err, errlen, "USRP gives %.6f MS/s instead of %.6f MS/s (set master_clock_rate=... in args)",
                 actual / 1e6, p->rate / 1e6);
        ud_close(d);
        return NULL;
    }
    d->rate = actual;
    uhd_tune_request_t tr = { .target_freq = p->freq_hz * (1.0 - p->ppm * 1e-6),
                              .rf_freq_policy = UHD_TUNE_REQUEST_POLICY_AUTO,
                              .dsp_freq_policy = UHD_TUNE_REQUEST_POLICY_AUTO };
    uhd_tune_result_t res;
    if (uhd_usrp_set_tx_freq(d->usrp, &tr, ch, &res)) {
        last_error(err, errlen, "set_tx_freq");
        ud_close(d);
        return NULL;
    }
    uhd_usrp_set_tx_gain(d->usrp, p->gain, ch, "");
    uhd_usrp_set_tx_bandwidth(d->usrp, p->bandwidth > 0 ? p->bandwidth : fmin(56e6, p->rate), ch);
    if (!strstr(ua, "antenna="))
        uhd_usrp_set_tx_antenna(d->usrp, "TX/RX", ch);

    if (uhd_tx_streamer_make(&d->tx)) {
        last_error(err, errlen, "tx_streamer_make");
        ud_close(d);
        return NULL;
    }
    char cpu[] = "fc32", otw[] = "sc16", sargs[] = "";
    uhd_stream_args_t sa = { .cpu_format = cpu, .otw_format = otw, .args = sargs, .channel_list = &ch, .n_channels = 1 };
    if (uhd_usrp_get_tx_stream(d->usrp, &sa, d->tx)) {
        last_error(err, errlen, "get_tx_stream");
        ud_close(d);
        return NULL;
    }
    uhd_tx_streamer_max_num_samps(d->tx, &d->max_samps);
    uhd_tx_metadata_make(&d->md, false, 0, 0.0, false, false);

    int ms = p->buffer_ms > 0 ? p->buffer_ms : 50;
    d->size = (size_t)(p->rate * ms / 1000.0);
    if (d->size < 16 * d->max_samps)
        d->size = 16 * d->max_samps;
    d->ring = malloc(sizeof(float complex) * d->size);
    if (!d->ring) {
        snprintf(err, errlen, "out of memory");
        ud_close(d);
        return NULL;
    }
    if (!pthread_create(&d->async_th, NULL, async_thread, d))
        d->async_started = 1;
    if (!pthread_create(&d->send_th, NULL, send_thread, d))
        d->send_started = 1;
    return d;
}

static int ud_start(sdr_dev *d)
{
    (void)d;
    return 0;   /* sending starts once the ring is full */
}

static int ud_write(sdr_dev *d, const float complex *const *bufs, int n, float scale)
{
    const float complex *in = bufs[0];
    size_t done = 0;
    while (done < (size_t)n) {
        pthread_mutex_lock(&d->lock);
        while (!d->stop && d->used == d->size) {
            if (!d->started) {
                d->started = 1;
                pthread_cond_broadcast(&d->cond);
            }
            pthread_cond_wait(&d->cond, &d->lock);
        }
        if (d->stop) {
            pthread_mutex_unlock(&d->lock);
            return -1;
        }
        size_t chunk = (size_t)n - done, space = d->size - d->used;
        if (chunk > space)
            chunk = space;
        if (chunk > d->size - d->wr)
            chunk = d->size - d->wr;
        size_t wr = d->wr;
        pthread_mutex_unlock(&d->lock);
        float complex *o = d->ring + wr;
        for (size_t i = 0; i < chunk; i++)
            o[i] = in[done + i] * scale;
        pthread_mutex_lock(&d->lock);
        d->wr = (d->wr + chunk) % d->size;
        d->used += chunk;
        pthread_cond_broadcast(&d->cond);
        pthread_mutex_unlock(&d->lock);
        done += chunk;
    }
    return 0;
}

static void ud_stats(sdr_dev *d, sdr_stats *s)
{
    pthread_mutex_lock(&d->lock);
    s->samples = d->sent;
    s->at_ns = d->at_ns;
    s->fill = (double)d->used / (double)d->size;
    pthread_mutex_unlock(&d->lock);
    s->underruns = d->underruns;
    s->late = d->late;
}

const sdr_driver dvbtx_sdr_driver = {
    .abi = SDR_ABI, .name = "uhd", .open = ud_open, .start = ud_start,
    .write = ud_write, .get_stats = ud_stats, .close = ud_close,
};
