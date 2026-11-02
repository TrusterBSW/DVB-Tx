/* SPDX-License-Identifier: GPL-3.0-or-later
 * DVB-T2 modulator (EN 302 755): BBFRAME/BCH/LDPC (dvb_fec.c), bit interleaver and demux,
 * constellation mapping with rotation and cyclic Q delay, cell interleaver, time interleaver,
 * frame builder with L1-pre / L1-post signalling, frequency interleaver, pilots, IFFT,
 * guard interval and P1 symbol.
 * Derived from GNU Radio gr-dtv (Copyright Free Software Foundation, Inc. / Ron Economos):
 * dvbt2_interleaver_bb, dvbt2_modulator_bc, dvbt2_cellinterleaver_cc, dvbt2_framemapper_cc,
 * dvbt2_freqinterleaver_cc, dvbt2_pilotgenerator_cc, digital ofdm_cyclic_prefixer and
 * dvbt2_p1insertion_cc, configured as VERSION_111, SISO, PAPR_OFF, INBAND_OFF,
 * EQUALIZATION_OFF. The gr-dtv behaviour is kept even where it departs from the standard
 * (see docs/DVBT2_PORT.md).
 *
 * Pipeline: the cells of each FEC block are written straight to their final place in the
 * T2 frame (cell interleaver + time interleaver + frame mapping in one scatter), into the
 * second of two frame buffers while the first one is being transmitted symbol by symbol. */
#include "dvbt2.h"

#include <fftw3.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dvb_fec.h"

#define DVBT2_TABLES_T2
#include "dvbt2_tables.h"

/* gr-dtv enum values, which are also the L1 / P1 field codes */
enum { GI_1_32, GI_1_16, GI_1_8, GI_1_4, GI_1_128, GI_19_128, GI_19_256 };
enum { FFT_2K, FFT_8K, FFT_4K, FFT_1K, FFT_16K, FFT_32K, FFT_8K_T2GI, FFT_32K_T2GI };

#define KBCH_1_4 3072
#define NBCH_1_4 3240
#define KBCH_1_2 7032
#define NBCH_1_2 7200
#define KSIG_PRE 200
#define KSIG_POST 350
#define NBCH_PARITY 168
#define N_L1PRE 1840
#define M_TI ((1 << 19) + (1 << 15))
#define MAX_CARRIERS 27841
#define CHIPS 2624

/* ------------------------------------------------------------------ parameters */

/* guard intervals in gr-dtv order of table 67 columns: 1/128 1/32 1/16 19/256 1/8 19/128 1/4 */
static const struct { int num, den, code; } gis[7] = {
    { 1, 128, GI_1_128 }, { 1, 32, GI_1_32 }, { 1, 16, GI_1_16 }, { 19, 256, GI_19_256 },
    { 1, 8, GI_1_8 }, { 19, 128, GI_19_128 }, { 1, 4, GI_1_4 },
};
#define P(n) (1 << (n))
/* allowed pilot patterns, EN 302 755 table 58 (SISO); [fft 1K..32K][gi as above]; 0 = GI not allowed */
static const uint16_t pp_allowed[6][7] = {
    { 0, 0, P(4) | P(5), 0, P(2) | P(3), 0, P(1) },                                       /* 1K */
    { 0, P(7) | P(4), P(4) | P(5), 0, P(2) | P(3), 0, P(1) },                             /* 2K */
    { 0, P(7) | P(4), P(4) | P(5), 0, P(2) | P(3), 0, P(1) },                             /* 4K */
    { P(7), P(7) | P(4), P(8) | P(4) | P(5), P(8) | P(4) | P(5), P(2) | P(3) | P(8),
      P(2) | P(3) | P(8), P(1) | P(8) },                                                  /* 8K */
    { P(7), P(7) | P(4) | P(6), P(2) | P(8) | P(4) | P(5), P(2) | P(8) | P(4) | P(5),
      P(2) | P(3) | P(8), P(2) | P(3) | P(8), P(1) | P(8) },                              /* 16K */
    { P(7), P(4) | P(6), P(2) | P(8) | P(4), P(2) | P(8) | P(4), P(2) | P(8), P(2) | P(8), 0 }, /* 32K */
};
#undef P

/* C_DATA, N_FC, C_FC (tables 48-50, SISO, no TR) [fft][extended][pp-1] as in gr-dtv */
static const int cdata_tab[6][2][8][3] = {
    { { { 764, 568, 402 }, { 768, 710, 654 }, { 798, 710, 490 }, { 804, 780, 707 },
        { 818, 780, 544 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } } },
    { { { 1522, 1136, 804 }, { 1532, 1420, 1309 }, { 1596, 1420, 980 }, { 1602, 1562, 1415 },
        { 1632, 1562, 1088 }, { 0, 0, 0 }, { 1646, 1632, 1396 }, { 0, 0, 0 } } },
    { { { 3084, 2272, 1609 }, { 3092, 2840, 2619 }, { 3228, 2840, 1961 }, { 3234, 3124, 2831 },
        { 3298, 3124, 2177 }, { 0, 0, 0 }, { 3328, 3266, 2792 }, { 0, 0, 0 } } },
    { { { 6208, 4544, 3218 }, { 6214, 5680, 5238 }, { 6494, 5680, 3922 }, { 6498, 6248, 5662 },
        { 6634, 6248, 4354 }, { 0, 0, 0 }, { 6698, 6532, 5585 }, { 6698, 0, 0 } },
      { { 6296, 4608, 3264 }, { 6298, 5760, 5312 }, { 6584, 5760, 3978 }, { 6588, 6336, 5742 },
        { 6728, 6336, 4416 }, { 0, 0, 0 }, { 6788, 6624, 5664 }, { 6788, 0, 0 } } },
    { { { 12418, 9088, 6437 }, { 12436, 11360, 10476 }, { 12988, 11360, 7845 },
        { 13002, 12496, 11324 }, { 13272, 12496, 8709 }, { 13288, 13064, 11801 },
        { 13416, 13064, 11170 }, { 13406, 0, 0 } },
      { { 12678, 9280, 6573 }, { 12698, 11600, 10697 }, { 13262, 11600, 8011 },
        { 13276, 12760, 11563 }, { 13552, 12760, 8893 }, { 13568, 13340, 12051 },
        { 13698, 13340, 11406 }, { 13688, 0, 0 } } },
    { { { 0, 0, 0 }, { 24886, 22720, 20952 }, { 0, 0, 0 }, { 26022, 24992, 22649 }, { 0, 0, 0 },
        { 26592, 26128, 23603 }, { 26836, 0, 0 }, { 26812, 0, 0 } },
      { { 0, 0, 0 }, { 25412, 23200, 21395 }, { 0, 0, 0 }, { 26572, 25520, 23127 }, { 0, 0, 0 },
        { 27152, 26680, 24102 }, { 27404, 0, 0 }, { 27376, 0, 0 } } },
};
static const int np2_tab[6] = { 16, 8, 4, 2, 1, 1 };
static const int cp2_tab[6] = { 558, 1118, 2236, 4472, 8944, 22432 };
static const int cps_tab[6][2] = { { 853, 0 }, { 1705, 0 }, { 3409, 0 }, { 6817, 6913 },
                                   { 13633, 13921 }, { 27265, 27841 } };
static const int kext_tab[6] = { 0, 0, 0, 48, 144, 288 };
static const int dxdy[8][2] = { { 3, 4 }, { 6, 2 }, { 6, 4 }, { 12, 2 }, { 12, 4 }, { 24, 2 },
                                { 24, 4 }, { 6, 16 } };

typedef struct t2p {
    int N, fidx, gi, gicode, fftcode;
    int cps, kext, koff, np2, cp2, cdata, nfc, cfc, dx, dy;
    int ldata, lf, nfec, nti, cells, mod, eta, npost, npunc, mapped, ndummy, stream;
    int tnum, tden;                /* elementary period T = tnum / tden microseconds */
    double fs;
    long frame_samples;
    dvb_fec_code code;
} t2p;

static const char *derive(const dvbt2_cfg *c, t2p *p)
{
    memset(p, 0, sizeof *p);
    double bw = c->bw_hz;
    if (fabs(bw - 1.7e6) < 2e4 || fabs(bw - 1.712e6) < 2e4) {
        p->tnum = 71;
        p->tden = 131;
    } else if (fabs(bw - 5e6) < 1) {
        p->tnum = 7; p->tden = 40;
    } else if (fabs(bw - 6e6) < 1) {
        p->tnum = 7; p->tden = 48;
    } else if (fabs(bw - 7e6) < 1) {
        p->tnum = 1; p->tden = 8;
    } else if (fabs(bw - 8e6) < 1) {
        p->tnum = 7; p->tden = 64;
    } else if (fabs(bw - 10e6) < 1) {
        p->tnum = 7; p->tden = 80;
    } else {
        return "DVB-T2 bandwidth must be 1.7, 5, 6, 7, 8 or 10 MHz";
    }
    p->fs = 1e6 * p->tden / p->tnum;

    switch (c->fft) {
    case 1024: p->fidx = 0; break;
    case 2048: p->fidx = 1; break;
    case 4096: p->fidx = 2; break;
    case 8192: p->fidx = 3; break;
    case 16384: p->fidx = 4; break;
    case 32768: p->fidx = 5; break;
    default: return "DVB-T2 FFT size must be 1K, 2K, 4K, 8K, 16K or 32K";
    }
    p->N = c->fft;
    if (c->extended && p->fidx < 3)
        return "extended carrier mode exists only for 8K, 16K and 32K";
    int ext = c->extended ? 1 : 0;

    int g = -1;
    for (int i = 0; i < 7; i++)
        if (gis[i].num == c->gi_num && gis[i].den == c->gi_den)
            g = i;
    if (g < 0)
        return "guard interval must be 1/128, 1/32, 1/16, 19/256, 1/8, 19/128 or 1/4";
    if (!pp_allowed[p->fidx][g])
        return "this guard interval is not allowed with this FFT size (EN 302 755 table 67)";
    p->gi = p->N / gis[g].den * gis[g].num;
    p->gicode = gis[g].code;
    if (c->pilot < 1 || c->pilot > 8)
        return "pilot pattern must be PP1..PP8";
    if (!(pp_allowed[p->fidx][g] & (1 << c->pilot)))
        return "this pilot pattern is not allowed with this FFT size and guard interval (EN 302 755 table 58)";
    static const int fftcodes[6] = { FFT_1K, FFT_2K, FFT_4K, FFT_8K, FFT_16K, FFT_32K };
    p->fftcode = fftcodes[p->fidx];
    int t2gi = g == 0 || g == 3 || g == 5;   /* 1/128, 19/256, 19/128 */
    if (t2gi && p->fidx == 3)
        p->fftcode = FFT_8K_T2GI;
    if (t2gi && p->fidx == 5)
        p->fftcode = FFT_32K_T2GI;

    if (c->bits != 2 && c->bits != 4 && c->bits != 6 && c->bits != 8)
        return "constellation must be QPSK, 16-QAM, 64-QAM or 256-QAM (2, 4, 6 or 8 bits)";
    p->mod = c->bits;
    int crok = (c->cr_num == 1 && c->cr_den == 2) || (c->cr_num == 3 && c->cr_den == 5) ||
               (c->cr_num == 2 && c->cr_den == 3) || (c->cr_num == 3 && c->cr_den == 4) ||
               (c->cr_num == 4 && c->cr_den == 5) || (c->cr_num == 5 && c->cr_den == 6);
    if (!crok || dvb_fec_code_get(c->short_frame, c->cr_num, c->cr_den, DVB_FEC_T2, &p->code))
        return "DVB-T2 code rate must be 1/2, 3/5, 2/3, 3/4, 4/5 or 5/6";
    p->cells = p->code.nldpc / p->mod;
    switch (c->l1_bits) {
    case 1: case 2: case 4: case 6: p->eta = c->l1_bits; break;
    default: return "L1-post constellation must be BPSK, QPSK, 16-QAM or 64-QAM (1, 2, 4 or 6 bits)";
    }
    if (c->papr)
        return "PAPR reduction is not supported";
    if (c->cell_id < 0 || c->cell_id > 0xffff || c->network_id < 0 || c->network_id > 0xffff ||
        c->t2_system_id < 0 || c->t2_system_id > 0xffff)
        return "cell_id, network_id and t2_system_id are 16-bit values";
    if (c->t2_frames < 0 || c->t2_frames > 255)
        return "t2_frames must be 1..255";

    p->np2 = np2_tab[p->fidx];
    p->cp2 = cp2_tab[p->fidx];
    p->cps = cps_tab[p->fidx][ext];
    p->kext = ext ? kext_tab[p->fidx] : 0;
    p->koff = ext ? 0 : kext_tab[p->fidx];
    p->cdata = cdata_tab[p->fidx][ext][c->pilot - 1][0];
    p->nfc = cdata_tab[p->fidx][ext][c->pilot - 1][1];
    p->cfc = cdata_tab[p->fidx][ext][c->pilot - 1][2];
    if (!p->cdata)
        return "pilot pattern not available for this FFT size";
    if ((p->gicode == GI_1_128 && c->pilot == 7) || (p->gicode == GI_1_32 && c->pilot == 4) ||
        (p->gicode == GI_1_16 && c->pilot == 2) || (p->gicode == GI_19_256 && c->pilot == 2))
        p->nfc = p->cfc = 0;   /* no frame closing symbol */
    p->dx = dxdy[c->pilot - 1][0];
    p->dy = dxdy[c->pilot - 1][1];

    /* L1-post size (EN 302 755 clause 7.3.1.2) */
    int npunc_temp = (6 * (KBCH_1_2 - KSIG_POST)) / 5;
    int npost_temp = KSIG_POST + NBCH_PARITY + 9000 - npunc_temp;
    int d = p->np2 == 1 ? 2 * p->eta : p->eta * p->np2;
    p->npost = (npost_temp + d - 1) / d * d;
    p->npunc = npunc_temp - (p->npost - npost_temp);

    /* frame length */
    int S = p->N + p->gi;
    int lfmin = p->np2 + (p->fidx == 5 ? 3 : 7);
    /* T_F = (2048 + LF S) T <= 250 ms */
    long lfmax = ((long)250000 * p->tden / p->tnum - 2048) / S;
    while (lfmax > 0 && (2048 + lfmax * S) * (long)p->tnum > (long)250000 * p->tden)
        lfmax--;
    if (p->fidx == 5 && (lfmax & 1))
        lfmax--;
    if (lfmax > p->np2 + 4095)
        lfmax = p->np2 + 4095;
    if (c->data_symbols) {
        p->lf = p->np2 + c->data_symbols;
        if (p->lf < lfmin)
            return p->fidx == 5 ? "too few data symbols (32K: at least 3)" : "too few data symbols (at least 7)";
        if (p->lf > lfmax)
            return "too many data symbols: the T2 frame would be longer than 250 ms";
        if (p->fidx == 5 && (p->lf & 1))
            return "32K: the number of symbols per frame (data symbols + 1) must be even";
    } else {
        p->lf = (int)lfmax;
        if (p->lf < lfmin)
            return "frame too short";
    }
    p->ldata = p->lf - p->np2;
    if (p->nfc)
        p->mapped = p->np2 * p->cp2 + (p->ldata - 1) * p->cdata + p->nfc;
    else
        p->mapped = p->np2 * p->cp2 + p->ldata * p->cdata;
    int avail = p->mapped - N_L1PRE - p->npost / p->eta - (p->nfc - p->cfc);
    int nmax = avail / p->cells;
    if (nmax > 1023)
        nmax = 1023;
    if (nmax < 1)
        return "the T2 frame is too short for one FEC block";
    if (c->fec_blocks < 0 || c->fec_blocks > nmax)
        return "too many FEC blocks for the T2 frame";
    p->nfec = c->fec_blocks ? c->fec_blocks : nmax;
    if (c->ti_blocks) {
        p->nti = c->ti_blocks;
        if (p->nti < 0 || p->nti > p->nfec || p->nti > 255)
            return "ti_blocks must be between 1 and the number of FEC blocks";
        if ((long)((p->nfec + p->nti - 1) / p->nti) * p->cells > M_TI)
            return "time interleaver memory exceeded (EN 302 755 clause 6.5.1): use more TI blocks";
    } else {
        p->nti = 1;
        while ((long)((p->nfec + p->nti - 1) / p->nti) * p->cells > M_TI)
            p->nti++;
    }
    p->stream = p->nfec * p->cells;
    p->ndummy = avail - p->stream;
    p->frame_samples = 2048 + (long)p->lf * S;
    return NULL;
}

const char *dvbt2_cfg_check(const dvbt2_cfg *c)
{
    t2p p;
    return derive(c, &p);
}

double dvbt2_sample_rate(const dvbt2_cfg *c)
{
    t2p p;
    if (derive(c, &p))
        return 0;
    return p.fs;
}

double dvbt2_ts_rate(const dvbt2_cfg *c)
{
    t2p p;
    if (derive(c, &p))
        return 0;
    /* nfec BBFRAMEs of DFL = Kbch - 80 bits of 188-byte packets per frame */
    return (double)p.nfec * (p.code.kbch - 80) * p.fs / (double)p.frame_samples;
}

double dvbt2_occupied_bw(const dvbt2_cfg *c)
{
    t2p p;
    if (derive(c, &p))
        return 0;
    return p.cps * p.fs / p.N;
}

int dvbt2_get_info(const dvbt2_cfg *c, dvbt2_info *info)
{
    t2p p;
    if (derive(c, &p))
        return -1;
    info->data_symbols = p.ldata;
    info->fec_blocks = p.nfec;
    info->ti_blocks = p.nti;
    info->n_p2 = p.np2;
    info->frame_samples = (int)p.frame_samples;
    info->cells_per_fec = p.cells;
    info->frame_seconds = p.frame_samples / p.fs;
    return 0;
}

/* ------------------------------------------------------------------ state */

typedef struct cmap {          /* one carrier map: data carriers and pilots */
    int ndata, npilot;
    int32_t *data_rel;         /* carrier index relative to the centre carrier (= FFT bin from DC) */
    int32_t *pilot_rel;
    float *pilot_val;          /* amplitude with the reference sequence sign */
} cmap;

struct dvbt2 {
    dvbt2_cfg c;
    t2p p;
    dvb_fec fec;
    int t2_frames;
    uint32_t frequency;

    /* FEC block -> cells */
    uint8_t *cw;               /* codeword (parity interleaved, except QPSK: natural) */
    uint8_t *cellv;            /* cell values of one FEC block */
    int bi_ncol, bi_rows;      /* column twist interleaver (0 columns: QPSK, none) */
    int bi_twist[16];
    uint16_t bi_lut[2][256];   /* demux of a row: columns 0-7 / 8-15 -> output bits */
    float complex constel[256];

    /* cell + time interleaver + frame map */
    uint32_t *cperm;           /* cell interleaver permutation */
    int pn_degree;
    int small_n, big_n, nsmall, nbig;
    uint32_t *icperm;          /* its inverse */
    float complex *mcells;     /* modulated cells of one FEC block */
    uint32_t *zz;              /* P2 interleaving of the frame map (NULL if N_P2 = 1) */
    float complex *mbuf[2];
    int cur;
    int enc_block;             /* FEC blocks of the next frame done */
    int enc_ti, enc_r, enc_n, enc_tistart;   /* position of the next FEC block in the TI structure */
    int frame_count;
    int t2_frame_num;

    /* L1 */
    float complex l1pre[N_L1PRE];
    float complex *l1post;
    float complex l1_qpsk[4], l1_16qam[16], l1_64qam[64];
    float complex *dummy;
    dvb_bch l1bch;
    dvb_ldpc l1ldpc_pre, l1ldpc_post;

    /* OFDM */
    uint32_t *Hd[2], *Hp2[2], *Hfc[2];
    cmap p2map, fcmap, *dmap;  /* dmap[dy] */
    uint8_t pn[CHIPS];
    float norm;
    float p1pat[1024];         /* P1 carriers (1K spacing), index 512 on DC */
    float complex *symcells;   /* cells of the symbol being built (cache-friendly copy) */
    int l;                     /* next step of the frame: -1 = P1, then symbols 0 .. LF-1 */
    int started;
    struct dvbt2_group *own;   /* the k = 1 group used by dvbt2_samples() */
};

#define MAX_MEMBERS 64

struct dvbt2_group {
    int k, N, Nt, gi, lf, n;   /* Nt = k N, gi in radio samples */
    dvbt2_timing_t timing;
    struct gmember {
        dvbt2 *t;
        int off, off1;         /* offset in data / P1 subcarrier spacings */
        int lo, hi;            /* occupied bins */
        float gain, pre;       /* cells x pre (x oscillator phase), then IFFT x post */
        int owned;
        volatile int muted;    /* off the air: still coded (its multiplex keeps its clock), not placed */
    } m[MAX_MEMBERS];
    float post;
    fftwf_complex *fbuf[2], *p1buf;   /* two spectra: one being filled, one transformed */
    fftwf_plan plan, p1plan;          /* plan: fbuf[0] -> sym + gi (also run on fbuf[1]) */
    float complex *sym, *p1a;
    int sym_len, sym_pos;
    int l;                     /* step to output next: -1 = P1, else symbol */
    int64_t T;                 /* its first radio sample */
    int bi;                    /* spectrum of that step */

    /* worker threads (nthreads > 0): member i is handled by worker i % nthreads, which runs
     * its FEC share and carrier placement for the step after the one being transformed */
    int nthreads;
    pthread_t *tid;
    pthread_mutex_t lock;
    pthread_cond_t go, done;
    unsigned gen;
    int pending, quit, inflight;
    int job_l, job_bi;
    int64_t job_win;
};

static pthread_mutex_t plan_lock = PTHREAD_MUTEX_INITIALIZER;   /* FFTW planner is not thread-safe */

static uint32_t zzpos(const dvbt2 *t, int i) { return t->zz ? t->zz[i] : (uint32_t)i; }

/* ------------------------------------------------------------------ bit interleaver */

/* Bit interleaver (EN 302 755 clause 6.1.2, as gr-dtv dvbt2_interleaver_bb): the parity
 * interleaving is done by the LDPC encoder; the column twist interleaver writes the codeword
 * column by column (column c = stored bits [c rows, (c+1) rows), rotated by twist[c]) and reads
 * it row by row; each row of ncol bits is demultiplexed into 2 cells (1 for short 256QAM). */
static void build_bit_interleaver(dvbt2 *t)
{
    const dvb_fec_code *fc = &t->p.code;
    int n = fc->nldpc, mod = t->p.mod, shortf = fc->short_frame;
    int num = fc->cr_num, den = fc->cr_den;
    const uint8_t *twist, *mux;
    int ncol;
    if (mod == 2) {   /* QPSK: no bit interleaving (except T2-Lite 1/3 2/5, not supported) */
        t->bi_ncol = 0;
        return;
    }
    if (mod == 4) {
        ncol = 8;
        twist = shortf ? t2_bi_twist16s : t2_bi_twist16n;
    } else if (mod == 6) {
        ncol = 12;
        twist = shortf ? t2_bi_twist64s : t2_bi_twist64n;
    } else if (!shortf) {
        ncol = 16;
        twist = t2_bi_twist256n;
    } else {
        ncol = 8;
        twist = t2_bi_twist256s;
    }
    int r35 = num == 3 && den == 5, r13 = num == 1 && den == 3, r25 = num == 2 && den == 5;
    if (mod == 4)
        mux = (r35 && !shortf) ? t2_bi_mux16_35 : (r13 && shortf) ? t2_bi_mux16_13 :
              (r25 && shortf) ? t2_bi_mux16_25 : t2_bi_mux16;
    else if (mod == 6)
        mux = (r35 && !shortf) ? t2_bi_mux64_35 : (r13 && shortf) ? t2_bi_mux64_13 :
              (r25 && shortf) ? t2_bi_mux64_25 : t2_bi_mux64;
    else if (!shortf)
        mux = r35 ? t2_bi_mux256_35 : (num == 2 && den == 3) ? t2_bi_mux256_23 : t2_bi_mux256;
    else
        mux = r13 ? t2_bi_mux256s_13 : r25 ? t2_bi_mux256s_25 : t2_bi_mux256s;
    t->bi_ncol = ncol;
    t->bi_rows = n / ncol;
    for (int c = 0; c < ncol; c++)
        t->bi_twist[c] = twist[c];
    /* column e of a row goes to bit mux[e] (MSB first) of the ncol-bit group */
    for (int h = 0; h < 2; h++)
        for (int v = 0; v < 256; v++) {
            unsigned o = 0;
            for (int b = 0; b < 8; b++) {
                int e = 8 * h + b;
                if (e < ncol && ((v >> (7 - b)) & 1))
                    o |= 1u << (ncol - 1 - mux[e]);
            }
            t->bi_lut[h][v] = (uint16_t)o;
        }
}

static inline uint64_t transpose8(uint64_t x)
{
    uint64_t y;
    y = (x ^ (x >> 7)) & 0x00AA00AA00AA00AAULL;
    x = x ^ y ^ (y << 7);
    y = (x ^ (x >> 14)) & 0x0000CCCC0000CCCCULL;
    x = x ^ y ^ (y << 14);
    y = (x ^ (x >> 28)) & 0x00000000F0F0F0F0ULL;
    return x ^ y ^ (y << 28);
}

static inline int cw_bit(const uint8_t *cw, int pos) { return (cw[pos >> 3] >> (7 - (pos & 7))) & 1; }

/* t->cw -> t->cellv */
static void bits_to_cells(dvbt2 *t)
{
    const uint8_t *cw = t->cw;
    uint8_t *cv = t->cellv;
    int mod = t->p.mod, cells = t->p.cells;
    if (!t->bi_ncol) {
        for (int c = 0; c < cells; c++)
            cv[c] = (cw[c >> 2] >> (6 - 2 * (c & 3))) & 3;
        return;
    }
    int ncol = t->bi_ncol, rows = t->bi_rows, one = ncol == mod;
    unsigned mask = (1u << mod) - 1;
    const uint16_t *l0 = t->bi_lut[0], *l1 = t->bi_lut[1];
    int j0 = 0;
    for (; j0 + 8 <= rows; j0 += 8) {
        uint64_t x0 = 0, x1 = 0;
        for (int c = 0; c < ncol; c++) {
            int st = j0 - t->bi_twist[c];
            if (st < 0)
                st += rows;
            unsigned byte;
            if (st + 8 <= rows) {
                int pos = c * rows + st;
                unsigned w = (unsigned)cw[pos >> 3] << 8 | cw[(pos >> 3) + 1];
                byte = (w >> (8 - (pos & 7))) & 0xff;
            } else {
                byte = 0;
                for (int i = 0; i < 8; i++) {
                    int r = st + i;
                    if (r >= rows)
                        r -= rows;
                    byte = byte << 1 | (unsigned)cw_bit(cw, c * rows + r);
                }
            }
            if (c < 8)
                x0 |= (uint64_t)byte << (56 - 8 * c);
            else
                x1 |= (uint64_t)byte << (56 - 8 * (c - 8));
        }
        x0 = transpose8(x0);
        x1 = transpose8(x1);
        for (int i = 0; i < 8; i++) {
            unsigned pack = l0[(x0 >> (56 - 8 * i)) & 0xff] | l1[(x1 >> (56 - 8 * i)) & 0xff];
            int j = j0 + i;
            if (one) {
                cv[j] = (uint8_t)pack;
            } else {
                cv[2 * j] = (uint8_t)(pack >> mod);
                cv[2 * j + 1] = (uint8_t)(pack & mask);
            }
        }
    }
    for (int j = j0; j < rows; j++) {   /* remaining rows, bit by bit */
        unsigned b0 = 0, b1 = 0;
        for (int c = 0; c < ncol; c++) {
            int r = j - t->bi_twist[c];
            if (r < 0)
                r += rows;
            unsigned bit = (unsigned)cw_bit(cw, c * rows + r);
            if (c < 8)
                b0 |= bit << (7 - c);
            else
                b1 |= bit << (15 - c);
        }
        unsigned pack = l0[b0] | l1[b1];
        if (one) {
            cv[j] = (uint8_t)pack;
        } else {
            cv[2 * j] = (uint8_t)(pack >> mod);
            cv[2 * j + 1] = (uint8_t)(pack & mask);
        }
    }
}

/* ------------------------------------------------------------------ constellations */

static void build_constellations(dvbt2 *t)
{
    static const double l16[4] = { 3.0, 1.0, -3.0, -1.0 };
    static const double l64[8] = { 7.0, 5.0, 1.0, 3.0, -7.0, -5.0, -1.0, -3.0 };
    static const double l256[16] = { 15.0, 13.0, 9.0, 11.0, 1.0, 3.0, 7.0, 5.0,
                                     -15.0, -13.0, -9.0, -11.0, -1.0, -3.0, -7.0, -5.0 };
    double norm, angle;
    int m = t->p.mod, n = 1 << m;
    for (int i = 0; i < n; i++) {
        int re, im;
        switch (m) {
        case 2:
            norm = sqrt(2.0);
            t->constel[i] = (float)((i & 2 ? -1.0 : 1.0) / norm) + I * (float)((i & 1 ? -1.0 : 1.0) / norm);
            break;
        case 4:
            norm = sqrt(10.0);
            re = ((i & 0x8) >> 2) | ((i & 0x2) >> 1);
            im = ((i & 0x4) >> 1) | (i & 0x1);
            t->constel[i] = (float)(l16[re] / norm) + I * (float)(l16[im] / norm);
            break;
        case 6:
            norm = sqrt(42.0);
            re = ((i & 0x20) >> 3) | ((i & 0x8) >> 2) | ((i & 0x2) >> 1);
            im = ((i & 0x10) >> 2) | ((i & 0x4) >> 1) | (i & 0x1);
            t->constel[i] = (float)(l64[re] / norm) + I * (float)(l64[im] / norm);
            break;
        default:
            norm = sqrt(170.0);
            re = ((i & 0x80) >> 4) | ((i & 0x20) >> 3) | ((i & 0x8) >> 2) | ((i & 0x2) >> 1);
            im = ((i & 0x40) >> 3) | ((i & 0x10) >> 2) | ((i & 0x4) >> 1) | (i & 0x1);
            t->constel[i] = (float)(l256[re] / norm) + I * (float)(l256[im] / norm);
            break;
        }
    }
    if (t->c.rotation) {
        angle = m == 2 ? 29.0 : m == 4 ? 16.8 : m == 6 ? 8.6 : 3.576334375;
        angle = 2.0 * M_PI * angle / 360.0;
        float complex rot = (float)cos(angle) + I * (float)sin(angle);
        for (int i = 0; i < n; i++)
            t->constel[i] = t->constel[i] * rot;
    }
    /* L1-post constellations (never rotated) */
    norm = sqrt(2.0);
    for (int i = 0; i < 4; i++)
        t->l1_qpsk[i] = (float)((i & 2 ? -1.0 : 1.0) / norm) + I * (float)((i & 1 ? -1.0 : 1.0) / norm);
    norm = sqrt(10.0);
    for (int i = 0; i < 16; i++) {
        int re = ((i & 0x8) >> 2) | ((i & 0x2) >> 1), im = ((i & 0x4) >> 1) | (i & 0x1);
        t->l1_16qam[i] = (float)(l16[re] / norm) + I * (float)(l16[im] / norm);
    }
    norm = sqrt(42.0);
    for (int i = 0; i < 64; i++) {
        int re = ((i & 0x20) >> 3) | ((i & 0x8) >> 2) | ((i & 0x2) >> 1);
        int im = ((i & 0x10) >> 2) | ((i & 0x4) >> 1) | (i & 0x1);
        t->l1_64qam[i] = (float)(l64[re] / norm) + I * (float)(l64[im] / norm);
    }
}

/* ------------------------------------------------------------------ cell / time interleaver */

static int build_cell_interleaver(dvbt2 *t)
{
    static const int logic11[2] = { 0, 3 }, logic12[2] = { 0, 2 }, logic13[4] = { 0, 1, 4, 6 };
    static const int logic14[6] = { 0, 1, 4, 5, 9, 11 }, logic15[4] = { 0, 1, 2, 12 };
    int cells = t->p.cells, deg, xs;
    const int *logic;
    switch (cells) {
    case 32400: deg = 15; logic = logic15; xs = 4; break;
    case 16200: case 10800: deg = 14; logic = logic14; xs = 6; break;
    case 8100: deg = 13; logic = logic13; xs = 4; break;
    case 4050: case 2700: deg = 12; logic = logic12; xs = 2; break;
    default: deg = 11; logic = logic11; xs = 2; break;   /* 2025 */
    }
    int mask = (1 << (deg - 1)) - 1, states = 1 << deg, q = 0, lfsr = 0;
    t->pn_degree = deg;
    for (int i = 0; i < states; i++) {
        if (i == 0 || i == 1) {
            lfsr = 0;
        } else if (i == 2) {
            lfsr = 1;
        } else {
            int r = 0;
            for (int k = 0; k < xs; k++)
                r ^= (lfsr >> logic[k]) & 1;
            lfsr &= mask;
            lfsr >>= 1;
            lfsr |= r << (deg - 2);
        }
        lfsr |= (i % 2) << (deg - 1);
        if (lfsr < cells && q < cells)
            t->cperm[q++] = (uint32_t)lfsr;
    }
    if (q != cells)
        return -1;
    /* TI blocks: the small ones first */
    int nfec = t->p.nfec, nti = t->p.nti;
    t->small_n = nfec / nti;
    t->big_n = (nfec + nti - 1) / nti;
    t->nbig = nfec % nti;
    t->nsmall = nti - t->nbig;
    for (int w = 0; w < cells; w++)
        t->icperm[t->cperm[w]] = (uint32_t)w;
    return 0;
}

/* gr-dtv frame mapper P2 interleaving for N_P2 > 1: interleave index -> frame index */
static void build_zz(dvbt2 *t)
{
    int np2 = t->p.np2, cp2 = t->p.cp2, np = t->p.npost / t->p.eta, mapped = t->p.mapped;
    uint32_t *zz = t->zz;
    int read = 0, index = 0, count = 0, save;
    for (int n = 0; n < np2; n++) {
        save = read;
        for (int j = 0; j < N_L1PRE / np2; j++) {
            zz[read] = (uint32_t)index++;
            count++;
            read += np2;
        }
        read = save + 1;
        index += cp2 - N_L1PRE / np2;
    }
    read = N_L1PRE;
    index = N_L1PRE / np2;
    for (int n = 0; n < np2; n++) {
        save = read;
        for (int j = 0; j < np / np2; j++) {
            zz[read] = (uint32_t)index++;
            count++;
            read += np2;
        }
        read = save + 1;
        index += cp2 - np / np2;
    }
    read = N_L1PRE + np;
    index = N_L1PRE / np2 + np / np2;
    int rest = cp2 - N_L1PRE / np2 - np / np2;
    for (int n = 0; n < np2; n++) {
        for (int j = 0; j < rest; j++) {
            zz[read++] = (uint32_t)index++;
            count++;
        }
        index += cp2 - rest;
    }
    index -= cp2 - rest;
    for (int j = 0; j < mapped - count; j++)
        zz[read++] = (uint32_t)index++;
}

/* ------------------------------------------------------------------ L1 signalling */

static int put_bits(uint8_t *b, int off, unsigned v, int n)
{
    for (int i = n - 1; i >= 0; i--)
        b[off++] = (v >> i) & 1;
    return off;
}

static int add_crc32(uint8_t *b, int len)
{
    uint32_t crc = 0xffffffff;
    for (int i = 0; i < len; i++) {
        int x = b[i] ^ (int)((crc >> 31) & 1);
        crc <<= 1;
        if (x)
            crc ^= 0x04C11DB7;
    }
    return put_bits(b, len, crc, 32);
}

static void pack_bits(const uint8_t *bits, int n, uint8_t *bytes)
{
    for (int i = 0; i < n / 8; i++) {
        uint8_t v = 0;
        for (int k = 0; k < 8; k++)
            v = (uint8_t)(v << 1 | bits[8 * i + k]);
        bytes[i] = v;
    }
}

static void unpack_bytes(const uint8_t *bytes, int n, uint8_t *bits)
{
    for (int i = 0; i < n; i++)
        bits[i] = (bytes[i >> 3] >> (7 - (i & 7))) & 1;
}

/* BCH + LDPC of a shortened L1 block: bits[0..kbch) -> bits[0..16200) */
static void l1_encode(dvbt2 *t, const dvb_ldpc *ldpc, uint8_t *bits, int kbch, int nbch)
{
    uint8_t bytes[16200 / 8];
    pack_bits(bits, kbch, bytes);
    dvb_bch_encode(&t->l1bch, bytes, kbch, bytes + kbch / 8);
    dvb_ldpc_encode(ldpc, bytes, bytes + nbch / 8, 0);
    unpack_bytes(bytes, 16200, bits);
}

static void build_l1pre(dvbt2 *t)
{
    const t2p *p = &t->p;
    const dvbt2_cfg *c = &t->c;
    uint8_t b[16200];
    int o = 0;
    memset(b, 0, sizeof b);
    o = put_bits(b, o, 0, 8);                       /* TYPE: TS */
    o = put_bits(b, o, c->extended ? 1 : 0, 1);     /* BWT_EXT */
    o = put_bits(b, o, 0, 3);                       /* S1: T2_SISO */
    o = put_bits(b, o, (unsigned)p->fftcode & 7, 3);/* S2 */
    o = put_bits(b, o, 0, 1);                       /* S2 mixed bit */
    o = put_bits(b, o, 0, 1);                       /* L1_REPETITION_FLAG */
    o = put_bits(b, o, (unsigned)p->gicode, 3);
    o = put_bits(b, o, 0, 4);                       /* PAPR */
    o = put_bits(b, o, p->eta == 1 ? 0 : p->eta == 2 ? 1 : p->eta == 4 ? 2 : 3, 4);
    o = put_bits(b, o, 0, 2);                       /* L1_COD */
    o = put_bits(b, o, 0, 2);                       /* L1_FEC_TYPE */
    o = put_bits(b, o, (unsigned)(p->npost / p->eta), 18);
    o = put_bits(b, o, KSIG_POST - 32, 18);
    o = put_bits(b, o, (unsigned)c->pilot - 1, 4);
    o = put_bits(b, o, 0, 8);                       /* TX_ID_AVAILABILITY */
    o = put_bits(b, o, (unsigned)c->cell_id, 16);
    o = put_bits(b, o, (unsigned)c->network_id, 16);
    o = put_bits(b, o, (unsigned)c->t2_system_id, 16);
    o = put_bits(b, o, (unsigned)t->t2_frames, 8);
    o = put_bits(b, o, (unsigned)p->ldata, 12);
    o = put_bits(b, o, 0, 3);                       /* REGEN_FLAG */
    o = put_bits(b, o, 0, 1);                       /* L1_POST_EXTENSION */
    o = put_bits(b, o, 1, 3);                       /* NUM_RF */
    o = put_bits(b, o, 0, 3);                       /* CURRENT_RF_IDX */
    o = put_bits(b, o, 0, 4);                       /* T2_VERSION 1.1.1 */
    o = put_bits(b, o, 0, 1);                       /* L1_POST_SCRAMBLED */
    o = put_bits(b, o, 0, 1);                       /* T2_BASE_LITE */
    o = put_bits(b, o, 0, 4);                       /* RESERVED */
    o = add_crc32(b, o);                            /* 200 bits */
    l1_encode(t, &t->l1ldpc_pre, b, KBCH_1_4, NBCH_1_4);
    /* puncturing */
    for (int cc = 0; cc < 31; cc++) {
        int g = t2_l1_pre_puncture[cc];
        for (int c2 = 0; c2 < 360; c2++)
            b[c2 * 36 + g + NBCH_1_4] = 0x55;
    }
    int g = t2_l1_pre_puncture[31];
    for (int c2 = 0; c2 < 328; c2++)
        b[c2 * 36 + g + NBCH_1_4] = 0x55;
    int idx = 0;
    for (int w = 0; w < KSIG_PRE; w++)
        t->l1pre[idx++] = b[w] ? -1.0f : 1.0f;
    for (int w = 0; w < NBCH_PARITY; w++)
        t->l1pre[idx++] = b[w + KBCH_1_4] ? -1.0f : 1.0f;
    for (int w = 0; w < 16200 - NBCH_1_4; w++)
        if (b[w + NBCH_1_4] != 0x55)
            t->l1pre[idx++] = b[w + NBCH_1_4] ? -1.0f : 1.0f;
}

static void build_l1post(dvbt2 *t, int frame_idx, float complex *out)
{
    const t2p *p = &t->p;
    uint8_t info[KSIG_POST], map[KBCH_1_2], b[16200], il[16200], tmp[16200];
    int o = 0;
    o = put_bits(info, o, 1, 15);                   /* SUB_SLICES_PER_FRAME */
    o = put_bits(info, o, 1, 8);                    /* NUM_PLP */
    o = put_bits(info, o, 0, 4);                    /* NUM_AUX */
    o = put_bits(info, o, 0, 8);                    /* AUX_CONFIG_RFU */
    o = put_bits(info, o, 0, 3);                    /* RF_IDX */
    o = put_bits(info, o, t->frequency, 32);
    o = put_bits(info, o, 0, 8);                    /* PLP_ID */
    o = put_bits(info, o, 1, 3);                    /* PLP_TYPE: data type 1 */
    o = put_bits(info, o, 3, 5);                    /* PLP_PAYLOAD_TYPE: TS */
    o = put_bits(info, o, 0, 1);                    /* FF_FLAG */
    o = put_bits(info, o, 0, 3);                    /* FIRST_RF_IDX */
    o = put_bits(info, o, 0, 8);                    /* FIRST_FRAME_IDX */
    o = put_bits(info, o, 1, 8);                    /* PLP_GROUP_ID */
    static const int cod[6][2] = { { 1, 2 }, { 3, 5 }, { 2, 3 }, { 3, 4 }, { 4, 5 }, { 5, 6 } };
    int plp_cod = 0;
    for (int i = 0; i < 6; i++)
        if (cod[i][0] == t->c.cr_num && cod[i][1] == t->c.cr_den)
            plp_cod = i;
    o = put_bits(info, o, (unsigned)plp_cod, 3);
    o = put_bits(info, o, p->mod == 2 ? 0 : p->mod == 4 ? 1 : p->mod == 6 ? 2 : 3, 3);
    o = put_bits(info, o, t->c.rotation ? 1 : 0, 1);
    o = put_bits(info, o, p->code.short_frame ? 0 : 1, 2);   /* PLP_FEC_TYPE */
    o = put_bits(info, o, (unsigned)p->nfec, 10);  /* PLP_NUM_BLOCKS_MAX */
    o = put_bits(info, o, 1, 8);                    /* FRAME_INTERVAL */
    o = put_bits(info, o, (unsigned)p->nti, 8);    /* TIME_IL_LENGTH */
    o = put_bits(info, o, 0, 1);                    /* TIME_IL_TYPE */
    o = put_bits(info, o, 0, 1);                    /* IN_BAND_A_FLAG */
    o = put_bits(info, o, 0, 1);                    /* IN_BAND_B_FLAG */
    o = put_bits(info, o, 0, 11);                   /* RESERVED_1 */
    o = put_bits(info, o, 0, 2);                    /* PLP_MODE (1.1.1) */
    o = put_bits(info, o, 0, 1);                    /* STATIC_FLAG */
    o = put_bits(info, o, 0, 1);                    /* STATIC_PADDING_FLAG */
    o = put_bits(info, o, 0, 2);                    /* FEF_LENGTH_MSB */
    o = put_bits(info, o, 0, 30);                   /* RESERVED_2 */
    o = put_bits(info, o, (unsigned)frame_idx, 8); /* FRAME_IDX */
    o = put_bits(info, o, 0, 22);                   /* SUB_SLICE_INTERVAL */
    o = put_bits(info, o, 0, 22);                   /* TYPE_2_START */
    o = put_bits(info, o, 0, 8);                    /* L1_CHANGE_COUNTER */
    o = put_bits(info, o, 0, 3);                    /* START_RF_IDX */
    o = put_bits(info, o, 0, 8);                    /* RESERVED_3 */
    o = put_bits(info, o, 0, 8);                    /* PLP_ID */
    o = put_bits(info, o, 0, 22);                   /* PLP_START */
    o = put_bits(info, o, (unsigned)p->nfec, 10);  /* PLP_NUM_BLOCKS */
    o = put_bits(info, o, 0, 8);                    /* RESERVED_4 */
    o = put_bits(info, o, 0, 8);                    /* RESERVED_5 */
    o = add_crc32(info, o);                         /* 350 bits */

    /* shortening: padded positions of the 7032-bit BCH information */
    const uint8_t *pad = p->eta == 4 ? t2_l1_post_padding_16qam : p->eta == 6 ? t2_l1_post_padding_64qam
                                                                              : t2_l1_post_padding_bqpsk;
    const uint8_t *punc = p->eta == 4 ? t2_l1_post_puncture_16qam : p->eta == 6 ? t2_l1_post_puncture_64qam
                                                                                : t2_l1_post_puncture_bqpsk;
    memset(map, 0, sizeof map);
    int m, last;
    if (o <= 360) {
        m = 20 - 1;
        last = 360 - o;
    } else {
        m = (KBCH_1_2 - o) / 360;
        last = KBCH_1_2 - o - 360 * m;
    }
    for (int n = 0; n < m; n++) {
        int index = pad[n] * 360, len = pad[n] == 19 ? 192 : 360;
        memset(map + index, 7, (size_t)len);
    }
    int index = pad[m] * 360 + (pad[m] == 19 ? 192 : 360) - last;
    memset(map + index, 7, (size_t)last);
    index = 0;
    for (int n = 0; n < KBCH_1_2; n++)
        b[n] = map[n] != 7 ? info[index++] : 0;
    l1_encode(t, &t->l1ldpc_post, b, KBCH_1_2, NBCH_1_2);
    /* puncturing */
    int np = p->npunc;
    for (int cc = 0; cc < np / 360; cc++) {
        int g = punc[cc];
        for (int c2 = 0; c2 < 360; c2++)
            b[c2 * 25 + g + NBCH_1_2] = 0x55;
    }
    int g = punc[np / 360];
    for (int c2 = 0; c2 < np - (np / 360) * 360; c2++)
        b[c2 * 25 + g + NBCH_1_2] = 0x55;
    index = 0;
    for (int w = 0; w < KBCH_1_2; w++)
        if (map[w] != 7)
            il[index++] = b[w];
    for (int w = 0; w < NBCH_PARITY; w++)
        il[index++] = b[w + KBCH_1_2];
    for (int w = 0; w < 16200 - NBCH_1_2; w++)
        if (b[w + NBCH_1_2] != 0x55)
            il[index++] = b[w + NBCH_1_2];
    int npost = p->npost;
    if (p->eta == 4 || p->eta == 6) {     /* bit interleaver: written in columns, read in rows */
        int ncol = p->eta == 4 ? 8 : 12, rows = npost / ncol, k = 0;
        for (int r = 0; r < rows; r++)
            for (int w = 0; w < ncol; w++)
                tmp[k++] = il[rows * w + r];
    }
    int produced = 0;
    switch (p->eta) {
    case 1:
        for (int d = 0; d < npost; d++)
            out[produced++] = il[d] ? -1.0f : 1.0f;
        break;
    case 2:
        for (int d = 0; d < npost / 2; d++)
            out[produced++] = t->l1_qpsk[il[2 * d] << 1 | il[2 * d + 1]];
        break;
    case 4:
        for (int d = 0; d < npost / 8; d++) {
            int pack = 0;
            for (int e = 0; e < 8; e++)
                pack = pack << 1 | tmp[8 * d + t2_l1_mux16[e]];
            out[produced++] = t->l1_16qam[pack >> 4];
            out[produced++] = t->l1_16qam[pack & 0xf];
        }
        break;
    default:
        for (int d = 0; d < npost / 12; d++) {
            int pack = 0;
            for (int e = 0; e < 12; e++)
                pack = pack << 1 | tmp[12 * d + t2_l1_mux64[e]];
            out[produced++] = t->l1_64qam[pack >> 6];
            out[produced++] = t->l1_64qam[pack & 0x3f];
        }
        break;
    }
}

/* ------------------------------------------------------------------ frequency interleaver */

static int fi_build(int fidx, int ncells, int odd, uint32_t *H)
{
    static const int lg1[2] = { 0, 4 }, lg2[2] = { 0, 3 }, lg4[2] = { 0, 2 }, lg8[4] = { 0, 1, 4, 6 };
    static const int lg16[6] = { 0, 1, 4, 5, 9, 11 }, lg32[4] = { 0, 1, 2, 12 };
    static const int *lgs[6] = { lg1, lg2, lg4, lg8, lg16, lg32 };
    static const int xss[6] = { 2, 2, 2, 4, 6, 4 };
    const uint8_t *perm[6][2] = {
        { t2_fi_bitperm1keven, t2_fi_bitperm1kodd }, { t2_fi_bitperm2keven, t2_fi_bitperm2kodd },
        { t2_fi_bitperm4keven, t2_fi_bitperm4kodd }, { t2_fi_bitperm8keven, t2_fi_bitperm8kodd },
        { t2_fi_bitperm16keven, t2_fi_bitperm16kodd }, { t2_fi_bitperm32k, t2_fi_bitperm32k },
    };
    int deg = 9 + fidx, mask = (1 << deg) - 1, states = 1 << (deg + 1), lfsr = 0, q = 0;
    const int *logic = lgs[fidx];
    const uint8_t *bp = perm[fidx][fidx == 5 ? 1 : odd];
    for (int i = 0; i < states; i++) {
        if (i == 0 || i == 1) {
            lfsr = 0;
        } else if (i == 2) {
            lfsr = 1;
        } else {
            int r = 0;
            for (int k = 0; k < xss[fidx]; k++)
                r ^= (lfsr >> logic[k]) & 1;
            lfsr &= mask;
            lfsr >>= 1;
            lfsr |= r << (deg - 1);
        }
        int v = 0;
        for (int n = 0; n < deg; n++)
            v |= ((lfsr >> n) & 1) << bp[n];
        v += (i % 2) * (states / 2);
        if (v < ncells) {
            if (q >= ncells)
                return -1;
            H[q++] = (uint32_t)v;
        }
    }
    if (q != ncells)
        return -1;
    if (fidx == 5 && !odd) {   /* 32K: even symbols use the inverse of the odd permutation */
        uint32_t *inv = malloc(sizeof(uint32_t) * (size_t)ncells);
        if (!inv)
            return -1;
        for (int j = 0; j < ncells; j++)
            inv[H[j]] = (uint32_t)j;
        memcpy(H, inv, sizeof(uint32_t) * (size_t)ncells);
        free(inv);
    }
    return 0;
}

/* ------------------------------------------------------------------ pilots */

enum { C_DATA = 1, C_P2PILOT, C_PAPR, C_SP, C_CP };

static int cmap_build(dvbt2 *t, const uint8_t *type, const float *amp, int expect, const uint8_t *prbs, cmap *m)
{
    const t2p *p = &t->p;
    int nd = 0, np = 0;
    for (int k = 0; k < p->cps; k++) {
        if (type[k] == C_DATA)
            nd++;
        else if (type[k] != C_PAPR)
            np++;
    }
    if (nd != expect)
        return -1;
    m->ndata = nd;
    m->npilot = np;
    m->data_rel = malloc(sizeof(int32_t) * (size_t)nd);
    m->pilot_rel = malloc(sizeof(int32_t) * (size_t)(np ? np : 1));
    m->pilot_val = malloc(sizeof(float) * (size_t)(np ? np : 1));
    if (!m->data_rel || !m->pilot_rel || !m->pilot_val)
        return -1;
    /* gr-dtv: carrier k at position (N - C_PS) / 2 + 1 + k of the shifted spectrum, i.e. the
     * centre carrier (C_PS - 1) / 2 on DC */
    int centre = (p->cps - 1) / 2;
    nd = np = 0;
    for (int k = 0; k < p->cps; k++) {
        int32_t rel = k - centre;
        if (type[k] == C_DATA) {
            m->data_rel[nd++] = rel;
        } else if (type[k] != C_PAPR) {
            m->pilot_rel[np] = rel;
            m->pilot_val[np++] = prbs[k + p->koff] ? -amp[type[k]] : amp[type[k]];
        }
    }
    return 0;
}

static void cmap_free(cmap *m)
{
    free(m->data_rel);
    free(m->pilot_rel);
    free(m->pilot_val);
}

static int build_pilots(dvbt2 *t)
{
    const t2p *p = &t->p;
    int cps = p->cps, fidx = p->fidx, pp = t->c.pilot;
    static const int fftk[6] = { 1, 2, 4, 8, 16, 32 };
    uint8_t *prbs = malloc(MAX_CARRIERS);
    uint8_t *type = malloc((size_t)cps);
    if (!prbs || !type) {
        free(prbs);
        free(type);
        return -1;
    }
    int sr = 0x7ff;
    for (int i = 0; i < MAX_CARRIERS; i++) {
        int b = (sr ^ (sr >> 2)) & 1;
        prbs[i] = (uint8_t)(sr & 1);
        sr >>= 1;
        if (b)
            sr |= 0x400;
    }
    for (int i = 0; i < CHIPS / 8; i++)
        for (int k = 7; k >= 0; k--)
            t->pn[i * 8 + 7 - k] = (t2_pn_sequence_table[i] >> k) & 1;

    float amp[6] = { 0 };
    amp[C_P2PILOT] = (float)(fidx == 5 ? sqrt(37.0) / 5.0 : sqrt(31.0) / 5.0);
    amp[C_CP] = (float)(fidx <= 1 ? 4.0 / 3.0 : fidx == 2 ? 4.0 * sqrt(2.0) / 3.0 : 8.0 / 3.0);
    amp[C_SP] = (float)(pp <= 2 ? 4.0 / 3.0 : pp <= 4 ? 7.0 / 4.0 : 7.0 / 3.0);
    int rc = 0;

    /* P2 */
    memset(type, C_DATA, (size_t)cps);
    int step = fidx == 5 ? 6 : 3;
    for (int i = 0; i < cps; i += step)
        type[i] = C_P2PILOT;
    for (int i = 0; i < p->kext; i++) {
        type[i] = C_P2PILOT;
        type[i + cps - p->kext] = C_P2PILOT;
    }
    {
        static const uint16_t *maps[6] = { t2_p2_papr_map_1k, t2_p2_papr_map_2k, t2_p2_papr_map_4k,
                                           t2_p2_papr_map_8k, t2_p2_papr_map_16k, t2_p2_papr_map_32k };
        static const int nmap[6] = { 10, 18, 36, 72, 144, 288 };
        int off = fidx >= 3 ? p->kext : 0;
        for (int i = 0; i < nmap[fidx]; i++)
            type[maps[fidx][i] + off] = C_PAPR;
    }
    rc |= cmap_build(t, type, amp, p->cp2, prbs, &t->p2map);

    /* frame closing symbol */
    if (p->nfc) {
        memset(type, C_DATA, (size_t)cps);
        for (int i = 0; i < cps; i++)
            if (i % p->dx == 0)
                type[i] = C_SP;
        if ((fidx == 0 && (pp == 4 || pp == 5)) || (fidx == 1 && pp == 7))
            type[cps - 2] = C_SP;
        type[0] = type[cps - 1] = C_SP;
        rc |= cmap_build(t, type, amp, p->nfc, prbs, &t->fcmap);
    }

    /* data symbols: the scattered pilots repeat every dy symbols */
    t->dmap = calloc((size_t)p->dy, sizeof(cmap));
    if (!t->dmap)
        rc = -1;
    for (int s = 0; s < p->dy && !rc; s++) {
        memset(type, C_DATA, (size_t)cps);
        for (unsigned u = 0; u < sizeof t2_cp_uses / sizeof t2_cp_uses[0]; u++) {
            const struct t2_cp_use *cu = &t2_cp_uses[u];
            if (cu->fftk != fftk[fidx] || cu->pp != pp || (cu->ext && !t->c.extended))
                continue;
            for (int i = 0; i < cu->n; i++) {
                int k = cu->mod ? cu->tab[i] % cu->mod : cu->tab[i];
                /* gr-dtv has 3636 for the first 16K extended PP1 pilot: a typo for 13636
                 * (EN 302 755 table G.2) that leaves C_DATA + 1 data carriers, so gr-dtv's
                 * own output is inconsistent in that mode; follow the standard */
                if (cu->tab == t2_pp1_16k && k == 3636)
                    k = 13636;
                type[k] = C_CP;
            }
        }
        int period = p->dx * p->dy;
        for (int i = 0; i < cps; i++) {
            int rem = (i - p->kext) % period;
            if (rem < 0)
                rem += period;
            if (rem == p->dx * s)
                type[i] = C_SP;
        }
        type[0] = type[cps - 1] = C_SP;
        rc |= cmap_build(t, type, amp, p->cdata, prbs, &t->dmap[s]);
    }
    free(prbs);
    free(type);
    return rc ? -1 : 0;
}

/* ------------------------------------------------------------------ P1 */

static void build_p1_pattern(dvbt2 *t)
{
    int seq[384], d[385], rnd[384];
    int s1 = 0, s2 = (t->p.fftcode & 7) << 1, idx = 0;
    for (int i = 0; i < 8; i++)
        for (int j = 7; j >= 0; j--)
            seq[idx++] = (t2_p1_s1_patterns[s1 * 8 + i] >> j) & 1;
    for (int i = 0; i < 32; i++)
        for (int j = 7; j >= 0; j--)
            seq[idx++] = (t2_p1_s2_patterns[s2 * 32 + i] >> j) & 1;
    for (int i = 0; i < 8; i++)
        for (int j = 7; j >= 0; j--)
            seq[idx++] = (t2_p1_s1_patterns[s1 * 8 + i] >> j) & 1;
    int sr = 0x4e46;
    for (int i = 0; i < 384; i++) {
        int b = (sr ^ (sr >> 1)) & 1;
        rnd[i] = b ? -1 : 1;
        sr >>= 1;
        if (b)
            sr |= 0x4000;
    }
    d[0] = 1;
    for (int i = 1; i < 385; i++)
        d[i] = seq[i - 1] ? -d[i - 1] : d[i - 1];
    memset(t->p1pat, 0, sizeof t->p1pat);
    for (int i = 0; i < 384; i++)
        t->p1pat[t2_p1_active_carriers[i] + 86] = (float)(d[i + 1] * rnd[i]);
}

/* ------------------------------------------------------------------ construction */

static void member_free(dvbt2 *t)
{
    if (!t)
        return;
    dvb_fec_free(&t->fec);
    dvb_ldpc_free(&t->l1ldpc_pre);
    dvb_ldpc_free(&t->l1ldpc_post);
    free(t->cw);
    free(t->cellv);
    free(t->cperm);
    free(t->icperm);
    free(t->mcells);
    free(t->zz);
    free(t->mbuf[0]);
    free(t->mbuf[1]);
    free(t->l1post);
    free(t->dummy);
    for (int i = 0; i < 2; i++) {
        free(t->Hd[i]);
        free(t->Hp2[i]);
        free(t->Hfc[i]);
    }
    cmap_free(&t->p2map);
    cmap_free(&t->fcmap);
    if (t->dmap)
        for (int s = 0; s < t->p.dy; s++)
            cmap_free(&t->dmap[s]);
    free(t->dmap);
    free(t->symcells);
    free(t);
}

void dvbt2_free(dvbt2 *t)
{
    if (!t)
        return;
    dvbt2_group_free(t->own);   /* does not own t */
    member_free(t);
}

static dvbt2 *member_new(const dvbt2_cfg *c, ts_pull_fn pull, void *ctx)
{
    dvbt2 *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->c = *c;
    if (derive(c, &t->p)) {
        free(t);
        return NULL;
    }
    const t2p *p = &t->p;
    t->t2_frames = c->t2_frames ? c->t2_frames : 2;
    t->frequency = c->frequency ? c->frequency : 729833333u;
    int fail = dvb_fec_init(&t->fec, &p->code, 0, pull, ctx);

    dvb_fec_code l1c;
    dvb_bch_init(&t->l1bch, 1, NBCH_PARITY);
    dvb_fec_code_get(1, 1, 4, DVB_FEC_T2, &l1c);
    fail |= dvb_ldpc_init(&t->l1ldpc_pre, &l1c);
    dvb_fec_code_get(1, 1, 2, DVB_FEC_T2, &l1c);
    fail |= dvb_ldpc_init(&t->l1ldpc_post, &l1c);

    int n = p->code.nldpc, cells = p->cells;
    t->cw = malloc((size_t)n / 8 + 8);
    t->cellv = malloc((size_t)cells);
    t->cperm = malloc(sizeof(uint32_t) * (size_t)cells);
    t->icperm = malloc(sizeof(uint32_t) * (size_t)cells);
    t->mcells = malloc(sizeof(float complex) * (size_t)cells);
    t->mbuf[0] = calloc((size_t)p->mapped, sizeof(float complex));
    t->mbuf[1] = calloc((size_t)p->mapped, sizeof(float complex));
    t->l1post = malloc(sizeof(float complex) * (size_t)(p->npost / p->eta));
    t->dummy = malloc(sizeof(float complex) * (size_t)(p->ndummy ? p->ndummy : 1));
    if (p->np2 > 1)
        t->zz = malloc(sizeof(uint32_t) * (size_t)p->mapped);
    for (int i = 0; i < 2; i++) {
        t->Hd[i] = malloc(sizeof(uint32_t) * (size_t)p->cdata);
        t->Hp2[i] = malloc(sizeof(uint32_t) * (size_t)p->cp2);
        t->Hfc[i] = malloc(sizeof(uint32_t) * (size_t)(p->nfc ? p->nfc : 1));
    }
    t->symcells = malloc(sizeof(float complex) * (size_t)(p->cdata > p->cp2 ? p->cdata : p->cp2));
    if (fail || !t->cw || !t->cellv || !t->cperm || !t->icperm ||
        !t->mcells || !t->mbuf[0] || !t->mbuf[1] || !t->l1post || !t->dummy ||
        (p->np2 > 1 && !t->zz) || !t->Hd[0] || !t->Hd[1] || !t->Hp2[0] || !t->Hp2[1] ||
        !t->Hfc[0] || !t->Hfc[1] || !t->symcells)
        goto err;

    build_bit_interleaver(t);
    build_constellations(t);
    if (build_cell_interleaver(t))
        goto err;
    if (t->zz)
        build_zz(t);
    for (int i = 0; i < 2; i++) {
        if (fi_build(p->fidx, p->cdata, i, t->Hd[i]) || fi_build(p->fidx, p->cp2, i, t->Hp2[i]))
            goto err;
        if (p->nfc && fi_build(p->fidx, p->nfc, i, t->Hfc[i]))
            goto err;
    }
    if (build_pilots(t))
        goto err;
    build_l1pre(t);
    {
        int sr = 0x4A80;
        for (int i = 0; i < p->ndummy; i++) {
            int b = (sr ^ (sr >> 1)) & 1;
            t->dummy[i] = b ? -1.0f : 1.0f;
            sr >>= 1;
            if (b)
                sr |= 0x4000;
        }
    }
    t->norm = (float)(5.0 / sqrt(27.0 * p->cps));
    build_p1_pattern(t);
    t->l = -1;
    return t;
err:
    member_free(t);
    return NULL;
}

/* ------------------------------------------------------------------ running */

/* Encode the next FEC block of the frame being prepared into mbuf[!cur]. */
static void encode_block(dvbt2 *t)
{
    const t2p *p = &t->p;
    int cells = p->cells;
    dvb_fec_frame(&t->fec, t->cw, t->bi_ncol != 0);
    bits_to_cells(t);
    const uint8_t *cv = t->cellv;
    /* position in the TI structure */
    int big = t->enc_ti >= t->nsmall;
    int per = big ? t->big_n : t->small_n;
    if (t->enc_r == 0)
        t->enc_n = 0;
    int shift = cells;
    while (shift >= cells) {
        int tmp = t->enc_n;
        shift = 0;
        for (int k = 0; k < t->pn_degree; k++) {
            shift |= tmp & 1;
            shift <<= 1;
            tmp >>= 1;
        }
        t->enc_n++;
    }
    /* modulation (with the cyclic Q delay of rotated constellations) */
    const float complex *m = t->constel;
    float complex *mc = t->mcells;
    if (t->c.rotation) {
        float q = cimagf(m[cv[cells - 1]]);
        for (int w = 0; w < cells; w++) {
            float complex v = m[cv[w]];
            mc[w] = crealf(v) + I * q;
            q = cimagf(v);
        }
    } else {
        for (int w = 0; w < cells; w++)
            mc[w] = m[cv[w]];
    }
    /* Cell interleaver (cell w goes to P = (cperm[w] + shift) mod cells of the FEC block)
     * and time interleaver (the TI-block is written in columns of 'rows' cells, 5 per FEC
     * block, and read in rows of 5 * per cells), walked in output order so that consecutive
     * writes stay within a few cache lines. */
    int rows = cells / 5, ncols = 5 * per;
    uint32_t base = (uint32_t)(N_L1PRE + p->npost / p->eta + t->enc_tistart + 5 * t->enc_r);
    float complex *M = t->mbuf[!t->cur];
    const uint32_t *ic = t->icperm;
    uint32_t P0 = (uint32_t)(cells - shift);   /* P - shift mod cells, for P = 0 */
    for (int col = 0; col < 5; col++) {
        for (int k = 0; k < rows; k++) {
            uint32_t P = P0 + (uint32_t)(col * rows + k);
            if (P >= (uint32_t)cells)
                P -= (uint32_t)cells;
            uint32_t d = base + (uint32_t)col + (uint32_t)k * (uint32_t)ncols;
            M[zzpos(t, (int)d)] = mc[ic[P]];
        }
    }
    /* advance */
    if (++t->enc_r == per) {
        t->enc_r = 0;
        t->enc_tistart += per * cells;
        t->enc_ti++;
    }
    t->enc_block++;
}

/* Complete the frame being prepared (L1, dummy cells) and make it current. */
static void finish_frame(dvbt2 *t)
{
    const t2p *p = &t->p;
    while (t->enc_block < p->nfec)
        encode_block(t);
    float complex *M = t->mbuf[!t->cur];
    int np = p->npost / p->eta;
    for (int i = 0; i < N_L1PRE; i++)
        M[zzpos(t, i)] = t->l1pre[i];
    build_l1post(t, t->t2_frame_num, t->l1post);
    t->t2_frame_num = (t->t2_frame_num + 1) % t->t2_frames;
    for (int i = 0; i < np; i++)
        M[zzpos(t, N_L1PRE + i)] = t->l1post[i];
    int o = N_L1PRE + np + p->stream;
    for (int i = 0; i < p->ndummy; i++)
        M[zzpos(t, o + i)] = t->dummy[i];
    o += p->ndummy;
    for (int i = 0; i < p->nfc - p->cfc; i++)
        M[zzpos(t, o + i)] = 0;
    t->cur = !t->cur;
    t->enc_block = t->enc_ti = t->enc_r = t->enc_n = t->enc_tistart = 0;
    t->frame_count++;
}

/* Before each step (P1 or symbol) of a member: frame boundaries and the share of the FEC
 * work of the next frame (spread over the LF + 1 steps of the current one). */
static void member_step(dvbt2 *t)
{
    const t2p *p = &t->p;
    if (!t->started) {
        finish_frame(t);   /* encode the whole first frame */
        t->started = 1;
        t->l = -1;
    } else if (t->l == p->lf) {
        finish_frame(t);
        t->l = -1;
    }
    int step = t->l + 1;
    int target = (int)(((long)p->nfec * (step + 1) + p->lf) / (p->lf + 1));
    if (target > p->nfec)
        target = p->nfec;
    while (t->enc_block < target)
        encode_block(t);
}

/* Place the carriers of symbol l (0 = first P2) in the spectrum b of size Nt, centre carrier
 * at bin 'off', each carrier multiplied by sc (unless unit). Members do not overlap. */
static void member_place(dvbt2 *t, int l, float complex *b, int Nt, int off, float complex sc, int unit)
{
    const t2p *p = &t->p;
    const float complex *M = t->mbuf[t->cur];
    const float complex *cells;
    const uint32_t *H;
    const cmap *m;
    if (l < p->np2) {
        cells = M + (size_t)l * p->cp2;
        H = t->Hp2[l & 1];
        m = &t->p2map;
    } else {
        cells = M + (size_t)p->np2 * p->cp2 + (size_t)(l - p->np2) * p->cdata;
        if (p->nfc && l == p->lf - 1) {
            H = t->Hfc[l & 1];
            m = &t->fcmap;
        } else {
            H = t->Hd[l & 1];
            m = &t->dmap[l % p->dy];
        }
    }
    float complex *c = t->symcells;
    memcpy(c, cells, sizeof(float complex) * (size_t)m->ndata);   /* sequential, then random reads in cache */
    float s = t->pn[l] ? -1.0f : 1.0f;
    if (unit) {
        for (int i = 0; i < m->ndata; i++) {
            int bin = m->data_rel[i] + off;
            b[bin < 0 ? bin + Nt : bin] = c[H[i]];
        }
        for (int i = 0; i < m->npilot; i++) {
            int bin = m->pilot_rel[i] + off;
            b[bin < 0 ? bin + Nt : bin] = m->pilot_val[i] * s;
        }
    } else {
        for (int i = 0; i < m->ndata; i++) {
            int bin = m->data_rel[i] + off;
            b[bin < 0 ? bin + Nt : bin] = c[H[i]] * sc;
        }
        for (int i = 0; i < m->npilot; i++) {
            int bin = m->pilot_rel[i] + off;
            b[bin < 0 ? bin + Nt : bin] = (m->pilot_val[i] * s) * sc;
        }
    }
}

/* ------------------------------------------------------------------ groups */

int dvbt2_timing(const dvbt2_cfg *c, dvbt2_timing_t *tm)
{
    t2p p;
    if (derive(c, &p))
        return -1;
    memset(tm, 0, sizeof *tm);
    tm->fft = p.N;
    tm->guard = p.gi;
    tm->n_p2 = p.np2;
    tm->l_data = p.ldata;
    tm->frame_samples = (int)p.frame_samples;
    tm->t_num = p.tnum;
    tm->t_den = p.tden;
    tm->sample_rate = p.fs;
    return 0;
}

int dvbt2_timing_equal(const dvbt2_timing_t *a, const dvbt2_timing_t *b)
{
    return a->fft == b->fft && a->guard == b->guard && a->n_p2 == b->n_p2 && a->l_data == b->l_data &&
           a->frame_samples == b->frame_samples && a->t_num == b->t_num && a->t_den == b->t_den;
}

const char *dvbt2_group_error(int err)
{
    switch (err) {
    case DVBT2_GROUP_EINVAL: return "invalid DVB-T2 parameters";
    case DVBT2_GROUP_ETIMING: return "frame timing differs from the other carriers of the group "
                                     "(bandwidth, FFT size, guard interval and data symbols must be equal)";
    case DVBT2_GROUP_EOVERLAP: return "overlaps another carrier of the group";
    case DVBT2_GROUP_EBAND: return "outside the radio bandwidth";
    case DVBT2_GROUP_EP1: return "offset is not a multiple of the P1 subcarrier spacing (fft / 1024 bins)";
    case DVBT2_GROUP_EFULL: return "too many carriers in the group";
    case DVBT2_GROUP_ENOMEM: return "out of memory";
    default: return err >= 0 ? "no error" : "unknown error";
    }
}

int dvbt2_group_members(const dvbt2_group *g) { return g->n; }

int dvbt2_group_worker(const dvbt2_group *g, int member, pthread_t *th, int *sharing)
{
    if (!g->nthreads || member < 0 || member >= g->n)
        return 0;
    int w = member % g->nthreads;
    *th = g->tid[w];
    *sharing = (g->n - 1 - w) / g->nthreads + 1;      /* members w, w + nthreads, ... */
    return 1;
}

static void group_job(dvbt2_group *g, int w);

static void *group_worker(void *arg)
{
    dvbt2_group *g = ((void **)arg)[0];
    int w = (int)(intptr_t)((void **)arg)[1];
    free(arg);
    /* created with the group, before the carrier threads take their real-time priority: take it here too,
     * or the source processes delay the symbols and the radio underruns (best effort, like rf.c) */
    struct sched_param sp = { .sched_priority = 40 };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    unsigned seen = 0;
    pthread_mutex_lock(&g->lock);
    for (;;) {
        while (!g->quit && g->gen == seen)
            pthread_cond_wait(&g->go, &g->lock);
        if (g->quit)
            break;
        seen = g->gen;
        pthread_mutex_unlock(&g->lock);
        group_job(g, w);
        pthread_mutex_lock(&g->lock);
        if (--g->pending == 0)
            pthread_cond_signal(&g->done);
    }
    pthread_mutex_unlock(&g->lock);
    return NULL;
}

dvbt2_group *dvbt2_group_new(int k, int threads)
{
    if (k < 1 || k > 64 || threads < 0 || threads > MAX_MEMBERS)
        return NULL;
    dvbt2_group *g = calloc(1, sizeof *g);
    if (!g)
        return NULL;
    g->k = k;
    pthread_mutex_init(&g->lock, NULL);
    pthread_cond_init(&g->go, NULL);
    pthread_cond_init(&g->done, NULL);
    if (threads > 0) {
        g->tid = calloc((size_t)threads, sizeof(pthread_t));
        if (!g->tid) {
            free(g);
            return NULL;
        }
        for (int w = 0; w < threads; w++) {
            void **arg = malloc(2 * sizeof(void *));
            if (!arg)
                break;
            arg[0] = g;
            arg[1] = (void *)(intptr_t)w;
            if (pthread_create(&g->tid[w], NULL, group_worker, arg)) {
                free(arg);
                break;
            }
            g->nthreads = w + 1;
        }
    }
    return g;
}

static void job_wait(dvbt2_group *g)
{
    if (!g->inflight)
        return;
    pthread_mutex_lock(&g->lock);
    while (g->pending)
        pthread_cond_wait(&g->done, &g->lock);
    pthread_mutex_unlock(&g->lock);
    g->inflight = 0;
}

void dvbt2_group_free(dvbt2_group *g)
{
    if (!g)
        return;
    job_wait(g);
    if (g->tid) {
        pthread_mutex_lock(&g->lock);
        g->quit = 1;
        pthread_cond_broadcast(&g->go);
        pthread_mutex_unlock(&g->lock);
        for (int w = 0; w < g->nthreads; w++)
            pthread_join(g->tid[w], NULL);
        free(g->tid);
    }
    pthread_mutex_destroy(&g->lock);
    pthread_cond_destroy(&g->go);
    pthread_cond_destroy(&g->done);
    for (int i = 0; i < g->n; i++)
        if (g->m[i].owned)
            member_free(g->m[i].t);
    pthread_mutex_lock(&plan_lock);
    if (g->plan)
        fftwf_destroy_plan(g->plan);
    if (g->p1plan)
        fftwf_destroy_plan(g->p1plan);
    pthread_mutex_unlock(&plan_lock);
    for (int i = 0; i < 2; i++)
        if (g->fbuf[i])
            fftwf_free(g->fbuf[i]);
    if (g->p1buf)
        fftwf_free(g->p1buf);
    if (g->sym)
        fftwf_free(g->sym);
    free(g->p1a);
    free(g);
}

static void group_scales(dvbt2_group *g)
{
    /* a lone member keeps gr-dtv's order (IFFT, then x normalisation, its cells untouched);
     * with several members everything is applied to the cells (no pass after the IFFT) */
    if (g->n == 1) {
        float n0 = g->m[0].t->norm * g->m[0].gain;
        g->post = n0 != 0.0f ? n0 : 1.0f;
    } else {
        g->post = 1.0f;
    }
    for (int i = 0; i < g->n; i++)
        g->m[i].pre = g->m[i].t->norm * g->m[i].gain / g->post;
}

static int group_attach(dvbt2_group *g, dvbt2 *t, int off, float gain, int owned)
{
    const t2p *p = &t->p;
    dvbt2_timing_t tm;
    dvbt2_timing(&t->c, &tm);
    if (g->n >= MAX_MEMBERS)
        return DVBT2_GROUP_EFULL;
    if (g->sym_len)
        return DVBT2_GROUP_EINVAL;   /* already running */
    if (g->n && !dvbt2_timing_equal(&tm, &g->timing))
        return DVBT2_GROUP_ETIMING;
    int Nt = p->N * g->k, half = (p->cps - 1) / 2;
    if (off - half <= -Nt / 2 || off + half >= Nt / 2)
        return DVBT2_GROUP_EBAND;
    if ((off * 1024) % p->N)
        return DVBT2_GROUP_EP1;
    for (int i = 0; i < g->n; i++)
        if (off + half >= g->m[i].lo && off - half <= g->m[i].hi)
            return DVBT2_GROUP_EOVERLAP;
    if (!g->n) {
        g->timing = tm;
        g->N = p->N;
        g->Nt = Nt;
        g->gi = p->gi * g->k;
        g->lf = p->lf;
        size_t len = (size_t)(Nt + g->gi > 2048 * g->k ? Nt + g->gi : 2048 * g->k);
        g->sym = (float complex *)fftwf_alloc_complex(len);
        g->p1a = malloc(sizeof(float complex) * 1024 * (size_t)g->k);
        g->fbuf[0] = fftwf_alloc_complex((size_t)Nt);
        g->fbuf[1] = fftwf_alloc_complex((size_t)Nt);
        g->p1buf = fftwf_alloc_complex(1024 * (size_t)g->k);
        if (!g->sym || !g->p1a || !g->fbuf[0] || !g->fbuf[1] || !g->p1buf)
            return DVBT2_GROUP_ENOMEM;
        pthread_mutex_lock(&plan_lock);
        /* P1 plan first: planned after a MEASURE of the same size, FFTW would reuse that wisdom */
        g->p1plan = fftwf_plan_dft_1d(1024 * g->k, g->p1buf, g->p1buf, FFTW_BACKWARD, FFTW_ESTIMATE);
        g->plan = fftwf_plan_dft_1d(Nt, g->fbuf[0], (fftwf_complex *)(g->sym + g->gi), FFTW_BACKWARD,
                                    FFTW_MEASURE | FFTW_PRESERVE_INPUT);
        pthread_mutex_unlock(&plan_lock);
        if (!g->plan || !g->p1plan)
            return DVBT2_GROUP_ENOMEM;
        /* planning wrote the arrays; bins outside the members stay zero from now on */
        memset(g->fbuf[0], 0, sizeof(fftwf_complex) * (size_t)Nt);
        memset(g->fbuf[1], 0, sizeof(fftwf_complex) * (size_t)Nt);
        g->l = -1;
    }
    struct gmember *m = &g->m[g->n++];
    m->t = t;
    m->off = off;
    m->off1 = off * 1024 / p->N;
    m->lo = off - half;
    m->hi = off + half;
    m->gain = gain;
    m->owned = owned;
    group_scales(g);
    return g->n - 1;
}

int dvbt2_group_add(dvbt2_group *g, const dvbt2_cfg *c, int offset_bins, float gain, ts_pull_fn pull, void *ctx)
{
    dvbt2_timing_t tm;
    if (dvbt2_timing(c, &tm))
        return DVBT2_GROUP_EINVAL;
    if (g->n && !dvbt2_timing_equal(&tm, &g->timing))
        return DVBT2_GROUP_ETIMING;   /* before the expensive construction */
    dvbt2 *t = member_new(c, pull, ctx);
    if (!t)
        return DVBT2_GROUP_ENOMEM;
    int r = group_attach(g, t, offset_bins, gain, 1);
    if (r < 0)
        member_free(t);
    return r;
}

/* exp(j 2 pi num / den), exactly 1 for num = 0 */
static float complex cis_frac(int64_t num, int64_t den)
{
    num %= den;
    if (num < 0)
        num += den;
    if (!num)
        return 1.0f;
    double a = 2.0 * M_PI * (double)num / (double)den;
    return (float)cos(a) + I * (float)sin(a);
}

/* P1 at k times the rate: the 1K patterns of the members at their offsets, A = IFFT, C and B
 * from the pattern shifted by one 1K subcarrier (f_SH); each member's phase follows a
 * continuous oscillator at its offset (the same one as for the data symbols). */
static void group_p1(dvbt2_group *g, int64_t Tf)
{
    int k = g->k, M = 1024 * k;
    float complex *x = (float complex *)g->p1buf;
    float complex *o = g->sym;
    float sc = (float)sqrt(384.0);
    for (int pass = 0; pass < 2; pass++) {   /* 0: A (unshifted), 1: C/B (shifted) */
        memset(x, 0, sizeof(float complex) * (size_t)M);
        for (int i = 0; i < g->n; i++) {
            const struct gmember *m = &g->m[i];
            if (m->muted)
                continue;
            int64_t start = pass ? Tf : Tf + 542 * k;     /* where the segment's index 0 lies */
            float complex ph = cis_frac((int64_t)m->off1 * (start % M), M);
            int unit = ph == 1.0f && m->gain == 1.0f;
            float complex gp = m->gain * ph;
            for (int j = 0; j < 1024; j++) {
                float v = m->t->p1pat[j];
                if (v == 0.0f)
                    continue;
                int bin = j - 512 + m->off1 + pass;
                bin %= M;
                if (bin < 0)
                    bin += M;
                x[bin] = unit ? v : v * gp;
            }
        }
        fftwf_execute(g->p1plan);
        if (pass == 0) {
            for (int i = 0; i < M; i++)
                g->p1a[i] = x[i] / sc;
        } else {
            for (int i = 0; i < 542 * k; i++)
                o[i] = x[i] / sc;
            memcpy(o + 542 * k, g->p1a, sizeof(float complex) * (size_t)M);
            for (int i = 542 * k; i < M; i++)
                o[i + M] = x[i] / sc;
        }
    }
    g->sym_len = 2048 * k;
    g->sym_pos = 0;
}

/* Work of worker w (or of the caller without workers) for step job_l: the members' FEC
 * shares, then (for a symbol) their carriers in spectrum job_bi. */
static void group_job(dvbt2_group *g, int w)
{
    int Nt = g->Nt, step = g->nthreads ? g->nthreads : 1;
    float complex *b = (float complex *)g->fbuf[g->job_bi];
    for (int i = w; i < g->n; i += step) {
        const struct gmember *m = &g->m[i];
        member_step(m->t);
        if (g->job_l >= 0) {
            /* this member's bins (the IFFT does not modify its input; gaps stay zero) */
            int lo = m->lo, hi = m->hi;
            if (hi < 0) {
                memset(b + Nt + lo, 0, sizeof(float complex) * (size_t)(hi - lo + 1));
            } else if (lo < 0) {
                memset(b + Nt + lo, 0, sizeof(float complex) * (size_t)(-lo));
                memset(b, 0, sizeof(float complex) * (size_t)(hi + 1));
            } else {
                memset(b + lo, 0, sizeof(float complex) * (size_t)(hi - lo + 1));
            }
            float complex ph = cis_frac((int64_t)m->off * (g->job_win % Nt), Nt);
            int unit = ph == 1.0f && m->pre == 1.0f;
            if (!m->muted)
                member_place(m->t, g->job_l, b, Nt, m->off, m->pre * ph, unit);
        }
        m->t->l++;
    }
}

static void job_start(dvbt2_group *g, int l, int64_t T, int bi)
{
    g->job_l = l;
    g->job_win = T + g->gi;   /* start of the FFT window */
    g->job_bi = bi;
    if (!g->nthreads) {
        group_job(g, 0);
        return;
    }
    pthread_mutex_lock(&g->lock);
    g->pending = g->nthreads;
    g->gen++;
    pthread_cond_broadcast(&g->go);
    pthread_mutex_unlock(&g->lock);
    g->inflight = 1;
}

int dvbt2_group_samples(dvbt2_group *g, float complex *out, int max)
{
    if (!g->n)
        return 0;
    if (g->sym_pos >= g->sym_len) {
        int l = g->l, bi = g->bi;
        int64_t T = g->T;
        if (!g->sym_len && !g->inflight)   /* first step */
            job_start(g, l, T, bi);
        job_wait(g);
        /* the next step is prepared (by the workers, meanwhile) in the other spectrum */
        int len = l < 0 ? 2048 * g->k : g->Nt + g->gi;
        int l2 = l + 1 == g->lf ? -1 : l + 1;
        g->l = l2;
        g->T = T + len;
        g->bi = !bi;
        if (g->nthreads)
            job_start(g, l2, g->T, g->bi);
        if (l < 0) {
            group_p1(g, T);
        } else {
            float complex *o = g->sym;
            int Nt = g->Nt, gi = g->gi;
            fftwf_execute_dft(g->plan, g->fbuf[bi], (fftwf_complex *)(o + gi));
            if (g->post != 1.0f)
                for (int i = 0; i < Nt; i++)
                    o[gi + i] *= g->post;
            memcpy(o, o + Nt, sizeof(float complex) * (size_t)gi);
            g->sym_len = Nt + gi;
            g->sym_pos = 0;
        }
        if (!g->nthreads)
            job_start(g, l2, g->T, g->bi);   /* synchronous: prepare the next step now */
    }
    int n = g->sym_len - g->sym_pos;
    if (n > max)
        n = max;
    memcpy(out, g->sym + g->sym_pos, sizeof(float complex) * (size_t)n);
    g->sym_pos += n;
    return n;
}

dvbt2 *dvbt2_new(const dvbt2_cfg *c, ts_pull_fn pull, void *ctx)
{
    dvbt2 *t = member_new(c, pull, ctx);
    if (!t)
        return NULL;
    t->own = dvbt2_group_new(1, 0);
    if (!t->own || group_attach(t->own, t, 0, 1.0f, 0) < 0) {
        dvbt2_free(t);
        return NULL;
    }
    return t;
}

int dvbt2_samples(dvbt2 *t, float complex *out, int max)
{
    return dvbt2_group_samples(t->own, out, max);
}

void dvbt2_group_mute(dvbt2_group *g, int member, int muted)
{
    if (member >= 0 && member < g->n)
        g->m[member].muted = muted;
}
