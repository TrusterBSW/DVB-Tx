/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "tsfill.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "util.h"

#define MAX_POLLERS 8

struct tsfill {
    char url[256], host[128], path[128];
    int port, period_ms;
    char tune[160];               /* GET sent when it (re)starts: settings it loses then (freq, disable) */
    int tuned, tune_failed;
    int64_t tune_next;
    long long want_freq, want_mask;   /* what the tune path sets: checked in its answer and in /status */
    long long uptime;
    pthread_t th;
    pthread_mutex_t lock;
    tsfill_sample s[TSFILL_MUX];
    uint64_t seq;
    int failing;
    int users;                    /* its multiplexes on the air (atomic): none, the board is left alone */
};

static tsfill *g_pollers[MAX_POLLERS];
static int g_npollers;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* one GET of path, the answer in buf; its length, -2 if the connection is refused (nothing listens: dvbt_tx
 * restarting), -1 on another error (timeout: the board or the network down) */
static int http_get(tsfill *f, const char *path, char *buf, int len)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai = NULL;
    char port[8];
    snprintf(port, sizeof port, "%d", f->port);
    if (getaddrinfo(f->host, port, &hints, &ai) || !ai)
        return -1;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct timeval tv = { 2, 0 };            /* connect, send and each recv (a /set answer waits up to 600 ms,
                                              * its single-threaded server up to 2 s for a slow client) */
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int n = -1;
    if (fd >= 0 && connect(fd, ai->ai_addr, ai->ai_addrlen))
        n = errno == ECONNREFUSED ? -2 : -1;
    else if (fd >= 0) {
        char req[320];
        int rl = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", path, f->host);
        if (send(fd, req, rl, MSG_NOSIGNAL) == rl) {
            n = 0;
            for (int r; n < len - 1 && (r = recv(fd, buf + n, len - 1 - n, 0)) > 0;)
                n += r;
            buf[n] = 0;
        }
    }
    if (fd >= 0)
        close(fd);
    freeaddrinfo(ai);
    return n;
}

static long long field(const char *obj, const char *end, const char *key)
{
    const char *p = strstr(obj, key);
    return p && p < end ? strtoll(p + strlen(key), NULL, 10) : -1;
}

/* the value of NAME= in a query string, -1 if absent (decimal or 0x hex) */
static long long query_num(const char *q, const char *name)
{
    size_t l = strlen(name);
    for (const char *p = strchr(q, '?'); p; p = strchr(p + 1, '&'))
        if (!strncmp(p + 1, name, l) && p[1 + l] == '=')
            return strtoll(p + 2 + l, NULL, 0);
    return -1;
}

static void *poll_thread(void *arg)
{
    tsfill *f = arg;
    char *buf = malloc(16384);
    while (buf) {
        int64_t t = mono_ns();
        if (__atomic_load_n(&f->users, __ATOMIC_ACQUIRE) <= 0) {
            /* all its multiplexes stopped: not polled nor set (it may serve someone else meanwhile);
             * set again at the first answer once one starts */
            f->tuned = f->tune_failed = 0;
            f->tune_next = 0;
            f->uptime = -1;
            struct timespec ts = { 0, 100000000 };
            nanosleep(&ts, NULL);
            continue;
        }
        int n = http_get(f, f->path, buf, 16384);
        const char *body = n > 0 ? strstr(buf, "\r\n\r\n") : NULL;
        /* its settings (/set, not saved by the board), sent again whenever it restarted (uptime_s went
         * down), did not answer, or shows other values (someone else changed them); a refused or
         * mismatching answer: tried again 1 s later */
        const char *bend = body ? body + strlen(body) : NULL;
        long long up = body ? field(body, bend, "\"uptime_s\":") : -1;
        if (!body || (up >= 0 && up < f->uptime)) {
            f->tuned = 0;
            f->tune_failed = 0;   /* set at once when it answers again */
            f->tune_next = 0;
        }
        f->uptime = up;
        if (body && f->tune[0] && f->tuned) {
            long long fr = field(body, bend, "\"freq_mux1_hz\":"), dm = field(body, bend, "\"disable_mask\":");
            long long nm = field(body, bend, "\"multiplexes\":");
            long long want_dm = nm > 0 && nm < 63 ? f->want_mask & ((1LL << nm) - 1) : f->want_mask;
            if ((f->want_freq >= 0 && fr >= 0 && fr != f->want_freq) || (f->want_mask >= 0 && dm >= 0 && dm != want_dm)) {
                LOGW("%s: the board shows multiplex 1 at %lld Hz, disable mask %lld: set again", f->url, fr, dm);
                f->tuned = 0;
            }
        }
        if (body && f->tune[0] && !f->tuned && t >= f->tune_next) {
            char ans[1024];
            int a = http_get(f, f->tune, ans, sizeof ans);
            const char *ab = a > 0 ? strstr(ans, "\r\n\r\n") : NULL, *aend = ab ? ab + strlen(ab) : NULL;
            const char *ok = ab ? strstr(ab, "\"ok\":") : NULL;
            while (ok && (ok[5] == ' ' || ok[5] == '\t'))
                ok++;
            long long fr = ab ? field(ab, aend, "\"freq_mux1_hz\":") : -1, dm = ab ? field(ab, aend, "\"disable_mask\":") : -1;
            long long nm = field(body, bend, "\"multiplexes\":");
            long long want_dm = nm > 0 && nm < 63 ? f->want_mask & ((1LL << nm) - 1) : f->want_mask;
            f->tuned = ab && !strncmp(ans, "HTTP/", 5) && strstr(ans, " 200 ") && ok && !strncmp(ok + 5, "true", 4)
                       && (f->want_freq < 0 || fr == f->want_freq) && (f->want_mask < 0 || dm == want_dm);
            if (f->tuned)
                LOGI("%s: set %s (multiplex 1 at %lld Hz, disable mask %lld)", f->url, f->tune, fr, dm);
            else {
                /* tried again 1 s, then 5 s, then every 10 s later; refused connection (dvbt_tx restarting):
                 * every 2 s. Other values may only be pending (applied between two DMA blocks): set again */
                static const int backoff_s[] = { 1, 5, 10 };
                int k = f->tune_failed < 3 ? f->tune_failed : 2;
                int64_t d = a == -2 ? 2 : backoff_s[k];
                LOGW("%s: %s %s, tried again in %d s", f->url, f->tune,
                     a == -2 ? "connection refused" : a < 0 ? "unanswered"
                     : !ok || strncmp(ok + 5, "true", 4) ? "refused" : "answered with other values", (int)d);
                f->tune_failed++;
                f->tune_next = t + d * 1000000000LL;
            }
            if (f->tuned)
                f->tune_failed = 0;
        }
        pthread_mutex_lock(&f->lock);
        f->seq++;
        for (int i = 0; i < TSFILL_MUX; i++) {
            f->s[i].seq = f->seq;
            f->s[i].ok = 0;
            f->s[i].at_ns = t;
            f->s[i].fails++;          /* back to 0 below if it is listed */
        }
        int found = 0;
        /* {"mux":K,...,"null_inserted":N,"dropped":N,...,"queued":N,...} in "inputs" */
        for (const char *p = body; p && (p = strstr(p, "{\"mux\":"));) {
            int k = atoi(p + 7);
            const char *end = strchr(p, '}');
            if (!end)
                break;
            if (k >= 1 && k <= TSFILL_MUX) {
                tsfill_sample *s = &f->s[k - 1];
                long q = field(p, end, "\"queued\":");
                if (q >= 0) {
                    s->ok = 1;
                    s->fails = 0;
                    s->last_ok_ns = t;
                    s->queued = q;
                    s->null_inserted = field(p, end, "\"null_inserted\":");
                    s->dropped = field(p, end, "\"dropped\":");
                    found++;
                }
            }
            p = end;
        }
        if ((found == 0) != f->failing) {
            f->failing = !found;
            if (f->failing)
                LOGW("%s: no fill level (%s)", f->url, n < 0 ? "no answer" : "no multiplex listed");
            else
                LOGI("%s: fill levels read", f->url);
        }
        pthread_mutex_unlock(&f->lock);
        int64_t left = t + (int64_t)f->period_ms * 1000000 - mono_ns();
        if (left > 0) {
            struct timespec ts = { left / 1000000000, left % 1000000000 };
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

tsfill *tsfill_get(const char *url, int period_ms, const char *tune)
{
    pthread_mutex_lock(&g_lock);
    tsfill *f = NULL;
    for (int i = 0; i < g_npollers; i++)
        if (!strcmp(g_pollers[i]->url, url))
            f = g_pollers[i];
    if (!f && g_npollers < MAX_POLLERS && !strncmp(url, "http://", 7)) {
        f = calloc(1, sizeof *f);
        snprintf(f->url, sizeof f->url, "%s", url);
        const char *h = url + 7, *slash = strchr(h, '/'), *colon = strchr(h, ':');
        if (!slash)
            slash = h + strlen(h);
        if (colon && colon > slash)
            colon = NULL;
        snprintf(f->host, sizeof f->host, "%.*s", (int)((colon ? colon : slash) - h), h);
        f->port = colon ? atoi(colon + 1) : 80;
        snprintf(f->path, sizeof f->path, "%s", *slash ? slash : "/");
        f->period_ms = period_ms > 50 ? period_ms : 50;
        snprintf(f->tune, sizeof f->tune, "%s", tune ? tune : "");
        f->want_freq = query_num(f->tune, "freq");
        f->want_mask = query_num(f->tune, "disable");
        pthread_mutex_init(&f->lock, NULL);
        if (!f->host[0] || f->port <= 0 || pthread_create(&f->th, NULL, poll_thread, f)) {
            free(f);
            f = NULL;
        } else {
            pthread_setname_np(f->th, "fill-poll");
            g_pollers[g_npollers++] = f;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return f;
}

void tsfill_use(tsfill *f, int delta)
{
    __atomic_add_fetch(&f->users, delta, __ATOMIC_RELEASE);
}

void tsfill_read(tsfill *f, int mux, tsfill_sample *s)
{
    pthread_mutex_lock(&f->lock);
    *s = f->s[(mux >= 1 && mux <= TSFILL_MUX ? mux : 1) - 1];
    pthread_mutex_unlock(&f->lock);
}
