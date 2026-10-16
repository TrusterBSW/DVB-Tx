/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dvbs.h"

#include <stdlib.h>

#include "outer.h"
#include "util.h"

#define MASK_X 0x4Fu   /* 171 octal, bit 0 = current input */
#define MASK_Y 0x6Du   /* 133 octal */

struct dvbs {
    outer o;
    int k, n;
    const char *pat;
    const int *pidx;
    uint8_t cw[RS_LEN];
    int pos, bit;               /* next byte / bit of cw */
    unsigned sr;
    uint8_t out[8];             /* punctured bits of the current period */
    int nout, oread;
    ts_pull_fn pull;
    void *ctx;
};

double dvbs_ts_rate(double symbol_rate, int cr_k, int cr_n) { return symbol_rate * 2.0 * cr_k / cr_n * 188.0 / 204.0; }

dvbs *dvbs_new(int k, int n, ts_pull_fn pull, void *ctx)
{
    static const char *punct[9] = { 0, 0, "XY", "XYY", "XYYX", 0, "XYYXYX", 0, "XYYYYXYX" };
    static const int pidx[9][8] = { { 0 }, { 0 }, { 0, 0 }, { 0, 0, 1 }, { 0, 0, 1, 2 }, { 0 },
                                    { 0, 0, 1, 2, 3, 4 }, { 0 }, { 0, 0, 1, 2, 3, 4, 5, 6 } };
    if (!((k == 1 && n == 2) || (k == 2 && n == 3) || (k == 3 && n == 4) || (k == 5 && n == 6) || (k == 7 && n == 8)))
        return NULL;
    dvbs *s = xcalloc(1, sizeof *s);
    outer_init(&s->o);
    s->k = k;
    s->n = n;
    s->pat = punct[n];
    s->pidx = pidx[n];
    s->pos = RS_LEN;
    s->pull = pull;
    s->ctx = ctx;
    return s;
}

void dvbs_free(dvbs *s) { free(s); }

static unsigned next_bit(dvbs *s)
{
    if (s->pos == RS_LEN) {
        uint8_t pkt[TS_LEN];
        s->pull(s->ctx, pkt);
        outer_packet(&s->o, pkt, s->cw);
        s->pos = 0;
        s->bit = 0;
    }
    unsigned b = (s->cw[s->pos] >> (7 - s->bit)) & 1;
    if (++s->bit == 8) {
        s->bit = 0;
        s->pos++;
    }
    return b;
}

static unsigned coded_bit(dvbs *s)
{
    if (s->oread == s->nout) {            /* encode one puncturing period */
        uint8_t xs[8], ys[8];
        for (int i = 0; i < s->k; i++) {
            s->sr = ((s->sr << 1) | next_bit(s)) & 0x7F;
            xs[i] = (uint8_t)__builtin_parity(s->sr & MASK_X);
            ys[i] = (uint8_t)__builtin_parity(s->sr & MASK_Y);
        }
        for (int j = 0; j < s->n; j++)
            s->out[j] = s->pat[j] == 'X' ? xs[s->pidx[j]] : ys[s->pidx[j]];
        s->nout = s->n;
        s->oread = 0;
    }
    return s->out[s->oread++];
}

void dvbs_symbol(void *ctx, float complex *sym)
{
    dvbs *s = ctx;
    const float a = 0.70710678f;
    unsigned bi = coded_bit(s), bq = coded_bit(s);
    *sym = (bi ? -a : a) + (bq ? -a : a) * I;
}
