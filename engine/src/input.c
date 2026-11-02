/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "input.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "util.h"

#define MAX_PENDING 32768
#define WINDOW_NS 2000000000LL
#define SLEW_PPM 30

typedef struct sec_asm {
    int len, need;
    uint8_t buf[1024 + 8];
} sec_asm;

typedef struct pend {
    int64_t arrival;
    uint64_t pos;
    uint8_t pkt[TS_PKT];
} pend;

struct in_priv {
    uint8_t cc[8192];          /* last CC + 1, 0 = unknown */
    sec_asm pat, pmt;
    int pmt_pid, pat_version, pmt_version;
    prog_info prog;
    uint8_t keep[8192];

    uint64_t pos;              /* packets received (all PIDs) */
    int have_pcr;
    int64_t pcr_ext, pcr_raw_last;
    uint64_t pcr_pos;
    int64_t pcr_arrival;
    int64_t anchor;            /* t_ideal = anchor + pcr * 1e9 / 27e6 */
    pend *pending;
    int npend;
    int video_pid;

    int64_t win_start, win_min;
    int late_windows;
    int force_reanchor;
    uint8_t next_flags;
};

static int64_t pcr_to_ns(int64_t pcr) { return pcr * 1000 / 27; }

static int parse_addr(const char *addr, struct sockaddr_in *sa, char *ifname, size_t iflen)
{
    /* [udp://][iface@]host:port */
    const char *p = addr;
    if (!strncmp(p, "udp://", 6))
        p += 6;
    const char *at = strchr(p, '@');
    ifname[0] = 0;
    if (at) {
        snprintf(ifname, iflen, "%.*s", (int)(at - p), p);
        p = at + 1;
    }
    char host[128];
    const char *colon = strrchr(p, ':');
    if (!colon)
        return -1;
    snprintf(host, sizeof host, "%.*s", (int)(colon - p), p);
    int port = atoi(colon + 1);
    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    sa->sin_port = htons((uint16_t)port);
    if (!host[0] || !strcmp(host, "0.0.0.0") || !strcmp(host, "*")) {
        sa->sin_addr.s_addr = htonl(INADDR_ANY);
        return 0;
    }
    if (inet_pton(AF_INET, host, &sa->sin_addr) == 1)
        return 0;
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *res;
    if (getaddrinfo(host, NULL, &hints, &res))
        return -1;
    sa->sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 0;
}

input *input_new(int id, const char *name, const char *addr, int program, int buffer_ms,
                 int ring_pkts, char *err, int errlen)
{
    struct sockaddr_in sa;
    char ifname[IFNAMSIZ];
    if (parse_addr(addr, &sa, ifname, sizeof ifname)) {
        snprintf(err, errlen, "bad UDP address '%s'", addr);
        return NULL;
    }
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    int rcv = 32 << 20;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcv, sizeof rcv))
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
    int mcast = IN_MULTICAST(ntohl(sa.sin_addr.s_addr));
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa)) {
        snprintf(err, errlen, "bind %s: %s", addr, strerror(errno));
        close(fd);
        return NULL;
    }
    if (mcast) {
        struct ip_mreqn mr = { .imr_multiaddr = sa.sin_addr };
        if (ifname[0])
            mr.imr_ifindex = (int)if_nametoindex(ifname);
        if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr)) {
            snprintf(err, errlen, "join %s: %s", addr, strerror(errno));
            close(fd);
            return NULL;
        }
    }
    input *in = xcalloc(1, sizeof *in);
    in->id = id;
    snprintf(in->name, sizeof in->name, "%s", name);
    snprintf(in->addr, sizeof in->addr, "%s", addr);
    in->fd = fd;
    in->program = program;
    in->buffer_ns = (int64_t)buffer_ms * 1000000;
    uint64_t cap = 1024;
    while (cap < (uint64_t)ring_pkts)
        cap <<= 1;
    in->ring = xcalloc(cap, sizeof *in->ring);
    in->mask = cap - 1;
    pthread_mutex_init(&in->lock, NULL);
    in->priv = xcalloc(1, sizeof *in->priv);
    in->priv->pending = xcalloc(MAX_PENDING, sizeof(pend));
    in->priv->pmt_pid = -1;
    in->priv->video_pid = -1;
    in->priv->pat_version = -1;
    in->priv->pmt_version = -1;
    atomic_store(&in->slack_min_ns, INT64_MAX);
    return in;
}

int input_read(input *in, uint64_t pos, in_entry *out)
{
    uint64_t h = atomic_load_explicit(&in->head, memory_order_acquire);
    if (pos >= h)
        return 1;
    if (h - pos >= in->mask)   /* keep one slot of margin for the entry being written */
        return -1;
    *out = in->ring[pos & in->mask];
    atomic_thread_fence(memory_order_acquire);
    h = atomic_load_explicit(&in->head, memory_order_relaxed);
    if (h - pos >= in->mask)
        return -1;
    return 0;
}

int64_t input_key_between(input *in, int64_t from, int64_t to)
{
    uint32_t n = atomic_load_explicit(&in->nkeys, memory_order_acquire);
    for (uint32_t i = 0; i < IN_KEYS && i < n; i++) {
        int64_t t = atomic_load_explicit(&in->key_ideal[(n - 1 - i) % IN_KEYS], memory_order_relaxed);
        if (t >= from && t < to)
            return t;
    }
    return -1;
}

int64_t input_oldest(input *in)
{
    uint64_t h = input_head(in);
    if (!h)
        return -1;
    in_entry e;
    uint64_t pos = h > in->mask - 64 ? h - (in->mask - 64) : 0;
    return input_read(in, pos, &e) == 0 ? e.t_ideal : -1;
}

void input_get_prog(input *in, prog_info *out, int *gen)
{
    pthread_mutex_lock(&in->lock);
    *out = in->prog;
    *gen = atomic_load(&in->prog_gen);
    pthread_mutex_unlock(&in->lock);
}

static void publish(input *in, const uint8_t *pkt, int64_t t_ideal, int64_t pcr, uint8_t flags)
{
    if (ts_pid(pkt) == in->priv->video_pid && ts_pusi(pkt) && ts_rai(pkt)) {
        uint32_t k = atomic_load_explicit(&in->nkeys, memory_order_relaxed);
        atomic_store_explicit(&in->key_ideal[k % IN_KEYS], t_ideal, memory_order_relaxed);
        atomic_store_explicit(&in->nkeys, k + 1, memory_order_release);
    }
    uint64_t h = atomic_load_explicit(&in->head, memory_order_relaxed);
    in_entry *e = &in->ring[h & in->mask];
    e->t_ideal = t_ideal;
    e->pcr = pcr;
    e->flags = flags;
    memcpy(e->pkt, pkt, TS_PKT);
    atomic_store_explicit(&in->head, h + 1, memory_order_release);
    atomic_fetch_add_explicit(&in->kept_pkts, 1, memory_order_relaxed);
    if (t_ideal > atomic_load_explicit(&in->last_ideal_ns, memory_order_relaxed) || (flags & IN_DISC))
        atomic_store_explicit(&in->last_ideal_ns, t_ideal, memory_order_relaxed);
}

/* ---- PSI ---- */

static int sec_feed(sec_asm *s, const uint8_t *p, int start_cb_ok)
{
    (void)start_cb_ok;
    int off = ts_payload_off(p);
    if (off < 0)
        return 0;
    const uint8_t *q = p + off;
    int n = TS_PKT - off;
    if (ts_pusi(p)) {
        int ptr = q[0];
        if (ptr + 1 >= n)
            return 0;
        q += 1 + ptr;
        n -= 1 + ptr;
        s->len = 0;
        s->need = -1;
    } else if (s->need < 0 && s->len == 0) {
        return 0;
    }
    if (s->len + n > (int)sizeof s->buf)
        n = (int)sizeof s->buf - s->len;
    memcpy(s->buf + s->len, q, n);
    s->len += n;
    if (s->need < 0 && s->len >= 3)
        s->need = 3 + (((s->buf[1] & 0x0F) << 8) | s->buf[2]);
    if (s->need > 0 && s->len >= s->need) {
        if (s->buf[0] == 0xFF)
            return 0;
        if (crc32_mpeg(s->buf, s->need) != 0)
            return 0;
        return 1;
    }
    return 0;
}

static void parse_pat(input *in, const uint8_t *s, int len)
{
    struct in_priv *pv = in->priv;
    if (s[0] != 0x00)
        return;
    int version = (s[5] >> 1) & 0x1F;
    int pmt = -1;
    for (int i = 8; i + 4 <= len - 4; i += 4) {
        int prog = (s[i] << 8) | s[i + 1];
        int pid = ((s[i + 2] & 0x1F) << 8) | s[i + 3];
        if (prog == 0)
            continue;
        if (in->program == 0 || prog == in->program) {
            pmt = pid;
            break;
        }
    }
    if (pmt >= 0 && (pmt != pv->pmt_pid || version != pv->pat_version)) {
        pv->pmt_pid = pmt;
        pv->pmt_version = -1;
        pv->pat_version = version;
    }
}

static void parse_pmt(input *in, const uint8_t *s, int len)
{
    struct in_priv *pv = in->priv;
    if (s[0] != 0x02)
        return;
    int version = (s[5] >> 1) & 0x1F;
    if (version == pv->pmt_version)
        return;
    prog_info pi;
    memset(&pi, 0, sizeof pi);
    pi.program_number = (s[3] << 8) | s[4];
    pi.pcr_pid = ((s[8] & 0x1F) << 8) | s[9];
    int pil = ((s[10] & 0x0F) << 8) | s[11];
    if (12 + pil > len - 4)
        return;
    pi.pinfo_len = pil > 250 ? 0 : pil;
    memcpy(pi.pinfo, s + 12, pi.pinfo_len);
    for (int i = 12 + pil; i + 5 <= len - 4;) {
        int type = s[i], pid = ((s[i + 1] & 0x1F) << 8) | s[i + 2];
        int el = ((s[i + 3] & 0x0F) << 8) | s[i + 4];
        if (pi.nes < IN_MAX_ES) {
            es_info *e = &pi.es[pi.nes++];
            e->pid = (uint16_t)pid;
            e->type = (uint8_t)type;
            e->desc_len = (uint8_t)(el > 250 ? 0 : el);
            memcpy(e->desc, s + i + 5, e->desc_len);
        }
        i += 5 + el;
    }
    pi.valid = 1;
    pv->pmt_version = version;
    pv->video_pid = -1;
    for (int i = 0; i < pi.nes && pv->video_pid < 0; i++)
        switch (pi.es[i].type) {
        case 0x01: case 0x02: case 0x10: case 0x1B: case 0x24: case 0x42: case 0xD1: case 0xEA:
            pv->video_pid = pi.es[i].pid;
        }
    atomic_store(&in->has_video, pv->video_pid >= 0);
    memset(pv->keep, 0, sizeof pv->keep);
    for (int i = 0; i < pi.nes; i++)
        pv->keep[pi.es[i].pid] = 1;
    if (pi.pcr_pid < PID_NULL)
        pv->keep[pi.pcr_pid] = 1;
    pv->prog = pi;
    pthread_mutex_lock(&in->lock);
    in->prog = pi;
    atomic_fetch_add(&in->prog_gen, 1);
    pthread_mutex_unlock(&in->lock);
    LOGI("input %s: program %d, PCR PID %d, %d streams", in->name, pi.program_number, pi.pcr_pid,
         pi.nes);
}

/* ---- timing ---- */

static void flush_pending_by_arrival(input *in)
{
    struct in_priv *pv = in->priv;
    for (int i = 0; i < pv->npend; i++) {
        int64_t t = pv->pending[i].arrival + in->buffer_ns;
        int64_t pcr = (t - pv->anchor) * 27 / 1000;
        publish(in, pv->pending[i].pkt, t, pcr, pv->next_flags);
        pv->next_flags = 0;
    }
    pv->npend = 0;
}

static void on_pcr(input *in, const uint8_t *pkt, int64_t arrival)
{
    struct in_priv *pv = in->priv;
    int64_t raw = ts_get_pcr(pkt);
    int64_t P;
    if (!pv->have_pcr) {
        P = raw;
    } else {
        int64_t d = raw - pv->pcr_raw_last;
        if (d > PCR_WRAP / 2)
            d -= PCR_WRAP;
        else if (d < -PCR_WRAP / 2)
            d += PCR_WRAP;
        P = pv->pcr_ext + d;
    }
    pv->pcr_raw_last = raw;

    int64_t dP = P - pv->pcr_ext;
    int disc = !pv->have_pcr || dP <= 0 || dP > PCR_HZ * 7 / 10 || pv->force_reanchor ||
               (ts_af_len(pkt) >= 1 && (pkt[5] & 0x80));
    if (disc) {
        if (!pv->have_pcr) {
            atomic_fetch_add(&in->dropped_nopcr, (uint64_t)pv->npend);
            pv->npend = 0;
        } else {
            flush_pending_by_arrival(in);
            atomic_fetch_add(&in->discont, 1);
            LOGD("input %s: PCR discontinuity (%.3f s)", in->name, dP / 27e6);
        }
        pv->anchor = arrival + in->buffer_ns - pcr_to_ns(P);
        pv->have_pcr = 1;
        pv->force_reanchor = 0;
        pv->late_windows = 0;
        pv->next_flags = IN_DISC;
        pv->win_start = arrival;
        pv->win_min = INT64_MAX;
    } else {
        uint64_t dpos = pv->pos - pv->pcr_pos;
        int64_t ivl = pcr_to_ns(dP);
        if (ivl > atomic_load(&in->pcr_interval_max_ns))
            atomic_store(&in->pcr_interval_max_ns, ivl);
        for (int i = 0; i < pv->npend; i++) {
            int64_t pcr = pv->pcr_ext + (int64_t)((double)dP * (double)(pv->pending[i].pos - pv->pcr_pos) / (double)dpos);
            publish(in, pv->pending[i].pkt, pv->anchor + pcr_to_ns(pcr), pcr, pv->next_flags);
            pv->next_flags = 0;
        }
        pv->npend = 0;
    }
    pv->pcr_ext = P;
    pv->pcr_pos = pv->pos;
    pv->pcr_arrival = arrival;

    int64_t t = pv->anchor + pcr_to_ns(P);
    publish(in, pkt, t, P, pv->next_flags);
    pv->next_flags = 0;

    /* buffer control: keep the smallest slack of each window within [D/4, 3D] by slewing the
     * anchor slowly (<= SLEW_PPM), re-anchor if hopeless */
    int64_t slack = t - arrival;
    if (slack < pv->win_min)
        pv->win_min = slack;
    if (arrival - pv->win_start >= WINDOW_NS) {
        int64_t m = pv->win_min, D = in->buffer_ns;
        atomic_store(&in->slack_min_ns, m);
        int64_t step = (arrival - pv->win_start) * SLEW_PPM / 1000000;
        if (m < D / 4 && m > -D)
            pv->anchor += step;
        else if (m > D * 3 / 2 && m < 3 * D)
            pv->anchor -= step;
        if (m <= -D || m >= 3 * D) {
            if (++pv->late_windows >= 3) {
                LOGW("input %s: buffer %s by %.0f ms, re-anchoring", in->name,
                     m < 0 ? "late" : "early", (double)(m < 0 ? -m : m) / 1e6);
                pv->force_reanchor = 1;
            }
        } else {
            pv->late_windows = 0;
        }
        atomic_store(&in->faster_than_realtime, m >= 3 * D);
        pv->win_start = arrival;
        pv->win_min = INT64_MAX;
    }
}

static void on_packet(input *in, const uint8_t *pkt, int64_t arrival)
{
    struct in_priv *pv = in->priv;
    if (pkt[0] != 0x47)
        return;
    pv->pos++;
    atomic_fetch_add_explicit(&in->rx_pkts, 1, memory_order_relaxed);
    int pid = ts_pid(pkt);
    if (pid == PID_NULL)
        return;
    if (pkt[1] & 0x80)          /* transport error indicator */
        return;
    if (ts_has_payload(pkt)) {
        int cc = ts_cc(pkt);
        int disc_ind = ts_af_len(pkt) >= 1 && (pkt[5] & 0x80);
        if (pv->cc[pid] && !disc_ind && ((pv->cc[pid] - 1 + 1) & 15) != cc &&
            ((pv->cc[pid] - 1) & 15) != cc)
            atomic_fetch_add_explicit(&in->cc_errors, 1, memory_order_relaxed);
        pv->cc[pid] = (uint8_t)(cc + 1);
    }
    if (pid == 0) {
        if (sec_feed(&pv->pat, pkt, 1))
            parse_pat(in, pv->pat.buf, pv->pat.need);
        return;
    }
    if (pid == pv->pmt_pid) {
        if (sec_feed(&pv->pmt, pkt, 1))
            parse_pmt(in, pv->pmt.buf, pv->pmt.need);
        if (!pv->keep[pid])
            return;
    }
    if (!pv->prog.valid || !pv->keep[pid])
        return;
    if (pid == pv->prog.pcr_pid && ts_has_pcr(pkt)) {
        on_pcr(in, pkt, arrival);
        atomic_store_explicit(&in->last_pub_ns, arrival, memory_order_relaxed);
        return;
    }
    if (pv->npend == MAX_PENDING) {     /* no PCR for far too long */
        if (pv->have_pcr)
            flush_pending_by_arrival(in);
        else {
            atomic_fetch_add(&in->dropped_nopcr, (uint64_t)pv->npend);
            pv->npend = 0;
        }
    }
    pend *p = &pv->pending[pv->npend++];
    p->arrival = arrival;
    p->pos = pv->pos;
    memcpy(p->pkt, pkt, TS_PKT);
}

static void on_datagram(input *in, const uint8_t *buf, int len, int64_t arrival)
{
    atomic_fetch_add_explicit(&in->rx_bytes, (uint64_t)len, memory_order_relaxed);
    atomic_store_explicit(&in->last_rx_ns, arrival, memory_order_relaxed);
    int off = 0;
    if (len >= 12 + TS_PKT && buf[0] != 0x47 && (buf[0] & 0xC0) == 0x80 && buf[12] == 0x47)
        off = 12 + (buf[0] & 0x0F) * 4;            /* RTP header (+ CSRCs) */
    while (off < len && buf[off] != 0x47)
        off++;
    for (; off + TS_PKT <= len; off += TS_PKT)
        on_packet(in, buf + off, arrival);
}

/* ---- receive thread ---- */

static pthread_t g_thread;
static int g_started, g_ep = -1;
static volatile int g_stop;
static _Atomic uint64_t g_epoch;      /* loops of the receive thread: tells when it left a detached input */

#define BATCH 16
#define DGRAM 65536

static void *rx_thread(void *arg)
{
    (void)arg;
    static uint8_t bufs[BATCH][DGRAM];
    struct mmsghdr msgs[BATCH];
    struct iovec iov[BATCH];
    while (!g_stop) {
        atomic_fetch_add(&g_epoch, 1);
        struct epoll_event evs[64];
        int n = epoll_wait(g_ep, evs, 64, 200);
        for (int e = 0; e < n; e++) {
            input *in = evs[e].data.ptr;
            for (;;) {
                for (int i = 0; i < BATCH; i++) {
                    iov[i].iov_base = bufs[i];
                    iov[i].iov_len = DGRAM;
                    memset(&msgs[i].msg_hdr, 0, sizeof msgs[i].msg_hdr);
                    msgs[i].msg_hdr.msg_iov = &iov[i];
                    msgs[i].msg_hdr.msg_iovlen = 1;
                }
                int r = recvmmsg(in->fd, msgs, BATCH, MSG_DONTWAIT, NULL);
                if (r <= 0)
                    break;
                int64_t now = mono_ns();
                for (int i = 0; i < r; i++)
                    on_datagram(in, bufs[i], (int)msgs[i].msg_len, now);
                if (r < BATCH)
                    break;
            }
        }
    }
    return NULL;
}

/* what arrived before (while the radios were being prepared: seconds of FFT plans) is dropped: read
 * at once, it would look like a source running ahead of real time */
static void drain(input *in)
{
    char tmp[2048];
    long dropped = 0;
    while (recv(in->fd, tmp, sizeof tmp, MSG_DONTWAIT) > 0)
        dropped++;
    if (dropped)
        LOGD("input %s: %ld datagrams received before the start dropped", in->name, dropped);
}

int input_attach(input *in)
{
    drain(in);
    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = in };
    return epoll_ctl(g_ep, EPOLL_CTL_ADD, in->fd, &ev);
}

void input_detach(input *in)
{
    if (in->detached)
        return;
    epoll_ctl(g_ep, EPOLL_CTL_DEL, in->fd, NULL);
    in->detached = 1;
    in->detach_epoch = atomic_load(&g_epoch);
}

/* the receive thread has started a new loop since the input left its poll set, and no service reads it */
int input_can_free(const input *in)
{
    return in->detached && atomic_load(&g_epoch) >= in->detach_epoch + 2 && atomic_load(&in->refs) == 0;
}

void input_free(input *in)
{
    close(in->fd);
    free(in->ring);
    free(in->priv->pending);
    free(in->priv);
    pthread_mutex_destroy(&in->lock);
    free(in);
}

int inputs_start(input **in, int n)
{
    g_ep = epoll_create1(EPOLL_CLOEXEC);
    if (g_ep < 0)
        return -1;
    for (int i = 0; i < n; i++)
        if (in[i] && input_attach(in[i]))
            return -1;
    if (pthread_create(&g_thread, NULL, rx_thread, NULL))
        return -1;
    g_started = 1;
    return 0;
}

void inputs_stop(void)
{
    g_stop = 1;
    if (g_started)
        pthread_join(g_thread, NULL);
}
