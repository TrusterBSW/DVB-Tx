/* SPDX-License-Identifier: GPL-3.0-or-later
 * Engine configuration: an INI file written by the dvbtx supervisor.
 *   [kind index]       e.g. [carrier 0], [service 3]
 *   key = value        keys may repeat (table = ...) */
#ifndef DVBTX_CONF_H
#define DVBTX_CONF_H

typedef struct conf_kv { char *k, *v; } conf_kv;

typedef struct conf_sec {
    char kind[32];
    int index;
    conf_kv *kv;
    int nkv;
} conf_sec;

typedef struct conf {
    conf_sec *sec;
    int nsec;
} conf;

conf *conf_load(const char *path, char *err, int errlen);
void conf_free(conf *c);
int conf_count(const conf *c, const char *kind);
conf_sec *conf_find(const conf *c, const char *kind, int index);
const char *conf_str(const conf_sec *s, const char *key, const char *def);
double conf_num(const conf_sec *s, const char *key, double def);   /* decimal, 0x hex */
long long conf_int(const conf_sec *s, const char *key, long long def);
/* n-th occurrence of a repeated key, NULL past the end */
const char *conf_nth(const conf_sec *s, const char *key, int n);
/* fails (die) on a mandatory key */
const char *conf_req(const conf_sec *s, const char *key);

#endif
