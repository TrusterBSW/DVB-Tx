/* SPDX-License-Identifier: GPL-3.0-or-later
 * dvbtx engine - small shared helpers: logging, clocks, CRC-32/MPEG-2. */
#ifndef DVBTX_UTIL_H
#define DVBTX_UTIL_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

enum { LOG_ERR = 0, LOG_WARN = 1, LOG_INFO = 2, LOG_DEBUG = 3 };
extern int g_log_level;
void logmsg(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define LOGE(...) logmsg(LOG_ERR, __VA_ARGS__)
#define LOGW(...) logmsg(LOG_WARN, __VA_ARGS__)
#define LOGI(...) logmsg(LOG_INFO, __VA_ARGS__)
#define LOGD(...) do { if (g_log_level >= LOG_DEBUG) logmsg(LOG_DEBUG, __VA_ARGS__); } while (0)

static inline int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static inline int64_t real_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

uint32_t crc32_mpeg(const uint8_t *p, size_t n);
void *xcalloc(size_t n, size_t sz);
void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

#endif
