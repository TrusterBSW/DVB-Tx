"""Monitoring web page and API.

GET  /                       dashboard
GET  /api/status             everything (plan + live statistics + alerts)
GET  /api/history?last=600   time series of the last seconds
GET  /api/analysis/<n>       last full TSDuck report of carrier n
GET  /api/epg/<sid>          events of a service
GET  /api/calc?standard=dvbt&constellation=64qam&...   useful bit rate of a channel
GET  /metrics                Prometheus exposition
POST /api/input/<n>/restart  restart one ingest process
POST /api/reload             re-read the EPG and rebuild the SI tables
GET    /api/channels                    the channels (number, service_id, name, source, backup, ...)
POST   /api/channels                    add one: {"name", "source", "carrier"?, "lcn"?, "sid"?, "backup"?, options}
PATCH  /api/channels/<channel>          change it: {"name"?, "lcn"?, "swap"?, "source"?, "backup"?, options}
DELETE /api/channels/<channel>          remove it
  <channel>: number (LCN), service_id (0x0101) or name (URL-encoded). The changes are applied
  at once to that channel only, and written to the configuration file. Answer: {"ok", "sid",
  "changes", "saved", "warning"?} or {"error"} with status 400.

Requests that change something must come from this page or a program (no other web site: Origin
checked, JSON body), and carry the token when web.token is set (Authorization: Bearer <token>).
"""

import datetime as dt
import json
import logging
import os
import re
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlsplit

log = logging.getLogger("dvbtx.web")
HERE = os.path.dirname(os.path.abspath(__file__))


def _json_default(o):
    if isinstance(o, (dt.datetime, dt.date)):
        return o.isoformat()
    if isinstance(o, (set, tuple)):
        return list(o)
    return str(o)


def metrics(sup):
    st = sup.stats or {}
    L = []

    def m(name, value, labels=None, help_=None):
        if value is None:
            return
        lab = ""
        if labels:
            lab = "{" + ",".join(f'{k}="{str(v).replace(chr(34), "")}"' for k, v in labels.items()) + "}"
        L.append(f"dvbtx_{name}{lab} {float(value):.6g}")

    m("up", 1 if sup.engine is not None else 0)
    m("alerts_active", len(sup.alerts.active))
    for r in st.get("radios", []):
        lb = {"radio": r["index"], "driver": r["driver"]}
        m("radio_underruns_total", r["underruns"], lb)
        m("radio_device_gaps_total", r.get("late", 0), lb)
        m("radio_buffer_fill", r["fill"], lb)
        m("radio_cpu_percent", r["cpu"], lb)
    for c in st.get("carriers", []):
        car = sup.plan.carriers[c["carrier"]]
        lb = {"carrier": c["carrier"], "name": car.name}
        m("carrier_capacity_bps", c["ts_rate"], lb)
        m("carrier_useful_bps", c["rate"] - c["null_rate"], lb)
        m("carrier_null_bps", c["null_rate"], lb)
        m("carrier_si_bps", c["si_rate"], lb)
        m("carrier_cpu_percent", c.get("cpu"), lb)
        by_sid = {x.sid: x for x in car.services}
        for s in c.get("services", []):
            svc = by_sid.get(s["sid"])
            if svc is None:
                continue
            sl = {"carrier": c["carrier"], "service": svc.name, "sid": svc.sid, "lcn": svc.lcn}
            m("service_bps", s["rate"], sl)
            m("service_video_margin_min_ms", s.get("video_margin_min_ms"), sl)
            m("service_audio_margin_min_ms", s.get("audio_margin_min_ms"), sl)
            m("service_late_max_ms", s.get("late_max_ms"), sl)
            m("service_dropped_late_total", s.get("dropped_late"), sl)
            m("service_on_backup", 1 if s.get("source") == "backup" else 0, sl)
            m("service_switches_total", s.get("switches"), sl)
    for i in st.get("inputs", []):
        lb = {"input": i["index"], "name": i["name"]}
        m("input_bps", i["rate"], lb)
        m("input_cc_errors_total", i["cc_errors"], lb)
        m("input_idle_ms", i["idle_ms"], lb)
        m("input_slack_min_ms", i["slack_min_ms"], lb)
    for p in sup.ingest:
        m("ingest_restarts_total", p.restarts, {"input": p.inp.index, "name": p.inp.name})
    return "\n".join(L) + "\n"


class Handler(BaseHTTPRequestHandler):
    sup = None
    server_version = "dvbtx"

    def log_message(self, fmt, *args):
        log.debug("%s " + fmt, self.address_string(), *args)

    def _send(self, code, body, ctype="application/json"):
        if not isinstance(body, (bytes, bytearray)):
            body = json.dumps(body, default=_json_default, separators=(",", ":")).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path, _, query = self.path.partition("?")
        q = dict(p.split("=", 1) for p in query.split("&") if "=" in p)
        sup = self.sup
        try:
            if path in ("/", "/index.html"):
                with open(os.path.join(HERE, "web", "index.html"), "rb") as f:
                    return self._send(200, f.read(), "text/html; charset=utf-8")
            if path == "/api/status":
                return self._send(200, sup.status())
            if path == "/api/history":
                last = int(q.get("last", "600"))
                return self._send(200, sup.history.dump(last))
            m = re.fullmatch(r"/api/analysis/(\d+)", path)
            if m:
                i = int(m.group(1))
                an = next((a for a in sup.analyzers if a.car.index == i), None)
                return self._send(200 if an and an.report else 404, an.report if an and an.report else {"error": "no report"})
            m = re.fullmatch(r"/api/epg/(\d+)", path)
            if m:
                sid = int(m.group(1))
                evs = sup.store.events(sid) if sup.store else []
                return self._send(200, [{"event_id": e["event_id"], "start": e["start"], "duration": e["duration"],
                                         "title": e.get("title"), "subtitle": e.get("subtitle")} for e in evs[:200]])
            if path == "/api/calc":
                from urllib.parse import unquote_plus
                from . import capacity
                p = {k: unquote_plus(v) for k, v in q.items()}
                std = p.pop("standard", "dvbt")
                for k in ("bandwidth", "symbol_rate"):
                    if k in p:
                        p[k] = float(p[k])
                for b in ("pilots", "extended", "rotation"):
                    if b in p:
                        p[b] = p[b] in ("1", "true", "on")
                n = int(p.pop("services", "1") or 1)
                try:
                    r = capacity.rate(std, **p)
                except (capacity.CapacityError, TypeError, ValueError, KeyError) as e:
                    return self._send(400, {"error": str(e)})
                return self._send(200, {"rate": r, "per_service": r * 0.97 / max(1, n)})
            if path == "/metrics":
                return self._send(200, metrics(sup).encode(), "text/plain; version=0.0.4")
            if path == "/api/channels":
                return self._send(200, sup.channels())
            return self._send(404, {"error": "not found"})
        except (BrokenPipeError, ConnectionResetError):
            pass

    # ---------------------------------------------------------------- changes

    def _allowed(self, body_expected):
        """same origin (a page of another site cannot drive the transmitter), JSON body, token if set"""
        origin = self.headers.get("Origin")
        if origin and urlsplit(origin).netloc != self.headers.get("Host", ""):
            self._send(403, {"error": "request from another web site refused"})
            return False
        if body_expected and not (self.headers.get("Content-Type") or "").startswith("application/json"):
            self._send(415, {"error": "JSON body expected (Content-Type: application/json)"})
            return False
        token = (self.sup.cfg.get("web") or {}).get("token") or ""
        if token:
            given = self.headers.get("Authorization", "")
            given = given[7:] if given.startswith("Bearer ") else self.headers.get("X-Dvbtx-Token", "")
            if given != token:
                self._send(401, {"error": "token required (web.token)"})
                return False
        return True

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n > 65536:
            raise ValueError("body too large")
        data = json.loads(self.rfile.read(n) or b"{}")
        if not isinstance(data, dict):
            raise ValueError("JSON object expected")
        return data

    def _change(self, fn):
        from .channels import ChannelError
        try:
            res = self.sup.call(fn)
            return self._send(200, dict(res, ok=True))
        except (ChannelError, ValueError) as e:
            return self._send(400, {"ok": False, "error": str(e)})

    def do_POST(self):
        sup = self.sup
        path = self.path.partition("?")[0]
        if not self._allowed(path == "/api/channels"):
            return
        m = re.fullmatch(r"/api/input/(\d+)/restart", path)
        if m:
            p = sup.proc_of(int(m.group(1)))
            if p:
                p.restart()
                return self._send(200, {"ok": True})
            return self._send(404, {"error": "no such input"})
        if path == "/api/reload":
            sup.reload_si()
            return self._send(200, {"ok": True})
        if path == "/api/channels":
            try:
                spec = self._body()
            except ValueError as e:
                return self._send(400, {"error": str(e)})
            allow = bool(sup.cfg["web"].get("allow_exec_sources"))
            return self._change(lambda: sup.channel_add(spec, allow))
        return self._send(404, {"error": "not found"})

    def do_PATCH(self):
        sup = self.sup
        m = re.fullmatch(r"/api/channels/([^/?]+)", self.path.partition("?")[0])
        if not m:
            return self._send(404, {"error": "not found"})
        if not self._allowed(True):
            return
        try:
            changes = self._body()
        except ValueError as e:
            return self._send(400, {"error": str(e)})
        ref = unquote(m.group(1))
        allow = bool(sup.cfg["web"].get("allow_exec_sources"))
        return self._change(lambda: sup.channel_update(ref, changes, allow))

    def do_DELETE(self):
        sup = self.sup
        m = re.fullmatch(r"/api/channels/([^/?]+)", self.path.partition("?")[0])
        if not m:
            return self._send(404, {"error": "not found"})
        if not self._allowed(False):
            return
        ref = unquote(m.group(1))
        return self._change(lambda: sup.channel_remove(ref))


def start(sup, listen):
    host, _, port = listen.rpartition(":")
    Handler.sup = sup
    srv = ThreadingHTTPServer((host or "0.0.0.0", int(port)), Handler)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    log.info("web page on http://%s:%s/", host or "0.0.0.0", port)
    return srv
