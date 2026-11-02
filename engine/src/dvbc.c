/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dvbc.h"

#include <math.h>
#include <stdlib.h>

#include "outer.h"
#include "util.h"

struct dvbc {
    outer o;
    int m, q;
    float complex constel[256];
    uint8_t cw[RS_LEN];
    int pos;                    /* next byte of cw */
    uint32_t acc;
    int nbits;
    int rot;                    /* rotation (quarter turns) of the last symbol */
    ts_pull_fn pull;
    void *ctx;
};

double dvbc_ts_rate(double symbol_rate, int bits) { return symbol_rate * bits * 188.0 / 204.0; }

static int gray_decode(const int *bits, int n)
{
    int v = 0, acc = 0;
    for (int i = 0; i < n; i++) {
        acc ^= bits[i];
        v = (v << 1) | acc;
    }
    return v;
}

/* Quadrant 1 of the cross constellations (EN 300 429 figures 7 and 8): odd coordinates (I, Q)
 * of the q LSBs b(q-1)..b0 */
static const int8_t q1_32[8][2] = {
    [0x0] = { 1, 1 }, [0x1] = { 3, 1 }, [0x3] = { 5, 1 }, [0x4] = { 1, 3 },
    [0x5] = { 3, 3 }, [0x7] = { 5, 3 }, [0x6] = { 1, 5 }, [0x2] = { 3, 5 },
};
static const int8_t q1_128[32][2] = {
    [0x00] = { 1, 1 },  [0x01] = { 3, 1 },  [0x05] = { 5, 1 },  [0x04] = { 7, 1 },  [0x0C] = { 9, 1 },  [0x0D] = { 11, 1 },
    [0x02] = { 1, 3 },  [0x03] = { 3, 3 },  [0x07] = { 5, 3 },  [0x06] = { 7, 3 },  [0x0E] = { 9, 3 },  [0x0F] = { 11, 3 },
    [0x12] = { 1, 5 },  [0x13] = { 3, 5 },  [0x17] = { 5, 5 },  [0x16] = { 7, 5 },  [0x1E] = { 9, 5 },  [0x1F] = { 11, 5 },
    [0x10] = { 1, 7 },  [0x11] = { 3, 7 },  [0x15] = { 5, 7 },  [0x14] = { 7, 7 },  [0x1C] = { 9, 7 },  [0x1D] = { 11, 7 },
    [0x18] = { 1, 9 },  [0x19] = { 3, 9 },  [0x09] = { 5, 9 },  [0x08] = { 7, 9 },
    [0x1A] = { 1, 11 }, [0x1B] = { 3, 11 }, [0x0B] = { 5, 11 }, [0x0A] = { 7, 11 },
};

/* EN 300 429 figures 7/8 (square ones as validated with a cable modem in mini-cmts): index = Ik Qk
 * then the q LSBs; for the square constellations, in quadrant 1 the Q coordinate uses the odd LSBs,
 * I the even ones, both Gray coded; the cross ones (32, 128) come from the tables above; the
 * other quadrants are rotations of quadrant 1 (table 1). */
static void build_constellation(dvbc *c)
{
    static const float complex rot[4] = { 1, -I, I, -1 };   /* IkQk 00: 0, 01: -pi/2, 10: +pi/2, 11: pi */
    int q = c->q, n = 1 << c->m;
    double pw = 0;
    for (int s = 0; s < n; s++) {
        int iq = s >> q, l = s & ((1 << q) - 1);
        float x, y;
        if (c->m == 5) {
            x = q1_32[l][0];
            y = q1_32[l][1];
        } else if (c->m == 7) {
            x = q1_128[l][0];
            y = q1_128[l][1];
        } else {
            int lsb[8], ib[4], qb[4], ni = 0, nq = 0;
            for (int i = 0; i < q; i++)
                lsb[i] = (l >> (q - 1 - i)) & 1;
            for (int i = 0; i < q; i++) {
                if (i % 2 == 0)
                    qb[nq++] = lsb[i];
                else
                    ib[ni++] = lsb[i];
            }
            x = 2.0f * gray_decode(ib, ni) + 1;
            y = 2.0f * gray_decode(qb, nq) + 1;
        }
        c->constel[s] = (x + y * I) * rot[iq];
        pw += x * x + y * y;
    }
    float k = (float)(1.0 / sqrt(pw / n));
    for (int s = 0; s < n; s++)
        c->constel[s] *= k;
}

dvbc *dvbc_new(int bits, ts_pull_fn pull, void *ctx)
{
    if (bits < 4 || bits > 8)
        return NULL;
    dvbc *c = xcalloc(1, sizeof *c);
    outer_init(&c->o);
    c->m = bits;
    c->q = bits - 2;
    c->pos = RS_LEN;
    c->pull = pull;
    c->ctx = ctx;
    build_constellation(c);
    return c;
}

void dvbc_free(dvbc *c) { free(c); }

void dvbc_symbol(void *ctx, float complex *sym)
{
    static const int drot_of_ab[4] = { 0, 3, 1, 2 };          /* Ak Bk -> rotation increment */
    static const int iq_of_rot[4] = { 0, 2, 3, 1 };           /* rotation -> Ik Qk */
    dvbc *c = ctx;
    while (c->nbits < c->m) {
        if (c->pos == RS_LEN) {
            uint8_t pkt[TS_LEN];
            c->pull(c->ctx, pkt);
            outer_packet(&c->o, pkt, c->cw);
            c->pos = 0;
        }
        c->acc = (c->acc << 8) | c->cw[c->pos++];
        c->nbits += 8;
    }
    int s = (int)(c->acc >> (c->nbits - c->m)) & ((1 << c->m) - 1);
    c->nbits -= c->m;
    c->rot = (c->rot + drot_of_ab[s >> c->q]) & 3;
    *sym = c->constel[(iq_of_rot[c->rot] << c->q) | (s & ((1 << c->q) - 1))];
}
