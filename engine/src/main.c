/* SPDX-License-Identifier: GPL-3.0-or-later
 * dvbtx-engine: multiplexer + modulator + radio output, configured by an INI file written by the
 * dvbtx supervisor. Statistics are printed on stdout as one JSON object per line; commands are
 * read from stdin, one per line; SIGHUP reloads the SI tables.
 *   reload | quit
 *   input add id=N addr=HOST:PORT [program= buffer_ms= ring_pkts=] name=...
 *   input del id=N                       (freed once no service uses it)
 *   service add carrier=N sid=N pid_base=N primary=ID [backup=ID] [switch_timeout_ms= return_ms=
 *               max_late_ms= early_ms= pcr_max_ms= pmt_period_ms= priority=] name=...
 *   service del carrier=N sid=N
 *   service set carrier=N sid=N [primary=ID] [backup=ID|-1] [name=...]
 * name= takes the rest of the line. Any command may start with seq=N: the answer is a JSON line
 * {"type":"reply","seq":N,"ok":true} or {..."ok":false,"error":"..."}. */
#define _GNU_SOURCE
#include <sched.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "conf.h"
#include "input.h"
#include "rf.h"
#include "util.h"

#define VERSION_STR "0.1.0"
#define MAX_INPUTS 1024

static volatile sig_atomic_t g_quit, g_reload;

static void on_signal(int sig)
{
    if (sig == SIGHUP)
        g_reload = 1;
    else
        g_quit = 1;
}

static input *g_in[MAX_INPUTS];   /* by id; NULL: none (removed, or a gap left by removals) */
static int g_nin;                 /* highest id + 1 */

static int inputs_json(char *buf, int len, input **in, int n, double dt)
{
    static uint64_t prev_rx[MAX_INPUTS], prev_kept[MAX_INPUTS];
    int64_t now = mono_ns();
    int o = snprintf(buf, len, "\"inputs\":[");
    int first = 1;
    for (int i = 0; i < n && o < len - 512; i++) {
        input *x = in[i];
        if (!x)
            continue;
        uint64_t rx = atomic_load(&x->rx_bytes), kept = atomic_load(&x->kept_pkts);
        int64_t last = atomic_load(&x->last_rx_ns), slack = atomic_load(&x->slack_min_ns);
        int64_t pcrmax = atomic_exchange(&x->pcr_interval_max_ns, 0);
        double r = dt > 0 ? (rx - prev_rx[i]) * 8.0 / dt : 0;
        double k = dt > 0 ? (kept - prev_kept[i]) * 188.0 * 8.0 / dt : 0;
        prev_rx[i] = rx;
        prev_kept[i] = kept;
        o += snprintf(buf + o, len - o,
                      "%s{\"index\":%d,\"name\":\"%s\",\"addr\":\"%s\",\"rx_rate\":%.1f,\"rate\":%.1f,"
                      "\"cc_errors\":%llu,\"discont\":%llu,\"streams\":%d,\"idle_ms\":%.0f,"
                      "\"slack_min_ms\":%.1f,\"pcr_interval_max_ms\":%.1f,\"faster_than_realtime\":%d}",
                      first ? "" : ",", i, x->name, x->addr, r, k, (unsigned long long)atomic_load(&x->cc_errors),
                      (unsigned long long)atomic_load(&x->discont), x->prog.valid ? x->prog.nes : 0,
                      last ? (now - last) / 1e6 : -1.0, slack == INT64_MAX ? 0.0 : slack / 1e6, pcrmax / 1e6,
                      atomic_load(&x->faster_than_realtime));
        first = 0;
    }
    o += snprintf(buf + o, len - o, "]");
    return o;
}

/* ---- commands ---- */

#define KV_MAX 24
typedef struct kv {
    int n;
    char *k[KV_MAX], *v[KV_MAX];
} kv;

/* "word word key=value ... name=rest of the line" (modifies line) */
static void kv_parse(char *line, kv *o, char **w1, char **w2)
{
    o->n = 0;
    *w1 = *w2 = NULL;
    char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        char *tok = p, *eq = NULL;
        if (!strncmp(p, "name=", 5)) {           /* the rest of the line */
            p += strlen(p);
            eq = tok + 4;
        } else {
            while (*p && *p != ' ' && *p != '\t') {
                if (*p == '=' && !eq)
                    eq = p;
                p++;
            }
            if (*p)
                *p++ = 0;
        }
        if (!eq) {
            if (!*w1)
                *w1 = tok;
            else if (!*w2)
                *w2 = tok;
            continue;
        }
        *eq = 0;
        if (o->n < KV_MAX) {
            o->k[o->n] = tok;
            o->v[o->n++] = eq + 1;
        }
    }
}

static const char *kv_str(const kv *o, const char *k)
{
    for (int i = 0; i < o->n; i++)
        if (!strcmp(o->k[i], k))
            return o->v[i];
    return NULL;
}

static long kv_int(const kv *o, const char *k, long def)
{
    const char *v = kv_str(o, k);
    return v && *v ? strtol(v, NULL, 0) : def;
}

static input *input_by_id(long id)
{
    return id >= 0 && id < g_nin && g_in[id] && !g_in[id]->deleting ? g_in[id] : NULL;
}

static void reply(long seq, int ok, const char *err)
{
    char e[256];
    int n = 0;
    for (const char *p = err ? err : ""; *p && n < (int)sizeof e - 2; p++)
        if (*p != '"' && *p != '\\' && (unsigned char)*p >= 0x20)
            e[n++] = *p;
    e[n] = 0;
    if (ok)
        printf("{\"type\":\"reply\",\"seq\":%ld,\"ok\":true%s%s%s}\n", seq, n ? ",\"note\":\"" : "", e, n ? "\"" : "");
    else
        printf("{\"type\":\"reply\",\"seq\":%ld,\"ok\":false,\"error\":\"%s\"}\n", seq, e);
    fflush(stdout);
}

#define OP_TIMEOUT_MS 2000

static void command(char *line, int *quit, int *reload)
{
    kv o;
    char *w1, *w2, err[256] = "";
    kv_parse(line, &o, &w1, &w2);
    long seq = kv_int(&o, "seq", -1);
    if (!w1)
        return;
    if (!strcmp(w1, "reload")) {
        *reload = 1;
        return;
    }
    if (!strcmp(w1, "quit")) {
        *quit = 1;
        return;
    }
    int r = -1;
    if (!strcmp(w1, "input") && w2) {
        long id = kv_int(&o, "id", -1);
        if (!strcmp(w2, "add")) {
            const char *addr = kv_str(&o, "addr");
            if (id < 0 || id >= MAX_INPUTS || !addr)
                snprintf(err, sizeof err, "input add: id (0..%d) and addr required", MAX_INPUTS - 1);
            else if (id < g_nin && g_in[id])
                snprintf(err, sizeof err, "input %ld exists", id);
            else {
                const char *nm = kv_str(&o, "name");
                input *in = input_new((int)id, nm ? nm : "input", addr, (int)kv_int(&o, "program", 0),
                                      (int)kv_int(&o, "buffer_ms", 1000), (int)kv_int(&o, "ring_pkts", 65536), err,
                                      sizeof err);
                if (in && input_attach(in)) {
                    snprintf(err, sizeof err, "input %ld: %s", id, strerror(errno));
                    input_free(in);
                    in = NULL;
                }
                if (in) {
                    g_in[id] = in;
                    if (id >= g_nin)
                        g_nin = (int)id + 1;
                    LOGI("input %s added (%s)", in->name, in->addr);
                    r = 0;
                }
            }
        } else if (!strcmp(w2, "del")) {
            input *in = input_by_id(id);
            if (!in)
                snprintf(err, sizeof err, "no input %ld", id);
            else {
                in->deleting = 1;          /* closed and freed by the main loop once unused */
                r = 0;
            }
        } else {
            snprintf(err, sizeof err, "input %s: unknown", w2);
        }
    } else if (!strcmp(w1, "service") && w2) {
        mux *m = rf_carrier_mux((int)kv_int(&o, "carrier", -1));
        int sid = (int)kv_int(&o, "sid", -1);
        if (!m)
            snprintf(err, sizeof err, "no such carrier");
        else if (sid < 1 || sid > 0xFFFF)
            snprintf(err, sizeof err, "sid 1..65535 required");
        else if (!strcmp(w2, "add")) {
            mux_svc_cfg sc = { 0 };
            const char *nm = kv_str(&o, "name");
            snprintf(sc.name, sizeof sc.name, "%s", nm ? nm : "service");
            sc.sid = sid;
            sc.pid_base = (int)kv_int(&o, "pid_base", -1);
            sc.primary = input_by_id(kv_int(&o, "primary", -1));
            long bi = kv_int(&o, "backup", -1);
            sc.backup = bi >= 0 ? input_by_id(bi) : NULL;
            sc.switch_timeout_ms = (int)kv_int(&o, "switch_timeout_ms", 3000);
            sc.return_ms = (int)kv_int(&o, "return_ms", 10000);
            sc.max_late_ms = (int)kv_int(&o, "max_late_ms", 600);
            sc.early_ms = (int)kv_int(&o, "early_ms", 0);
            sc.pcr_max_ms = (int)kv_int(&o, "pcr_max_ms", 35);
            sc.pmt_period_ms = (int)kv_int(&o, "pmt_period_ms", 100);
            sc.priority = (int)kv_int(&o, "priority", 5);
            if (sc.priority < 1 || sc.priority > 10)
                sc.priority = 5;
            if (sc.pid_base < 0x20 || sc.pid_base > 0x1FC0)
                snprintf(err, sizeof err, "pid_base 0x20..0x1FC0 required");
            else if (!sc.primary || (bi >= 0 && !sc.backup))
                snprintf(err, sizeof err, "unknown input");
            else {
                atomic_fetch_add(&sc.primary->refs, 1);
                if (sc.backup)
                    atomic_fetch_add(&sc.backup->refs, 1);
                r = mux_service_add(m, &sc, OP_TIMEOUT_MS, err, sizeof err);
            }
        } else if (!strcmp(w2, "del")) {
            r = mux_service_del(m, sid, OP_TIMEOUT_MS, err, sizeof err);
        } else if (!strcmp(w2, "set")) {
            long pi = kv_int(&o, "primary", -2), bi = kv_int(&o, "backup", -2);
            input *p = pi >= 0 ? input_by_id(pi) : NULL, *b = bi >= 0 ? input_by_id(bi) : NULL;
            if ((pi >= 0 && !p) || (bi >= 0 && !b) || pi == -1)
                snprintf(err, sizeof err, "unknown input");
            else {
                if (p)
                    atomic_fetch_add(&p->refs, 1);
                if (b)
                    atomic_fetch_add(&b->refs, 1);
                r = mux_service_set(m, sid, p, b, bi == -1, kv_str(&o, "name"), OP_TIMEOUT_MS, err, sizeof err);
            }
        } else {
            snprintf(err, sizeof err, "service %s: unknown", w2);
        }
    } else {
        snprintf(err, sizeof err, "unknown command '%s'", w1);
    }
    if (r < 0)
        LOGW("command %s %s: %s", w1, w2 ? w2 : "", err);
    reply(seq, r >= 0, r > 0 || r < 0 ? err : NULL);
}

/* removed inputs: out of the poll set once no service uses them, then freed */
static void inputs_reap(void)
{
    for (int i = 0; i < g_nin; i++) {
        input *in = g_in[i];
        if (!in || !in->deleting || atomic_load(&in->refs))
            continue;
        if (!in->detached)
            input_detach(in);
        else if (input_can_free(in)) {
            LOGI("input %s removed", in->name);
            input_free(in);
            g_in[i] = NULL;
        }
    }
}

/* Keeps every thread of the engine (the ones libhackrf / libusb start later included: the affinity
 * is inherited) off the cores that take the interrupts of the USB host controllers. A real-time
 * thread of ours on such a core delays the USB softirq work (ksoftirqd runs at normal priority):
 * the transfers reach the radio late and its FIFO runs dry, a gap on air (the lesson of the CMTS
 * bench on the same host). cpus: "auto" (default), "all", or a list such as "1-5". */
static void place_threads(const char *spec)
{
    cpu_set_t allowed, keep, irq;
    if (sched_getaffinity(0, sizeof allowed, &allowed))
        return;
    CPU_ZERO(&keep);
    CPU_ZERO(&irq);
    if (spec && !strcmp(spec, "all"))
        return;
    if (spec && strcmp(spec, "auto")) {
        for (const char *p = spec; *p;) {
            char *e;
            long a = strtol(p, &e, 10), b = a;
            if (e == p)
                break;
            if (*e == '-')
                b = strtol(e + 1, &e, 10);
            for (long c = a; c <= b && c < CPU_SETSIZE; c++)
                CPU_SET((int)c, &keep);
            p = *e == ',' ? e + 1 : e;
        }
        if (CPU_COUNT(&keep) && !sched_setaffinity(0, sizeof keep, &keep))
            LOGI("threads on CPU %s (cpus)", spec);
        return;
    }
    FILE *f = fopen("/proc/interrupts", "r");
    if (!f)
        return;
    char line[4096], cpus[64] = "";
    while (fgets(line, sizeof line, f)) {
        if (!strstr(line, "xhci") && !strstr(line, "ehci"))
            continue;
        int n = atoi(line);
        char path[64], buf[256];
        snprintf(path, sizeof path, "/proc/irq/%d/effective_affinity_list", n);
        FILE *g = fopen(path, "r");
        if (!g)
            continue;
        if (fgets(buf, sizeof buf, g)) {
            for (const char *p = buf; *p && *p != '\n';) {
                char *e;
                long a = strtol(p, &e, 10), b = a;
                if (e == p)
                    break;
                if (*e == '-')
                    b = strtol(e + 1, &e, 10);
                for (long c = a; c <= b && c < CPU_SETSIZE; c++)
                    CPU_SET((int)c, &irq);
                p = *e == ',' ? e + 1 : e;
            }
        }
        fclose(g);
    }
    fclose(f);
    int nk = 0;
    for (int c = 0; c < CPU_SETSIZE; c++) {
        if (CPU_ISSET(c, &allowed) && !CPU_ISSET(c, &irq)) {
            CPU_SET(c, &keep);
            nk++;
        } else if (CPU_ISSET(c, &irq) && strlen(cpus) < sizeof cpus - 6) {
            snprintf(cpus + strlen(cpus), sizeof cpus - strlen(cpus), "%s%d", *cpus ? "," : "", c);
        }
    }
    if (!CPU_COUNT(&irq) || nk < 2) {
        LOGI("threads on every CPU (USB interrupts on %s)", *cpus ? cpus : "?");
        return;
    }
    if (!sched_setaffinity(0, sizeof keep, &keep))
        LOGI("threads kept off CPU %s (USB interrupts)", cpus);
}

/* Deep C-states of the processor while transmitting: the package's exit latency (up to ~1 ms, C10)
 * delays the USB controller's DMA, and the HackRF's FIFO (about 1 ms at 16 MS/s) runs dry: gaps
 * on air. Measured on the PVE host with the stock hackrf_transfer and the engine running beside it:
 * 3 gaps in 90 s at 16.46 MS/s, 8 a minute at 18.29 MS/s; none with /dev/cpu_dma_latency held at 0
 * (as the CMTS bench does). The request lasts as long as the file stays open. cpu_latency_us: the
 * latency allowed (default 0), -1: no request. */
static int g_lat_fd = -1;

static void hold_cpu_latency(long us)
{
    if (us < 0)
        return;
    int fd = open("/dev/cpu_dma_latency", O_WRONLY | O_CLOEXEC);
    int32_t v = (int32_t)us;
    if (fd < 0 || write(fd, &v, sizeof v) != (ssize_t)sizeof v) {
        LOGW("deep C-states not blocked (/dev/cpu_dma_latency: %s): gaps on air are likely at high rates; "
             "give the service CAP_SYS_ADMIN or run it as root", strerror(errno));
        if (fd >= 0)
            close(fd);
        return;
    }
    g_lat_fd = fd;
    LOGI("processor wake-up latency held at %ld us while transmitting", us);
}

int main(int argc, char **argv)
{
    const char *cfg = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v"))
            g_log_level = LOG_DEBUG;
        else if (!strcmp(argv[i], "--version")) {
            printf("dvbtx-engine %s\n", VERSION_STR);
            return 0;
        } else
            cfg = argv[i];
    }
    if (!cfg) {
        fprintf(stderr, "usage: dvbtx-engine [-v] engine.ini\n");
        return 2;
    }
    /* never keep transmitting if the supervisor dies */
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() == 1)
        die("parent already gone");
    char err[512];
    conf *cf = conf_load(cfg, err, sizeof err);
    if (!cf)
        die("%s", err);
    conf_sec *eng = conf_find(cf, "engine", 0);
    const char *ll = conf_str(eng, "log_level", NULL);
    if (ll && g_log_level < LOG_DEBUG)
        g_log_level = !strcmp(ll, "debug") ? LOG_DEBUG : !strcmp(ll, "warn") ? LOG_WARN : !strcmp(ll, "error") ? LOG_ERR : LOG_INFO;
    int stats_ms = (int)conf_int(eng, "stats_interval_ms", 1000);
    place_threads(conf_str(eng, "cpus", "auto"));
    hold_cpu_latency(conf_int(eng, "cpu_latency_us", 0));

    /* inputs by id: removals while the supervisor runs leave gaps in the numbering */
    for (int si = 0; si < cf->nsec; si++) {
        conf_sec *s = &cf->sec[si];
        if (strcmp(s->kind, "input"))
            continue;
        int i = s->index;
        if (i < 0 || i >= MAX_INPUTS || g_in[i])
            die("input %d: bad or repeated number", i);
        g_in[i] = input_new(i, conf_str(s, "name", "input"), conf_req(s, "addr"), (int)conf_int(s, "program", 0),
                            (int)conf_int(s, "buffer_ms", 1000), (int)conf_int(s, "ring_pkts", 65536), err,
                            sizeof err);
        if (!g_in[i])
            die("input %d: %s", i, err);
        if (i >= g_nin)
            g_nin = i + 1;
    }
    input **inputs = g_in;
    if (rf_setup(cf, inputs, g_nin, err, sizeof err))
        die("%s", err);

    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (inputs_start(inputs, g_nin))
        die("cannot start the input thread");
    if (rf_start())
        die("cannot start the radios");
    int nin = 0;
    for (int i = 0; i < g_nin; i++)
        nin += g_in[i] != NULL;
    LOGI("dvbtx-engine %s running: %d input(s)", VERSION_STR, nin);

    static char buf[1 << 20];
    int64_t t_prev = mono_ns(), t_start = t_prev;
    static char lbuf[16384];
    int lfill = 0;
    int stdin_open = !isatty(0);
    while (!g_quit && rf_running()) {
        struct pollfd pf = { .fd = 0, .events = POLLIN };
        int64_t wait = (int64_t)stats_ms - (mono_ns() - t_prev) / 1000000;
        int r = poll(&pf, stdin_open ? 1 : 0, wait < 1 ? 1 : (int)wait);
        if (r > 0 && (pf.revents & (POLLIN | POLLHUP))) {
            ssize_t got = read(0, lbuf + lfill, sizeof lbuf - 1 - lfill);
            if (got <= 0 && !(got < 0 && errno == EINTR)) {
                stdin_open = 0;
            } else if (got > 0) {
                lfill += (int)got;
                char *start = lbuf, *nl;
                while ((nl = memchr(start, '\n', lbuf + lfill - start))) {
                    *nl = 0;
                    int q = 0, rl = 0;
                    command(start, &q, &rl);
                    g_quit |= q;
                    g_reload |= rl;
                    start = nl + 1;
                }
                lfill -= (int)(start - lbuf);
                memmove(lbuf, start, lfill);
                if (lfill == (int)sizeof lbuf - 1)
                    lfill = 0;            /* a line too long: dropped */
            }
        }
        inputs_reap();
        if (g_reload) {
            g_reload = 0;
            rf_reload_tables();
            LOGI("SI tables reload requested");
        }
        int64_t now = mono_ns();
        int stalled = rf_stalled(now, 5000000000LL);
        if (stalled >= 0 && now - t_start > 10000000000LL) {
            LOGE("radio %d does not consume samples any more (unplugged?): exiting", stalled);
            fflush(stderr);
            _exit(3);                      /* the supervisor restarts the engine */
        }
        if (now - t_prev >= (int64_t)stats_ms * 1000000 - 5000000) {
            double dt = (now - t_prev) / 1e9;
            t_prev = now;
            int n = snprintf(buf, sizeof buf, "{\"type\":\"stats\",\"time\":%.3f,\"uptime\":%.1f,\"interval\":%.3f,",
                             real_ns() / 1e9, (now - t_start) / 1e9, dt);
            n += inputs_json(buf + n, sizeof buf - n, g_in, g_nin, dt);
            buf[n++] = ',';
            n += rf_stats_json(buf + n, sizeof buf - n - 4, dt);
            n += snprintf(buf + n, sizeof buf - n, "}\n");
            fwrite(buf, 1, n, stdout);
            fflush(stdout);
        }
    }
    LOGI("stopping");
    rf_stop();
    inputs_stop();
    conf_free(cf);
    return 0;
}
