/* SPDX-License-Identifier: GPL-3.0-or-later
 * Radio back-ends. Each one is a plugin (dvbtx-sdr-<name>.so) loaded with dlopen, so the
 * engine only needs the libraries of the radios really used (libhackrf, libuhd, SoapySDR). */
#ifndef DVBTX_SDR_H
#define DVBTX_SDR_H

#include <complex.h>
#include <stdint.h>

#define SDR_ABI 1

typedef struct sdr_params {
    double freq_hz;       /* LO / centre frequency */
    double rate;          /* sample rate */
    double bandwidth;     /* analog filter, 0 = automatic */
    double gain;          /* main TX gain in dB (HackRF: TX VGA 0-47, UHD: 0-89.75) */
    int amp;              /* HackRF front-end amplifier (+14 dB) */
    double ppm;           /* frequency correction */
    int channels;         /* TX channels (UHD: 1 or 2) */
    const char *args;     /* device arguments, e.g. "serial=..." */
    const char *clock;    /* clock/time source: internal, external, gpsdo */
    int buffer_ms;        /* host buffering */
} sdr_params;

typedef struct sdr_stats {
    uint64_t samples;     /* samples consumed by the radio */
    int64_t at_ns;        /* CLOCK_MONOTONIC when that count was observed */
    uint64_t underruns;   /* gaps filled with zeros (host too slow) */
    uint64_t late;        /* late packets reported by the device */
    double fill;          /* host buffer fill 0..1 */
    char serial[64];      /* serial number of the device opened, "" if unknown (zeroed by the caller: a
                             plugin built before this field leaves it empty) */
} sdr_stats;

typedef struct sdr_dev sdr_dev;

typedef struct sdr_driver {
    int abi;
    const char *name;
    sdr_dev *(*open)(const sdr_params *p, char *err, int errlen);
    int (*start)(sdr_dev *d);
    /* blocking: returns once the n samples (per channel) are queued; scale maps 1.0 to full scale */
    int (*write)(sdr_dev *d, const float complex *const *bufs, int n, float scale);
    void (*get_stats)(sdr_dev *d, sdr_stats *s);
    void (*close)(sdr_dev *d);
} sdr_driver;

/* loads dvbtx-sdr-<name>.so from DVBTX_PLUGIN_DIR (or next to the binary) */
const sdr_driver *sdr_load(const char *name, char *err, int errlen);

#endif
