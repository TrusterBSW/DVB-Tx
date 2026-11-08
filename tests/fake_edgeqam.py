#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A stand-in for the edgeQAM board (FPGA DVB-T modulator, dvbt_tx): UDP inputs, one per multiplex, each
consumed at the exact DVB-T rate (2k 64-QAM 7/8 GI 1/32 8 MHz) on its own clock (ppm off), from a buffer of
8192 packets: a null packet is inserted when it is empty, packets are dropped when it is full. HTTP API
like the board's: GET /status (JSON, "queued" per multiplex, in steps of 86 like the board's DMA blocks),
GET /set?freq=&disable= (not saved: a restart, POST /restart here, forgets them).

    fake_edgeqam.py [--ppm 5] [--nmux 4] [--port 1001] [--api 8080] [--host 127.0.0.1]
                    [--record FILE --record-mux K --record-s SECONDS]

--record FILE: the packets multiplex K sends on air (its packets and the nulls inserted), the first
SECONDS of them, to check the PCRs against the board's clock (the packet index)."""
import argparse
import json
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

RATE = 31_668_449.2                  # bit/s: 7938 bits per symbol x 188/204 / 231 us
CAPACITY = 8192
STEP = 86                            # "queued" moves by DMA blocks of 344 packets shared by 4 multiplexes
NULL = b"\x47\x1f\xff\x10" + b"\xff" * 184


class Mux:
    def __init__(self, k, port, host, record=None, record_pkts=0):
        self.k, self.port = k, port
        self.lock = threading.Lock()
        self.queue = []
        self.rx = self.null_inserted = self.dropped = self.resync = 0
        self.disabled = 0
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
        self.sock.bind((host, port))
        self.out = open(record, "wb") if record else None
        self.left = record_pkts
        threading.Thread(target=self.recv, daemon=True).start()

    def recv(self):
        while True:
            d = self.sock.recv(4096)
            if len(d) % 188 == 12 and d[0] == 0x80:
                d = d[12:]
            with self.lock:
                for i in range(0, len(d) - 187, 188):
                    self.rx += 1
                    if len(self.queue) >= CAPACITY:
                        self.dropped += 1
                    else:
                        self.queue.append(d[i:i + 188])

    def take(self, n):
        with self.lock:
            got, self.queue = self.queue[:n], self.queue[n:]
            self.null_inserted += n - len(got)
        if self.out and self.left > 0:
            self.out.write((b"".join(got) + NULL * (n - len(got)))[:self.left * 188])
            self.left -= n
            if self.left <= 0:
                self.out.close()

    def reset(self):
        with self.lock:
            self.queue = []
            self.rx = self.null_inserted = self.dropped = self.resync = 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ppm", type=float, default=5.0, help="the board's clock error (+ = faster)")
    ap.add_argument("--nmux", type=int, default=4)
    ap.add_argument("--port", type=int, default=1001)
    ap.add_argument("--api", type=int, default=8080)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--freq", type=int, default=474_000_000)
    ap.add_argument("--record")
    ap.add_argument("--record-mux", type=int, default=1)
    ap.add_argument("--record-s", type=float, default=30)
    a = ap.parse_args()
    pkt_s = RATE * (1 + a.ppm * 1e-6) / 1504
    muxes = [Mux(k + 1, a.port + k, a.host, *((a.record, int(a.record_s * pkt_s)) if k + 1 == a.record_mux and a.record
                                               else ())) for k in range(a.nmux)]
    st = {"freq": a.freq, "disable": 0, "start": time.monotonic(), "sets": 0, "polls": 0}

    def consume():
        t0, done = time.monotonic(), 0
        while True:
            time.sleep(0.005)
            due = int((time.monotonic() - t0) * pkt_s) - done
            if due > 0:
                for m in muxes:
                    m.take(due)
                done += due
    threading.Thread(target=consume, daemon=True).start()

    class H(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def reply(self, code, obj):
            body = (json.dumps(obj) + "\n").encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            u = urlparse(self.path)
            if u.path == "/status":
                st["polls"] += 1
                n = a.nmux
                lo = st["freq"] + (n - 1) * 4_000_000
                ins = []
                for m in muxes:
                    with m.lock:
                        q = len(m.queue)
                    ins.append({"mux": m.k, "centre_hz": lo + (2 * (m.k - 1) - (n - 1)) * 4_000_000,
                                "input": f"udp:{m.port}", "rx_packets": m.rx, "null_inserted": m.null_inserted,
                                "dropped": m.dropped, "resync": m.resync, "queued": q - q % STEP,
                                "disabled": (st["disable"] >> (m.k - 1)) & 1})
                self.reply(200, {"multiplexes": n, "freq_mux1_hz": st["freq"], "lo_hz": lo,
                                 "uptime_s": int(time.monotonic() - st["start"]), "sets": st["sets"],
                                 "polls": st["polls"],
                                 "disable_mask": st["disable"], "ts_mbit_per_mux": RATE / 1e6, "inputs": ins})
            elif u.path == "/set":
                q = {k: v[0] for k, v in parse_qs(u.query).items()}
                any_ = False
                if "freq" in q and 70_000_000 <= int(float(q["freq"])) <= 6_000_000_000:   # else ignored
                    st["freq"] = int(float(q["freq"]))
                    any_ = True
                if "disable" in q:
                    st["disable"] = int(q["disable"], 0) & ((1 << a.nmux) - 1)
                    any_ = True
                st["sets"] += any_
                self.reply(200 if any_ else 400, {"ok": any_, "freq_mux1_hz": st["freq"],
                                                  "lo_hz": st["freq"] + (a.nmux - 1) * 4_000_000 - 2,
                                                  "attenuation_db": 89.75, "disable_mask": st["disable"],
                                                  "saved": False})
            else:
                self.reply(404, {"error": "GET /status or /set"})

        def do_POST(self):            # the daemon restarted: buffers, counters and settings lost
            if urlparse(self.path).path == "/restart":
                for m in muxes:
                    m.reset()
                st.update(freq=a.freq, disable=0, start=time.monotonic())
                self.reply(200, {"ok": True})
            else:
                self.reply(404, {"error": "POST /restart"})

    ThreadingHTTPServer((a.host, a.api), H).serve_forever()


if __name__ == "__main__":
    main()
