#!/usr/bin/env python3
# -*- coding: utf-8 -*-

#
# SPDX-License-Identifier: GPL-3.0
#
# GNU Radio Python Flow Graph
# Title: Dvbt Tx Pipe
# GNU Radio version: 3.10.11.0

from gnuradio import blocks
import pmt
from gnuradio import digital
from gnuradio import dtv
from gnuradio import gr
from gnuradio.filter import firdes
from gnuradio.fft import window
import sys
import signal
from argparse import ArgumentParser
from gnuradio.eng_arg import eng_float, intx
from gnuradio import eng_notation
import osmosdr
import time
import threading




class dvbt_tx_pipe(gr.top_block):

    def __init__(self):
        gr.top_block.__init__(self, "Dvbt Tx Pipe", catch_exceptions=True)
        self.flowgraph_started = threading.Event()

        ##################################################
        # Variables
        ##################################################
        self.samp_rate = samp_rate = (8000000.0 * 8) / 7
        self.center_freq = center_freq = 554e6  ##DVT UHF CH31
        #self.center_freq = center_freq = 226e6 ##Uniquement DVB-T2

        ##################################################
        # Blocks
        ##################################################

        self.osmosdr_sink_0 = osmosdr.sink(
            args="numchan=" + str(1) + " " + "hackrf"
        )
        self.osmosdr_sink_0.set_time_unknown_pps(osmosdr.time_spec_t())
        self.osmosdr_sink_0.set_sample_rate(samp_rate)
        self.osmosdr_sink_0.set_center_freq(center_freq, 0)
        self.osmosdr_sink_0.set_freq_corr(0, 0)
        self.osmosdr_sink_0.set_gain(30, 0)
        self.osmosdr_sink_0.set_if_gain(20, 0)
        self.osmosdr_sink_0.set_bb_gain(20, 0)
        self.osmosdr_sink_0.set_antenna('', 0)
        self.osmosdr_sink_0.set_bandwidth(0, 0)
        self.dtv_dvbt_symbol_inner_interleaver_0 = dtv.dvbt_symbol_inner_interleaver(6048, dtv.T8k, 1)
        self.dtv_dvbt_reference_signals_0 = dtv.dvbt_reference_signals(
            gr.sizeof_gr_complex,
            6048,
            8192,
            dtv.MOD_64QAM,
            dtv.NH,
            dtv.C7_8,
            dtv.C7_8,
            dtv.GI_1_32,
            dtv.T8k,
            1,
            16)
        self.dtv_dvbt_reed_solomon_enc_0 = dtv.dvbt_reed_solomon_enc(2, 8, 0x11d, 255, 239, 8, 51, 8)
        self.dtv_dvbt_map_0 = dtv.dvbt_map(6048, dtv.MOD_64QAM, dtv.NH, dtv.T8k, 1)
        self.dtv_dvbt_inner_coder_0 = dtv.dvbt_inner_coder(1, 6048, dtv.MOD_64QAM, dtv.NH, dtv.C7_8)
        self.dtv_dvbt_energy_dispersal_0 = dtv.dvbt_energy_dispersal(1)
        self.dtv_dvbt_convolutional_interleaver_0 = dtv.dvbt_convolutional_interleaver(136, 12, 17)
        self.dtv_dvbt_bit_inner_interleaver_0 = dtv.dvbt_bit_inner_interleaver(6048, dtv.MOD_64QAM, dtv.NH, dtv.T8k)
        self.digital_ofdm_cyclic_prefixer_0 = digital.ofdm_cyclic_prefixer(
            8192,
            8192 + 256,
            0,
            '')
        self.blocks_file_source_0 = blocks.file_source(gr.sizeof_char*1, 'mpeg-live.ts', False, 0, 0)
        self.blocks_file_source_0.set_begin_tag(pmt.PMT_NIL)


        ##################################################
        # Connections
        ##################################################
        self.connect((self.blocks_file_source_0, 0), (self.dtv_dvbt_energy_dispersal_0, 0))
        self.connect((self.digital_ofdm_cyclic_prefixer_0, 0), (self.osmosdr_sink_0, 0))
        self.connect((self.dtv_dvbt_bit_inner_interleaver_0, 0), (self.dtv_dvbt_symbol_inner_interleaver_0, 0))
        self.connect((self.dtv_dvbt_convolutional_interleaver_0, 0), (self.dtv_dvbt_inner_coder_0, 0))
        self.connect((self.dtv_dvbt_energy_dispersal_0, 0), (self.dtv_dvbt_reed_solomon_enc_0, 0))
        self.connect((self.dtv_dvbt_inner_coder_0, 0), (self.dtv_dvbt_bit_inner_interleaver_0, 0))
        self.connect((self.dtv_dvbt_map_0, 0), (self.dtv_dvbt_reference_signals_0, 0))
        self.connect((self.dtv_dvbt_reed_solomon_enc_0, 0), (self.dtv_dvbt_convolutional_interleaver_0, 0))
        self.connect((self.dtv_dvbt_reference_signals_0, 0), (self.digital_ofdm_cyclic_prefixer_0, 0))
        self.connect((self.dtv_dvbt_symbol_inner_interleaver_0, 0), (self.dtv_dvbt_map_0, 0))


    def get_samp_rate(self):
        return self.samp_rate

    def set_samp_rate(self, samp_rate):
        self.samp_rate = samp_rate
        self.osmosdr_sink_0.set_sample_rate(self.samp_rate)

    def get_center_freq(self):
        return self.center_freq

    def set_center_freq(self, center_freq):
        self.center_freq = center_freq
        self.osmosdr_sink_0.set_center_freq(self.center_freq, 0)




def main(top_block_cls=dvbt_tx_pipe, options=None):
    tb = top_block_cls()

    def sig_handler(sig=None, frame=None):
        tb.stop()
        tb.wait()

        sys.exit(0)

    signal.signal(signal.SIGINT, sig_handler)
    signal.signal(signal.SIGTERM, sig_handler)

    tb.start()
    tb.flowgraph_started.set()

    try:
        input('Press Enter to quit: ')
    except EOFError:
        pass
    tb.stop()
    tb.wait()


if __name__ == '__main__':
    main()
