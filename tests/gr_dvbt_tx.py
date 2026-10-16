#!/usr/bin/env python3
"""Known-good reference transmitter: gr-dtv DVB-T 8k 64QAM 7/8 1/32 -> HackRF (osmosdr)."""
import sys, time
from gnuradio import gr, blocks, dtv, digital
import osmosdr
ts, freq, gain = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
amp = int(sys.argv[4]) if len(sys.argv) > 4 else 0
rate = 64e6 / 7
tb = gr.top_block()
chain = [blocks.file_source(1, ts, True), dtv.dvbt_energy_dispersal(1),
         dtv.dvbt_reed_solomon_enc(2, 8, 0x11d, 255, 239, 8, 51, 8), dtv.dvbt_convolutional_interleaver(136, 12, 17),
         dtv.dvbt_inner_coder(1, 6048, dtv.MOD_64QAM, dtv.NH, dtv.C7_8),
         dtv.dvbt_bit_inner_interleaver(6048, dtv.MOD_64QAM, dtv.NH, dtv.T8k),
         dtv.dvbt_symbol_inner_interleaver(6048, dtv.T8k, 1), dtv.dvbt_map(6048, dtv.MOD_64QAM, dtv.NH, dtv.T8k, 1),
         dtv.dvbt_reference_signals(gr.sizeof_gr_complex, 6048, 8192, dtv.MOD_64QAM, dtv.NH, dtv.C7_8, dtv.C7_8,
                                    dtv.GI_1_32, dtv.T8k, 1, 0),
         digital.ofdm_cyclic_prefixer(8192, 8192 + 256, 0, ''), blocks.multiply_const_cc(0.25)]
sink = osmosdr.sink(args="numchan=1 hackrf=0")
sink.set_sample_rate(rate); sink.set_center_freq(freq, 0); sink.set_gain(14 if amp else 0, 0)
sink.set_if_gain(gain, 0); sink.set_bb_gain(20, 0); sink.set_bandwidth(0, 0)
chain.append(sink)
for a, b in zip(chain, chain[1:]):
    tb.connect(a, b)
tb.start(); time.sleep(float(sys.argv[5]) if len(sys.argv) > 5 else 30); tb.stop()
