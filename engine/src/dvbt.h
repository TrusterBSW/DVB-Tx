/* SPDX-License-Identifier: GPL-3.0-or-later
 * DVB-T (ETSI EN 300 744) channel coding and frame building, non-hierarchical.
 * Produces, one OFDM symbol at a time, the Kmax+1 active carriers (data, pilots, TPS);
 * the IFFT is done by the OFDM synthesiser so that several carriers can share it. */
#ifndef DVBTX_DVBT_H
#define DVBTX_DVBT_H

#include <complex.h>
#include <stdint.h>

#include "outer.h"

typedef struct dvbt_cfg {
    int fft;          /* 2048 (2k) or 8192 (8k) */
    int bits;         /* bits per cell: 2 QPSK, 4 16-QAM, 6 64-QAM */
    int cr_k, cr_n;   /* code rate k/n: 1/2 2/3 3/4 5/6 7/8 */
    int gi;           /* guard interval denominator: 4 8 16 32 */
    double bw_hz;     /* channel bandwidth: 5e6 6e6 7e6 8e6 */
    int cell_id;      /* -1: not signalled */
} dvbt_cfg;

/* Pull one 188-byte TS packet (the multiplexer always delivers one, null if idle). */
typedef void (*ts_pull_fn)(void *ctx, uint8_t *pkt);

typedef struct dvbt dvbt;

const char *dvbt_cfg_check(const dvbt_cfg *c);   /* NULL if valid, else a message */
double dvbt_sample_rate(const dvbt_cfg *c);      /* elementary rate 8/7 * BW */
double dvbt_ts_rate(const dvbt_cfg *c);          /* useful TS bit rate */
int dvbt_ncarriers(const dvbt_cfg *c);           /* Kmax + 1: 1705 or 6817 */
double dvbt_mean_power(const dvbt_cfg *c);       /* mean sum |c_k|^2 of one symbol */

dvbt *dvbt_new(const dvbt_cfg *c, ts_pull_fn pull, void *ctx);
void dvbt_free(dvbt *t);
/* Fills car[0..Kmax] for the next symbol, returns the symbol index in the frame (0..67). */
int dvbt_symbol(dvbt *t, float complex *car);

#endif
