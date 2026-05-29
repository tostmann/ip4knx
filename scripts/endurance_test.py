#!/usr/bin/env python3
"""
Long-running endurance / soak test for ip4knx KNXnet/IP gateways.

Primary purpose: verify that a TULX32 (ESP32-C6 + NCN5130) sustains continuous
KNXnet/IP tunneling load while powered ONLY from the KNX bus (no USB / external
supply). Secondary: confirm no memory leak, stable tunnel reconnection, and
acceptable ACK latency under sustained 1 fps load.

Per target gateway (run in parallel):
  - one KNXnet/IP tunnel, kept alive via CONNECTIONSTATE_REQUEST every 60s
  - send 1 L_Data.req group-write per second; expect TUNNELING_ACK
  - every 300s: snapshot /api/status (uptime, heap_free, active_clients)
  - on >=3 consecutive ACK timeouts or any send error: disconnect + reconnect
    with exponential backoff; the run continues
  - writes a per-target CSV (one row per frame) and a shared heartbeat log

Outputs (under --out, default ./endurance_<timestamp>/):
  <label>.csv      ts_utc,frame_idx,ack_ok,elapsed_ms,event
  heartbeat.log    periodic snapshots + reconnect/error events (UTC timestamps)

Usage:
  python3 endurance_test.py 192.0.2.10 192.0.2.11
  python3 endurance_test.py --duration 43200 --interval 1.0 gw-a.local gw-b.local
  python3 endurance_test.py --group 0/0/1 --out /tmp/soak 192.0.2.10

Analyze afterwards (per-target latency percentiles):
  python3 - <<'EOF'
  import csv, statistics
  lat=[]; tot=ok=to=0
  for r in csv.DictReader(open("endurance_<ts>/gw0.csv")):
      tot+=1
      (lat.append(float(r['elapsed_ms'])) or ok.__add__(0)) if r['ack_ok']=='1' else None
  EOF
"""

import argparse
import json
import os
import signal
import socket
import sys
import threading
import time
import urllib.request
from datetime import datetime, timezone

# Import the KNXnet/IP tunnel client + helpers from the sibling regression test.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_tunnel_source import (  # noqa: E402
    TunClient, build_cemi_group_write, build_tunneling_ack,
    build_connectionstate_req, parse_header,
    group_to_int, int_to_ia,
    TUNNELING_REQUEST, TUNNELING_ACK, CONNECTIONSTATE_RESP,
)

KNX_IP_PORT = 3671

stop_evt = threading.Event()
_log_lock = threading.Lock()
HEARTBEAT_LOG = None  # set in main()


def _sig(_s, _f):
    stop_evt.set()


def hb(line):
    ts = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
    msg = f"{ts}  {line}"
    with _log_lock:
        if HEARTBEAT_LOG:
            with open(HEARTBEAT_LOG, "a") as f:
                f.write(msg + "\n")
        print(msg, flush=True)


def http_status(host, timeout=4):
    try:
        with urllib.request.urlopen(f"http://{host}/api/status", timeout=timeout) as r:
            return json.loads(r.read())
    except Exception as e:
        return {"_error": f"{type(e).__name__}: {e}"}


class EnduranceTun(TunClient):
    """TunClient that tracks ACKs, echoes and CONNECTIONSTATE responses."""

    def __init__(self, host, name):
        super().__init__(host, name, verbose=False)
        self.ack_evt = threading.Event()
        self.last_ack_seq = -1
        self.ack_count = 0
        self.echo_count = 0
        self.connstate_resp_count = 0

    def _start_receiver(self):
        self.sock.settimeout(0.2)
        super()._start_receiver()

    def _receiver(self):
        while self.recv_run:
            try:
                data, _ = self.sock.recvfrom(512)
            except socket.timeout:
                continue
            except OSError:
                return
            hdr = parse_header(data)
            if not hdr:
                continue
            svc, _ = hdr
            if svc == TUNNELING_REQUEST and len(data) >= 10:
                seq = data[8]
                try:
                    self.sock.sendto(build_tunneling_ack(self.channel, seq),
                                     (self.host, KNX_IP_PORT))
                except Exception:
                    pass
                self.echo_count += 1
            elif svc == TUNNELING_ACK and len(data) >= 10:
                self.last_ack_seq = data[8]
                self.ack_count += 1
                self.ack_evt.set()
            elif svc == CONNECTIONSTATE_RESP:
                self.connstate_resp_count += 1

    def send_wait(self, cemi, timeout):
        seq = self.tx_seq
        self.ack_evt.clear()
        super().send_tunneling(cemi)
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.last_ack_seq == seq:
                return True
            self.ack_evt.wait(0.05)
            self.ack_evt.clear()
        return False

    def send_heartbeat(self):
        req = build_connectionstate_req(self.channel, self.local_ip, self.local_port)
        self.sock.sendto(req, (self.host, KNX_IP_PORT))


def worker(label, host, cfg):
    csv_path = os.path.join(cfg.out, f"{label}.csv")
    with open(csv_path, "w") as f:
        f.write("ts_utc,frame_idx,ack_ok,elapsed_ms,event\n")

    src_dst = group_to_int(cfg.group)
    total_sent = total_ok = total_to = total_disc = 0
    next_hb = time.monotonic() + cfg.heartbeat
    next_snap = time.monotonic() + cfg.snapshot
    deadline = time.monotonic() + cfg.duration

    def open_tunnel():
        nonlocal total_disc
        retry = 0
        while not stop_evt.is_set():
            try:
                c = EnduranceTun(host, label).connect()
                hb(f"[{label}] tunnel up ch={c.channel} IA={int_to_ia(c.assigned_ia)}")
                return c
            except Exception as e:
                total_disc += 1
                wait = min(60, 2 ** retry)
                hb(f"[{label}] connect failed: {type(e).__name__}: {e} — retry in {wait}s")
                if stop_evt.wait(wait):
                    return None
                retry += 1
        return None

    c = open_tunnel()
    if c is None:
        hb(f"[{label}] giving up before start")
        return

    frame_idx = 0
    consec_timeouts = 0
    next_send = time.monotonic()
    while not stop_evt.is_set() and time.monotonic() < deadline:
        now = time.monotonic()

        if now >= next_hb:
            try:
                c.send_heartbeat()
            except Exception as e:
                hb(f"[{label}] heartbeat send err: {type(e).__name__}: {e}")
            next_hb = now + cfg.heartbeat

        if now >= next_snap:
            st = http_status(host)
            if "_error" in st:
                hb(f"[{label}] /api/status err: {st['_error']}  "
                   f"local sent={total_sent} ok={total_ok} to={total_to} disc={total_disc}")
            else:
                hb(f"[{label}] uptime={st.get('uptime','?')}  "
                   f"heap_free={st.get('hardware', {}).get('heap_free', '?')}  "
                   f"active_clients={st.get('active_clients', '?')}  "
                   f"rx_frames={st.get('rx_frames', '?')}  "
                   f"connstate_resp={c.connstate_resp_count}  "
                   f"|| sent={total_sent} ok={total_ok} to={total_to} "
                   f"disc={total_disc} echo={c.echo_count}")
            next_snap = now + cfg.snapshot

        if now >= next_send:
            ts_utc = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")
            cemi = build_cemi_group_write(c.assigned_ia, src_dst, value=frame_idx & 1)
            t0 = time.monotonic()
            try:
                ok = c.send_wait(cemi, timeout=cfg.ack_timeout)
            except Exception as e:
                hb(f"[{label}] send err: {type(e).__name__}: {e} — reconnecting")
                try:
                    c.disconnect()
                except Exception:
                    pass
                c = open_tunnel()
                if c is None:
                    break
                continue
            elapsed_ms = (time.monotonic() - t0) * 1000
            total_sent += 1
            if ok:
                total_ok += 1
                consec_timeouts = 0
            else:
                total_to += 1
                consec_timeouts += 1
            with open(csv_path, "a") as f:
                f.write(f"{ts_utc},{frame_idx},{1 if ok else 0},{elapsed_ms:.2f},"
                        f"{'OK' if ok else 'TIMEOUT'}\n")
            frame_idx += 1
            next_send += cfg.interval
            if consec_timeouts >= cfg.max_consec_timeouts:
                hb(f"[{label}] {consec_timeouts} consec timeouts — force reconnect")
                try:
                    c.disconnect()
                except Exception:
                    pass
                c = open_tunnel()
                if c is None:
                    break
                consec_timeouts = 0
                continue
            if time.monotonic() - next_send > 5:
                next_send = time.monotonic() + cfg.interval

        stop_evt.wait(0.05)

    hb(f"[{label}] end of run.  total sent={total_sent} ok={total_ok} "
       f"timeouts={total_to} disc={total_disc}")
    try:
        c.disconnect()
    except Exception:
        pass


def main():
    global HEARTBEAT_LOG
    ap = argparse.ArgumentParser(description="ip4knx KNXnet/IP endurance test")
    ap.add_argument("hosts", nargs="+", help="gateway IP(s) / hostname(s) to soak")
    ap.add_argument("--duration", type=float, default=12 * 3600, help="seconds (default 12h)")
    ap.add_argument("--interval", type=float, default=1.0, help="frame interval s (default 1.0)")
    ap.add_argument("--ack-timeout", type=float, default=2.0, dest="ack_timeout")
    ap.add_argument("--heartbeat", type=float, default=60.0, help="CONNECTIONSTATE interval s")
    ap.add_argument("--snapshot", type=float, default=300.0, help="/api/status snapshot interval s")
    ap.add_argument("--group", default="0/0/1", help="destination group address")
    ap.add_argument("--max-consec-timeouts", type=int, default=3, dest="max_consec_timeouts")
    ap.add_argument("--out", default=None, help="output dir (default ./endurance_<ts>)")
    cfg = ap.parse_args()

    if cfg.out is None:
        cfg.out = f"./endurance_{datetime.now().strftime('%Y%m%d_%H%M%S')}"
    os.makedirs(cfg.out, exist_ok=True)
    HEARTBEAT_LOG = os.path.join(cfg.out, "heartbeat.log")

    signal.signal(signal.SIGTERM, _sig)
    signal.signal(signal.SIGINT, _sig)

    hb(f"=== ip4knx endurance test starting "
       f"(targets={cfg.hosts} duration={cfg.duration:.0f}s interval={cfg.interval}s) ===")
    hb(f"output dir: {cfg.out}")

    threads = []
    for i, host in enumerate(cfg.hosts):
        label = f"gw{i}"
        t = threading.Thread(target=worker, args=(label, host, cfg), daemon=False)
        t.start()
        threads.append(t)
    try:
        for t in threads:
            while t.is_alive():
                t.join(timeout=10)
    except KeyboardInterrupt:
        stop_evt.set()
    hb("=== run complete ===")


if __name__ == "__main__":
    main()
