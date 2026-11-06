/* SPDX-License-Identifier: GPL-3.0-or-later
 * "null" radio: consumes samples at the nominal rate of the system clock, for tests without
 * hardware. args: file=/path writes the I/Q (cf32, or cs8 with format=cs8, cs16 with format=cs16);
 * absent=/path: the radio is missing while that file exists (not found, or unplugged while running);
 * gaps=/path: its "late" counter grows while that file exists (gaps on air, for the tests). */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sdr.h"

struct sdr_dev {
    double rate;
    FILE *f;
    int fmt;                   /* 0 cf32, 1 cs8, 2 cs16 */
    int64_t t0;
    uint64_t samples;
    int64_t at_ns;
    void *conv;
    size_t conv_len;
    char absent[512], gaps[512];
    char serial[64];           /* serial=...: reported as the device's (tests) */
    uint64_t late;
    int calls;
};

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void arg(const char *args, const char *key, char *out, size_t len)
{
    out[0] = 0;
    if (!args)
        return;
    const char *p = strstr(args, key);
    if (!p || p[strlen(key)] != '=')
        return;
    p += strlen(key) + 1;
    size_t n = strcspn(p, ",");
    if (n >= len)
        n = len - 1;
    memcpy(out, p, n);
    out[n] = 0;
}

static sdr_dev *nl_open(const sdr_params *p, char *err, int errlen)
{
    sdr_dev *d = calloc(1, sizeof *d);
    d->rate = p->rate;
    arg(p->args, "absent", d->absent, sizeof d->absent);
    arg(p->args, "gaps", d->gaps, sizeof d->gaps);
    arg(p->args, "serial", d->serial, sizeof d->serial);
    if (d->absent[0] && !access(d->absent, F_OK)) {
        snprintf(err, errlen, "no null radio (%s exists)", d->absent);
        free(d);
        return NULL;
    }
    char path[512], fmt[16];
    arg(p->args, "file", path, sizeof path);
    arg(p->args, "format", fmt, sizeof fmt);
    d->fmt = !strcmp(fmt, "cs8") ? 1 : !strcmp(fmt, "cs16") ? 2 : 0;
    if (path[0]) {
        d->f = fopen(path, "wb");
        if (!d->f) {
            snprintf(err, errlen, "cannot write %s", path);
            free(d);
            return NULL;
        }
    }
    return d;
}

static int nl_start(sdr_dev *d) { (void)d; return 0; }

static int nl_write(sdr_dev *d, const float complex *const *bufs, int n, float scale)
{
    if (!d->t0)
        d->t0 = now_ns();
    if (d->absent[0] && ++d->calls % 64 == 0 && !access(d->absent, F_OK))
        return -1;                                     /* "unplugged" */
    if (d->f) {
        const float *in = (const float *)bufs[0];
        size_t ns = (size_t)n * 2;
        if (d->fmt == 0) {
            float *o = d->conv_len >= ns * 4 ? d->conv : (d->conv = realloc(d->conv, d->conv_len = ns * 4));
            for (size_t i = 0; i < ns; i++)
                o[i] = in[i] * scale;
            fwrite(o, 4, ns, d->f);
        } else if (d->fmt == 1) {
            int8_t *o = d->conv_len >= ns ? d->conv : (d->conv = realloc(d->conv, d->conv_len = ns));
            for (size_t i = 0; i < ns; i++) {
                float v = in[i] * scale * 127.0f;
                o[i] = (int8_t)lrintf(v > 127 ? 127 : v < -127 ? -127 : v);
            }
            fwrite(o, 1, ns, d->f);
        } else {
            int16_t *o = d->conv_len >= ns * 2 ? d->conv : (d->conv = realloc(d->conv, d->conv_len = ns * 2));
            for (size_t i = 0; i < ns; i++) {
                float v = in[i] * scale * 32767.0f;
                o[i] = (int16_t)lrintf(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
            }
            fwrite(o, 2, ns, d->f);
        }
    }
    d->samples += (uint64_t)n;
    int64_t due = d->t0 + (int64_t)((double)d->samples * 1e9 / d->rate) - 30000000;  /* 30 ms ahead */
    int64_t now = now_ns();
    if (due > now) {
        struct timespec ts = { (time_t)((due - now) / 1000000000), (long)((due - now) % 1000000000) };
        nanosleep(&ts, NULL);
    }
    d->at_ns = now_ns();
    return 0;
}

static void nl_stats(sdr_dev *d, sdr_stats *s)
{
    memset(s, 0, sizeof *s);
    snprintf(s->serial, sizeof s->serial, "%s", d->serial);
    if (!d->t0)
        return;
    int64_t now = now_ns();
    uint64_t played = (uint64_t)((now - d->t0) * d->rate / 1e9);
    s->samples = played < d->samples ? played : d->samples;
    s->at_ns = now;
    s->fill = 0.5;
    if (d->gaps[0] && !access(d->gaps, F_OK))
        d->late++;
    s->late = d->late;
}

static void nl_close(sdr_dev *d)
{
    if (d->f)
        fclose(d->f);
    free(d->conv);
    free(d);
}

const sdr_driver dvbtx_sdr_driver = {
    .abi = SDR_ABI, .name = "null", .open = nl_open, .start = nl_start,
    .write = nl_write, .get_stats = nl_stats, .close = nl_close,
};
