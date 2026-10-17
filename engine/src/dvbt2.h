/* SPDX-License-Identifier: GPL-3.0-or-later
 * DVB-T2 (ETSI EN 302 755) modulator: single PLP, input mode A (TS, CRC-8, no ISSY, no null
 * packet deletion), SISO, no PAPR reduction, normal/short FEC frames, QPSK..256-QAM with
 * optional rotation, code rates 1/2..5/6, 1K..32K (normal/extended carriers), all guard
 * intervals, PP1-PP8, P1, L1-pre/L1-post (BPSK..64-QAM) and the time interleaver.
 * Derived from GNU Radio gr-dtv (Copyright Free Software Foundation, Inc. / Ron Economos);
 * sample exact (up to float rounding) with the gr-dtv DVB-T2 transmitter chain configured
 * with VERSION_111, INBAND_OFF, PAPR_OFF, EQUALIZATION_OFF and the same parameters.
 *
 * Output: complex baseband at the elementary rate (dvbt2_sample_rate(), e.g. 64/7 MHz for
 * 8 MHz), mean power ~1 (P1 and pilots included), DC = centre carrier. */
#ifndef DVBTX_DVBT2_H
#define DVBTX_DVBT2_H

#include <complex.h>
#include <stdint.h>

#ifndef DVBTX_TS_PULL_FN
#define DVBTX_TS_PULL_FN
/* Pull one 188-byte TS packet (the multiplexer always delivers one, null if idle).
 * Same type as in dvbt.h (an identical typedef may be repeated in C11). */
typedef void (*ts_pull_fn)(void *ctx, uint8_t *pkt);
#endif

typedef struct dvbt2_cfg {
    double bw_hz;            /* 1.7e6 (T = 71/131 us), 5e6, 6e6, 7e6, 8e6, 10e6 */
    int fft;                 /* 1024 2048 4096 8192 16384 32768 */
    int extended;            /* extended carrier mode (8K, 16K, 32K) */
    int gi_num, gi_den;      /* 1/128 1/32 1/16 19/256 1/8 19/128 1/4 */
    int pilot;               /* 1..8 = PP1..PP8 */
    int bits;                /* 2 4 6 8 (QPSK..256QAM) */
    int rotation;            /* constellation rotation on/off */
    int cr_num, cr_den;      /* 1/2 3/5 2/3 3/4 4/5 5/6 */
    int short_frame;         /* 0: 64800, 1: 16200 */
    int fec_blocks;          /* FEC blocks per T2 frame, 0 = maximum that fits */
    int data_symbols;        /* L_data, 0 = longest frame <= 250 ms (EN 302 755 table 46) */
    int ti_blocks;           /* TI blocks per frame (gr: tiblocks), 0 = fewest that fit M_TI */
    int l1_bits;             /* L1-post constellation: 1 BPSK, 2 QPSK, 4 16QAM, 6 64QAM */
    int papr;                /* 0 only */
    int cell_id, network_id, t2_system_id;   /* gr-dtv uses 0, 0x3085, 0x8001 */
    /* optional (0 = gr-dtv value) */
    int t2_frames;           /* NUM_T2_FRAMES per super-frame, 0 = 2 */
    uint32_t frequency;      /* L1-post FREQUENCY field (Hz), 0 = 729833333 */
} dvbt2_cfg;

typedef struct dvbt2 dvbt2;

const char *dvbt2_cfg_check(const dvbt2_cfg *c);   /* NULL if valid else message */
double dvbt2_sample_rate(const dvbt2_cfg *c);       /* elementary rate (Hz) */
double dvbt2_ts_rate(const dvbt2_cfg *c);           /* exact useful TS rate (bit/s, 188-byte packets) */
double dvbt2_occupied_bw(const dvbt2_cfg *c);       /* Hz, active carriers */

/* Resolved parameters (defaults applied), for display / tests. */
typedef struct dvbt2_info {
    int data_symbols, fec_blocks, ti_blocks, n_p2;
    int frame_samples;        /* P1 + (N_P2 + L_data) * (FFT + GI) */
    int cells_per_fec;
    double frame_seconds;
} dvbt2_info;
int dvbt2_get_info(const dvbt2_cfg *c, dvbt2_info *info);   /* 0, or -1 if invalid */

/* Frame timing: carriers with equal timings can share one synthesis (dvbt2_group). */
typedef struct dvbt2_timing_t {
    int fft, guard;          /* samples at the elementary rate */
    int n_p2, l_data;
    int frame_samples;       /* P1 + (N_P2 + L_data) * (fft + guard), elementary rate */
    int t_num, t_den;        /* elementary period T = t_num / t_den microseconds */
    double sample_rate;
} dvbt2_timing_t;
int dvbt2_timing(const dvbt2_cfg *c, dvbt2_timing_t *t);              /* 0, or -1 if invalid */
int dvbt2_timing_equal(const dvbt2_timing_t *a, const dvbt2_timing_t *b);

dvbt2 *dvbt2_new(const dvbt2_cfg *c, ts_pull_fn pull, void *ctx);
void dvbt2_free(dvbt2 *t);
/* Writes the next chunk of time-domain samples (P1, then P2/data symbols with their guard
 * interval) and returns how many (<= max, never more than one OFDM symbol; call again for
 * more). The first call encodes a whole T2 frame; afterwards the FEC work for frame k+1 is
 * spread over the symbols of frame k, so packets are pulled at a steady pace.
 * (It is a dvbt2_group with k = 1 and one member at offset 0.) */
int dvbt2_samples(dvbt2 *t, float complex *out, int max);

/* ---- several frame-synchronous DVB-T2 carriers synthesised by one IFFT at the radio rate ----
 * Radio rate = k * dvbt2_sample_rate(cfg). Every symbol of the group is one IFFT of size
 * k * fft (guard interval k * guard), the P1 one IFFT of size 1024 k per segment; each member's
 * carriers sit at their offset and follow the phase of a continuous oscillator at that offset,
 * so the output equals, at the elementary sampling instants, the sum of the members'
 * dvbt2_samples() outputs each multiplied by exp(j 2 pi offset t). */
typedef struct dvbt2_group dvbt2_group;
enum {
    DVBT2_GROUP_EINVAL = -1,   /* dvbt2_cfg_check() fails */
    DVBT2_GROUP_ETIMING = -2,  /* different frame timing than the first member */
    DVBT2_GROUP_EOVERLAP = -3,
    DVBT2_GROUP_EBAND = -4,    /* carriers outside +-(k fft / 2) bins */
    DVBT2_GROUP_EP1 = -5,      /* offset_bins * 1024 / fft not an integer (P1 subcarrier grid) */
    DVBT2_GROUP_EFULL = -6,    /* more than 64 members */
    DVBT2_GROUP_ENOMEM = -7,
};
/* k: integer oversampling 1..64. threads: 0 = everything in the caller's thread, inside
 * dvbt2_group_samples(); n > 0 = n worker threads created here run the members' FEC encoding and
 * carrier placement for step s+1 while the caller's thread transforms step s (member i on worker
 * i % n). Each member's pull callback is then called from its worker thread, asynchronously
 * with the caller (never concurrently with itself). */
dvbt2_group *dvbt2_group_new(int k, int threads);
/* offset_bins: centre carrier relative to the radio LO, in data subcarrier spacings
 * (radio_rate / (fft * k)); must be a multiple of fft / 1024 (P1 grid). gain: linear amplitude
 * of this member (gain 1: mean power ~1 like dvbt2_samples). The first member fixes the timing
 * (bandwidth, FFT, guard interval, L_data); constellation, code rate, rotation, FEC blocks, pilot
 * pattern, carrier mode, L1 and ids may differ. Returns the member index or a DVBT2_GROUP_E*.
 * The first call plans the IFFT (FFTW_MEASURE, under the library's planner mutex). */
int dvbt2_group_add(dvbt2_group *g, const dvbt2_cfg *c, int offset_bins, float gain, ts_pull_fn pull, void *ctx);
const char *dvbt2_group_error(int err);
/* Next chunk of the summed signal at the radio rate: <= max samples, at most one step (P1 of
 * 2048 k samples or one symbol of k (fft + guard)); pulls like dvbt2_samples for each member. */
int dvbt2_group_samples(dvbt2_group *g, float complex *out, int max);
void dvbt2_group_free(dvbt2_group *g);

#endif
