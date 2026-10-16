/* SPDX-License-Identifier: GPL-3.0-or-later
 * SoapySDR back-end: any transmitter with a Soapy module (LimeSDR, PlutoSDR, bladeRF, HackRF, USRP...).
 * args: the Soapy device string, e.g. "driver=lime" or "driver=plutosdr,hostname=192.168.2.1". */
#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sdr.h"

struct sdr_dev {
    SoapySDRDevice *dev;
    SoapySDRStream *st;
    float complex *conv;
    size_t conv_len;
    uint64_t sent;
    int64_t at_ns;
    uint64_t underruns;
    pthread_mutex_t lock;
};

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void sp_close(sdr_dev *d)
{
    if (!d)
        return;
    if (d->st) {
        SoapySDRDevice_deactivateStream(d->dev, d->st, 0, 0);
        SoapySDRDevice_closeStream(d->dev, d->st);
    }
    if (d->dev)
        SoapySDRDevice_unmake(d->dev);
    free(d->conv);
    free(d);
}

static sdr_dev *sp_open(const sdr_params *p, char *err, int errlen)
{
    sdr_dev *d = calloc(1, sizeof *d);
    pthread_mutex_init(&d->lock, NULL);
    d->dev = SoapySDRDevice_makeStrArgs(p->args ? p->args : "");
    if (!d->dev) {
        snprintf(err, errlen, "SoapySDR: %s", SoapySDRDevice_lastError());
        free(d);
        return NULL;
    }
    double corr = 1.0 / (1.0 + p->ppm * 1e-6);
    if (SoapySDRDevice_setSampleRate(d->dev, SOAPY_SDR_TX, 0, p->rate) ||
        SoapySDRDevice_setFrequency(d->dev, SOAPY_SDR_TX, 0, p->freq_hz * corr, NULL)) {
        snprintf(err, errlen, "SoapySDR setup: %s", SoapySDRDevice_lastError());
        sp_close(d);
        return NULL;
    }
    double actual = SoapySDRDevice_getSampleRate(d->dev, SOAPY_SDR_TX, 0);
    if (fabs(actual - p->rate) > 1e-5 * p->rate) {
        snprintf(err, errlen, "device gives %.6f MS/s instead of %.6f MS/s", actual / 1e6, p->rate / 1e6);
        sp_close(d);
        return NULL;
    }
    SoapySDRDevice_setGain(d->dev, SOAPY_SDR_TX, 0, p->gain);
    if (p->bandwidth > 0)
        SoapySDRDevice_setBandwidth(d->dev, SOAPY_SDR_TX, 0, p->bandwidth);
    size_t ch = 0;
    d->st = SoapySDRDevice_setupStream(d->dev, SOAPY_SDR_TX, SOAPY_SDR_CF32, &ch, 1, NULL);
    if (!d->st || SoapySDRDevice_activateStream(d->dev, d->st, 0, 0, 0)) {
        snprintf(err, errlen, "SoapySDR stream: %s", SoapySDRDevice_lastError());
        d->st = NULL;
        sp_close(d);
        return NULL;
    }
    return d;
}

static int sp_start(sdr_dev *d)
{
    (void)d;
    return 0;
}

static int sp_write(sdr_dev *d, const float complex *const *bufs, int n, float scale)
{
    if (d->conv_len < (size_t)n) {
        free(d->conv);
        d->conv = malloc(sizeof(float complex) * n);
        d->conv_len = (size_t)n;
    }
    for (int i = 0; i < n; i++)
        d->conv[i] = bufs[0][i] * scale;
    size_t done = 0;
    while (done < (size_t)n) {
        const void *ptr = d->conv + done;
        int flags = 0;
        int r = SoapySDRDevice_writeStream(d->dev, d->st, &ptr, (size_t)n - done, &flags, 0, 1000000);
        if (r == SOAPY_SDR_TIMEOUT)
            continue;
        if (r == SOAPY_SDR_UNDERFLOW) {
            d->underruns++;
            continue;
        }
        if (r < 0)
            return -1;
        done += (size_t)r;
    }
    size_t chm = 0;
    int flags = 0;
    long long t = 0;
    while (SoapySDRDevice_readStreamStatus(d->dev, d->st, &chm, &flags, &t, 0) == SOAPY_SDR_UNDERFLOW)
        d->underruns++;
    pthread_mutex_lock(&d->lock);
    d->sent += done;
    d->at_ns = now_ns();
    pthread_mutex_unlock(&d->lock);
    return 0;
}

static void sp_stats(sdr_dev *d, sdr_stats *s)
{
    pthread_mutex_lock(&d->lock);
    s->samples = d->sent;
    s->at_ns = d->at_ns;
    pthread_mutex_unlock(&d->lock);
    s->underruns = d->underruns;
    s->late = 0;
    s->fill = 0;
}

const sdr_driver dvbtx_sdr_driver = {
    .abi = SDR_ABI, .name = "soapy", .open = sp_open, .start = sp_start,
    .write = sp_write, .get_stats = sp_stats, .close = sp_close,
};
