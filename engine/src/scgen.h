/* SPDX-License-Identifier: GPL-3.0-or-later
 * Single-carrier generator (DVB-C, DVB-S): root-raised-cosine shaping, resampling from the symbol
 * rate to any radio rate and frequency shift, in one polyphase filter followed by an NCO.
 *   y(t) = sum_k s_k p(t - k)   evaluated at t = n * Rs / Fs, p = RRC (rolloff, span symbols)  */
#ifndef DVBTX_SCGEN_H
#define DVBTX_SCGEN_H

#include <complex.h>

/* next modulation symbol (unit mean power) */
typedef void (*sym_fn)(void *ctx, float complex *sym);

typedef struct scgen scgen;

enum { PULSE_RRC = 0, PULSE_LOWPASS = 1 };

/* pulse PULSE_RRC: rolloff = RRC rolloff; PULSE_LOWPASS: input = samples of a signal already band
 * limited (e.g. DVB-T2 at its elementary rate), rolloff = occupied fraction of that rate (0..1) */
scgen *scgen_new2(double symbol_rate, double sample_rate, double offset_hz, int pulse, double rolloff, int span,
                  float gain, sym_fn get, void *ctx);
scgen *scgen_new(double symbol_rate, double sample_rate, double offset_hz, double rolloff, int span,
                 float gain, sym_fn get, void *ctx);
void scgen_free(scgen *g);
/* writes n samples (unit power before gain) */
void scgen_block(scgen *g, float complex *out, int n);
/* occupied bandwidth */
static inline double scgen_bandwidth(double symbol_rate, double rolloff) { return symbol_rate * (1 + rolloff); }

#endif
