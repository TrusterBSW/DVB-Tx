/* Test: DVB-S2 symbols (one complex per symbol) of a TS file from the dvbtx coder. */
#include <stdio.h>
#include <stdlib.h>
#include "../engine/src/dvbs2.h"
static FILE *tsf;
static void pull(void *ctx, uint8_t *pkt)
{
    (void)ctx;
    if (fread(pkt, 1, 188, tsf) != 188) { rewind(tsf); if (fread(pkt, 1, 188, tsf) != 188) exit(1); }
}
int main(int argc, char **argv)
{
    if (argc < 9) { fprintf(stderr, "usage: in.ts out.cf32 bits k n short pilots nsym\n"); return 1; }
    tsf = fopen(argv[1], "rb");
    FILE *o = fopen(argv[2], "wb");
    dvbs2_cfg c = { atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), atoi(argv[6]), atoi(argv[7]), 0.35, 0 };
    const char *e = dvbs2_cfg_check(&c);
    if (e) { fprintf(stderr, "%s\n", e); return 1; }
    dvbs2 *s = dvbs2_new(&c, pull, NULL);
    long n = atol(argv[8]);
    for (long i = 0; i < n; i++) { float complex v; dvbs2_symbol(s, &v); fwrite(&v, sizeof v, 1, o); }
    fclose(o);
    return 0;
}
