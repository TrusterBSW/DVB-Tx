/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "conf.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

conf *conf_load(const char *path, char *err, int errlen)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, errlen, "cannot open %s", path);
        return NULL;
    }
    conf *c = xcalloc(1, sizeof *c);
    char line[4096];
    int ln = 0;
    conf_sec *cur = NULL;
    while (fgets(line, sizeof line, f)) {
        ln++;
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';')
            continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (!e) {
                snprintf(err, errlen, "%s:%d: missing ]", path, ln);
                goto fail;
            }
            *e = 0;
            c->sec = realloc(c->sec, sizeof *c->sec * (c->nsec + 1));
            cur = &c->sec[c->nsec++];
            memset(cur, 0, sizeof *cur);
            char kind[32] = "";
            int idx = 0;
            sscanf(s + 1, "%31s %d", kind, &idx);
            snprintf(cur->kind, sizeof cur->kind, "%s", kind);
            cur->index = idx;
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq || !cur) {
            snprintf(err, errlen, "%s:%d: expected key = value inside a [section]", path, ln);
            goto fail;
        }
        *eq = 0;
        cur->kv = realloc(cur->kv, sizeof *cur->kv * (cur->nkv + 1));
        cur->kv[cur->nkv].k = strdup(trim(s));
        cur->kv[cur->nkv].v = strdup(trim(eq + 1));
        cur->nkv++;
    }
    fclose(f);
    return c;
fail:
    fclose(f);
    conf_free(c);
    return NULL;
}

void conf_free(conf *c)
{
    if (!c)
        return;
    for (int i = 0; i < c->nsec; i++) {
        for (int j = 0; j < c->sec[i].nkv; j++) {
            free(c->sec[i].kv[j].k);
            free(c->sec[i].kv[j].v);
        }
        free(c->sec[i].kv);
    }
    free(c->sec);
    free(c);
}

int conf_count(const conf *c, const char *kind)
{
    int n = 0;
    for (int i = 0; i < c->nsec; i++)
        if (!strcmp(c->sec[i].kind, kind))
            n++;
    return n;
}

conf_sec *conf_find(const conf *c, const char *kind, int index)
{
    for (int i = 0; i < c->nsec; i++)
        if (!strcmp(c->sec[i].kind, kind) && c->sec[i].index == index)
            return &c->sec[i];
    return NULL;
}

const char *conf_nth(const conf_sec *s, const char *key, int n)
{
    if (!s)
        return NULL;
    for (int i = 0; i < s->nkv; i++)
        if (!strcmp(s->kv[i].k, key) && n-- == 0)
            return s->kv[i].v;
    return NULL;
}

const char *conf_str(const conf_sec *s, const char *key, const char *def)
{
    const char *v = conf_nth(s, key, 0);
    return v ? v : def;
}

double conf_num(const conf_sec *s, const char *key, double def)
{
    const char *v = conf_nth(s, key, 0);
    if (!v || !*v)
        return def;
    if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X'))
        return (double)strtoll(v, NULL, 16);
    return strtod(v, NULL);
}

long long conf_int(const conf_sec *s, const char *key, long long def)
{
    const char *v = conf_nth(s, key, 0);
    if (!v || !*v)
        return def;
    return strtoll(v, NULL, 0);
}

const char *conf_req(const conf_sec *s, const char *key)
{
    const char *v = conf_nth(s, key, 0);
    if (!v)
        die("engine config: [%s %d] needs '%s'", s ? s->kind : "?", s ? s->index : 0, key);
    return v;
}
