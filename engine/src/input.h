/* SPDX-License-Identifier: GPL-3.0-or-later
 * UDP inputs (one SPTS each, usually from an ffmpeg ingest process, or pushed from elsewhere).
 * The receive thread demuxes PAT/PMT, keeps the packets of the selected program and gives
 * each one an ideal departure time, interpolated between PCRs and anchored on the arrival
 * times plus a fixed buffer: the multiplexer can then send them smoothly, whatever the jitter
 * of the network or the bursts of the source. Published to a lock-free broadcast ring that
 * several services may read (a shared backup stream). */
#ifndef DVBTX_INPUT_H
#define DVBTX_INPUT_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include "ts.h"

#define IN_MAX_ES 32
#define IN_DISC 0x01      /* timeline discontinuity before this packet */

typedef struct es_info {
    uint16_t pid;
    uint8_t type;
    uint8_t desc_len;
    uint8_t desc[250];
} es_info;

typedef struct prog_info {
    int valid;
    int program_number;
    int pcr_pid;
    int nes;
    es_info es[IN_MAX_ES];
    int pinfo_len;
    uint8_t pinfo[250];
} prog_info;

typedef struct in_entry {
    int64_t t_ideal;     /* CLOCK_MONOTONIC ns at which the packet should leave */
    int64_t pcr;         /* source clock (27 MHz, unwrapped) at that packet */
    uint8_t flags;
    uint8_t pkt[TS_PKT];
} in_entry;

typedef struct input {
    int id;
    char name[64];
    char addr[160];
    int fd;
    int program;             /* program_number to select, 0 = first */
    int64_t buffer_ns;       /* fixed delay between arrival and ideal departure */

    in_entry *ring;
    uint64_t mask;
    _Atomic uint64_t head;   /* entries published */

    pthread_mutex_t lock;    /* protects prog */
    prog_info prog;
    _Atomic int prog_gen;

    /* statistics, read by the stats thread */
    _Atomic uint64_t rx_bytes, rx_pkts, kept_pkts, cc_errors, discont, dropped_nopcr;
    _Atomic int64_t last_rx_ns, last_pub_ns;
    _Atomic int64_t slack_min_ns;          /* min (ideal - arrival) over the last window */
    _Atomic int64_t pcr_interval_max_ns;
    _Atomic int faster_than_realtime;

    struct in_priv *priv;    /* receive-thread state */
} input;

input *input_new(int id, const char *name, const char *addr, int program, int buffer_ms,
                 int ring_pkts, char *err, int errlen);
/* starts the receive thread for all inputs */
int inputs_start(input **in, int n);
void inputs_stop(void);

/* consumer side */
static inline uint64_t input_head(input *in)
{
    return atomic_load_explicit(&in->head, memory_order_acquire);
}
/* copies entry pos; returns 0 ok, 1 not yet available, -1 overwritten (consumer too slow) */
int input_read(input *in, uint64_t pos, in_entry *out);
void input_get_prog(input *in, prog_info *out, int *gen);

#endif
