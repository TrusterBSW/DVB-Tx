/* SPDX-License-Identifier: GPL-3.0-or-later
 * Frequency-domain synthesis of single-carrier signals (DVB-C, DVB-S, DVB-S2) at the radio rate,
 * several carriers sharing one IFFT (fast-convolution filter bank, overlap-save):
 *   for each block, every member's symbols go through an FFT of L = N * Rs / Fs points, are
 *   weighted by the exact root-raised-cosine response, placed at the member's frequency in the
 *   N-point spectrum of the radio, and one IFFT gives N samples of which the N - V central ones
 *   are kept (V = overlap, about 256 symbols of the slowest member: no truncation of the RRC).
 * Requires Rs / Fs rational with a small denominator (the planner chooses Fs accordingly);
 * other offsets: the nearest bin, the rest (< Fs / 2N, ~500 Hz) as a rotation of the symbols. */
#ifndef DVBTX_FCBANK_H
#define DVBTX_FCBANK_H

#include <complex.h>

#include "scgen.h"     /* sym_fn */

typedef struct fcbank fcbank;

/* NULL if Rs / Fs is not a usable rational for this radio rate; df (Hz, 0 = none): the block
 * size is chosen, when possible, so that offsets multiple of df fall exactly on a bin */
fcbank *fcbank_new(double fs, double symbol_rate, double df);
enum { FC_RRC, FC_FLAT };
/* adds a member; -1 if its symbol rate does not fit the bank's block sizes.
 * FC_RRC: root-raised-cosine shaping of unit-power symbols (unit output power times gain);
 * FC_FLAT: resampling of a signal sampled at symbol_rate whose content is within
 * +-rolloff * symbol_rate / 2 (unit gain in that band, cosine taper up to symbol_rate / 2) */
int fcbank_add(fcbank *b, double symbol_rate, int pulse, double rolloff, double offset_hz, float gain, sym_fn get,
               void *ctx);
/* n samples of the sum of the members */
void fcbank_block(fcbank *b, float complex *out, int n);
void fcbank_free(fcbank *b);
int fcbank_members(const fcbank *b);
/* frequency offset of member i (as synthesised) */
double fcbank_offset(const fcbank *b, int i);
/* block sizes, for the logs */
void fcbank_sizes(const fcbank *b, int i, int *N, int *H, int *L, int *h);

#endif
