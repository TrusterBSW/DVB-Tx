/* SPDX-License-Identifier: GPL-3.0-or-later
 * DVB-S2 (ETSI EN 302 307-1) symbol source: CCM, one TS, normal mode; normal or short FECFRAMEs,
 * QPSK 1/4..9/10, 8PSK 3/5..9/10, 16APSK 2/3..9/10, 32APSK 3/4..9/10, pilots on/off, Gold code n.
 * DVB-S2X (EN 302 307-2, VL-SNR excepted): the other MODCODs, up to 256APSK, chosen by the
 * number of bits per symbol and the code rate (the constellation variant follows: 8APSK for 8-ary
 * 100/180 and 104/180, 8+8APSK for 16-ary 90/180..20/30, 4+12+16 / 4+8+4+16 APSK for 32-ary,
 * 8+16+20+20 / 4+12+20+28 APSK for 64-ary), with their PLS codes, constellations and bit
 * interleaving from dvbs2x_tables.h; roll-off 0.15, 0.10, 0.05 too.
 * FEC from dvb_fec (BBFRAME, BCH, LDPC); bit interleaver, mapping and PL framing derived from GNU
 * Radio gr-dtv (dvbs2_interleaver_bb, dvbs2_modulator_bc, dvbs2_physical_cc; Copyright Free
 * Software Foundation, Inc. / Ron Economos). Shaped by scgen (RRC 0.35, 0.25 or 0.20). */
#ifndef DVBTX_DVBS2_H
#define DVBTX_DVBS2_H

#include <complex.h>

#include "dvb_fec.h"

typedef struct dvbs2_cfg {
    int bits;                /* 2 QPSK, 3 8PSK, 4 16APSK, 5 32APSK, 6 64APSK, 7 128APSK, 8 256APSK */
    int cr_num, cr_den;
    int short_frame;
    int pilots;
    double rolloff;          /* 0.35, 0.25, 0.20; DVB-S2X: 0.15, 0.10, 0.05 */
    int gold_code;           /* 0..262141 */
} dvbs2_cfg;

typedef struct dvbs2 dvbs2;

const char *dvbs2_cfg_check(const dvbs2_cfg *c);
double dvbs2_ts_rate(const dvbs2_cfg *c, double symbol_rate);
dvbs2 *dvbs2_new(const dvbs2_cfg *c, ts_pull_fn pull, void *ctx);
void dvbs2_free(dvbs2 *s);
void dvbs2_symbol(void *ctx, float complex *sym);   /* sym_fn: one symbol at the symbol rate */
void dvbs2_symbols(void *ctx, float complex *sym, int n);   /* the next n symbols */

#endif
