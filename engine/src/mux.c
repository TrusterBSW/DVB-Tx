/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "mux.h"

#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "psi.h"
#include "ts.h"
#include "util.h"

#define MAX_SVC 256
#define MAX_TAB 64
#define MAX_CAR (MAX_SVC + MAX_TAB + 4)
#define PCR_ONLY_PID_OFF 31

enum { CK_FILE, CK_PAT, CK_PMT, CK_TDT, CK_TOT, CK_FREE };
enum { OP_ADD, OP_DEL, OP_SET };
#define PRIMARY_SWAP_NS 15000000000LL   /* a new primary source that does not come is used anyway */

typedef struct carousel {
    int kind, svc;               /* svc: index for CK_PMT */
    int pid;
    int64_t period_ns;           /* whole cycle */
    int64_t next_due;
    uint8_t *pkts;               /* packets without CC */
    int npkts, cap;
    int *sec_first;              /* first packet of each section */
    int nsec;
    int cur_sec, cur_pkt;        /* cur_pkt >= 0 while a section is being sent */
    char path[512];
    struct timespec mtime;
    off_t size;
} carousel;

typedef struct win_stats {
    uint64_t out_pkts, dropped_late, overruns, pcr_inserted, disc;
    int64_t late_max;
    int64_t vmin, vsum;
    uint64_t vn;
    int64_t amin, asum;
    uint64_t an;
} win_stats;

typedef struct svc {
    mux_svc_cfg c;
    int pmt_pid;
    input *in[2];
    input *next_primary;         /* replacing in[0] as soon as it delivers */
    int64_t next_since;
    uint64_t pos[2];
    int cur;                     /* 0 primary, 1 backup, -1 none */
    int64_t up_since[2];
    int64_t next_check;
    int64_t created;
    int waiting_key;
    int64_t wait_start;
    int pending_disc;
    uint64_t switches;

    int prog_gen;
    prog_info prog;
    uint16_t map[8192];          /* input PID -> output PID, 0 = drop */
    uint8_t kind[8192];          /* 1 video, 2 audio (of the input PID) */
    int video_pid_in;
    int out_pcr_pid;
    int pmt_version;
    int car_pmt;

    in_entry head;
    int have_head;
    int64_t anchor_ns;           /* t_ideal - pcr: maps the source clock to monotonic time */
    int have_anchor;
    int64_t last_pcr_out;

    win_stats w, pubw;           /* local window, published window */
    int cur_pub, nes_pub, vtype_pub, natypes_pub;
    int in_pub[3];               /* ids of the primary, backup and pending primary inputs, -1 none */
    int atypes_pub[8];
} svc;

typedef struct mux_op {
    int kind, sid;
    svc *s;                      /* OP_ADD */
    input *primary, *backup;     /* OP_SET */
    int no_backup, set_name;
    char name[64];
    int result;
    char err[160];
    _Atomic int state;           /* 0 queued, 1 done, 2 abandoned by the caller (the multiplexer frees it) */
    struct mux_op *next;
} mux_op;

struct mux {
    mux_cfg c;
    mux_clock_fn clock;
    void *clock_ctx;
    double pkt_ns;
    uint64_t slot;
    uint8_t cc[8192];

    svc *svc[MAX_SVC];
    int nsvc;
    carousel car[MAX_CAR];
    int ncar;
    int pat_car;
    uint8_t tot_desc[256];
    int tot_desc_len;

    int64_t next_pub, next_reload;
    _Atomic int reload_req;

    /* carrier counters */
    uint64_t null_pkts, si_pkts, es_pkts, total_pkts;
    uint64_t pub_null, pub_si, pub_es, pub_total;
    uint64_t prev_null, prev_si, prev_es, prev_total;
    int64_t pub_now_lag;         /* emission time - monotonic now, published */
    pthread_mutex_t lock;

    int mon_fd, nmon;
    struct sockaddr_in mon_sa[8];
    uint8_t mon_buf[7 * TS_PKT];
    int mon_n;

    int pat_version;
    pthread_mutex_t op_lock;
    mux_op *ops, **ops_tail;
    _Atomic int nops;
};

/* ---- carousels ---- */

static void car_set_sections(carousel *k, const uint8_t *secs, int len)
{
    int npk = 0, nsec = 0;
    for (int off = 0; off + 3 <= len; off += psi_sec_len(secs + off)) {
        int l = psi_sec_len(secs + off);
        npk += (l + 1 + TS_PKT - 5) / (TS_PKT - 4);
        nsec++;
    }
    if (npk > k->cap) {
        k->pkts = realloc(k->pkts, (size_t)npk * TS_PKT);
        k->cap = npk;
    }
    k->sec_first = realloc(k->sec_first, sizeof(int) * (nsec + 1));
    int n = 0, s = 0;
    for (int off = 0; off + 3 <= len; off += psi_sec_len(secs + off)) {
        uint8_t cc = 0;
        k->sec_first[s++] = n;
        n += psi_packetize(secs + off, psi_sec_len(secs + off), k->pid, &cc, k->pkts + n * TS_PKT, npk - n);
    }
    k->sec_first[s] = n;
    k->npkts = n;
    k->nsec = s;
    if (k->cur_sec >= s)
        k->cur_sec = 0;
    k->cur_pkt = -1;
}

static carousel *car_add(mux *m, int kind, int pid, int period_ms)
{
    carousel *k = NULL;
    for (int i = 0; i < m->ncar && !k; i++)        /* slot of a removed service */
        if (m->car[i].kind == CK_FREE)
            k = &m->car[i];
    if (!k) {
        if (m->ncar >= MAX_CAR)
            return NULL;
        k = &m->car[m->ncar++];
    }
    uint8_t *pkts = k->pkts;
    int *sec_first = k->sec_first, cap = k->cap;
    memset(k, 0, sizeof *k);
    k->pkts = pkts;                                 /* buffers of a reused slot */
    k->sec_first = sec_first;
    k->cap = cap;
    k->kind = kind;
    k->pid = pid;
    k->period_ns = (int64_t)period_ms * 1000000;
    k->cur_pkt = -1;
    return k;
}

static int car_load_file(carousel *k)
{
    struct stat st;
    if (stat(k->path, &st))
        return -1;
    if (st.st_mtim.tv_sec == k->mtime.tv_sec && st.st_mtim.tv_nsec == k->mtime.tv_nsec &&
        st.st_size == k->size)
        return 0;
    uint8_t *buf;
    int nsec;
    int len = psi_load_sections(k->path, &buf, &nsec);
    if (len < 0) {
        LOGW("table %s: unreadable or malformed", k->path);
        return -1;
    }
    k->mtime = st.st_mtim;
    k->size = st.st_size;
    car_set_sections(k, buf, len);
    free(buf);
    LOGD("table %s: %d sections, %d packets on PID %d", k->path, k->nsec, k->npkts, k->pid);
    return 1;
}

/* ---- services ---- */

static int stream_kind(int type, const es_info *e)
{
    switch (type) {
    case 0x01: case 0x02: case 0x10: case 0x1B: case 0x24: case 0x42: case 0xD1: case 0xEA:
        return 1;
    case 0x03: case 0x04: case 0x0F: case 0x11: case 0x81: case 0x87:
        return 2;
    case 0x06:
        for (int i = 0; i + 2 <= e->desc_len; i += 2 + e->desc[i + 1])
            if (e->desc[i] == 0x6A || e->desc[i] == 0x7A || e->desc[i] == 0x7C || e->desc[i] == 0x81)
                return 2;
        return 0;
    }
    return 0;
}

static void svc_build_pmt(mux *m, svc *s)
{
    uint8_t sec[SEC_MAX];
    psi_es es[IN_MAX_ES];
    int nes = 0, len;
    if (s->prog.valid) {
        static __thread uint8_t dvbdesc[IN_MAX_ES][256];
        for (int i = 0; i < s->prog.nes; i++) {
            const es_info *e = &s->prog.es[i];
            es[nes].pid = s->map[e->pid];
            es[nes].type = e->type;
            es[nes].desc_len = e->desc_len;
            es[nes].desc = e->desc;
            /* ATSC AC-3 (0x81) / E-AC-3 (0x87) signalling -> DVB: private PES + AC-3 / enhanced AC-3
             * descriptor, otherwise European TVs play no sound */
            if ((e->type == 0x81 || e->type == 0x87) && e->desc_len + 3 <= 250) {
                uint8_t *d = dvbdesc[i];
                int n = 0;
                d[n++] = e->type == 0x81 ? 0x6A : 0x7A;
                d[n++] = 1;
                d[n++] = 0x00;
                for (int k = 0; k + 2 <= e->desc_len; k += 2 + e->desc[k + 1])   /* keep language etc. */
                    if (e->desc[k] != 0x05 && e->desc[k] != 0x81 && e->desc[k] != 0xCC && k + 2 + e->desc[k + 1] <= e->desc_len) {
                        memcpy(d + n, e->desc + k, 2 + e->desc[k + 1]);
                        n += 2 + e->desc[k + 1];
                    }
                es[nes].type = 0x06;
                es[nes].desc = d;
                es[nes].desc_len = n;
            }
            nes++;
        }
        len = psi_pmt(sec, s->c.sid, s->pmt_version, s->out_pcr_pid, s->prog.pinfo, s->prog.pinfo_len, es, nes);
    } else {
        len = psi_pmt(sec, s->c.sid, s->pmt_version, PID_NULL, NULL, 0, NULL, 0);
    }
    car_set_sections(&m->car[s->car_pmt], sec, len);
}

static void svc_take_prog(mux *m, svc *s, input *in)
{
    prog_info pi;
    int gen;
    input_get_prog(in, &pi, &gen);
    s->prog_gen = gen;
    s->prog = pi;
    memset(s->map, 0, sizeof s->map);
    memset(s->kind, 0, sizeof s->kind);
    s->video_pid_in = -1;
    for (int i = 0; i < pi.nes && i < PCR_ONLY_PID_OFF - 1; i++) {
        s->map[pi.es[i].pid] = (uint16_t)(s->c.pid_base + 1 + i);
        s->kind[pi.es[i].pid] = (uint8_t)stream_kind(pi.es[i].type, &pi.es[i]);
        if (s->kind[pi.es[i].pid] == 1 && s->video_pid_in < 0)
            s->video_pid_in = pi.es[i].pid;
    }
    if (pi.pcr_pid < PID_NULL) {
        if (!s->map[pi.pcr_pid])
            s->map[pi.pcr_pid] = (uint16_t)(s->c.pid_base + PCR_ONLY_PID_OFF);
        s->out_pcr_pid = s->map[pi.pcr_pid];
    } else {
        s->out_pcr_pid = PID_NULL;
    }
    s->pmt_version = (s->pmt_version + 1) & 31;
    svc_build_pmt(m, s);
}

static int input_up(input *in, int64_t now, int64_t timeout)
{
    if (!in || !in->prog.valid)
        return 0;
    int64_t t = atomic_load_explicit(&in->last_pub_ns, memory_order_relaxed);
    return t && now - t < timeout;
}

/* first ring position whose ideal time is >= t (entries are close to time order) */
static uint64_t seek_time(input *in, int64_t t)
{
    uint64_t h = input_head(in);
    uint64_t lo = h > in->mask - 64 ? h - (in->mask - 64) : 0, hi = h;
    in_entry e;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (input_read(in, mid, &e) != 0) {
            lo = mid + 1;
            continue;
        }
        if (e.t_ideal < t)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static void svc_switch(mux *m, svc *s, int to, int64_t slot_now)
{
    if (s->cur == 0 && to == 0)
        LOGI("service %s: new primary source", s->c.name);
    else if (s->cur >= 0)
        LOGI("service %s: %s -> %s", s->c.name, s->cur ? "backup" : "primary", to ? "backup" : "primary");
    else
        LOGI("service %s: on %s", s->c.name, to ? "backup" : "primary");
    if (s->cur >= 0)
        s->switches++;
    s->cur = to;
    s->have_head = 0;
    s->pos[to] = seek_time(s->in[to], slot_now);
    s->waiting_key = 1;
    s->wait_start = mono_ns();
    s->pending_disc = 1;
    svc_take_prog(m, s, s->in[to]);
}

static void svc_check(mux *m, svc *s, int64_t slot_now)
{
    int64_t now = mono_ns();
    if (now < s->next_check)
        return;
    s->next_check = now + (s->next_primary ? 2000000 : 10000000);
    int64_t to = (int64_t)s->c.switch_timeout_ms * 1000000;
    if (s->next_primary) {
        /* make before break: the old source plays until the new one has a video key frame due now (or,
         * without video, data due now); after PRIMARY_SWAP_NS, or off the primary, at once */
        input *n = s->next_primary;
        int64_t key = -1;
        int ready = s->cur != 0 || now - s->next_since > PRIMARY_SWAP_NS;
        if (!ready && input_up(n, now, to)) {
            if (atomic_load(&n->has_video))
                ready = (key = input_key_between(n, slot_now, slot_now + 40000000)) >= 0;
            else {
                int64_t old = input_oldest(n);
                ready = old >= 0 && old <= slot_now;
            }
        }
        if (ready) {
            atomic_fetch_sub(&s->in[0]->refs, 1);
            s->in[0] = n;
            s->next_primary = NULL;
            s->up_since[0] = 0;
            if (s->cur == 0) {
                svc_switch(m, s, 0, key >= 0 ? key : slot_now);
                return;
            }
        }
    }
    int up0 = input_up(s->in[0], now, to), up1 = input_up(s->in[1], now, to);
    for (int i = 0; i < 2; i++) {
        int up = i ? up1 : up0;
        if (!up)
            s->up_since[i] = 0;
        else if (!s->up_since[i])
            s->up_since[i] = now;
    }
    if (s->cur == 0 && !up0 && up1)
        svc_switch(m, s, 1, slot_now);
    else if (s->cur == 1 && up0 && now - s->up_since[0] >= (int64_t)s->c.return_ms * 1000000)
        svc_switch(m, s, 0, slot_now);
    else if (s->cur < 0 && up0)
        svc_switch(m, s, 0, slot_now);
    else if (s->cur < 0 && up1 && now - s->created >= to)   /* start-up: give the primary a chance */
        svc_switch(m, s, 1, slot_now);
    else if (s->cur >= 0 && atomic_load(&s->in[s->cur]->prog_gen) != s->prog_gen)
        svc_take_prog(m, s, s->in[s->cur]);
}

/* loads the next usable entry of the current input into s->head */
static int svc_peek(mux *m, svc *s, int64_t now)
{
    if (s->have_head)
        return 1;
    if (s->cur < 0)
        return 0;
    input *in = s->in[s->cur];
    for (int guard = 0; guard < 4096; guard++) {
        int r = input_read(in, s->pos[s->cur], &s->head);
        if (r == 1)
            return 0;
        if (r < 0) {           /* overwritten: we fell more than a ring behind */
            s->w.overruns++;
            s->pos[s->cur] = seek_time(in, now);
            continue;
        }
        s->pos[s->cur]++;
        const uint8_t *p = s->head.pkt;
        int pid = ts_pid(p);
        if (!s->map[pid])
            continue;
        if (s->head.flags & IN_DISC)
            s->pending_disc = 1;
        if (s->waiting_key) {
            int64_t waited = mono_ns() - s->wait_start;
            int key = s->video_pid_in < 0 ? ts_pusi(p)
                                          : (pid == s->video_pid_in && ts_pusi(p) && (ts_rai(p) || waited > 2000000000LL));
            if (!key)
                continue;
            s->waiting_key = 0;
        }
        s->have_head = 1;
        return 1;
    }
    (void)m;
    return 0;
}

static void svc_emit(mux *m, svc *s, uint8_t *out, int64_t now)
{
    in_entry *e = &s->head;
    memcpy(out, e->pkt, TS_PKT);
    int pid_in = ts_pid(out), pid = s->map[pid_in];
    ts_set_pid(out, pid);
    if (ts_has_payload(out)) {
        ts_set_cc(out, m->cc[pid]);
        m->cc[pid] = (uint8_t)((m->cc[pid] + 1) & 15);
    } else {
        ts_set_cc(out, (m->cc[pid] - 1) & 15);
    }
    int64_t late = now - e->t_ideal;
    int64_t pcr_now = e->pcr + late * 27 / 1000;
    s->anchor_ns = e->t_ideal - e->pcr * 1000 / 27;
    s->have_anchor = 1;
    if (pid == s->out_pcr_pid && ts_has_pcr(out)) {
        ts_put_pcr(out, pcr_now);
        if (s->pending_disc) {
            out[5] |= 0x80;
            s->pending_disc = 0;
            s->w.disc++;
        }
        s->last_pcr_out = now;
    }
    if (late > s->w.late_max)
        s->w.late_max = late;
    int k = s->kind[pid_in];
    if (k) {
        int64_t dts = ts_pes_dts(out);
        if (dts >= 0) {
            int64_t d = (dts - (pcr_now / 300)) & ((1LL << 33) - 1);
            if (d >= (1LL << 32))
                d -= 1LL << 33;
            int64_t margin = d * 100000 / 9;   /* 90 kHz -> ns */
            if (k == 1) {
                if (!s->w.vn || margin < s->w.vmin)
                    s->w.vmin = margin;
                s->w.vsum += margin;
                s->w.vn++;
            } else {
                if (!s->w.an || margin < s->w.amin)
                    s->w.amin = margin;
                s->w.asum += margin;
                s->w.an++;
            }
        }
    }
    s->w.out_pkts++;
    s->have_head = 0;
}

static void pcr_only(mux *m, svc *s, uint8_t *out, int64_t now)
{
    int pid = s->out_pcr_pid;
    out[0] = 0x47;
    out[1] = (uint8_t)((pid >> 8) & 0x1F);
    out[2] = (uint8_t)pid;
    out[3] = (uint8_t)(0x20 | ((m->cc[pid] - 1) & 15));
    out[4] = 183;
    out[5] = 0x10;
    if (s->pending_disc) {
        out[5] |= 0x80;
        s->pending_disc = 0;
    }
    memset(out + 12, 0xFF, TS_PKT - 12);
    ts_put_pcr(out, (now - s->anchor_ns) * 27 / 1000);
    s->last_pcr_out = now;
    s->w.pcr_inserted++;
}

/* ---- public ---- */

mux *mux_new(const mux_cfg *c, mux_clock_fn clock, void *clock_ctx)
{
    mux *m = xcalloc(1, sizeof *m);
    m->c = *c;
    m->clock = clock;
    m->clock_ctx = clock_ctx;
    m->pkt_ns = TS_PKT * 8 * 1e9 / c->ts_rate;
    pthread_mutex_init(&m->lock, NULL);
    pthread_mutex_init(&m->op_lock, NULL);
    m->ops_tail = &m->ops;
    m->mon_fd = -1;
    char list[sizeof c->monitor];
    snprintf(list, sizeof list, "%s", c->monitor);
    for (char *save = NULL, *tok = strtok_r(list, ",", &save); tok && m->nmon < 8; tok = strtok_r(NULL, ",", &save)) {
        const char *a = !strncmp(tok, "udp://", 6) ? tok + 6 : tok;
        char host[128];
        const char *colon = strrchr(a, ':');
        struct sockaddr_in *sa = &m->mon_sa[m->nmon];
        if (!colon)
            continue;
        snprintf(host, sizeof host, "%.*s", (int)(colon - a), a);
        sa->sin_family = AF_INET;
        sa->sin_port = htons((uint16_t)atoi(colon + 1));
        if (inet_pton(AF_INET, host, &sa->sin_addr) != 1) {
            LOGW("carrier %d: bad monitor address %s", c->carrier, tok);
            continue;
        }
        m->nmon++;
    }
    if (m->nmon) {
        m->mon_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        int snd = 8 << 20, ttl = 4;
        setsockopt(m->mon_fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof snd);
        setsockopt(m->mon_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    }
    if (c->tot_desc_path[0]) {
        FILE *f = fopen(c->tot_desc_path, "rb");
        if (f) {
            m->tot_desc_len = (int)fread(m->tot_desc, 1, sizeof m->tot_desc, f);
            fclose(f);
        }
    }
    return m;
}

double mux_ts_rate(const mux *m) { return m->c.ts_rate; }

static svc *svc_new(const mux_svc_cfg *c)
{
    svc *s = xcalloc(1, sizeof *s);
    s->c = *c;
    s->pmt_pid = c->pid_base;
    s->in[0] = c->primary;
    s->in[1] = c->backup;
    s->cur = -1;
    s->created = mono_ns();
    s->out_pcr_pid = PID_NULL;
    s->w.late_max = INT64_MIN;
    s->in_pub[0] = s->in_pub[1] = s->in_pub[2] = -1;
    /* a different version at every start: a TV that cached the PMT of the previous run reads it again */
    s->pmt_version = (int)((real_ns() / 1000000000LL + c->sid) & 31);
    return s;
}

static int svc_insert(mux *m, svc *s)
{
    if (m->nsvc >= MAX_SVC)
        return -1;
    carousel *k = car_add(m, CK_PMT, s->pmt_pid, s->c.pmt_period_ms);
    if (!k)
        return -1;
    k->svc = m->nsvc;
    s->car_pmt = (int)(k - m->car);
    svc_build_pmt(m, s);
    m->svc[m->nsvc] = s;
    return m->nsvc++;
}

int mux_add_service(mux *m, const mux_svc_cfg *c)
{
    svc *s = svc_new(c);
    int r = svc_insert(m, s);
    if (r < 0) {
        free(s);
        return -1;
    }
    if (c->primary)
        atomic_fetch_add(&c->primary->refs, 1);
    if (c->backup)
        atomic_fetch_add(&c->backup->refs, 1);
    return r;
}

int mux_add_table(mux *m, const mux_table_cfg *t)
{
    carousel *k = car_add(m, CK_FILE, t->pid, t->cycle_ms);
    if (!k)
        return -1;
    snprintf(k->path, sizeof k->path, "%s", t->path);
    if (car_load_file(k) < 0)
        LOGW("carrier %d: table %s not loaded yet", m->c.carrier, t->path);
    return 0;
}

static void pat_build(mux *m)
{
    psi_svc ps[MAX_SVC];
    for (int i = 0; i < m->nsvc; i++) {
        ps[i].sid = m->svc[i]->c.sid;
        ps[i].pmt_pid = m->svc[i]->pmt_pid;
    }
    uint8_t sec[SEC_MAX];
    int len = psi_pat(sec, m->c.tsid, m->pat_version, ps, m->nsvc, m->c.nit_pid);
    car_set_sections(&m->car[m->pat_car], sec, len);
}

void mux_start(mux *m)
{
    carousel *k = car_add(m, CK_PAT, 0, m->c.pat_period_ms);
    m->pat_car = (int)(k - m->car);
    m->pat_version = (int)((real_ns() / 1000000000LL + m->c.tsid) & 31);   /* see svc_new */
    pat_build(m);
    car_add(m, CK_TDT, 0x14, m->c.tdt_period_ms);
    if (m->tot_desc_len > 0 || m->c.tot_desc_path[0])
        car_add(m, CK_TOT, 0x14, m->c.tot_period_ms);
    /* stagger the first transmissions */
    for (int i = 0; i < m->ncar; i++)
        m->car[i].next_due = 0;
}

void mux_reload_tables(mux *m) { atomic_store(&m->reload_req, 1); }

static void do_reload(mux *m)
{
    for (int i = 0; i < m->ncar; i++)
        if (m->car[i].kind == CK_FILE)
            car_load_file(&m->car[i]);
    if (m->c.tot_desc_path[0]) {
        FILE *f = fopen(m->c.tot_desc_path, "rb");
        if (f) {
            m->tot_desc_len = (int)fread(m->tot_desc, 1, sizeof m->tot_desc, f);
            fclose(f);
        }
    }
}

static int car_emit(mux *m, carousel *k, uint8_t *out, int64_t now)
{
    if (k->cur_pkt < 0) {
        if (k->kind == CK_TDT || k->kind == CK_TOT) {
            uint8_t sec[512];
            int64_t utc = real_ns() / 1000000000LL;
            int len = k->kind == CK_TDT ? psi_tdt(sec, utc) : psi_tot(sec, utc, m->tot_desc, m->tot_desc_len);
            car_set_sections(k, sec, len);
        }
        if (!k->nsec)
            return 0;
        k->cur_pkt = k->sec_first[k->cur_sec];
    }
    memcpy(out, k->pkts + (size_t)k->cur_pkt * TS_PKT, TS_PKT);
    ts_set_cc(out, m->cc[k->pid]);
    m->cc[k->pid] = (uint8_t)((m->cc[k->pid] + 1) & 15);
    k->cur_pkt++;
    if (k->cur_pkt >= k->sec_first[k->cur_sec + 1]) {
        k->cur_pkt = -1;
        k->cur_sec = (k->cur_sec + 1) % k->nsec;
        int64_t step = k->period_ns / (k->nsec ? k->nsec : 1);
        k->next_due += step;
        if (k->next_due < now - k->period_ns)
            k->next_due = now;
    }
    return 1;
}

static void publish_stats(mux *m, int64_t now)
{
    pthread_mutex_lock(&m->lock);
    for (int i = 0; i < m->nsvc; i++) {
        svc *s = m->svc[i];
        win_stats *a = &s->pubw, *b = &s->w;
        a->out_pkts += b->out_pkts;
        a->dropped_late += b->dropped_late;
        a->overruns += b->overruns;
        a->pcr_inserted += b->pcr_inserted;
        a->disc += b->disc;
        if (b->late_max > a->late_max)
            a->late_max = b->late_max;
        if (b->vn && (!a->vn || b->vmin < a->vmin))
            a->vmin = b->vmin;
        a->vsum += b->vsum;
        a->vn += b->vn;
        if (b->an && (!a->an || b->amin < a->amin))
            a->amin = b->amin;
        a->asum += b->asum;
        a->an += b->an;
        memset(b, 0, sizeof *b);
        b->late_max = INT64_MIN;
        s->cur_pub = s->cur;
        s->in_pub[0] = s->in[0] ? s->in[0]->id : -1;
        s->in_pub[1] = s->in[1] ? s->in[1]->id : -1;
        s->in_pub[2] = s->next_primary ? s->next_primary->id : -1;
        s->nes_pub = s->prog.valid ? s->prog.nes : 0;
        s->vtype_pub = -1;
        s->natypes_pub = 0;
        for (int j = 0; s->prog.valid && j < s->prog.nes; j++) {
            int kd = stream_kind(s->prog.es[j].type, &s->prog.es[j]);
            if (kd == 1 && s->vtype_pub < 0)
                s->vtype_pub = s->prog.es[j].type;
            else if (kd == 2 && s->natypes_pub < 8)
                s->atypes_pub[s->natypes_pub++] = s->prog.es[j].type;
        }
    }
    m->pub_null = m->null_pkts;
    m->pub_si = m->si_pkts;
    m->pub_es = m->es_pkts;
    m->pub_total = m->total_pkts;
    m->pub_now_lag = now - mono_ns();
    pthread_mutex_unlock(&m->lock);
}

static void monitor_out(mux *m, const uint8_t *pkt)
{
    if (m->mon_fd < 0)
        return;
    memcpy(m->mon_buf + m->mon_n * TS_PKT, pkt, TS_PKT);
    if (++m->mon_n == 7) {
        for (int i = 0; i < m->nmon; i++)
            sendto(m->mon_fd, m->mon_buf, sizeof m->mon_buf, MSG_DONTWAIT, (struct sockaddr *)&m->mon_sa[i],
                   sizeof m->mon_sa[i]);
        m->mon_n = 0;
    }
}

/* ---- changes while running ---- */

static svc *svc_by_sid(mux *m, int sid, int *idx)
{
    for (int i = 0; i < m->nsvc; i++)
        if (m->svc[i]->c.sid == sid) {
            if (idx)
                *idx = i;
            return m->svc[i];
        }
    return NULL;
}

static void unref(input *in)
{
    if (in)
        atomic_fetch_sub(&in->refs, 1);
}

static int op_apply(mux *m, mux_op *op, int64_t now)
{
    int idx;
    svc *s = svc_by_sid(m, op->sid, &idx);
    switch (op->kind) {
    case OP_ADD:
        if (s) {
            snprintf(op->err, sizeof op->err, "service_id %d already on this carrier", op->sid);
            break;
        }
        for (int i = 0; i < m->nsvc; i++) {
            int b = m->svc[i]->c.pid_base;
            if (op->s->c.pid_base < b + 32 && b < op->s->c.pid_base + 32) {
                snprintf(op->err, sizeof op->err, "PIDs %d-%d overlap those of %s", op->s->c.pid_base,
                         op->s->c.pid_base + 31, m->svc[i]->c.name);
                break;
            }
        }
        if (op->err[0])
            break;
        op->s->created = mono_ns();
        pthread_mutex_lock(&m->lock);
        int r = svc_insert(m, op->s);
        if (r >= 0) {
            m->pat_version = (m->pat_version + 1) & 31;
            pat_build(m);
        }
        pthread_mutex_unlock(&m->lock);
        if (r < 0) {
            snprintf(op->err, sizeof op->err, "too many services on this carrier");
            break;
        }
        LOGI("service %s (0x%04X) added", op->s->c.name, op->sid);
        op->s = NULL;
        return 0;
    case OP_DEL:
        if (!s) {
            snprintf(op->err, sizeof op->err, "no service_id %d on this carrier", op->sid);
            break;
        }
        pthread_mutex_lock(&m->lock);
        carousel *k = &m->car[s->car_pmt];
        k->kind = CK_FREE;
        k->nsec = k->npkts = 0;
        k->cur_pkt = -1;
        memmove(&m->svc[idx], &m->svc[idx + 1], sizeof m->svc[0] * (m->nsvc - idx - 1));
        m->nsvc--;
        m->pat_version = (m->pat_version + 1) & 31;
        pat_build(m);
        pthread_mutex_unlock(&m->lock);
        LOGI("service %s (0x%04X) removed", s->c.name, s->c.sid);
        unref(s->in[0]);
        unref(s->in[1]);
        unref(s->next_primary);
        free(s);
        return 0;
    case OP_SET:
        if (!s) {
            snprintf(op->err, sizeof op->err, "no service_id %d on this carrier", op->sid);
            break;
        }
        if (op->set_name) {
            pthread_mutex_lock(&m->lock);
            snprintf(s->c.name, sizeof s->c.name, "%s", op->name);
            pthread_mutex_unlock(&m->lock);
        }
        if (op->backup || op->no_backup) {
            unref(s->in[1]);
            s->in[1] = op->backup;
            s->up_since[1] = 0;
            if (s->cur == 1) {
                if (op->backup)
                    svc_switch(m, s, 1, now);
                else
                    s->cur = -1;
            }
        }
        if (op->primary) {
            unref(s->next_primary);       /* a change not yet in use is replaced */
            s->next_primary = op->primary;
            s->next_since = mono_ns();
            s->next_check = 0;
        }
        return 0;
    }
    /* failed: the references the caller took for this change are dropped here */
    unref(op->primary);
    unref(op->backup);
    if (op->s) {
        unref(op->s->c.primary);
        unref(op->s->c.backup);
        free(op->s);
        op->s = NULL;
    }
    return -1;
}

static void ops_run(mux *m, int64_t now)
{
    pthread_mutex_lock(&m->op_lock);
    mux_op *list = m->ops;
    m->ops = NULL;
    m->ops_tail = &m->ops;
    atomic_store(&m->nops, 0);
    pthread_mutex_unlock(&m->op_lock);
    while (list) {
        mux_op *op = list;
        list = op->next;
        op->result = op_apply(m, op, now);
        if (atomic_exchange(&op->state, 1) == 2)
            free(op);
    }
}

static int op_submit(mux *m, mux_op *op, int timeout_ms, char *err, int errlen)
{
    pthread_mutex_lock(&m->op_lock);
    op->next = NULL;
    *m->ops_tail = op;
    m->ops_tail = &op->next;
    atomic_store(&m->nops, 1);
    pthread_mutex_unlock(&m->op_lock);
    int64_t until = mono_ns() + (int64_t)timeout_ms * 1000000;
    while (atomic_load(&op->state) == 0 && mono_ns() < until) {
        struct timespec ts = { 0, 2000000 };
        nanosleep(&ts, NULL);
    }
    if (atomic_exchange(&op->state, 2) == 0) {
        snprintf(err, errlen, "accepted, not applied yet (carrier not running?)");
        return 1;                 /* applied later by the multiplexer, which then frees op */
    }
    int r = op->result;
    if (r)
        snprintf(err, errlen, "%s", op->err);
    free(op);
    return r;
}

int mux_service_add(mux *m, const mux_svc_cfg *c, int timeout_ms, char *err, int errlen)
{
    mux_op *op = xcalloc(1, sizeof *op);
    op->kind = OP_ADD;
    op->sid = c->sid;
    op->s = svc_new(c);
    return op_submit(m, op, timeout_ms, err, errlen);
}

int mux_service_del(mux *m, int sid, int timeout_ms, char *err, int errlen)
{
    mux_op *op = xcalloc(1, sizeof *op);
    op->kind = OP_DEL;
    op->sid = sid;
    return op_submit(m, op, timeout_ms, err, errlen);
}

int mux_service_set(mux *m, int sid, input *primary, input *backup, int no_backup, const char *name,
                    int timeout_ms, char *err, int errlen)
{
    mux_op *op = xcalloc(1, sizeof *op);
    op->kind = OP_SET;
    op->sid = sid;
    op->primary = primary;
    op->backup = backup;
    op->no_backup = no_backup && !backup;
    if (name) {
        op->set_name = 1;
        snprintf(op->name, sizeof op->name, "%s", name);
    }
    return op_submit(m, op, timeout_ms, err, errlen);
}

void mux_pull(void *ctx, uint8_t *out)
{
    mux *m = ctx;
    int64_t now = m->clock(m->clock_ctx, m->slot);
    if (atomic_load_explicit(&m->nops, memory_order_relaxed))
        ops_run(m, now);
    m->slot++;
    m->total_pkts++;

    if ((m->slot & 255) == 0) {
        int64_t t = mono_ns();
        if (t >= m->next_pub) {
            publish_stats(m, now);
            m->next_pub = t + 100000000;
        }
        if (t >= m->next_reload || atomic_exchange(&m->reload_req, 0)) {
            do_reload(m);
            m->next_reload = t + 1000000000;
        }
    }

    /* 1. PSI/SI due */
    carousel *best = NULL;
    for (int i = 0; i < m->ncar; i++) {
        carousel *k = &m->car[i];
        if (k->cur_pkt >= 0) {        /* finish a section already started */
            best = k;
            break;
        }
        if (k->next_due <= now && (k->nsec || k->kind == CK_TDT || k->kind == CK_TOT) &&
            (!best || k->next_due < best->next_due))
            best = k;
    }
    if (best && car_emit(m, best, out, now)) {
        m->si_pkts++;
        monitor_out(m, out);
        return;
    }

    /* 2. most urgent service packet */
    for (int pass = 0; pass < 64; pass++) {
        svc *pick = NULL;
        int64_t pick_t = INT64_MAX, pick_d = INT64_MAX;
        for (int i = 0; i < m->nsvc; i++) {
            svc *s = m->svc[i];
            svc_check(m, s, now);
            if (!svc_peek(m, s, now))
                continue;
            int64_t t = s->head.t_ideal;
            /* earliest deadline first, the deadline moved 100 ms earlier per priority step above 5 */
            int64_t d = t - (int64_t)(s->c.priority - 5) * 100000000;
            if (t <= now + (int64_t)s->c.early_ms * 1000000 && d < pick_d) {
                pick = s;
                pick_t = t;
                pick_d = d;
            }
        }
        if (!pick)
            break;
        if (now - pick_t > (int64_t)pick->c.max_late_ms * 1000000) {
            pick->w.dropped_late++;
            pick->have_head = 0;
            continue;
        }
        svc_emit(m, pick, out, now);
        m->es_pkts++;
        monitor_out(m, out);
        return;
    }

    /* 3. overdue PCR */
    for (int i = 0; i < m->nsvc; i++) {
        svc *s = m->svc[i];
        if (s->have_anchor && s->out_pcr_pid != PID_NULL && s->c.pcr_max_ms > 0 &&
            now - s->last_pcr_out > (int64_t)s->c.pcr_max_ms * 1000000) {
            pcr_only(m, s, out, now);
            m->es_pkts++;
            monitor_out(m, out);
            return;
        }
    }

    /* 4. stuffing */
    ts_null(out);
    m->null_pkts++;
    monitor_out(m, out);
}

int mux_stats_json(mux *m, char *buf, int len, double dt)
{
    pthread_mutex_lock(&m->lock);
    double ppb = TS_PKT * 8.0 / (dt > 0 ? dt : 1);
    uint64_t tot = m->pub_total - m->prev_total;
    int n = snprintf(buf, len,
                     "{\"carrier\":%d,\"ts_rate\":%.1f,\"rate\":%.1f,\"null_rate\":%.1f,\"si_rate\":%.1f,"
                     "\"es_rate\":%.1f,\"clock_lead_ms\":%.2f,\"services\":[",
                     m->c.carrier, m->c.ts_rate, tot * ppb, (m->pub_null - m->prev_null) * ppb,
                     (m->pub_si - m->prev_si) * ppb, (m->pub_es - m->prev_es) * ppb, m->pub_now_lag / 1e6);
    m->prev_total = m->pub_total;
    m->prev_null = m->pub_null;
    m->prev_si = m->pub_si;
    m->prev_es = m->pub_es;
    for (int i = 0; i < m->nsvc && n < len - 512; i++) {
        svc *s = m->svc[i];
        win_stats *w = &s->pubw;
        n += snprintf(buf + n, len - n,
                      "%s{\"index\":%d,\"name\":\"%s\",\"sid\":%d,\"source\":\"%s\",\"streams\":%d,"
                      "\"rate\":%.1f,\"dropped_late\":%llu,\"overruns\":%llu,\"pcr_inserted\":%llu,"
                      "\"switches\":%llu,\"late_max_ms\":%.2f,",
                      i ? "," : "", i, s->c.name, s->c.sid,
                      s->cur_pub == 0 ? "primary" : s->cur_pub == 1 ? "backup" : "none", s->nes_pub,
                      w->out_pkts * ppb, (unsigned long long)w->dropped_late, (unsigned long long)w->overruns,
                      (unsigned long long)w->pcr_inserted, (unsigned long long)s->switches,
                      w->late_max == INT64_MIN || !w->out_pkts ? 0.0 : w->late_max / 1e6);
        if (w->vn)
            n += snprintf(buf + n, len - n, "\"video_margin_min_ms\":%.1f,\"video_margin_avg_ms\":%.1f,",
                          w->vmin / 1e6, (double)w->vsum / w->vn / 1e6);
        if (w->an)
            n += snprintf(buf + n, len - n, "\"audio_margin_min_ms\":%.1f,\"audio_margin_avg_ms\":%.1f,",
                          w->amin / 1e6, (double)w->asum / w->an / 1e6);
        n += snprintf(buf + n, len - n, "\"video_type\":%d,\"audio_types\":[", s->vtype_pub);
        for (int j = 0; j < s->natypes_pub; j++)
            n += snprintf(buf + n, len - n, "%s%d", j ? "," : "", s->atypes_pub[j]);
        n += snprintf(buf + n, len - n, "],\"disc\":%llu,\"inputs\":[%d,%d,%d]}", (unsigned long long)w->disc,
                      s->in_pub[0], s->in_pub[1], s->in_pub[2]);
        memset(w, 0, sizeof *w);
        w->late_max = INT64_MIN;
    }
    n += snprintf(buf + n, len - n, "]}");
    pthread_mutex_unlock(&m->lock);
    return n;
}
