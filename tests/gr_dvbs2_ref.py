#!/usr/bin/env python3
"""Reference: DVB-S2 symbols of a TS file through GNU Radio gr-dtv (physical layer output, 1 sample/symbol)."""
import sys
from gnuradio import gr, blocks, dtv

ts, out, bits, k, n, short, pilots, nsym = sys.argv[1:9]
bits, short, pilots, nsym = int(bits), int(short), int(pilots), int(nsym)
mod = {2: dtv.MOD_QPSK, 3: dtv.MOD_8PSK, 4: dtv.MOD_16APSK, 5: dtv.MOD_32APSK}[bits]
cr = getattr(dtv, f"C{k}_{n}")
fs = dtv.FECFRAME_SHORT if short else dtv.FECFRAME_NORMAL
tb = gr.top_block()
chain = [blocks.file_source(1, ts, True),
         dtv.dvb_bbheader_bb(dtv.STANDARD_DVBS2, fs, cr, dtv.RO_0_35, dtv.INPUTMODE_NORMAL, dtv.INBAND_OFF, 168, 4000000),
         dtv.dvb_bbscrambler_bb(dtv.STANDARD_DVBS2, fs, cr),
         dtv.dvb_bch_bb(dtv.STANDARD_DVBS2, fs, cr),
         dtv.dvb_ldpc_bb(dtv.STANDARD_DVBS2, fs, cr, dtv.MOD_OTHER),
         dtv.dvbs2_interleaver_bb(fs, cr, mod),
         dtv.dvbs2_modulator_bc(fs, cr, mod, dtv.INTERPOLATION_OFF),
         dtv.dvbs2_physical_cc(fs, cr, mod, dtv.PILOTS_ON if pilots else dtv.PILOTS_OFF, 0),
         blocks.keep_m_in_n(gr.sizeof_gr_complex, 1, 2, 0),
         blocks.head(gr.sizeof_gr_complex, nsym), blocks.file_sink(gr.sizeof_gr_complex, out)]
for a, b in zip(chain, chain[1:]):
    tb.connect(a, b)
tb.run()
