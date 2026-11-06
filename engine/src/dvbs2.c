/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dvbs2.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "dvbs2x_tables.h"
#include "util.h"

struct dvbs2 {
    dvbs2_cfg c;
    const struct dvbs2x_modcod *x; /* DVB-S2X MODCOD, NULL for DVB-S2 */
    dvb_fec fec;
    int nldpc, nsym, slots, npilot;
    int rowaddr[8];
    float complex constel[256];
    float complex plh[90];
    uint8_t *cscram;           /* scrambling of the slot and pilot symbols */
    float complex rot[4 * 256]; /* constellation point sym scrambled by r: rot[r << bits | sym] */
    float complex prot[4];     /* pilot scrambled by r */
    uint8_t *symv;             /* symbol indices of one frame (90 x slots rounded up + 8) */
    uint8_t cw[8100 + 8];      /* + padding for the byte-wise column reads, zeros: the 128APSK
                                * interleaver reads 6 bits past the codeword */
    float complex *frame;      /* one PLFRAME */
    int flen, fpos;
};

static const int qpsk_rates[11][2] = { { 1, 4 }, { 1, 3 }, { 2, 5 }, { 1, 2 }, { 3, 5 }, { 2, 3 },
                                       { 3, 4 }, { 4, 5 }, { 5, 6 }, { 8, 9 }, { 9, 10 } };
static const int psk8_rates[6][2] = { { 3, 5 }, { 2, 3 }, { 3, 4 }, { 5, 6 }, { 8, 9 }, { 9, 10 } };
static const int apsk16_rates[6][2] = { { 2, 3 }, { 3, 4 }, { 4, 5 }, { 5, 6 }, { 8, 9 }, { 9, 10 } };
static const int apsk32_rates[5][2] = { { 3, 4 }, { 4, 5 }, { 5, 6 }, { 8, 9 }, { 9, 10 } };

/* index of the code rate in the list of the modulation, -1 if not allowed */
static int rate_index(const dvbs2_cfg *c)
{
    const int(*t)[2];
    int n;
    switch (c->bits) {
    case 2: t = qpsk_rates; n = 11; break;
    case 3: t = psk8_rates; n = 6; break;
    case 4: t = apsk16_rates; n = 6; break;
    case 5: t = apsk32_rates; n = 5; break;
    default: return -1;
    }
    for (int i = 0; i < n; i++)
        if (t[i][0] == c->cr_num && t[i][1] == c->cr_den)
            return i;
    return -1;
}

/* the DVB-S2X MODCOD (not a DVB-S2 one) of a configuration, NULL if none */
static const struct dvbs2x_modcod *s2x_modcod(const dvbs2_cfg *c)
{
    if (rate_index(c) >= 0)
        return NULL;
    for (unsigned i = 0; i < sizeof dvbs2x_modcods / sizeof dvbs2x_modcods[0]; i++) {
        const struct dvbs2x_modcod *m = &dvbs2x_modcods[i];
        if (m->bits == c->bits && m->num == c->cr_num && m->den == c->cr_den &&
            m->short_frame == (c->short_frame ? 1 : 0))
            return m;
    }
    return NULL;
}

static int modcod(const dvbs2_cfg *c)
{
    int i = rate_index(c);
    return c->bits == 2 ? 1 + i : c->bits == 3 ? 12 + i : c->bits == 4 ? 18 + i : 24 + i;
}

const char *dvbs2_cfg_check(const dvbs2_cfg *c)
{
    if (rate_index(c) < 0 && !s2x_modcod(c))
        return "DVB-S2: code rate not defined for this modulation and frame size (DVB-S2: QPSK "
               "1/4-9/10, 8PSK 3/5-9/10, 16APSK 2/3-9/10, 32APSK 3/4-9/10; DVB-S2X: see EN 302 307-2 "
               "table 17a/17b)";
    static const double rolloffs[] = { 0.35, 0.25, 0.20, 0.15, 0.10, 0.05 };
    int ro_ok = 0;
    for (int i = 0; i < 6; i++)
        ro_ok |= fabs(c->rolloff - rolloffs[i]) < 1e-3;
    if (!ro_ok)
        return "DVB-S2: roll-off 0.35, 0.25, 0.20 (DVB-S2X: 0.15, 0.10, 0.05)";
    if (c->short_frame && c->cr_num == 9 && c->cr_den == 10)
        return "DVB-S2: no 9/10 with short frames";
    if (c->gold_code < 0 || c->gold_code > 262141)
        return "DVB-S2: gold code 0..262141";
    dvb_fec_code fc;
    if (dvb_fec_code_get(c->short_frame, c->cr_num, c->cr_den, DVB_FEC_S2, &fc))
        return "DVB-S2: unknown code";
    return NULL;
}

static void geometry(const dvbs2_cfg *c, int *nldpc, int *nsym, int *slots, int *npilot)
{
    *nldpc = c->short_frame ? 16200 : 64800;
    *nsym = (*nldpc + c->bits - 1) / c->bits;   /* 128APSK: 9258 symbols ... */
    *slots = (*nsym + 89) / 90;                 /* ... in 103 slots, 12 dummy symbols */
    *npilot = c->pilots ? ((*slots - 1) / 16) * 36 : 0;
}

double dvbs2_ts_rate(const dvbs2_cfg *c, double rs)
{
    dvb_fec_code fc;
    if (dvb_fec_code_get(c->short_frame, c->cr_num, c->cr_den, DVB_FEC_S2, &fc))
        return 0;
    int nldpc, nsym, slots, npilot;
    geometry(c, &nldpc, &nsym, &slots, &npilot);
    return (double)(fc.kbch - 80) * rs / (90 + 90 * slots + npilot);
}

static float complex polar(double r, double a) { return (float)(r * cos(a)) + (float)(r * sin(a)) * I; }

static void build_constellation(dvbs2 *s)
{
    if (s->x) {
        for (int k = 0; k < (1 << s->c.bits); k++)
            s->constel[k] = s->x->pts[k][0] + s->x->pts[k][1] * I;
        return;
    }
    const double P = M_PI;
    float complex *m = s->constel;
    int i = rate_index(&s->c);
    if (s->c.bits == 2) {
        m[0] = polar(1, P / 4); m[1] = polar(1, 7 * P / 4); m[2] = polar(1, 3 * P / 4); m[3] = polar(1, 5 * P / 4);
    } else if (s->c.bits == 3) {
        m[0] = polar(1, P / 4); m[1] = polar(1, 0); m[2] = polar(1, P); m[3] = polar(1, 5 * P / 4);
        m[4] = polar(1, P / 2); m[5] = polar(1, 7 * P / 4); m[6] = polar(1, 3 * P / 4); m[7] = polar(1, 3 * P / 2);
    } else if (s->c.bits == 4) {
        static const double gamma[6] = { 3.15, 2.85, 2.75, 2.70, 2.60, 2.57 };
        double r2 = 1, r1 = r2 / gamma[i], r0 = sqrt(4.0 / (r1 * r1 + 3.0 * r2 * r2));
        r1 *= r0;
        r2 *= r0;
        const double a2[12] = { P / 4, -P / 4, 3 * P / 4, -3 * P / 4, P / 12, -P / 12, 11 * P / 12, -11 * P / 12,
                                5 * P / 12, -5 * P / 12, 7 * P / 12, -7 * P / 12 };
        for (int k = 0; k < 12; k++)
            m[k] = polar(r2, a2[k]);
        m[12] = polar(r1, P / 4); m[13] = polar(r1, -P / 4); m[14] = polar(r1, 3 * P / 4); m[15] = polar(r1, -3 * P / 4);
    } else {
        static const double g1[5] = { 5.27, 4.87, 4.64, 4.33, 4.30 }, g2[5] = { 2.84, 2.72, 2.64, 2.54, 2.53 };
        double r3 = 1, r1 = r3 / g1[i], r2 = r1 * g2[i], r0 = sqrt(8.0 / (r1 * r1 + 3.0 * r2 * r2 + 4.0 * r3 * r3));
        r1 *= r0;
        r2 *= r0;
        r3 *= r0;
        const double R[32] = { r2, r2, r2, r2, r2, r2, r2, r2, r3, r3, r3, r3, r3, r3, r3, r3,
                               r2, r1, r2, r1, r2, r1, r2, r1, r3, r3, r3, r3, r3, r3, r3, r3 };
        const double A[32] = { P / 4, 5 * P / 12, -P / 4, -5 * P / 12, 3 * P / 4, 7 * P / 12, -3 * P / 4, -7 * P / 12,
                               P / 8, 3 * P / 8, -P / 4, -P / 2, 3 * P / 4, P / 2, -7 * P / 8, -5 * P / 8,
                               P / 12, P / 4, -P / 12, -P / 4, 11 * P / 12, 3 * P / 4, -11 * P / 12, -3 * P / 4,
                               0, P / 4, -P / 8, -3 * P / 8, 7 * P / 8, 5 * P / 8, P, -3 * P / 4 };
        for (int k = 0; k < 32; k++)
            m[k] = polar(R[k], A[k]);
    }
}

static int parity18(long a) { return __builtin_parityl(a & 0x3FFFF); }

static void build_plheader(dvbs2 *s)
{
    static const unsigned long g[7] = { 0x90AC2DDD, 0x55555555, 0x33333333, 0x0F0F0F0F, 0x00FF00FF,
                                        0x0000FFFF, 0xFFFFFFFF };
    static const int scram[64] = { 0, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 1, 1, 0, 0, 0, 0, 0,
                                   1, 1, 1, 1, 0, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 0, 1, 0, 0,
                                   0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0 };
    static const int sof[26] = { 0, 1, 1, 0, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0 };
    int b[90];
    for (int i = 0; i < 26; i++)
        b[i] = sof[i];
    int type = (s->c.short_frame ? 2 : 0) | (s->c.pilots ? 1 : 0);
    /* DVB-S2X: the 8-bit PLS code of the MODCOD (frame size implied), pilot bit 0 */
    unsigned code = s->x ? (unsigned)(s->x->pls | (s->c.pilots ? 1 : 0))
                         : (unsigned)((modcod(&s->c) << 2) | type);
    unsigned long t = 0;
    for (int k = 0; k < 7; k++)
        if (code & (0x80u >> k))
            t ^= g[k];
    for (int m = 0; m < 32; m++) {
        b[26 + 2 * m] = (t >> (31 - m)) & 1;
        b[26 + 2 * m + 1] = b[26 + 2 * m] ^ (int)(code & 1);
    }
    for (int m = 0; m < 64; m++)
        b[26 + m] ^= scram[m];
    const double a = M_PI / 4;
    for (int i = 0; i < 90; i++) {   /* pi/2-BPSK */
        if ((i & 1) == 0)
            s->plh[i] = b[i] ? polar(1, 5 * a) : polar(1, a);
        else
            s->plh[i] = b[i] ? ((float)cos(a) + (float)sin(5 * a) * I) : ((float)cos(5 * a) + (float)sin(a) * I);
    }
    if (code & 0x80)     /* DVB-S2X PLS codes: the PLS part rotated by 90 degrees (EN 302 307-2 5.5.2) */
        for (int i = 26; i < 90; i++)
            s->plh[i] *= I;
}

static void build_scrambler(dvbs2 *s, int n)
{
    long x = 0x00001, y = 0x3FFFF;
    for (int k = 0; k < s->c.gold_code; k++) {
        int xb = parity18(x & 0x0081);
        x >>= 1;
        if (xb)
            x |= 0x20000;
    }
    s->cscram = xcalloc(n, 1);
    for (int i = 0; i < n; i++) {
        int xa = parity18(x & 0x8050), xb = parity18(x & 0x0081), xc = x & 1;
        x >>= 1;
        if (xb)
            x |= 0x20000;
        int ya = parity18(y & 0x04A1), yb = parity18(y & 0xFF60), yc = y & 1;
        y >>= 1;
        if (ya)
            y |= 0x20000;
        s->cscram[i] = (uint8_t)(((xa ^ yb) << 1) | (xc ^ yc));
    }
}

static inline float complex scramble(float complex v, int r)
{
    switch (r) {
    case 1: return -cimagf(v) + crealf(v) * I;
    case 2: return -v;
    case 3: return cimagf(v) - crealf(v) * I;
    default: return v;
    }
}

/* spread[x]: byte k (memory order) = bit 7 - k of x (MSB first), 0 or 1 */
static uint64_t spread[256];
static pthread_once_t spread_once = PTHREAD_ONCE_INIT;

static void build_spread(void)
{
    for (int x = 0; x < 256; x++) {
        uint64_t v = 0;
        for (int k = 0; k < 8; k++)
            v |= (uint64_t)((x >> (7 - k)) & 1) << (8 * k);   /* little-endian host assumed */
        spread[x] = v;
    }
}

dvbs2 *dvbs2_new(const dvbs2_cfg *c, ts_pull_fn pull, void *ctx)
{
    if (dvbs2_cfg_check(c))
        return NULL;
    dvbs2 *s = xcalloc(1, sizeof *s);
    s->c = *c;
    s->x = s2x_modcod(c);
    dvb_fec_code fc;
    dvb_fec_code_get(c->short_frame, c->cr_num, c->cr_den, DVB_FEC_S2, &fc);
    int ro = fabs(c->rolloff - 0.25) < 1e-3   ? 1
             : fabs(c->rolloff - 0.20) < 1e-3 ? 2
             : fabs(c->rolloff - 0.15) < 1e-3 ? 4
             : fabs(c->rolloff - 0.10) < 1e-3 ? 5
             : fabs(c->rolloff - 0.05) < 1e-3 ? 6
                                              : 0;
    if (dvb_fec_init(&s->fec, &fc, ro, pull, ctx)) {
        free(s);
        return NULL;
    }
    geometry(c, &s->nldpc, &s->nsym, &s->slots, &s->npilot);
    int rows = s->nsym;
    for (int k = 0; k < c->bits; k++)
        s->rowaddr[k] = k * rows;
    if (c->bits == 3 && c->cr_num == 3 && c->cr_den == 5) {     /* 8PSK 3/5: columns 2 1 0 */
        s->rowaddr[0] = 2 * rows;
        s->rowaddr[1] = rows;
        s->rowaddr[2] = 0;
    }
    if (s->x && s->x->order[0] >= 0)                            /* DVB-S2X column orders */
        for (int k = 0; k < c->bits; k++)
            s->rowaddr[k] = s->x->order[k] * rows;
    build_constellation(s);
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < (1 << c->bits); k++)
            s->rot[r << c->bits | k] = scramble(s->constel[k], r);
    for (int r = 0; r < 4; r++)
        s->prot[r] = scramble(polar(1, M_PI / 4), r);
    pthread_once(&spread_once, build_spread);
    s->symv = xcalloc((size_t)(90 * s->slots + 15) / 8 * 8 + 8, 1);
    build_plheader(s);
    build_scrambler(s, 90 * s->slots + s->npilot);
    s->flen = 90 + 90 * s->slots + s->npilot;
    s->frame = xcalloc(s->flen, sizeof(float complex));
    s->fpos = s->flen;
    return s;
}

void dvbs2_free(dvbs2 *s)
{
    if (!s)
        return;
    dvb_fec_free(&s->fec);
    free(s->cscram);
    free(s->symv);
    free(s->frame);
    free(s);
}

/* symbol indices: bit b (MSB first) of symbol j is codeword bit rowaddr[b] + j for 8PSK and
 * above (column-wise bit interleaver), bit 2 j + b for QPSK */
static void demux_symbols(dvbs2 *s)
{
    const uint8_t *cw = s->cw;
    uint8_t *sv = s->symv;
    int nsym = s->nsym, bits = s->c.bits;
    if (bits == 2) {
        for (int i = 0; i < nsym / 4; i++) {
            unsigned v = cw[i];
            sv[4 * i] = (uint8_t)(v >> 6);
            sv[4 * i + 1] = (uint8_t)((v >> 4) & 3);
            sv[4 * i + 2] = (uint8_t)((v >> 2) & 3);
            sv[4 * i + 3] = (uint8_t)(v & 3);
        }
        return;
    }
    /* 8 symbols at a time, all their bits in one pass: one byte of each column spread over the
     * 8 symbol bytes at the bit of that column */
    int ngrp = (nsym + 7) / 8;
    const uint8_t *col[8];
    int sh[8], ls[8];
    for (int b = 0; b < bits; b++) {
        col[b] = cw + (s->rowaddr[b] >> 3);
        sh[b] = 8 - (s->rowaddr[b] & 7);
        ls[b] = bits - 1 - b;
    }
    for (int g = 0; g < ngrp; g++) {
        uint64_t o = 0;
        for (int b = 0; b < bits; b++) {
            const uint8_t *p = col[b] + g;
            o |= spread[((unsigned)(p[0] << 8 | p[1]) >> sh[b]) & 0xff] << ls[b];
        }
        memcpy(sv + 8 * g, &o, 8);
    }
    for (int j = nsym; j < 90 * s->slots; j++)   /* 128APSK: 12 dummy symbols ending the frame, as gr-dtv */
        sv[j] = 0x7f;
}

static void next_frame(dvbs2 *s)
{
    dvb_fec_frame(&s->fec, s->cw, 0);
    demux_symbols(s);
    float complex *o = s->frame;
    memcpy(o, s->plh, sizeof s->plh);
    o += 90;
    const uint8_t *sv = s->symv, *cs = s->cscram;
    const float complex *rot = s->rot;
    int bits = s->c.bits;
    for (int slot = 0; slot < s->slots; slot++) {
        for (int k = 0; k < 90; k++)
            o[k] = rot[cs[k] << bits | sv[k]];
        o += 90;
        sv += 90;
        cs += 90;
        if (s->c.pilots && (slot + 1) % 16 == 0 && slot < s->slots - 1) {
            for (int p = 0; p < 36; p++)
                o[p] = s->prot[cs[p]];
            o += 36;
            cs += 36;
        }
    }
    s->fpos = 0;
}

void dvbs2_symbol(void *ctx, float complex *sym)
{
    dvbs2 *s = ctx;
    if (s->fpos == s->flen)
        next_frame(s);
    *sym = s->frame[s->fpos++];
}

void dvbs2_symbols(void *ctx, float complex *sym, int n)
{
    dvbs2 *s = ctx;
    while (n > 0) {
        if (s->fpos == s->flen)
            next_frame(s);
        int c = s->flen - s->fpos;
        if (c > n)
            c = n;
        memcpy(sym, s->frame + s->fpos, sizeof(float complex) * c);
        s->fpos += c;
        sym += c;
        n -= c;
    }
}
