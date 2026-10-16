/* SPDX-License-Identifier: GPL-3.0-or-later
 * OFDM synthesiser shared by DVB-T carriers of the same mode, guard interval and bandwidth:
 * every carrier is placed on its own bins of one large IFFT running directly at the radio
 * sample rate, so there is no resampler, no mixer and no image to filter.
 * Requires: radio rate = nfft * subcarrier spacing, and offsets multiple of the spacing. */
#ifndef DVBTX_OFDM_H
#define DVBTX_OFDM_H

#include <complex.h>

typedef struct ofdm_group ofdm_group;

/* nfft: synthesis size (>= fft of the members), ncp: cyclic prefix in output samples */
ofdm_group *ofdm_group_new(int nfft, int ncp, int threads);
void ofdm_group_free(ofdm_group *g);
/* place a member: ncar active carriers, its centre carrier at bin_off from DC */
int ofdm_group_add(ofdm_group *g, int ncar, int bin_off, float gain);
/* cars[i] = carriers of member i for this symbol; writes nfft + ncp samples to out */
void ofdm_group_synth(ofdm_group *g, const float complex *const *cars, float complex *out);
int ofdm_group_len(const ofdm_group *g);

#endif
