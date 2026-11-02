/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

int g_log_level = LOG_INFO;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

void logmsg(int level, const char *fmt, ...)
{
    static const char *tag[] = { "ERR", "WRN", "INF", "DBG" };
    if (level > g_log_level)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&log_lock);
    fprintf(stderr, "%02d:%02d:%02d.%03ld %s ", tm.tm_hour, tm.tm_min, tm.tm_sec,
            ts.tv_nsec / 1000000, tag[level < 0 ? 0 : level > 3 ? 3 : level]);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    pthread_mutex_unlock(&log_lock);
    va_end(ap);
}

void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("fatal: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

void *xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p)
        die("out of memory (%zu x %zu)", n, sz);
    return p;
}

/* CRC-32/MPEG-2: poly 0x04C11DB7, init 0xFFFFFFFF, no reflection, no final xor */
uint32_t crc32_mpeg(const uint8_t *p, size_t n)
{
    static uint32_t tab[256];
    static int init;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i << 24;
            for (int j = 0; j < 8; j++)
                c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
            tab[i] = c;
        }
        init = 1;
    }
    uint32_t crc = 0xFFFFFFFFu;
    while (n--)
        crc = (crc << 8) ^ tab[((crc >> 24) ^ *p++) & 0xFF];
    return crc;
}
