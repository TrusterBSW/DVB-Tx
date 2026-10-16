/* SPDX-License-Identifier: GPL-3.0-or-later
 * DVB-C (ETSI EN 300 429 = ITU-T J.83 annex A) symbol source: outer coding (shared with DVB-T),
 * byte to m-tuple, differential coding of the two MSBs, 16/64/256-QAM mapping. Shaped and
 * placed on the radio by scgen (RRC rolloff 0.15). */
#ifndef DVBTX_DVBC_H
#define DVBTX_DVBC_H

#include <complex.h>

#include "dvbt.h"

typedef struct dvbc dvbc;

dvbc *dvbc_new(int bits, ts_pull_fn pull, void *ctx);    /* bits: 4, 6, 8 */
void dvbc_free(dvbc *c);
void dvbc_symbol(void *ctx, float complex *sym);           /* sym_fn */
double dvbc_ts_rate(double symbol_rate, int bits);

#endif
