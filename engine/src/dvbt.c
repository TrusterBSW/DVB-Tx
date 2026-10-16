/* SPDX-License-Identifier: GPL-3.0-or-later
 * DVB-T channel coding: inner convolutional code + puncturing (4.3.3), bit and symbol
 * interleavers (4.3.4), mapping (4.3.5), frame with continual/scattered pilots and TPS (4.4-4.6). */
#include "dvbt.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define SYMS_PER_FRAME 68
#define MAX_DATA 6048

/* 2k positions; the 8k ones are these repeated every 1704 carriers (EN 300 744 tables 7, 8) */
static const int cont_2k[45] = {
    0, 48, 54, 87, 141, 156, 192, 201, 255, 279, 282, 333, 432, 450, 483, 525, 531, 618, 636,
    714, 759, 765, 780, 804, 873, 888, 918, 939, 942, 969, 984, 1050, 1101, 1107, 1110, 1137,
    1140, 1146, 1206, 1269, 1323, 1377, 1491, 1683, 1704 };
static const int tps_2k[17] = {
    34, 50, 209, 346, 413, 569, 595, 688, 790, 901, 1073, 1219, 1262, 1286, 1469, 1594, 1687 };

enum { C_DATA = 0, C_PILOT = 1, C_TPS = 2 };

/* inner code, mother rate 1/2, K=7: X = 171o, Y = 133o; bit 0 of sr is the current input */
#define MASK_X_ 0x4Fu
#define MASK_Y_ 0x6Du

struct dvbt {
    dvbt_cfg c;
    ts_pull_fn pull;
    void *pull_ctx;
    outer outer;

    int kmax, ndata, v;
    int sym;                       /* symbol index in frame */
    int frame;                     /* frame index in superframe */

    uint8_t wk[6817];              /* pilot reference PRBS */
    uint8_t ctype[4][6817];        /* carrier type for symbol index mod 4 */
    uint16_t data_k[4][MAX_DATA];  /* data carrier positions for symbol index mod 4 */
    int ntps;
    uint16_t tps_k[68];
    float tps_val[68];             /* current DBPSK value of each TPS carrier */
    uint8_t tps_bits[4][SYMS_PER_FRAME];

    uint16_t H[MAX_DATA];          /* symbol interleaver permutation */
    float complex constel[64];

    /* fast paths, precomputed */
    uint16_t *dest[2][4];          /* [symbol parity][l mod 4]: carrier of the j-th bit-interleaver output */
    float complex *pilots[4];      /* carrier template (pilots, zeros elsewhere) for l mod 4 */
    uint16_t bsrc[126][6];         /* bit interleaver: source bit (within a 126*v block) of output w, bit e */
    uint16_t cc_tab[64][256];      /* convolutional coder: (state, byte) -> 8 X bits << 8 | 8 Y bits */

    /* inner coder input: outer coded bytes */
    uint8_t ib[16384];
    int ib_len;                    /* bytes present */
    long ib_bit;                   /* next bit to read */
    unsigned sr;                   /* convolutional encoder shift register */

    uint8_t coded[MAX_DATA * 6];   /* punctured bits of one symbol, one per byte */
    uint8_t ysym[MAX_DATA];
};

const char *dvbt_cfg_check(const dvbt_cfg *c)
{
    if (c->fft != 2048 && c->fft != 8192)
        return "DVB-T mode must be 2k or 8k";
    if (c->bits != 2 && c->bits != 4 && c->bits != 6)
        return "DVB-T constellation must be qpsk, 16qam or 64qam";
    int k = c->cr_k, n = c->cr_n;
    if (!((k == 1 && n == 2) || (k == 2 && n == 3) || (k == 3 && n == 4) || (k == 5 && n == 6) ||
          (k == 7 && n == 8)))
        return "DVB-T code rate must be 1/2, 2/3, 3/4, 5/6 or 7/8";
    if (c->gi != 4 && c->gi != 8 && c->gi != 16 && c->gi != 32)
        return "DVB-T guard interval must be 1/4, 1/8, 1/16 or 1/32";
    if (c->bw_hz < 1e6 || c->bw_hz > 10e6)
        return "DVB-T bandwidth must be 5, 6, 7 or 8 MHz";
    return NULL;
}

double dvbt_sample_rate(const dvbt_cfg *c) { return c->bw_hz * 8.0 / 7.0; }
int dvbt_ncarriers(const dvbt_cfg *c) { return c->fft == 8192 ? 6817 : 1705; }

double dvbt_ts_rate(const dvbt_cfg *c)
{
    double ndata = c->fft == 8192 ? 6048 : 1512;
    double tsym = (c->fft + c->fft / c->gi) / dvbt_sample_rate(c);
    double info_bits = ndata * c->bits * c->cr_k / c->cr_n;
    return info_bits * 188.0 / 204.0 / tsym;
}

double dvbt_mean_power(const dvbt_cfg *c)
{
    int nc = dvbt_ncarriers(c), nd = c->fft == 8192 ? 6048 : 1512, nt = c->fft == 8192 ? 68 : 17;
    return nd + nt + (nc - nd - nt) * 16.0 / 9.0;
}

static void build_tps(dvbt *t)
{
    static const int cr_code[9] = { 0, 0, 0, 1, 2, 0, 3, 0, 4 }; /* indexed by n: 2,3,4,6,8 */
    for (int m = 0; m < 4; m++) {
        uint8_t *s = t->tps_bits[m];
        memset(s, 0, SYMS_PER_FRAME);
        int pos = 0;
#define PUT(val, nbits) do { for (int b_ = (nbits) - 1; b_ >= 0; b_--) s[pos++] = ((val) >> b_) & 1; } while (0)
        PUT(t->wk[0], 1);                                   /* s0: reference, not used */
        PUT((m & 1) ? 0xCA11 : 0x35EE, 16);                 /* s1-s16 sync word */
        PUT(t->c.cell_id >= 0 ? 0x1F : 0x17, 6);            /* s17-s22 length indicator */
        PUT(m, 2);                                          /* s23-s24 frame number */
        PUT(t->c.bits == 2 ? 0 : t->c.bits == 4 ? 1 : 2, 2); /* s25-s26 constellation */
        PUT(0, 3);                                          /* s27-s29 non-hierarchical */
        PUT(cr_code[t->c.cr_n], 3);                         /* s30-s32 HP code rate */
        PUT(cr_code[t->c.cr_n], 3);                         /* s33-s35 LP code rate */
        PUT(t->c.gi == 32 ? 0 : t->c.gi == 16 ? 1 : t->c.gi == 8 ? 2 : 3, 2); /* s36-s37 */
        PUT(t->c.fft == 8192 ? 1 : 0, 2);                   /* s38-s39 transmission mode */
        int cid = t->c.cell_id >= 0 ? t->c.cell_id : 0;
        PUT((m & 1) ? (cid & 0xFF) : ((cid >> 8) & 0xFF), 8); /* s40-s47 cell id */
        PUT(0, 6);                                          /* s48-s53 */
#undef PUT
        /* s54-s67: BCH(67,53) shortened from BCH(127,113), g = x^14+x^9+x^8+x^6+x^5+x^4+x^2+x+1 */
        unsigned reg = 0;
        for (int i = 0; i < 113; i++) {
            unsigned in = i < 60 ? 0 : s[1 + i - 60];
            unsigned fb = (in ^ reg) & 1;
            reg >>= 1;
            reg |= fb << 13;
            if (fb)
                reg ^= (1u << 12) | (1u << 11) | (1u << 9) | (1u << 8) | (1u << 7) | (1u << 5) | (1u << 4);
        }
        for (int i = 0; i < 14; i++)
            s[54 + i] = (reg >> i) & 1;
    }
}

static void build_interleaver(dvbt *t)
{
    static const int perm2k[10] = { 9, 6, 4, 1, 0, 7, 3, 8, 5, 2 };   /* R bit j <- R' bit perm[j] */
    static const int perm8k[12] = { 8, 1, 3, 9, 2, 11, 5, 0, 6, 4, 7, 10 };
    int nr = t->c.fft == 8192 ? 13 : 11;
    const int *perm = t->c.fft == 8192 ? perm8k : perm2k;
    unsigned rp = 0;
    int q = 0;
    for (int i = 0; i < t->c.fft; i++) {
        if (i < 2)
            rp = 0;
        else if (i == 2)
            rp = 1;
        else {
            unsigned fb;
            if (nr == 11)
                fb = (rp ^ (rp >> 3)) & 1;
            else
                fb = (rp ^ (rp >> 1) ^ (rp >> 4) ^ (rp >> 6)) & 1;
            rp = (rp >> 1) | (fb << (nr - 2));
        }
        unsigned h = (unsigned)(i & 1) << (nr - 1);
        for (int j = 0; j < nr - 1; j++)
            h |= ((rp >> perm[j]) & 1u) << j;
        if ((int)h < t->ndata)
            t->H[q++] = (uint16_t)h;
    }
}

static void build_constellation(dvbt *t)
{
    int v = t->v;
    double norm = v == 2 ? sqrt(2.0) : v == 4 ? sqrt(10.0) : sqrt(42.0);
    for (int idx = 0; idx < (1 << v); idx++) {
        int y[6];
        for (int b = 0; b < v; b++)
            y[b] = (idx >> (v - 1 - b)) & 1;          /* y0 is the MSB of the index */
        double re, im;
        if (v == 2) {
            re = 1;
            im = 1;
        } else if (v == 4) {
            re = 2 + (1 - 2 * y[2]);
            im = 2 + (1 - 2 * y[3]);
        } else {
            re = 4 + (1 - 2 * y[2]) * (2 + (1 - 2 * y[4]));
            im = 4 + (1 - 2 * y[3]) * (2 + (1 - 2 * y[5]));
        }
        re *= 1 - 2 * y[0];
        im *= 1 - 2 * y[1];
        t->constel[idx] = (float)(re / norm) + (float)(im / norm) * I;
    }
}

dvbt *dvbt_new(const dvbt_cfg *c, ts_pull_fn pull, void *ctx)
{
    if (dvbt_cfg_check(c))
        return NULL;
    dvbt *t = xcalloc(1, sizeof *t);
    t->c = *c;
    t->pull = pull;
    t->pull_ctx = ctx;
    outer_init(&t->outer);
    t->kmax = dvbt_ncarriers(c) - 1;
    t->ndata = c->fft == 8192 ? 6048 : 1512;
    t->v = c->bits;

    unsigned reg = 0x7FF;                       /* X^11 + X^2 + 1, all ones */
    for (int k = 0; k <= t->kmax; k++) {
        t->wk[k] = reg & 1;
        unsigned nb = ((reg >> 2) ^ reg) & 1;
        reg = (reg >> 1) | (nb << 10);
    }

    int reps = c->fft == 8192 ? 4 : 1;
    uint8_t base[6817];
    memset(base, C_DATA, sizeof base);
    t->ntps = 0;
    for (int r = 0; r < reps; r++) {
        for (int i = 0; i < 45; i++)
            base[cont_2k[i] + 1704 * r] = C_PILOT;
        for (int i = 0; i < 17; i++)
            t->tps_k[t->ntps++] = (uint16_t)(tps_2k[i] + 1704 * r);
    }
    for (int i = 0; i < t->ntps; i++)
        base[t->tps_k[i]] = C_TPS;
    for (int l = 0; l < 4; l++) {
        memcpy(t->ctype[l], base, t->kmax + 1);
        for (int k = 3 * l; k <= t->kmax; k += 12)
            t->ctype[l][k] = C_PILOT;
        int n = 0;
        for (int k = 0; k <= t->kmax; k++)
            if (t->ctype[l][k] == C_DATA) {
                if (n >= MAX_DATA)
                    die("dvbt: data carrier count overflow");
                t->data_k[l][n++] = (uint16_t)k;
            }
        if (n != t->ndata)
            die("dvbt: %d data carriers in symbol %d, expected %d", n, l, t->ndata);
    }
    build_tps(t);
    build_interleaver(t);
    build_constellation(t);

    /* symbol interleaver + carrier placement as one permutation per symbol parity */
    for (int l = 0; l < 4; l++) {
        for (int par = 0; par < 2; par++) {
            uint16_t *d = t->dest[par][l] = xcalloc(MAX_DATA, sizeof(uint16_t));
            for (int q = 0; q < t->ndata; q++) {
                if (par == 0)
                    d[q] = t->data_k[l][t->H[q]];      /* even: y[H(q)] = y'[q] */
                else
                    d[t->H[q]] = t->data_k[l][q];      /* odd:  y[q] = y'[H(q)] */
            }
        }
        float complex *p = t->pilots[l] = xcalloc(t->kmax + 1, sizeof(float complex));
        for (int k = 0; k <= t->kmax; k++)
            p[k] = t->ctype[l][k] == C_PILOT ? (4.0f / 3.0f) * (1 - 2 * (int)t->wk[k]) : 0.0f;
    }
    /* bit interleaver: a[e][w] = b[e][(w + h_e) mod 126], b[dmx[i]][w] = x[w*v + i] */
    {
        static const int h[6] = { 0, 63, 105, 42, 21, 84 };
        static const int dmx2[2] = { 0, 1 }, dmx4[4] = { 0, 2, 1, 3 }, dmx6[6] = { 0, 2, 4, 1, 3, 5 };
        int v = t->v, inv[6];
        const int *dmx = v == 2 ? dmx2 : v == 4 ? dmx4 : dmx6;
        for (int i = 0; i < v; i++)
            inv[dmx[i]] = i;
        for (int w = 0; w < 126; w++)
            for (int e = 0; e < v; e++)
                t->bsrc[w][e] = (uint16_t)(((w + h[e]) % 126) * v + inv[e]);
    }
    /* convolutional coder, 8 input bits at a time */
    for (unsigned st = 0; st < 64; st++)
        for (unsigned byte = 0; byte < 256; byte++) {
            unsigned sr = st, x = 0, y = 0;
            for (int b = 7; b >= 0; b--) {
                sr = ((sr << 1) | ((byte >> b) & 1)) & 0x7F;
                x = (x << 1) | (unsigned)__builtin_parity(sr & MASK_X_);
                y = (y << 1) | (unsigned)__builtin_parity(sr & MASK_Y_);
            }
            t->cc_tab[st][byte] = (uint16_t)(x << 8 | y);
        }
    return t;
}

void dvbt_free(dvbt *t)
{
    if (!t)
        return;
    for (int l = 0; l < 4; l++) {
        free(t->dest[0][l]);
        free(t->dest[1][l]);
        free(t->pilots[l]);
    }
    free(t);
}

static void refill(dvbt *t, long need_bits)
{
    long avail = (long)t->ib_len * 8 - t->ib_bit;
    if (avail >= need_bits)
        return;
    int skip = (int)(t->ib_bit >> 3);
    memmove(t->ib, t->ib + skip, t->ib_len - skip);
    t->ib_len -= skip;
    t->ib_bit -= (long)skip * 8;
    uint8_t pkt[TS_LEN];
    while ((long)t->ib_len * 8 - t->ib_bit < need_bits) {
        t->pull(t->pull_ctx, pkt);
        outer_packet(&t->outer, pkt, t->ib + t->ib_len);
        t->ib_len += RS_LEN;
    }
}

static void inner_code(dvbt *t)
{
    static const char *punct[9] = { 0, 0, "XY", "XYY", "XYYX", 0, "XYYXYX", 0, "XYYYYXYX" };
    static const int pidx[9][8] = { { 0 }, { 0 }, { 0, 0 }, { 0, 0, 1 }, { 0, 0, 1, 2 }, { 0 },
                                    { 0, 0, 1, 2, 3, 4 }, { 0 }, { 0, 0, 1, 2, 3, 4, 5, 6 } };
    int k = t->c.cr_k, n = t->c.cr_n;
    int ncoded = t->ndata * t->v, periods = ncoded / n;
    long nbits = (long)periods * k;
    refill(t, nbits);
    /* X and Y bits of all the info bits of this symbol, one per byte, then punctured */
    static __thread uint8_t xs[MAX_DATA * 6], ys[MAX_DATA * 6];
    unsigned sr = t->sr;
    long bit = t->ib_bit, i = 0;
    const uint8_t *ib = t->ib;
    while (i < nbits && (bit & 7)) {           /* up to a byte boundary */
        sr = ((sr << 1) | ((ib[bit >> 3] >> (7 - (bit & 7))) & 1)) & 0x7F;
        xs[i] = (uint8_t)__builtin_parity(sr & MASK_X_);
        ys[i] = (uint8_t)__builtin_parity(sr & MASK_Y_);
        i++, bit++;
    }
    while (i + 8 <= nbits) {                   /* whole bytes through the table */
        uint8_t byte = ib[bit >> 3];
        unsigned xy = t->cc_tab[sr & 0x3F][byte];
        sr = ((sr << 8) | byte) & 0x7F;
        for (int b = 0; b < 8; b++) {
            xs[i + b] = (uint8_t)((xy >> (15 - b)) & 1);
            ys[i + b] = (uint8_t)((xy >> (7 - b)) & 1);
        }
        i += 8, bit += 8;
    }
    while (i < nbits) {
        sr = ((sr << 1) | ((ib[bit >> 3] >> (7 - (bit & 7))) & 1)) & 0x7F;
        xs[i] = (uint8_t)__builtin_parity(sr & MASK_X_);
        ys[i] = (uint8_t)__builtin_parity(sr & MASK_Y_);
        i++, bit++;
    }
    t->sr = sr;
    t->ib_bit = bit;
    const char *pat = punct[n];
    const int *pi = pidx[n];
    uint8_t *out = t->coded;
    for (int p = 0; p < periods; p++) {
        const uint8_t *xp = xs + (long)p * k, *yp = ys + (long)p * k;
        for (int j = 0; j < n; j++)
            *out++ = pat[j] == 'X' ? xp[pi[j]] : yp[pi[j]];
    }
}

/* demux + 126-bit block interleavers (4.3.4.1): one v-bit word per data cell */
static void interleave(dvbt *t)
{
    int v = t->v, nblk = t->ndata / 126;
    for (int blk = 0; blk < nblk; blk++) {
        const uint8_t *x = t->coded + blk * 126 * v;
        uint8_t *y = t->ysym + blk * 126;
        for (int w = 0; w < 126; w++) {
            const uint16_t *src = t->bsrc[w];
            unsigned s = 0;
            for (int e = 0; e < v; e++)
                s = (s << 1) | x[src[e]];
            y[w] = (uint8_t)s;
        }
    }
}

int dvbt_symbol(dvbt *t, float complex *car)
{
    inner_code(t);
    interleave(t);

    int l = t->sym, lm = l & 3;
    /* pilots, then the data cells through symbol interleaver + carrier map in one permutation */
    memcpy(car, t->pilots[lm], sizeof(float complex) * (t->kmax + 1));
    const uint16_t *dst = t->dest[l & 1][lm];
    const float complex *cst = t->constel;
    const uint8_t *ys = t->ysym;
    for (int j = 0; j < t->ndata; j++)
        car[dst[j]] = cst[ys[j]];
    const uint8_t *tb = t->tps_bits[t->frame];
    for (int i = 0; i < t->ntps; i++) {
        float vv;
        if (l == 0)
            vv = (float)(1 - 2 * (int)t->wk[t->tps_k[i]]);
        else
            vv = tb[l] ? -t->tps_val[i] : t->tps_val[i];
        t->tps_val[i] = vv;
        car[t->tps_k[i]] = vv;
    }

    if (++t->sym == SYMS_PER_FRAME) {
        t->sym = 0;
        t->frame = (t->frame + 1) & 3;
    }
    return l;
}
