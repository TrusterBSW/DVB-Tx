/* Test: scgen PULSE_LOWPASS resampling of a native-rate cf32 file to another rate with an offset. */
#include <stdio.h>
#include <stdlib.h>
#include "../engine/src/scgen.h"
static FILE *in;
static void get(void *ctx, float complex *v) { (void)ctx; if (fread(v, sizeof *v, 1, in) != 1) *v = 0; }
int main(int argc, char **argv)
{
    if (argc < 8) { fprintf(stderr, "usage: in.cf32 out.cf32 rate_in rate_out offset occupied_fraction nout\n"); return 1; }
    in = fopen(argv[1], "rb");
    FILE *o = fopen(argv[2], "wb");
    scgen *g = scgen_new2(atof(argv[3]), atof(argv[4]), atof(argv[5]), PULSE_LOWPASS, atof(argv[6]), argc > 8 ? atoi(argv[8]) : 32, 1.0f, get, NULL);
    long n = atol(argv[7]);
    static float complex buf[16384];
    for (long done = 0; done < n; done += 16384) { scgen_block(g, buf, 16384); fwrite(buf, sizeof(float complex), 16384, o); }
    fclose(o);
    return 0;
}
