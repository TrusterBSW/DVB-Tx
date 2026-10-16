/* SPDX-License-Identifier: GPL-3.0-or-later
 * Multiplexer of one carrier, pulled by its modulator at the exact TS rate of the channel:
 * each output slot gets, by priority, a due PSI/SI packet, the most urgent service packet
 * (earliest ideal time), a PCR-only packet if a PCR is overdue, or a null packet.
 * PCRs are restamped from the radio's sample clock, PIDs remapped per service, PAT/PMT
 * rebuilt, and each service can fall back to a backup input when its primary goes silent. */
#ifndef DVBTX_MUX_H
#define DVBTX_MUX_H

#include <pthread.h>
#include <stdint.h>

#include "input.h"

/* emission time (CLOCK_MONOTONIC ns) of output slot n of this carrier */
typedef int64_t (*mux_clock_fn)(void *ctx, uint64_t slot);

typedef struct mux mux;

typedef struct mux_svc_cfg {
    char name[64];
    int sid, pid_base;
    input *primary, *backup;
    int switch_timeout_ms, return_ms, max_late_ms, early_ms, pcr_max_ms;
    int pmt_period_ms;
    int priority;                  /* 1..10: under overload, higher ones are served first */
} mux_svc_cfg;

typedef struct mux_table_cfg {
    char path[512];
    int pid, cycle_ms;
} mux_table_cfg;

typedef struct mux_cfg {
    int carrier;
    int tsid;
    double ts_rate;
    int pat_period_ms, tdt_period_ms, tot_period_ms;
    char tot_desc_path[512];       /* local_time_offset_descriptor bytes, "" = no TOT */
    int nit_pid;                    /* listed in the PAT, 0 = none */
    char monitor[1024];             /* udp://host:port[,udp://...] copies of the output, "" = none */
} mux_cfg;

mux *mux_new(const mux_cfg *c, mux_clock_fn clock, void *clock_ctx);
int mux_add_service(mux *m, const mux_svc_cfg *s);
int mux_add_table(mux *m, const mux_table_cfg *t);
void mux_start(mux *m);          /* builds PAT, call after adding services */
void mux_pull(void *ctx, uint8_t *pkt);   /* ts_pull_fn */
void mux_reload_tables(mux *m);  /* re-read section files whose mtime changed */
/* JSON object with the carrier and service statistics since the previous call */
int mux_stats_json(mux *m, char *buf, int len, double interval_s);
double mux_ts_rate(const mux *m);

#endif
