/* SPDX-License-Identifier: GPL-3.0-or-later
 * DVB-S (ETSI EN 300 421) symbol source: outer coding (shared with DVB-T), convolutional code
 * K=7 (171, 133 octal) punctured to 1/2 2/3 3/4 5/6 7/8, QPSK. The punctured bit sequence is the
 * one of DVB-T (EN 300 744 table 2); DVB-S sends it as consecutive (I, Q) pairs. Shaped by scgen
 * with an RRC rolloff 0.35 (0.25 / 0.20 allowed by the receivers of DVB-S2). */
#ifndef DVBTX_DVBS_H
#define DVBTX_DVBS_H

#include <complex.h>

#include "dvbt.h"

typedef struct dvbs dvbs;

dvbs *dvbs_new(int cr_k, int cr_n, ts_pull_fn pull, void *ctx);
void dvbs_free(dvbs *s);
void dvbs_symbol(void *ctx, float complex *sym);         /* sym_fn */
double dvbs_ts_rate(double symbol_rate, int cr_k, int cr_n);

#endif
