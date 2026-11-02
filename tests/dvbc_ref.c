/* Test: DVB-C symbols of a TS file (cf32, one per symbol) from the dvbtx coder. */
#include <stdio.h>
#include <stdlib.h>
#include "../engine/src/dvbc.h"
static FILE *tsf;
static void pull(void *ctx, uint8_t *pkt)
{
    (void)ctx;
    if (fread(pkt, 1, 188, tsf) != 188) { rewind(tsf); if (fread(pkt, 1, 188, tsf) != 188) exit(1); }
}
int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: in.ts out.cf32 bits nsym\n"); return 1; }
    tsf = fopen(argv[1], "rb");
    FILE *o = fopen(argv[2], "wb");
    dvbc *c = dvbc_new(atoi(argv[3]), pull, NULL);
    long n = atol(argv[4]);
    for (long i = 0; i < n; i++) { float complex s; dvbc_symbol(c, &s); fwrite(&s, sizeof s, 1, o); }
    fclose(o);
    return 0;
}
