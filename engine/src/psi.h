/* SPDX-License-Identifier: GPL-3.0-or-later
 * PSI/SI sections built by the engine (PAT, PMT, TDT, TOT) and packetisation of any section. */
#ifndef DVBTX_PSI_H
#define DVBTX_PSI_H

#include <stdint.h>

#include "input.h"

#define SEC_MAX 4096

typedef struct psi_svc { int sid, pmt_pid; } psi_svc;

typedef struct psi_es {
    int pid, type, desc_len;
    const uint8_t *desc;
} psi_es;

int psi_pat(uint8_t *s, int tsid, int version, const psi_svc *svc, int n, int nit_pid);
int psi_pmt(uint8_t *s, int sid, int version, int pcr_pid, const uint8_t *pinfo, int pinfo_len,
            const psi_es *es, int nes);
int psi_tdt(uint8_t *s, int64_t utc_s);
int psi_tot(uint8_t *s, int64_t utc_s, const uint8_t *desc, int desc_len);

/* splits sections into TS packets (each section starts a packet); cc is the next CC of the PID;
 * returns the number of packets written to out (max maxpkts) */
int psi_packetize(const uint8_t *sec, int len, int pid, uint8_t *cc, uint8_t *out, int maxpkts);

/* loads a file of concatenated binary sections; returns the number of bytes, -1 on error */
int psi_load_sections(const char *path, uint8_t **buf, int *nsec);
/* length of the section at p (3 + section_length) */
static inline int psi_sec_len(const uint8_t *p) { return 3 + (((p[1] & 0x0F) << 8) | p[2]); }

#endif
