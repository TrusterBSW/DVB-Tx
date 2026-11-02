/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "psi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ts.h"
#include "util.h"

static int finish(uint8_t *s, int len)   /* len = bytes before the CRC */
{
    int sl = len + 4 - 3;
    s[1] = (uint8_t)((s[1] & 0xF0) | ((sl >> 8) & 0x0F));
    s[2] = (uint8_t)sl;
    uint32_t crc = crc32_mpeg(s, len);
    s[len] = (uint8_t)(crc >> 24);
    s[len + 1] = (uint8_t)(crc >> 16);
    s[len + 2] = (uint8_t)(crc >> 8);
    s[len + 3] = (uint8_t)crc;
    return len + 4;
}

static int long_header(uint8_t *s, int table_id, int ext, int version)
{
    s[0] = (uint8_t)table_id;
    s[1] = 0xB0;                       /* section_syntax_indicator, '0', reserved */
    s[2] = 0;
    s[3] = (uint8_t)(ext >> 8);
    s[4] = (uint8_t)ext;
    s[5] = (uint8_t)(0xC1 | ((version & 0x1F) << 1));   /* current_next = 1 */
    s[6] = 0;                          /* section_number */
    s[7] = 0;                          /* last_section_number */
    return 8;
}

int psi_pat(uint8_t *s, int tsid, int version, const psi_svc *svc, int n, int nit_pid)
{
    int p = long_header(s, 0x00, tsid, version);
    if (nit_pid > 0) {
        s[p++] = 0;
        s[p++] = 0;
        s[p++] = (uint8_t)(0xE0 | (nit_pid >> 8));
        s[p++] = (uint8_t)nit_pid;
    }
    for (int i = 0; i < n && p < 1000; i++) {
        s[p++] = (uint8_t)(svc[i].sid >> 8);
        s[p++] = (uint8_t)svc[i].sid;
        s[p++] = (uint8_t)(0xE0 | (svc[i].pmt_pid >> 8));
        s[p++] = (uint8_t)svc[i].pmt_pid;
    }
    return finish(s, p);
}

int psi_pmt(uint8_t *s, int sid, int version, int pcr_pid, const uint8_t *pinfo, int pinfo_len,
            const psi_es *es, int nes)
{
    int p = long_header(s, 0x02, sid, version);
    s[p++] = (uint8_t)(0xE0 | (pcr_pid >> 8));
    s[p++] = (uint8_t)pcr_pid;
    s[p++] = (uint8_t)(0xF0 | (pinfo_len >> 8));
    s[p++] = (uint8_t)pinfo_len;
    memcpy(s + p, pinfo, pinfo_len);
    p += pinfo_len;
    for (int i = 0; i < nes && p + 5 + es[i].desc_len < 1020; i++) {
        s[p++] = (uint8_t)es[i].type;
        s[p++] = (uint8_t)(0xE0 | (es[i].pid >> 8));
        s[p++] = (uint8_t)es[i].pid;
        s[p++] = (uint8_t)(0xF0 | (es[i].desc_len >> 8));
        s[p++] = (uint8_t)es[i].desc_len;
        memcpy(s + p, es[i].desc, es[i].desc_len);
        p += es[i].desc_len;
    }
    return finish(s, p);
}

static int bcd(int v) { return ((v / 10) << 4) | (v % 10); }

static void utc_field(uint8_t *q, int64_t t)   /* MJD (16 bits) + hh mm ss in BCD */
{
    int64_t days = t / 86400, secs = t % 86400;
    if (secs < 0) {
        secs += 86400;
        days--;
    }
    int mjd = (int)(days + 40587);
    q[0] = (uint8_t)(mjd >> 8);
    q[1] = (uint8_t)mjd;
    q[2] = (uint8_t)bcd((int)(secs / 3600));
    q[3] = (uint8_t)bcd((int)(secs / 60 % 60));
    q[4] = (uint8_t)bcd((int)(secs % 60));
}

int psi_tdt(uint8_t *s, int64_t utc_s)
{
    s[0] = 0x70;
    s[1] = 0x70;             /* syntax 0, reserved */
    s[2] = 5;
    utc_field(s + 3, utc_s);
    return 8;
}

int psi_tot(uint8_t *s, int64_t utc_s, const uint8_t *desc, int desc_len)
{
    s[0] = 0x73;
    s[1] = 0x70;
    utc_field(s + 3, utc_s);
    s[8] = (uint8_t)(0xF0 | (desc_len >> 8));
    s[9] = (uint8_t)desc_len;
    memcpy(s + 10, desc, desc_len);
    return finish(s, 10 + desc_len);
}

int psi_packetize(const uint8_t *sec, int len, int pid, uint8_t *cc, uint8_t *out, int maxpkts)
{
    int n = 0, off = 0;
    while (off < len && n < maxpkts) {
        uint8_t *p = out + n * TS_PKT;
        int first = off == 0;
        p[0] = 0x47;
        p[1] = (uint8_t)((first ? 0x40 : 0) | ((pid >> 8) & 0x1F));
        p[2] = (uint8_t)pid;
        p[3] = (uint8_t)(0x10 | (*cc & 0x0F));
        *cc = (uint8_t)((*cc + 1) & 0x0F);
        int q = 4;
        if (first)
            p[q++] = 0;                  /* pointer_field */
        int chunk = TS_PKT - q;
        if (chunk > len - off)
            chunk = len - off;
        memcpy(p + q, sec + off, chunk);
        q += chunk;
        off += chunk;
        memset(p + q, 0xFF, TS_PKT - q);
        n++;
    }
    return n;
}

int psi_load_sections(const char *path, uint8_t **buf, int *nsec)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > (64 << 20)) {
        fclose(f);
        return -1;
    }
    uint8_t *b = malloc(sz ? (size_t)sz : 1);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(b);
        return -1;
    }
    fclose(f);
    int n = 0;
    for (long off = 0; off + 3 <= sz;) {
        int l = psi_sec_len(b + off);
        if (off + l > sz || l > SEC_MAX) {
            free(b);
            return -1;
        }
        off += l;
        n++;
    }
    *buf = b;
    *nsec = n;
    return (int)sz;
}
