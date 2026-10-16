/* SPDX-License-Identifier: GPL-3.0-or-later
 * Outer coding shared by DVB-T, DVB-C (J.83 annex A) and DVB-S:
 * energy dispersal (PRBS 1+X^14+X^15), Reed-Solomon (204,188,t=8),
 * Forney convolutional interleaver I=12, M=17. EN 300 744 clauses 4.3.1-4.3.2. */
#ifndef DVBTX_OUTER_H
#define DVBTX_OUTER_H

#include <stdint.h>

#define TS_LEN 188
#define RS_LEN 204

typedef struct outer {
    int pkt_in_group;            /* 0..7, position in the 8-packet PRBS period */
    uint8_t prbs[8 * TS_LEN];    /* PRBS byte for each byte position of the period */
    uint8_t il_buf[12][11 * 17]; /* convolutional interleaver branches (branch j: j*17 bytes) */
    int il_pos[12];
    int il_branch;
} outer;

void outer_init(outer *o);
/* One TS packet in (sync byte ignored, 0x47 assumed), 204 coded bytes out. */
void outer_packet(outer *o, const uint8_t *in, uint8_t *out);

#endif
