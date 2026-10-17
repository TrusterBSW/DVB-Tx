/* SPDX-License-Identifier: GPL-3.0-or-later
 * Carriers and radios: one thread per carrier (multiplexer + channel coding, a few symbols
 * ahead), one thread per radio (OFDM synthesis of all its carriers + output). The radio's
 * sample counter is tracked against CLOCK_MONOTONIC so that the multiplexer knows when each
 * of its packets will really be on air. A carrier without radio ("ts" standard) only paces
 * its multiplex out to UDP or a file, for an external modulator. */
#ifndef DVBTX_RF_H
#define DVBTX_RF_H

#include "conf.h"
#include "input.h"
#include "mux.h"

int rf_setup(const conf *cf, input **inputs, int ninputs, char *err, int errlen);
int rf_start(void);
void rf_stop(void);
void rf_reload_tables(void);
int rf_running(void);
int rf_stalled(int64_t now, int64_t timeout_ns);
mux *rf_carrier_mux(int ci);     /* NULL: no such carrier */
/* appends "radios":[...],"carriers":[...] to buf */
int rf_stats_json(char *buf, int len, double interval_s);

#endif
