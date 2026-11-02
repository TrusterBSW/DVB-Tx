/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "outer.h"

#include <string.h>

/* GF(256), primitive polynomial x^8+x^4+x^3+x^2+1 (0x11d), lambda = 0x02 */
static uint8_t gf_exp[512], gf_log[256];
static uint8_t rs_gen[17];            /* g(x) = prod_{i=0}^{15} (x + lambda^i), rs_gen[16] = 1 */
static uint8_t rs_mul[16][256];       /* rs_mul[j][v] = v * rs_gen[j] */

static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    return (a && b) ? gf_exp[gf_log[a] + gf_log[b]] : 0;
}

__attribute__((constructor)) static void outer_tables(void)
{
    unsigned x = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp[i] = (uint8_t)x;
        gf_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100)
            x ^= 0x11d;
    }
    for (int i = 255; i < 512; i++)
        gf_exp[i] = gf_exp[i - 255];

    memset(rs_gen, 0, sizeof rs_gen);
    rs_gen[0] = 1;
    for (int i = 0; i < 16; i++) {          /* multiply by (x + lambda^i) */
        uint8_t r = gf_exp[i];
        for (int j = i + 1; j > 0; j--)
            rs_gen[j] = rs_gen[j - 1] ^ gf_mul(rs_gen[j], r);
        rs_gen[0] = gf_mul(rs_gen[0], r);
    }
    for (int j = 0; j < 16; j++)
        for (int v = 0; v < 256; v++)
            rs_mul[j][v] = gf_mul((uint8_t)v, rs_gen[j]);
}

void outer_init(outer *o)
{
    memset(o, 0, sizeof *o);
    /* register stages 1..15 loaded with 100101010000000 */
    unsigned reg[16] = { 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0 };
    for (int pos = 1; pos < 8 * TS_LEN; pos++) {   /* the first sync byte does not clock it */
        uint8_t byte = 0;
        for (int b = 0; b < 8; b++) {
            unsigned out = reg[14] ^ reg[15];
            for (int s = 15; s > 1; s--)
                reg[s] = reg[s - 1];
            reg[1] = out;
            byte = (uint8_t)(byte << 1 | out);
        }
        /* sync bytes of packets 2..8 clock the PRBS but are not scrambled */
        o->prbs[pos] = (pos % TS_LEN == 0) ? 0 : byte;
    }
}

static void rs_encode(const uint8_t *msg, uint8_t *par)
{
    uint8_t p[16] = { 0 };
    for (int i = 0; i < TS_LEN; i++) {
        uint8_t fb = msg[i] ^ p[15];
        for (int j = 15; j > 0; j--)
            p[j] = p[j - 1] ^ rs_mul[j][fb];
        p[0] = rs_mul[0][fb];
    }
    for (int j = 0; j < 16; j++)
        par[j] = p[15 - j];
}

void outer_packet(outer *o, const uint8_t *in, uint8_t *out)
{
    uint8_t pkt[RS_LEN];
    const uint8_t *prbs = o->prbs + o->pkt_in_group * TS_LEN;
    pkt[0] = o->pkt_in_group == 0 ? 0xB8 : 0x47;
    for (int i = 1; i < TS_LEN; i++)
        pkt[i] = in[i] ^ prbs[i];
    o->pkt_in_group = (o->pkt_in_group + 1) & 7;
    rs_encode(pkt, pkt + TS_LEN);

    /* Forney interleaver: byte i goes through branch i mod 12, a FIFO of 17*branch bytes */
    int br = o->il_branch;
    for (int i = 0; i < RS_LEN; i++) {
        if (br == 0) {
            out[i] = pkt[i];
        } else {
            int len = br * 17, pos = o->il_pos[br];
            out[i] = o->il_buf[br][pos];
            o->il_buf[br][pos] = pkt[i];
            o->il_pos[br] = pos + 1 == len ? 0 : pos + 1;
        }
        br = br == 11 ? 0 : br + 1;
    }
    o->il_branch = br;
}
