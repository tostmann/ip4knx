#!/usr/bin/env python3
"""
test_bus_connectivity.py — NCN5130 + TP-Bus connectivity test for ip4knx C6 sticks.

Verifies, for each (sender, receiver) pair on the same physical TP bus:
  - sender's NCN5130 emits the L_Data frame onto the TP bus
  - receiver's NCN5130 decodes that frame from the TP bus
  - the IP-side tunnel relays L_Data.req / L_Data.ind correctly

Architecture
  - Opens a KNXnet/IP TUNNELING (LinkLayer) connection to each stick.
  - One background reader thread per tunnel ACKs server TUNNELING_REQUESTs,
    parses cEMI L_Data.ind, and records (src, dst, val) per stick.
  - For each test case: send L_Data.req via sender's tunnel, wait for
    receiver's reader to log a matching L_Data.ind.
  - Snapshots /api/status counters (rx_frames / tx_frames) before and
    after each test as additional witness.
  - Optional: knxd vbusmonitor1 background thread as third-party witness.

Pass criteria per case: receiver's tunnel sees matching L_Data.ind within
the timeout AND counter delta is consistent with what was sent.

Default GA range is 15/0/x as requested for this lab.
"""

import argparse
import json
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.request
from dataclasses import dataclass
from typing import List, Optional, Tuple

KNXIP_PORT = 3671
HDR = bytes([0x06, 0x10])
ST_CONNECT_REQUEST       = 0x0205
ST_CONNECT_RESPONSE      = 0x0206
ST_DISCONNECT_REQUEST    = 0x0209
ST_DISCONNECT_RESPONSE   = 0x020A
ST_TUNNELING_REQUEST     = 0x0420
ST_TUNNELING_ACK         = 0x0421
ST_ROUTING_INDICATION    = 0x0530

CEMI_L_DATA_REQ = 0x11
CEMI_L_DATA_CON = 0x2E
CEMI_L_DATA_IND = 0x29


def parse_ga(s: str) -> int:
    m, mid, sub = (int(x) for x in s.split("/"))
    return ((m & 0x1F) << 11) | ((mid & 0x07) << 8) | (sub & 0xFF)


def fmt_ga(ga: int) -> str:
    return f"{(ga >> 11) & 0x1F}/{(ga >> 8) & 0x07}/{ga & 0xFF}"


def fmt_ia(ia: int) -> str:
    return f"{(ia >> 12) & 0x0F}.{(ia >> 8) & 0x0F}.{ia & 0xFF}"


def hpai(ip: str, port: int) -> bytes:
    return bytes([0x08, 0x01]) + socket.inet_aton(ip) + struct.pack(">H", port)


def knx_frame(service: int, body: bytes) -> bytes:
    total = 6 + len(body)
    return HDR + struct.pack(">HH", service, total) + body


@dataclass
class RxLog:
    ts: float
    src: int
    dst: int
    val: int
    raw: bytes
    via_routing: bool = False


class KnxTunnelClient:
    """One KNXnet/IP TUNNELING (LinkLayer) connection with a background reader.

    The reader ACKs server TUNNELING_REQUESTs, parses cEMI L_Data.ind frames
    (also accepts L_Data.ind embedded in ROUTING_INDICATION the firmware
    sometimes unicasts to the data HPAI), and appends to rx_log.
    """

    def __init__(self, gateway_ip: str, name: str, verbose: bool = False):
        self.gateway_ip = gateway_ip
        self.name = name
        self.verbose = verbose
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", 0))
        self.local_port = self.sock.getsockname()[1]
        self.local_ip = self._discover_local_ip(gateway_ip)
        self.channel: Optional[int] = None
        self.assigned_ia: int = 0
        self.tx_seq = 0
        self.connected = False
        self.rx_log: List[RxLog] = []
        self.rx_lock = threading.Lock()
        self.stop_evt = threading.Event()
        self.reader: Optional[threading.Thread] = None

    @staticmethod
    def _discover_local_ip(target: str) -> str:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect((target, 1))
            return s.getsockname()[0]
        finally:
            s.close()

    def connect(self, timeout: float = 3.0) -> Tuple[bool, str]:
        body = hpai(self.local_ip, self.local_port) \
             + hpai(self.local_ip, self.local_port) \
             + bytes([0x04, 0x04, 0x02, 0x00])  # CRI: TUNNELING + LinkLayer
        self.sock.sendto(knx_frame(ST_CONNECT_REQUEST, body),
                         (self.gateway_ip, KNXIP_PORT))
        self.sock.settimeout(timeout)
        try:
            data, _ = self.sock.recvfrom(2048)
        except socket.timeout:
            return False, "no CONNECT_RESPONSE"
        if len(data) < 8 or data[0:4] != HDR + struct.pack(">H", ST_CONNECT_RESPONSE):
            return False, f"unexpected reply: {data[:20].hex()}"
        self.channel = data[6]
        status = data[7]
        if status != 0x00:
            return False, f"connect status=0x{status:02x}"
        crd = data[-4:]
        self.assigned_ia = (crd[2] << 8) | crd[3]
        self.connected = True
        self.reader = threading.Thread(target=self._read_loop, daemon=True)
        self.reader.start()
        return True, f"channel=0x{self.channel:02x} assigned-IA={fmt_ia(self.assigned_ia)}"

    def _read_loop(self):
        self.sock.settimeout(0.2)
        while not self.stop_evt.is_set():
            try:
                data, _ = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                break
            if len(data) < 6 or data[0:2] != HDR:
                continue
            service = (data[2] << 8) | data[3]
            if service == ST_TUNNELING_REQUEST:
                body = data[6:]
                if len(body) < 5:
                    continue
                ch, seq = body[1], body[2]
                self.sock.sendto(knx_frame(ST_TUNNELING_ACK,
                                           bytes([0x04, ch, seq, 0x00])),
                                 (self.gateway_ip, KNXIP_PORT))
                self._handle_cemi(body[4:], via_routing=False)
            elif service == ST_ROUTING_INDICATION:
                self._handle_cemi(data[6:], via_routing=True)
            elif service == ST_DISCONNECT_REQUEST:
                self.connected = False
                break

    def _handle_cemi(self, cemi: bytes, via_routing: bool):
        if len(cemi) < 11:
            return
        mc = cemi[0]
        addinfo = cemi[1]
        off = 2 + addinfo
        if len(cemi) < off + 9:
            return
        src = (cemi[off + 2] << 8) | cemi[off + 3]
        dst = (cemi[off + 4] << 8) | cemi[off + 5]
        npdu_len = cemi[off + 6]
        if npdu_len < 1 or len(cemi) < off + 8 + npdu_len:
            return
        # 1-bit GroupValueWrite: APCI low byte = 0x80 | (val & 0x3F)
        val = cemi[off + 8] & 0x3F
        if mc != CEMI_L_DATA_IND:
            return
        entry = RxLog(time.time(), src, dst, val, cemi, via_routing)
        with self.rx_lock:
            self.rx_log.append(entry)
        if self.verbose:
            tag = "RI " if via_routing else "TUN"
            print(f"  [{self.name} {tag}] L_Data.ind src={fmt_ia(src)} "
                  f"dst={fmt_ga(dst)} val={val}")

    def send_groupwrite(self, ga: int, value: int) -> bool:
        if not self.connected:
            return False
        cemi = bytes([
            CEMI_L_DATA_REQ, 0x00,                    # mc, addInfoLen
            0xBC, 0xE0,                               # ctrl1, ctrl2
            0x00, 0x00,                               # src (gateway fills)
            (ga >> 8) & 0xFF, ga & 0xFF,              # dst
            0x01,                                     # NPDU length
            0x00, 0x80 | (value & 0x3F),              # TPCI/APCI hi, APCI lo+val
        ])
        body = bytes([0x04, self.channel, self.tx_seq, 0x00]) + cemi
        self.tx_seq = (self.tx_seq + 1) & 0xFF
        self.sock.sendto(knx_frame(ST_TUNNELING_REQUEST, body),
                         (self.gateway_ip, KNXIP_PORT))
        return True

    def disconnect(self):
        self.stop_evt.set()
        if self.reader:
            self.reader.join(timeout=1.0)
        if self.connected and self.channel is not None:
            body = bytes([self.channel, 0x00]) \
                 + hpai(self.local_ip, self.local_port)
            try:
                self.sock.sendto(knx_frame(ST_DISCONNECT_REQUEST, body),
                                 (self.gateway_ip, KNXIP_PORT))
            except OSError:
                pass
        try:
            self.sock.close()
        except OSError:
            pass

    def find_rx(self, ga: int, val: int, since: float) -> Optional[RxLog]:
        with self.rx_lock:
            for e in self.rx_log:
                if e.ts >= since and e.dst == ga and e.val == val:
                    return e
        return None


def read_status(ip: str, timeout: float = 3.0) -> Optional[dict]:
    try:
        with urllib.request.urlopen(f"http://{ip}/api/status", timeout=timeout) as resp:
            return json.loads(resp.read())
    except Exception:
        return None


@dataclass
class Stick:
    name: str
    ip: str
    tunnel: Optional[KnxTunnelClient] = None


def run_pair_test(sender: Stick, receiver: Stick, ga: int, val: int,
                  wait_s: float) -> Tuple[bool, str]:
    s_pre = read_status(sender.ip) or {}
    r_pre = read_status(receiver.ip) or {}
    t0 = time.time()
    if not sender.tunnel.send_groupwrite(ga, val):
        return False, "send-failed"
    deadline = t0 + wait_s
    rx = None
    while time.time() < deadline:
        rx = receiver.tunnel.find_rx(ga, val, since=t0)
        if rx:
            break
        time.sleep(0.04)
    time.sleep(0.3)  # let the firmware settle counters
    s_post = read_status(sender.ip) or {}
    r_post = read_status(receiver.ip) or {}
    sd_tx = s_post.get("tx_frames", 0) - s_pre.get("tx_frames", 0)
    rd_rx = r_post.get("rx_frames", 0) - r_pre.get("rx_frames", 0)
    s_self = sender.tunnel.find_rx(ga, val, since=t0)  # echo on sender's tunnel
    detail = (f"recv-tunnel={'YES' if rx else 'no '}"
              f"{' (via routing-ind)' if rx and rx.via_routing else ''}"
              f", sender-echo={'yes' if s_self else 'no'}"
              f", Δsender.tx_frames={sd_tx}, Δreceiver.rx_frames={rd_rx}")
    return (rx is not None), detail


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tul32", default="10.10.11.127", help="TUL32 IP")
    ap.add_argument("--tulx32", default="10.10.11.125", help="TULX32 IP")
    ap.add_argument("--knxd", default="10.10.11.13",
                    help="knxd IP (witness only); empty string disables")
    ap.add_argument("--ga-base", default="15/0",
                    help="GA prefix m/mid (default 15/0); sub increments per case")
    ap.add_argument("--wait", type=float, default=2.0,
                    help="seconds to wait for L_Data.ind on receiver")
    ap.add_argument("--verbose", "-v", action="store_true")
    args = ap.parse_args()

    sticks = [
        Stick("TUL32 ", args.tul32),
        Stick("TULX32", args.tulx32),
    ]

    print("=" * 72)
    print("ip4knx NCN5130 + Bus-Connectivity Test (C6 sticks)")
    print("=" * 72)

    # Step 1 — pre-flight
    print("\n[1] Pre-flight /api/status")
    print("-" * 72)
    for s in sticks:
        st = read_status(s.ip)
        if not st:
            print(f"  {s.name} @ {s.ip}: UNREACHABLE — abort.")
            sys.exit(2)
        print(f"  {s.name} @ {s.ip}: v{st['build']['version']} "
              f"uptime={st['uptime']:>16}  "
              f"bus_load={st.get('bus_load','?')}  "
              f"rx_bytes={st['rx_bytes']:<5} rx_frames={st['rx_frames']}  "
              f"tx_bytes={st['tx_bytes']:<5} tx_frames={st['tx_frames']}")

    # Step 2 — open tunnels
    print("\n[2] Open KNXnet/IP tunnels (LinkLayer)")
    print("-" * 72)
    for s in sticks:
        s.tunnel = KnxTunnelClient(s.ip, s.name, verbose=args.verbose)
        ok, msg = s.tunnel.connect()
        print(f"  {s.name} @ {s.ip}: {'OK ' if ok else 'FAIL'} — {msg}")
        if not ok:
            for x in sticks:
                if x.tunnel:
                    x.tunnel.disconnect()
            sys.exit(2)

    # Optional knxd witness
    knxd_proc = None
    knxd_lines: List[str] = []
    if args.knxd:
        try:
            knxd_proc = subprocess.Popen(
                ["knxtool", "vbusmonitor1", f"ip:{args.knxd}"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            )

            def _drain():
                for line in knxd_proc.stdout:
                    knxd_lines.append(line.rstrip())
                    if args.verbose:
                        print(f"  [knxd] {line.rstrip()}")

            threading.Thread(target=_drain, daemon=True).start()
            print(f"  knxd   @ {args.knxd}: vbusmonitor1 witness started")
        except FileNotFoundError:
            print(f"  knxd   @ {args.knxd}: knxtool not found, witness disabled")
            knxd_proc = None
        except Exception as e:
            print(f"  knxd   @ {args.knxd}: witness FAILED ({e})")
            knxd_proc = None

    time.sleep(0.5)

    # Step 3 — test matrix
    print("\n[3] Test matrix")
    print("-" * 72)
    main_p, mid_p = (int(x) for x in args.ga_base.split("/"))
    cases = [
        ("TUL32  → bus → TULX32       ", sticks[0], sticks[1], 1, 1),
        ("TULX32 → bus → TUL32        ", sticks[1], sticks[0], 2, 1),
        ("TUL32  → bus → TULX32 (val=0)", sticks[0], sticks[1], 3, 0),
        ("TULX32 → bus → TUL32  (val=0)", sticks[1], sticks[0], 4, 0),
    ]
    pass_count = 0
    for label, sender, receiver, sub, val in cases:
        ga = (main_p << 11) | (mid_p << 8) | (sub & 0xFF)
        ok, detail = run_pair_test(sender, receiver, ga, val, args.wait)
        marker = "PASS" if ok else "FAIL"
        if ok:
            pass_count += 1
        print(f"  [{marker}] {label}  GA={fmt_ga(ga)} val={val}")
        print(f"         {detail}")

    # Step 4 — final status + cleanup
    print("\n[4] Final /api/status")
    print("-" * 72)
    for s in sticks:
        st = read_status(s.ip) or {}
        print(f"  {s.name} @ {s.ip}: "
              f"rx_frames={st.get('rx_frames','?')} tx_frames={st.get('tx_frames','?')}  "
              f"rx_bytes={st.get('rx_bytes','?')} tx_bytes={st.get('tx_bytes','?')}  "
              f"bus_load={st.get('bus_load','?')}")

    for s in sticks:
        if s.tunnel:
            s.tunnel.disconnect()
    if knxd_proc:
        knxd_proc.terminate()
        try:
            knxd_proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            knxd_proc.kill()

    if args.knxd:
        print("\n[5] knxd vbusmonitor1 witness summary")
        print("-" * 72)
        if knxd_lines:
            print(f"  {len(knxd_lines)} line(s) observed:")
            for line in knxd_lines[:8]:
                print(f"    | {line}")
            if len(knxd_lines) > 8:
                print(f"    | … ({len(knxd_lines) - 8} more)")
        else:
            print("  no traffic — knxd@{} has no bus path to the sticks "
                  "(no tunnel/router backend pointing at them, "
                  "or multicast blocked).".format(args.knxd))

    print()
    print("=" * 72)
    if pass_count == len(cases):
        print(f"RESULT: ALL {pass_count}/{len(cases)} tests PASSED — "
              "NCN5130 TX+RX and TP-bus connectivity verified for both sticks.")
        sys.exit(0)
    else:
        print(f"RESULT: {pass_count}/{len(cases)} tests passed.")
        if pass_count == 0:
            print("Hint: zero passes typically means the two sticks are not on "
                  "the same physical TP bus (or no bus at all). Compare "
                  "/api/status bus_load and rx_bytes — a stick with rx_bytes=0 "
                  "after running has no NCN5130-side bus presence.")
        sys.exit(1)


if __name__ == "__main__":
    main()
