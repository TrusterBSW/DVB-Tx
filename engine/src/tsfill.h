/* SPDX-License-Identifier: GPL-3.0-or-later
 * Fill level of an external modulator's input buffers (edgeQAM board: GET /status, "queued" per
 * multiplex), polled by one thread per board whatever the number of its multiplexes, so that a TS output
 * can steer its rate on the modulator's clock. */
#ifndef DVBTX_TSFILL_H
#define DVBTX_TSFILL_H

#include <stdint.h>

#define TSFILL_MUX 16

typedef struct tsfill tsfill;

typedef struct tsfill_sample {
    uint64_t seq;          /* number of the poll, changes at each one (answered or not) */
    int ok;                /* the board answered and listed this multiplex */
    int64_t at_ns;         /* CLOCK_MONOTONIC of the answer */
    int64_t last_ok_ns;    /* last answer that listed it */
    int fails;             /* polls failed in a row since then */
    long queued;           /* packets waiting in its buffer */
    long null_inserted, dropped;   /* the board's counters (since it started) */
} tsfill_sample;

/* the poller of this URL (http://HOST:PORT/PATH), shared, started on the first call; NULL if bad.
 * tune: "" or a path (/set?freq=..&disable=..) sent to the board on its first answer and again whenever it
 * restarted (its "uptime_s" went down) or stopped answering: settings it does not keep (the first
 * caller's is used) */
tsfill *tsfill_get(const char *url, int period_ms, const char *tune);
/* a multiplex of this board on the air (+1) or stopped (-1): with none, the board is neither polled nor set */
void tsfill_use(tsfill *f, int delta);
/* latest poll for multiplex mux (1..TSFILL_MUX) */
void tsfill_read(tsfill *f, int mux, tsfill_sample *s);

#endif
