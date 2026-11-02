/* SPDX-License-Identifier: GPL-3.0-or-later
 * DVB-S2 / DVB-T2 BBFRAME, BB scrambler, BCH and LDPC encoders.
 * Derived from GNU Radio gr-dtv (Copyright Free Software Foundation, Inc. / Ron Economos). */
#include "dvb_fec.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define DVBT2_TABLES_LDPC
#include "dvbt2_tables.h"

static const struct { int num, den, kbch_n, kldpc_n, par_n, kbch_s, kldpc_s; } rates[] = {
    { 1, 4, 16008, 16200, 192, 3072, 3240 },
    { 1, 3, 21408, 21600, 192, 5232, 5400 },
    { 2, 5, 25728, 25920, 192, 6312, 6480 },
    { 1, 2, 32208, 32400, 192, 7032, 7200 },
    { 3, 5, 38688, 38880, 192, 9552, 9720 },
    { 2, 3, 43040, 43200, 160, 10632, 10800 },
    { 3, 4, 48408, 48600, 192, 11712, 11880 },
    { 4, 5, 51648, 51840, 192, 12432, 12600 },
    { 5, 6, 53840, 54000, 160, 13152, 13320 },
    { 8, 9, 57472, 57600, 128, 14232, 14400 },
    { 9, 10, 58192, 58320, 128, 0, 0 },
};

int dvb_fec_code_get(int short_frame, int cr_num, int cr_den, int standard, dvb_fec_code *c)
{
    for (unsigned i = 0; i < sizeof rates / sizeof rates[0]; i++) {
        if (rates[i].num != cr_num || rates[i].den != cr_den)
            continue;
        memset(c, 0, sizeof *c);
        c->short_frame = short_frame ? 1 : 0;
        c->cr_num = cr_num;
        c->cr_den = cr_den;
        c->standard = standard;
        if (short_frame) {
            if (!rates[i].kbch_s)
                return -1;
            c->nldpc = 16200;
            c->kbch = rates[i].kbch_s;
            c->kldpc = rates[i].kldpc_s;
            c->nparity_bch = 168;
        } else {
            c->nldpc = 64800;
            c->kbch = rates[i].kbch_n;
            c->kldpc = rates[i].kldpc_n;
            c->nparity_bch = rates[i].par_n;
        }
        c->q = (c->nldpc - c->kldpc) / 360;
        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------ BBFRAME */

#define CRC_POLY 0xAB   /* header CRC-8, processed LSB first as gr-dtv does */
#define CRC_POLYR 0xD5  /* user packet CRC-8 (x^8+x^7+x^6+x^4+x^2+1), MSB first */

void dvb_bbframer_init(dvb_bbframer *b, int kbch, int ro, ts_pull_fn pull, void *ctx)
{
    memset(b, 0, sizeof *b);
    b->kbch = kbch;
    /* TS/GS = 11 (transport), SIS/MIS = 1, CCM = 1, ISSYI = 0, NPD = 0, RO */
    b->matype1 = (uint8_t)(0xC0 | 0x20 | 0x10 | (ro & 3));
    b->pull = pull;
    b->ctx = ctx;
    for (int i = 0; i < 256; i++) {
        int r = i, crc = 0;
        for (int j = 7; j >= 0; j--) {
            if (((r >> j) & 1) ^ ((crc & 0x80) ? 1 : 0))
                crc = ((crc << 1) ^ CRC_POLYR) & 0xff;
            else
                crc = (crc << 1) & 0xff;
        }
        b->crc_tab[i] = (uint8_t)crc;
    }
}

static uint8_t bitrev8(uint8_t x)
{
    x = (uint8_t)((x >> 4) | (x << 4));
    x = (uint8_t)(((x & 0xCC) >> 2) | ((x & 0x33) << 2));
    return (uint8_t)(((x & 0xAA) >> 1) | ((x & 0x55) << 1));
}

void dvb_bbframer_frame(dvb_bbframer *b, uint8_t *out)
{
    int dfl = b->kbch - 80;
    int syncd = b->count == 0 ? 0 : (188 - b->count) * 8;
    out[0] = b->matype1;
    out[1] = 0;                       /* MATYPE-2 (ISI), single stream */
    out[2] = (188 * 8) >> 8;          /* UPL */
    out[3] = (188 * 8) & 0xff;
    out[4] = (uint8_t)(dfl >> 8);     /* DFL */
    out[5] = (uint8_t)dfl;
    out[6] = 0x47;                    /* SYNC */
    out[7] = (uint8_t)(syncd >> 8);   /* SYNCD */
    out[8] = (uint8_t)syncd;
    unsigned crc = 0;
    for (int i = 0; i < 72; i++) {
        int bit = (out[i >> 3] >> (7 - (i & 7))) & 1;
        int fb = bit ^ (crc & 1);
        crc >>= 1;
        if (fb)
            crc ^= CRC_POLY;
    }
    out[9] = bitrev8((uint8_t)crc);   /* gr-dtv sends the CRC LSB first */

    uint8_t *d = out + 10;
    int n = dfl / 8, count = b->count;
    uint8_t c = b->crc;
    const uint8_t *pkt = b->pkt;
    for (int j = 0; j < n;) {
        if (count == 0) {
            b->pull(b->ctx, b->pkt);
            if (b->pkt[0] != 0x47)
                b->sync_errors++;
            d[j++] = c;               /* sync byte replaced by the CRC-8 of the previous packet */
            c = 0;
            count = 1;
        }
        int m = 188 - count;
        if (m > n - j)
            m = n - j;
        for (int k = 0; k < m; k++) {
            uint8_t v = pkt[count + k];
            d[j + k] = v;
            c = b->crc_tab[v ^ c];
        }
        j += m;
        count += m;
        if (count == 188)
            count = 0;
    }
    b->count = count;
    b->crc = c;
}

static uint8_t bb_prbs[8100];
static pthread_once_t bb_prbs_once = PTHREAD_ONCE_INIT;

static void bb_prbs_init(void)
{
    int sr = 0x4A80;
    for (int i = 0; i < 64800; i++) {
        int b = (sr ^ (sr >> 1)) & 1;
        bb_prbs[i >> 3] |= (uint8_t)(b << (7 - (i & 7)));
        sr >>= 1;
        if (b)
            sr |= 0x4000;
    }
}

void dvb_bbscramble(uint8_t *buf, int nbytes)
{
    pthread_once(&bb_prbs_once, bb_prbs_init);
    int i = 0;
    for (; i + 8 <= nbytes; i += 8) {
        uint64_t a, p;
        memcpy(&a, buf + i, 8);
        memcpy(&p, bb_prbs + i, 8);
        a ^= p;
        memcpy(buf + i, &a, 8);
    }
    for (; i < nbytes; i++)
        buf[i] ^= bb_prbs[i];
}

/* ------------------------------------------------------------------ BCH */

static int poly_mult(const int *a, int la, const int *b, int lb, int *out)
{
    memset(out, 0, sizeof(int) * (la + lb));
    for (int i = 0; i < la; i++)
        for (int j = 0; j < lb; j++)
            if (a[i] && b[j])
                out[i + j] ^= 1;
    int max = 0;
    for (int i = 0; i < la + lb; i++)
        if (out[i])
            max = i;
    return max + 1;
}

static const int bch_n[12][17] = {
    { 1, 0, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 },
    { 1, 1, 0, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1 },
    { 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1 },
    { 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 1 },
    { 1, 1, 1, 1, 0, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 1 },
    { 1, 0, 1, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1 },
    { 1, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 0, 1, 1 },
    { 1, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 1, 0, 1 },
    { 1, 0, 0, 0, 0, 1, 0, 1, 0, 1, 1, 1, 0, 0, 0, 0, 1 },
    { 1, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 1, 1, 1, 0, 1 },
    { 1, 0, 1, 1, 0, 1, 0, 0, 0, 1, 0, 1, 1, 1, 0, 0, 1 },
    { 1, 1, 0, 0, 0, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 0, 1 },
};
static const int bch_s[12][15] = {
    { 1, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1 },
    { 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 0, 1 },
    { 1, 1, 1, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 0, 1 },
    { 1, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 0, 1, 0, 1 },
    { 1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 1 },
    { 1, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1 },
    { 1, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 1, 1 },
    { 1, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1 },
    { 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 1 },
    { 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1 },
    { 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1 },
    { 1, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 0, 0, 1, 1 },
};

/* 192-bit register, w[0] most significant */
static void r192_shl1(uint64_t *w)
{
    w[0] = (w[0] << 1) | (w[1] >> 63);
    w[1] = (w[1] << 1) | (w[2] >> 63);
    w[2] <<= 1;
}

void dvb_bch_init(dvb_bch *b, int short_frame, int nparity)
{
    int pa[200], pb[200], len;
    int *cur = pa, *nxt = pb;
    int nfac = short_frame ? 12 : nparity / 16;   /* 8, 10 or 12 factors of degree 16 */
    if (short_frame) {
        len = poly_mult(bch_s[0], 15, bch_s[1], 15, cur);
        for (int i = 2; i < nfac; i++) {
            len = poly_mult(bch_s[i], 15, cur, len, nxt);
            int *t = cur; cur = nxt; nxt = t;
        }
    } else {
        len = poly_mult(bch_n[0], 17, bch_n[1], 17, cur);
        for (int i = 2; i < nfac; i++) {
            len = poly_mult(bch_n[i], 17, cur, len, nxt);
            int *t = cur; cur = nxt; nxt = t;
        }
    }
    (void)len;
    b->nparity = nparity;
    /* g(x) without x^n, left aligned: coefficient of x^i at register bit 191 - (n - 1 - i) */
    uint64_t g[3] = { 0, 0, 0 };
    for (int i = 0; i < nparity; i++)
        if (cur[i]) {
            int bit = 192 - nparity + i;
            g[2 - bit / 64] |= 1ULL << (bit % 64);
        }
    for (int v = 0; v < 256; v++) {
        uint64_t r[3] = { (uint64_t)v << 56, 0, 0 };
        for (int k = 0; k < 8; k++) {
            int top = (int)(r[0] >> 63);
            r192_shl1(r);
            if (top) {
                r[0] ^= g[0];
                r[1] ^= g[1];
                r[2] ^= g[2];
            }
        }
        b->tab[v][0] = r[0];
        b->tab[v][1] = r[1];
        b->tab[v][2] = r[2];
    }
}

void dvb_bch_encode(const dvb_bch *b, const uint8_t *in, int kbch, uint8_t *parity)
{
    uint64_t r0 = 0, r1 = 0, r2 = 0;
    int n = kbch / 8;
    for (int i = 0; i < n; i++) {
        const uint64_t *t = b->tab[(r0 >> 56) ^ in[i]];
        r0 = ((r0 << 8) | (r1 >> 56)) ^ t[0];
        r1 = ((r1 << 8) | (r2 >> 56)) ^ t[1];
        r2 = (r2 << 8) ^ t[2];
    }
    uint64_t r[3] = { r0, r1, r2 };
    for (int i = 0; i < b->nparity / 8; i++)
        parity[i] = (uint8_t)(r[i / 8] >> (56 - 8 * (i % 8)));
}

/* ------------------------------------------------------------------ LDPC
 * Information bit m = 360 * row + n uses parity addresses (x + n Q) mod (360 Q) for each x
 * of its table row. Writing x = x0 + Q x1, that is parity "row" x0 (of a Q x 360 matrix
 * whose element (r, c) is parity bit r + Q c) at column (x1 + n) mod 360: the row x0
 * receives the 360 information bits of the group rotated by x1. The accumulator
 * p_j ^= p_(j-1) becomes a prefix XOR over the rows plus a prefix XOR along the columns.
 * Row r of that matrix, read in column order, is exactly the T2 parity interleaver output. */

#define W6 6   /* 360 bits in 6 words */

int dvb_ldpc_init(dvb_ldpc *l, const dvb_fec_code *c)
{
    const struct dvb_ldpc_code_desc *d = NULL;
    for (unsigned i = 0; i < sizeof dvb_ldpc_codes / sizeof dvb_ldpc_codes[0]; i++) {
        const struct dvb_ldpc_code_desc *e = &dvb_ldpc_codes[i];
        if (e->short_frame != c->short_frame || e->num != c->cr_num || e->den != c->cr_den)
            continue;
        if (e->variant == 1 && c->standard != DVB_FEC_T2)
            continue;
        if (e->variant == 2 && c->standard != DVB_FEC_S2)
            continue;
        d = e;
        break;
    }
    memset(l, 0, sizeof *l);
    if (!d || d->rows * 360 != c->kldpc)
        return -1;
    l->nldpc = c->nldpc;
    l->kldpc = c->kldpc;
    l->q = c->q;
    l->rows = d->rows;
    int n = 0;
    const uint16_t *t = d->tab;
    for (int r = 0; r < d->rows; r++) {
        n += t[0];
        t += t[0] + 1;
    }
    l->nent = n;
    l->row_end = malloc(sizeof(uint32_t) * d->rows);
    l->ent = malloc(sizeof(uint32_t) * (n ? n : 1));
    if (!l->row_end || !l->ent) {
        dvb_ldpc_free(l);
        return -1;
    }
    t = d->tab;
    n = 0;
    for (int r = 0; r < d->rows; r++) {
        int cnt = t[0];
        for (int k = 1; k <= cnt; k++) {
            int x = t[k], x0 = x % l->q, x1 = x / l->q;
            int o = 360 - x1;                       /* window offset in the doubled group */
            l->ent[n++] = (uint32_t)x0 << 16 | (uint32_t)(o >> 6) << 8 | (uint32_t)(o & 63);
        }
        l->row_end[r] = (uint32_t)n;
        t += cnt + 1;
    }
    return 0;
}

void dvb_ldpc_free(dvb_ldpc *l)
{
    free(l->row_end);
    free(l->ent);
    l->row_end = NULL;
    l->ent = NULL;
}

static uint8_t rev_tab[256];
static pthread_once_t rev_once = PTHREAD_ONCE_INIT;

static void rev_init(void)
{
    for (int i = 0; i < 256; i++)
        rev_tab[i] = bitrev8((uint8_t)i);
}

/* 45 MSB-first bytes -> 360 bits LSB-first in 6 words */
static inline void load360(const uint8_t *p, uint64_t *w)
{
    uint8_t b[48];
    for (int i = 0; i < 45; i++)
        b[i] = rev_tab[p[i]];
    b[45] = b[46] = b[47] = 0;
    memcpy(w, b, 48);   /* little-endian host assumed (x86, ARM) */
}

static inline void store360(const uint64_t *w, uint8_t *p)
{
    uint8_t b[48];
    memcpy(b, w, 48);
    for (int i = 0; i < 45; i++)
        p[i] = rev_tab[b[i]];
}

#define MASK40 ((1ULL << 40) - 1)

void dvb_ldpc_encode(const dvb_ldpc *l, const uint8_t *in, uint8_t *parity, int parity_interleaved)
{
    int q = l->q;
    uint64_t P[180][W6];   /* q <= 135 */
    uint64_t dd[13];
    pthread_once(&rev_once, rev_init);
    memset(P, 0, sizeof(uint64_t) * W6 * q);
    uint32_t e0 = 0;
    for (int r = 0; r < l->rows; r++) {
        uint64_t D[W6];
        load360(in + 45 * r, D);
        /* dd = D | D << 360 (720 bits) */
        for (int k = 0; k < 5; k++)
            dd[k] = D[k];
        dd[5] = D[5] | (D[0] << 40);
        for (int k = 1; k < 6; k++)
            dd[5 + k] = (D[k - 1] >> 24) | (D[k] << 40);
        dd[11] = D[5] >> 24;
        dd[12] = 0;
        uint32_t e1 = l->row_end[r];
        for (uint32_t e = e0; e < e1; e++) {
            uint32_t v = l->ent[e];
            uint64_t *p = P[v >> 16];
            const uint64_t *s = dd + ((v >> 8) & 0xff);
            unsigned sh = v & 63;
            if (sh == 0) {
                for (int k = 0; k < W6; k++)
                    p[k] ^= s[k];
            } else {
                for (int k = 0; k < W6; k++)
                    p[k] ^= (s[k] >> sh) | (s[k + 1] << (64 - sh));
            }
        }
        e0 = e1;
    }
    /* accumulator: prefix XOR over rows, then the exclusive prefix XOR of the last row along c */
    P[0][5] &= MASK40;
    for (int r = 1; r < q; r++) {
        P[r][5] &= MASK40;
        for (int k = 0; k < W6; k++)
            P[r][k] ^= P[r - 1][k];
    }
    uint64_t I[W6], E[W6];
    uint64_t carry = 0;
    for (int k = 0; k < W6; k++) {
        uint64_t x = P[q - 1][k];
        x ^= x << 1;
        x ^= x << 2;
        x ^= x << 4;
        x ^= x << 8;
        x ^= x << 16;
        x ^= x << 32;
        if (carry)
            x = ~x;
        I[k] = x;
        carry = x >> 63;
    }
    E[0] = I[0] << 1;
    for (int k = 1; k < W6; k++)
        E[k] = (I[k] << 1) | (I[k - 1] >> 63);
    E[5] &= MASK40;
    for (int r = 0; r < q; r++)
        for (int k = 0; k < W6; k++)
            P[r][k] ^= E[k];

    if (parity_interleaved) {
        for (int r = 0; r < q; r++)
            store360(P[r], parity + 45 * r);
    } else {
        /* natural order: parity bit j = r + Q c */
        int j = 0;
        uint8_t acc = 0;
        for (int c = 0; c < 360; c++) {
            int wi = c >> 6, bi = c & 63;
            for (int r = 0; r < q; r++) {
                acc = (uint8_t)(acc << 1 | ((P[r][wi] >> bi) & 1));
                if ((++j & 7) == 0)
                    parity[(j >> 3) - 1] = acc;
            }
        }
    }
}

/* ------------------------------------------------------------------ all */

int dvb_fec_init(dvb_fec *f, const dvb_fec_code *c, int ro, ts_pull_fn pull, void *ctx)
{
    memset(f, 0, sizeof *f);
    f->code = *c;
    dvb_bbframer_init(&f->bb, c->kbch, ro, pull, ctx);
    dvb_bch_init(&f->bch, c->short_frame, c->nparity_bch);
    return dvb_ldpc_init(&f->ldpc, c);
}

void dvb_fec_free(dvb_fec *f)
{
    dvb_ldpc_free(&f->ldpc);
}

void dvb_fec_frame(dvb_fec *f, uint8_t *cw, int parity_interleaved)
{
    const dvb_fec_code *c = &f->code;
    dvb_bbframer_frame(&f->bb, cw);
    dvb_bbscramble(cw, c->kbch / 8);
    dvb_bch_encode(&f->bch, cw, c->kbch, cw + c->kbch / 8);
    dvb_ldpc_encode(&f->ldpc, cw, cw + c->kldpc / 8, parity_interleaved);
}
