#!/usr/bin/env python3
"""Reference: modulate a TS file with GNU Radio gr-dtv (cf32 at the elementary rate)."""
import sys
from gnuradio import gr, blocks, dtv, digital

ts, out, fft, bits, k, n, gi, nsym = sys.argv[1:9]
fft, bits, nsym = int(fft), int(bits), int(nsym)
mode = dtv.T8k if fft == 8192 else dtv.T2k
mod = {2: dtv.MOD_QPSK, 4: dtv.MOD_16QAM, 6: dtv.MOD_64QAM}[bits]
cr = {"1/2": dtv.C1_2, "2/3": dtv.C2_3, "3/4": dtv.C3_4, "5/6": dtv.C5_6, "7/8": dtv.C7_8}[k + "/" + n]
g = {"4": dtv.GI_1_4, "8": dtv.GI_1_8, "16": dtv.GI_1_16, "32": dtv.GI_1_32}[gi]
nd = 6048 if fft == 8192 else 1512
cp = fft // int(gi)
tb = gr.top_block()
src = blocks.file_source(1, ts, True)
chain = [src, dtv.dvbt_energy_dispersal(1), dtv.dvbt_reed_solomon_enc(2, 8, 0x11d, 255, 239, 8, 51, 8),
         dtv.dvbt_convolutional_interleaver(136, 12, 17), dtv.dvbt_inner_coder(1, nd, mod, dtv.NH, cr),
         dtv.dvbt_bit_inner_interleaver(nd, mod, dtv.NH, mode), dtv.dvbt_symbol_inner_interleaver(nd, mode, 1),
         dtv.dvbt_map(nd, mod, dtv.NH, mode, 1),
         dtv.dvbt_reference_signals(gr.sizeof_gr_complex, nd, fft, mod, dtv.NH, cr, cr, g, mode, 1, 0),
         digital.ofdm_cyclic_prefixer(fft, fft + cp, 0, ''),
         blocks.head(gr.sizeof_gr_complex, nsym * (fft + cp)), blocks.file_sink(gr.sizeof_gr_complex, out)]
for a, b in zip(chain, chain[1:]):
    tb.connect(a, b)
tb.run()
