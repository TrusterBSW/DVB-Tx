/* SPDX-License-Identifier: GPL-3.0-or-later
 * dvbtx-engine: multiplexer + modulator + radio output, configured by an INI file written by the
 * dvbtx supervisor. Statistics are printed on stdout as one JSON object per line; commands are
 * read from stdin ("reload", "quit"); SIGHUP reloads the SI tables. */
#define _GNU_SOURCE
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

static volatile sig_atomic_t g_quit, g_reload;

static void on_signal(int sig)
{
    if (sig == SIGHUP)
        g_reload = 1;
    else
        g_quit = 1;
}

static int inputs_json(char *buf, int len, input **in, int n, double dt)
{
    static uint64_t prev_rx[1024], prev_kept[1024];
    int64_t now = mono_ns();
    int o = snprintf(buf, len, "\"inputs\":[");
    for (int i = 0; i < n && o < len - 512; i++) {
        input *x = in[i];
        uint64_t rx = atomic_load(&x->rx_bytes), kept = atomic_load(&x->kept_pkts);
        int64_t last = atomic_load(&x->last_rx_ns), slack = atomic_load(&x->slack_min_ns);
        int64_t pcrmax = atomic_exchange(&x->pcr_interval_max_ns, 0);
        double r = dt > 0 ? (rx - (i < 1024 ? prev_rx[i] : 0)) * 8.0 / dt : 0;
        double k = dt > 0 ? (kept - (i < 1024 ? prev_kept[i] : 0)) * 188.0 * 8.0 / dt : 0;
        if (i < 1024) {
            prev_rx[i] = rx;
            prev_kept[i] = kept;
        }
        o += snprintf(buf + o, len - o,
                      "%s{\"index\":%d,\"name\":\"%s\",\"addr\":\"%s\",\"rx_rate\":%.1f,\"rate\":%.1f,"
                      "\"cc_errors\":%llu,\"discont\":%llu,\"streams\":%d,\"idle_ms\":%.0f,"
                      "\"slack_min_ms\":%.1f,\"pcr_interval_max_ms\":%.1f,\"faster_than_realtime\":%d}",
                      i ? "," : "", i, x->name, x->addr, r, k, (unsigned long long)atomic_load(&x->cc_errors),
                      (unsigned long long)atomic_load(&x->discont), x->prog.valid ? x->prog.nes : 0,
                      last ? (now - last) / 1e6 : -1.0, slack == INT64_MAX ? 0.0 : slack / 1e6, pcrmax / 1e6,
                      atomic_load(&x->faster_than_realtime));
    }
    o += snprintf(buf + o, len - o, "]");
    return o;
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

    int nin = conf_count(cf, "input");
    input **inputs = xcalloc(nin, sizeof *inputs);
    for (int i = 0; i < nin; i++) {
        conf_sec *s = conf_find(cf, "input", i);
        if (!s)
            die("inputs must be numbered 0..%d", nin - 1);
        inputs[i] = input_new(i, conf_str(s, "name", "input"), conf_req(s, "addr"), (int)conf_int(s, "program", 0),
                              (int)conf_int(s, "buffer_ms", 1000), (int)conf_int(s, "ring_pkts", 65536), err,
                              sizeof err);
        if (!inputs[i])
            die("input %d: %s", i, err);
    }
    if (rf_setup(cf, inputs, nin, err, sizeof err))
        die("%s", err);

    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (inputs_start(inputs, nin))
        die("cannot start the input thread");
    if (rf_start())
        die("cannot start the radios");
    LOGI("dvbtx-engine %s running: %d input(s)", VERSION_STR, nin);

    static char buf[1 << 20];
    int64_t t_prev = mono_ns(), t_start = t_prev;
    char line[256];
    int stdin_open = !isatty(0);
    while (!g_quit && rf_running()) {
        struct pollfd pf = { .fd = 0, .events = POLLIN };
        int r = poll(&pf, stdin_open ? 1 : 0, stats_ms);
        if (r > 0 && (pf.revents & (POLLIN | POLLHUP))) {
            if (!fgets(line, sizeof line, stdin)) {
                stdin_open = 0;
            } else if (!strncmp(line, "reload", 6)) {
                g_reload = 1;
            } else if (!strncmp(line, "quit", 4)) {
                g_quit = 1;
            }
        }
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
            n += inputs_json(buf + n, sizeof buf - n, inputs, nin, dt);
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
