/* SPDX-License-Identifier: GPL-3.0-or-later
 * MPEG-TS packet helpers (ISO/IEC 13818-1). */
#ifndef DVBTX_TS_H
#define DVBTX_TS_H

#include <stdint.h>
#include <string.h>

#define TS_PKT 188
#define PID_NULL 0x1FFF
#define PCR_HZ 27000000LL
#define PCR_WRAP (8589934592LL * 300)       /* 2^33 * 300 */

static inline int ts_pid(const uint8_t *p) { return ((p[1] & 0x1F) << 8) | p[2]; }
static inline void ts_set_pid(uint8_t *p, int pid)
{
    p[1] = (uint8_t)((p[1] & 0xE0) | ((pid >> 8) & 0x1F));
    p[2] = (uint8_t)pid;
}
static inline int ts_pusi(const uint8_t *p) { return (p[1] >> 6) & 1; }
static inline int ts_afc(const uint8_t *p) { return (p[3] >> 4) & 3; }  /* 1 payload, 2 AF, 3 both */
static inline int ts_has_payload(const uint8_t *p) { return ts_afc(p) & 1; }
static inline int ts_cc(const uint8_t *p) { return p[3] & 0x0F; }
static inline void ts_set_cc(uint8_t *p, int cc) { p[3] = (uint8_t)((p[3] & 0xF0) | (cc & 0x0F)); }

static inline int ts_af_len(const uint8_t *p) { return (ts_afc(p) & 2) ? p[4] : -1; }

static inline int ts_has_pcr(const uint8_t *p)
{
    return (ts_afc(p) & 2) && p[4] >= 7 && (p[5] & 0x10);
}

static inline int ts_rai(const uint8_t *p) { return (ts_afc(p) & 2) && p[4] >= 1 && (p[5] & 0x40); }

static inline int64_t ts_get_pcr(const uint8_t *p)
{
    const uint8_t *q = p + 6;
    int64_t base = ((int64_t)q[0] << 25) | ((int64_t)q[1] << 17) | ((int64_t)q[2] << 9) |
                   ((int64_t)q[3] << 1) | (q[4] >> 7);
    int ext = ((q[4] & 1) << 8) | q[5];
    return base * 300 + ext;
}

static inline void ts_put_pcr(uint8_t *p, int64_t pcr)
{
    pcr %= PCR_WRAP;
    if (pcr < 0)
        pcr += PCR_WRAP;
    int64_t base = pcr / 300;
    int ext = (int)(pcr % 300);
    uint8_t *q = p + 6;
    q[0] = (uint8_t)(base >> 25);
    q[1] = (uint8_t)(base >> 17);
    q[2] = (uint8_t)(base >> 9);
    q[3] = (uint8_t)(base >> 1);
    q[4] = (uint8_t)(((base & 1) << 7) | 0x7E | (ext >> 8));
    q[5] = (uint8_t)ext;
}

/* payload offset in the packet, -1 if none */
static inline int ts_payload_off(const uint8_t *p)
{
    int afc = ts_afc(p);
    if (!(afc & 1))
        return -1;
    int off = 4;
    if (afc & 2)
        off += 1 + p[4];
    return off < TS_PKT ? off : -1;
}

/* 33-bit PES timestamp at q (5 bytes) */
static inline int64_t pes_ts(const uint8_t *q)
{
    return ((int64_t)(q[0] & 0x0E) << 29) | ((int64_t)q[1] << 22) | ((int64_t)(q[2] & 0xFE) << 14) |
           ((int64_t)q[3] << 7) | (q[4] >> 1);
}

/* DTS (or PTS when there is no DTS) of a PES starting in this packet, -1 if none */
static inline int64_t ts_pes_dts(const uint8_t *p)
{
    if (!ts_pusi(p))
        return -1;
    int off = ts_payload_off(p);
    if (off < 0 || off + 14 > TS_PKT)
        return -1;
    const uint8_t *q = p + off;
    if (q[0] || q[1] || q[2] != 1)
        return -1;
    int sid = q[3];
    if (sid == 0xBC || sid == 0xBE || sid == 0xBF || sid == 0xF0 || sid == 0xF1 || sid == 0xFF ||
        sid == 0xF2 || sid == 0xF8)
        return -1;
    int flags = q[7] >> 6;
    if (flags == 3 && off + 19 <= TS_PKT)
        return pes_ts(q + 14);
    if (flags >= 2)
        return pes_ts(q + 9);
    return -1;
}

static inline void ts_null(uint8_t *p)
{
    p[0] = 0x47;
    p[1] = 0x1F;
    p[2] = 0xFF;
    p[3] = 0x10;
    memset(p + 4, 0xFF, TS_PKT - 4);
}

#endif
