/* SPDX-License-Identifier: GPL-3.0-or-later
 * DVB-S2 / DVB-T2 outer coding: BBFRAME building (single TS stream, CCM, normal mode = DVB-T2
 * input mode A: CRC-8 in place of the sync bytes, no ISSY, no null packet deletion), BB
 * scrambling, BCH and LDPC encoding (EN 302 307-1 clause 5, EN 302 755 clause 5-6).
 * Derived from GNU Radio gr-dtv (Copyright Free Software Foundation, Inc. / Ron Economos):
 * dvb_bbheader_bb, dvb_bbscrambler_bb, dvb_bch_bb, dvb_ldpc_bb; bit-exact with them.
 *
 * Everything is bit-packed, MSB first (bit i of a frame is bit 7 - i % 8 of byte i / 8).
 * The LDPC encoder works on 360-bit words: the parity of each group of 360 information bits
 * is a cyclic rotation of the group, so a codeword costs a few thousand 64-bit XORs. */
#ifndef DVBTX_DVB_FEC_H
#define DVBTX_DVB_FEC_H

#include <stdint.h>

#ifndef DVBTX_TS_PULL_FN
#define DVBTX_TS_PULL_FN
/* Pull one 188-byte TS packet (the multiplexer always delivers one, null if idle). */
typedef void (*ts_pull_fn)(void *ctx, uint8_t *pkt);
#endif

enum { DVB_FEC_S2 = 0, DVB_FEC_T2 = 1 };

typedef struct dvb_fec_code {
    int short_frame;  /* 0: 64800 bits, 1: 16200 bits */
    int cr_num, cr_den;
    int standard;     /* DVB_FEC_S2 or DVB_FEC_T2 (they differ for normal 2/3 and short 3/5) */
    int nldpc;        /* codeword length */
    int kldpc;        /* LDPC information bits = Nbch */
    int kbch;         /* BCH information bits = BBFRAME length */
    int nparity_bch;  /* 128, 160, 192 (normal t = 8, 10, 12) or 168 (short) */
    int q;            /* LDPC Q parameter: parity bits = 360 * q */
} dvb_fec_code;

/* Fills c for a code rate; returns 0, or -1 if the rate does not exist for this frame size.
 * Normal: 1/4 1/3 2/5 1/2 3/5 2/3 3/4 4/5 5/6 8/9 9/10; short: the same without 9/10
 * (the short "1/2" is really 4/9 etc., as in the standards). */
int dvb_fec_code_get(int short_frame, int cr_num, int cr_den, int standard, dvb_fec_code *c);

/* ---- BBFRAME (header + data field, kbch bits) ---- */
typedef struct dvb_bbframer {
    int kbch;
    uint8_t matype1;          /* TS/GS=11 SIS CCM ISSYI=0 NPD=0 RO */
    int count;                /* position in the current TS packet of the next data field byte */
    uint8_t crc;              /* running CRC-8 of the current packet */
    uint8_t pkt[188];
    long sync_errors;         /* packets that did not start with 0x47 */
    ts_pull_fn pull;
    void *ctx;
    uint8_t crc_tab[4][256];  /* [k][x]: CRC-8 register x after k + 1 more zero bytes */
} dvb_bbframer;

/* ro: DVB-S2 roll-off code (0: 0.35, 1: 0.25, 2: 0.20), 0 for DVB-T2 */
void dvb_bbframer_init(dvb_bbframer *b, int kbch, int ro, ts_pull_fn pull, void *ctx);
/* Writes kbch / 8 bytes; pulls TS packets as needed ((kbch - 80) / 8 bytes of them). */
void dvb_bbframer_frame(dvb_bbframer *b, uint8_t *out);

/* XOR with the BB scrambling sequence (nbytes <= 8100) */
void dvb_bbscramble(uint8_t *buf, int nbytes);

/* ---- BCH ---- */
typedef struct dvb_bch {
    int nparity;              /* parity bits */
    uint64_t tab[4][256][3];  /* remainder of byte * x^(n + 8 k), left aligned in 192 bits */
} dvb_bch;

void dvb_bch_init(dvb_bch *b, int short_frame, int nparity);
/* in: kbch/8 bytes, parity: nparity/8 bytes (to append) */
void dvb_bch_encode(const dvb_bch *b, const uint8_t *in, int kbch, uint8_t *parity);

/* ---- LDPC ---- */
typedef struct dvb_ldpc {
    int nldpc, kldpc, q, rows;
    int nent;
    uint32_t *row_end;        /* rows entries: end of each row in ent[] */
    uint32_t *ent;            /* x0 << 16 | word << 8 | shift (see dvb_fec.c) */
} dvb_ldpc;

int dvb_ldpc_init(dvb_ldpc *l, const dvb_fec_code *c);   /* 0 or -1 */
void dvb_ldpc_free(dvb_ldpc *l);
/* in: kldpc/8 bytes; parity: (nldpc - kldpc)/8 bytes.
 * parity_interleaved = 0: natural order (DVB-S2); 1: DVB-T2 parity interleaving applied
 * (u[K + 360 t + s] = lambda[K + Q s + t], EN 302 755 clause 6.1.2), which is free here. */
void dvb_ldpc_encode(const dvb_ldpc *l, const uint8_t *in, uint8_t *parity, int parity_interleaved);

/* ---- all of it ---- */
typedef struct dvb_fec {
    dvb_fec_code code;
    dvb_bbframer bb;
    dvb_bch bch;
    dvb_ldpc ldpc;
} dvb_fec;

int dvb_fec_init(dvb_fec *f, const dvb_fec_code *c, int ro, ts_pull_fn pull, void *ctx);
void dvb_fec_free(dvb_fec *f);
/* One FECFRAME: nldpc/8 bytes (BBFRAME, BCH parity, LDPC parity). */
void dvb_fec_frame(dvb_fec *f, uint8_t *cw, int parity_interleaved);

#endif
