/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dvbs2.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

struct dvbs2 {
    dvbs2_cfg c;
    dvb_fec fec;
    int nldpc, nsym, slots, npilot;
    int rowaddr[5];
    float complex constel[32];
    float complex plh[90];
    uint8_t *cscram;           /* scrambling of the slot and pilot symbols */
    uint8_t cw[8100];
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

static int modcod(const dvbs2_cfg *c)
{
    int i = rate_index(c);
    return c->bits == 2 ? 1 + i : c->bits == 3 ? 12 + i : c->bits == 4 ? 18 + i : 24 + i;
}

const char *dvbs2_cfg_check(const dvbs2_cfg *c)
{
    if (rate_index(c) < 0)
        return "DVB-S2: code rate not defined for this modulation (QPSK 1/4-9/10, 8PSK 3/5-9/10, "
               "16APSK 2/3-9/10, 32APSK 3/4-9/10)";
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
    *nsym = *nldpc / c->bits;
    *slots = *nsym / 90;
    *npilot = c->pilots ? ((*slots - 1) / 16) * 36 : 0;
}

double dvbs2_ts_rate(const dvbs2_cfg *c, double rs)
{
    dvb_fec_code fc;
    if (dvb_fec_code_get(c->short_frame, c->cr_num, c->cr_den, DVB_FEC_S2, &fc))
        return 0;
    int nldpc, nsym, slots, npilot;
    geometry(c, &nldpc, &nsym, &slots, &npilot);
    return (double)(fc.kbch - 80) * rs / (90 + nsym + npilot);
}

static float complex polar(double r, double a) { return (float)(r * cos(a)) + (float)(r * sin(a)) * I; }

static void build_constellation(dvbs2 *s)
{
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
    unsigned code = (unsigned)((modcod(&s->c) << 2) | type);
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

dvbs2 *dvbs2_new(const dvbs2_cfg *c, ts_pull_fn pull, void *ctx)
{
    if (dvbs2_cfg_check(c))
        return NULL;
    dvbs2 *s = xcalloc(1, sizeof *s);
    s->c = *c;
    dvb_fec_code fc;
    dvb_fec_code_get(c->short_frame, c->cr_num, c->cr_den, DVB_FEC_S2, &fc);
    int ro = fabs(c->rolloff - 0.25) < 1e-3 ? 1 : fabs(c->rolloff - 0.20) < 1e-3 ? 2 : 0;
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
    build_constellation(s);
    build_plheader(s);
    build_scrambler(s, s->nsym + s->npilot);
    s->flen = 90 + s->nsym + s->npilot;
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
    free(s->frame);
    free(s);
}

static inline int bit_at(const uint8_t *cw, int i) { return (cw[i >> 3] >> (7 - (i & 7))) & 1; }

static inline float complex scramble(float complex v, int r)
{
    switch (r) {
    case 1: return -cimagf(v) + crealf(v) * I;
    case 2: return -v;
    case 3: return cimagf(v) - crealf(v) * I;
    default: return v;
    }
}

static void next_frame(dvbs2 *s)
{
    dvb_fec_frame(&s->fec, s->cw, 0);
    float complex *o = s->frame;
    memcpy(o, s->plh, sizeof s->plh);
    o += 90;
    int n = 0, bits = s->c.bits, j = 0;
    const float complex pilot = polar(1, M_PI / 4);
    for (int slot = 0; slot < s->slots; slot++) {
        for (int k = 0; k < 90; k++, j++) {
            int sym;
            if (bits == 2)
                sym = (bit_at(s->cw, 2 * j) << 1) | bit_at(s->cw, 2 * j + 1);
            else {
                sym = 0;
                for (int b = 0; b < bits; b++)
                    sym = (sym << 1) | bit_at(s->cw, s->rowaddr[b] + j);
            }
            *o++ = scramble(s->constel[sym], s->cscram[n++]);
        }
        if (s->c.pilots && (slot + 1) % 16 == 0 && slot < s->slots - 1)
            for (int p = 0; p < 36; p++)
                *o++ = scramble(pilot, s->cscram[n++]);
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
