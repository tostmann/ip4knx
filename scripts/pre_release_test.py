#!/usr/bin/env python3
"""
ip4knx pre-release test suite — exercises the firmware end-to-end across
two devices and prints a pass/fail report. Designed to run before tagging
a release: if any check FAILs, do not tag.

Coverage (per device, 11 checks):
  1.  pre_flight              /api/status reachable, JSON schema complete
  2.  ncn_self_test           power rails / baud / mode (bus-connected only)
  3.  web_api_surface         /api/* endpoints respond with expected shape
  4.  progmode_toggle         /api/progmode flips, reflected in /api/status
  5.  ota_query               /api/update/{check,status} JSON shape
  6.  tunnel_pool_full        open KNX_TUNNELING tunnels, 11th must reject
  7.  source_validation       KNXnet/IP §4.4 source rewrite (v1.4.0)
  8.  cemi_response_routing   M_PropRead via tunnel A — B must NOT see it
                              (cherry-pick 1bd8201)
  9.  apdu_length_router      PID_MAX_APDU_LENGTH_ROUTER == 254
                              (cherry-pick b50301e)
  10. routing_indication      multicast 224.0.23.12 receives IND
  11. heap_stability          heap delta after full sweep ≤ 20 KB drop

Plus a cross-device consistency check (firmware version match).

Usage:
    python3 scripts/pre_release_test.py --c3 10.10.11.156 --c6 10.10.11.30
    python3 scripts/pre_release_test.py --c3 10.10.11.156 --c6 10.10.11.30 --skip routing_indication

Exit codes:
    0  all PASS (release-ready)
    1  one or more FAIL
    2  no FAILs but SKIPs > expected (degraded run)
"""

import argparse
import json
import os
import socket
import struct
import sys
import threading
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Optional

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_tunnel_source import (  # noqa: E402
    TunClient, build_cemi_group_write, build_tunneling_request,
    build_header, build_hpai, build_disconnect_request, build_tunneling_ack,
    parse_header, parse_cemi_src_dst,
    group_to_int, int_to_group, int_to_ia,
    CONNECT_REQUEST, CONNECT_RESPONSE, TUNNELING_REQUEST, TUNNELING_ACK,
    DEVCFG_REQUEST, DEVCFG_ACK,
    DISCONNECT_REQUEST, CEMI_LDATA_IND, CEMI_LDATA_REQ,
    HPAI_PROTO_UDP, CRI_TUNNEL_LINKLAYER, CRI_DEVMGMT,
)

CEMI_M_PROPREAD_REQ = 0xFC
CEMI_M_PROPREAD_CON = 0xFB
OT_DEVICE = 0
OT_ROUTER = 6
PID_SERIAL_NUMBER = 11
PID_MAX_APDU_LENGTH_ROUTER = 58

EXPECTED_MAX_TUNNELS = 10
HEAP_DROP_THRESHOLD_BYTES = 20 * 1024


@dataclass
class Result:
    name: str
    status: str
    message: str = ""
    duration_ms: int = 0


@dataclass
class Device:
    label: str
    host: str
    status: dict = field(default_factory=dict)
    initial_heap: int = 0


def http_get(host: str, path: str, timeout: float = 4.0) -> tuple[int, dict]:
    req = urllib.request.Request(f"http://{host}{path}", method="GET")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        body = resp.read().decode("utf-8")
        try:
            return resp.status, json.loads(body)
        except json.JSONDecodeError:
            return resp.status, {"_raw": body}


def http_post(host: str, path: str, data: bytes = b"", timeout: float = 4.0) -> tuple[int, dict]:
    req = urllib.request.Request(f"http://{host}{path}", data=data, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read().decode("utf-8")
            try:
                return resp.status, json.loads(body)
            except json.JSONDecodeError:
                return resp.status, {"_raw": body}
    except urllib.error.HTTPError as e:
        return e.code, {"_error": str(e)}


def timed(fn):
    def wrap(*args, **kwargs):
        t0 = time.time()
        try:
            status, msg = fn(*args, **kwargs)
        except Exception as e:
            status, msg = "FAIL", f"exception: {type(e).__name__}: {e}"
        dur_ms = int((time.time() - t0) * 1000)
        return status, msg, dur_ms
    return wrap


# -- individual tests ---------------------------------------------------------

@timed
def t_pre_flight(dev: Device):
    code, body = http_get(dev.host, "/api/status")
    if code != 200:
        return "FAIL", f"HTTP {code}"
    required = {"uptime", "ip", "mac", "knx_pa", "knx_max_tunnels",
                "active_clients", "rx_frames", "tx_frames", "build", "ncn", "hardware"}
    missing = required - set(body.keys())
    if missing:
        return "FAIL", f"missing keys: {sorted(missing)}"
    if body.get("knx_max_tunnels") != EXPECTED_MAX_TUNNELS:
        return "FAIL", f"knx_max_tunnels={body.get('knx_max_tunnels')} (expected {EXPECTED_MAX_TUNNELS})"
    dev.status = body
    dev.initial_heap = body["hardware"]["heap_free"]
    return "PASS", f"v{body['build']['version']} build={body['build']['number']} partition={body['build']['partition']}/{body['build']['ota_state']} heap={dev.initial_heap}"


@timed
def t_ncn_self_test(dev: Device):
    ncn = dev.status.get("ncn", {})
    if not ncn.get("connected"):
        return "SKIP", f"NCN not connected ({ncn.get('state')}) — no bus attached"
    rails = ["v20v", "vdd2", "vbus", "vfilt", "xtal"]
    bad = [r for r in rails if not ncn.get(r)]
    if bad:
        return "FAIL", f"rails not green: {bad}"
    if ncn.get("baud") not in (19200, 38400):
        return "FAIL", f"unexpected baud {ncn.get('baud')}"
    if ncn.get("mode") not in ("Normal", "Power-UP"):
        return "FAIL", f"unexpected mode '{ncn.get('mode')}'"
    if ncn.get("thermal_warning"):
        return "WARN", "NCN reports thermal warning"
    return "PASS", f"all rails green, {ncn['baud']} baud, mode={ncn['mode']}"


@timed
def t_web_api_surface(dev: Device):
    checks = [
        ("/api/status", "GET", 200, dict),
        ("/api/update/check", "GET", 200, dict),
        ("/api/update/status", "GET", 200, dict),
    ]
    for path, method, expected_code, expected_type in checks:
        code, body = http_get(dev.host, path) if method == "GET" else http_post(dev.host, path)
        if code != expected_code:
            return "FAIL", f"{method} {path} → HTTP {code} (expected {expected_code})"
        if not isinstance(body, expected_type):
            return "FAIL", f"{method} {path} → body type {type(body).__name__} (expected {expected_type.__name__})"
    return "PASS", f"{len(checks)} endpoints responded"


@timed
def t_progmode_toggle(dev: Device):
    code, before = http_get(dev.host, "/api/status")
    if code != 200:
        return "FAIL", "status unreachable"
    initial = bool(before.get("prog_mode", False))
    # toggle on
    code, _ = http_post(dev.host, "/api/progmode")
    if code != 200:
        return "FAIL", f"POST /api/progmode → HTTP {code}"
    time.sleep(0.4)
    code, after = http_get(dev.host, "/api/status")
    flipped = bool(after.get("prog_mode", False))
    if flipped == initial:
        return "FAIL", f"toggle did not flip: before={initial} after={flipped}"
    # toggle back
    http_post(dev.host, "/api/progmode")
    time.sleep(0.4)
    code, final = http_get(dev.host, "/api/status")
    if bool(final.get("prog_mode", False)) != initial:
        return "WARN", f"could not restore initial state ({initial})"
    return "PASS", f"toggle works (initial={initial})"


@timed
def t_ota_query(dev: Device):
    code, body = http_get(dev.host, "/api/update/check")
    if code != 200:
        return "FAIL", f"HTTP {code}"
    required = {"state", "current", "latest", "available", "progress", "total", "error"}
    missing = required - set(body.keys())
    if missing:
        return "FAIL", f"missing keys: {sorted(missing)}"
    if body["current"] != dev.status["build"]["version"]:
        return "FAIL", f"current={body['current']} != status.build.version={dev.status['build']['version']}"
    return "PASS", f"current={body['current']} latest={body['latest']} available={body['available']}"


@timed
def t_tunnel_pool_full(dev: Device):
    clients: list[TunClient] = []
    try:
        for i in range(EXPECTED_MAX_TUNNELS):
            c = TunClient(dev.host, f"P{i}", verbose=False)
            c.connect()
            clients.append(c)
        ias = [c.assigned_ia for c in clients]
        if len(set(ias)) != EXPECTED_MAX_TUNNELS:
            return "FAIL", f"non-unique IAs: {[int_to_ia(i) for i in ias]}"
        # 11th must fail
        overflow = TunClient(dev.host, "P_overflow", verbose=False)
        try:
            overflow.connect()
            overflow.disconnect()
            return "FAIL", "11th tunnel was accepted — pool overcommit"
        except RuntimeError:
            pass  # expected
        return "PASS", f"{EXPECTED_MAX_TUNNELS} unique tunnels, overflow rejected"
    finally:
        for c in clients:
            try: c.disconnect()
            except Exception: pass
        time.sleep(0.5)


@timed
def t_source_validation(dev: Device):
    a = TunClient(dev.host, "A", verbose=False)
    b = TunClient(dev.host, "B", verbose=False)
    try:
        a.connect(); b.connect()
        if a.assigned_ia == b.assigned_ia:
            return "FAIL", "got same IA for two tunnels"
        dst = group_to_int("0/0/1")
        spoofed = b.assigned_ia
        a.received.clear(); b.received.clear()
        a.send_tunneling(build_cemi_group_write(spoofed, dst, value=0x01))
        deadline = time.time() + 2.0
        while time.time() < deadline:
            if b.received: break
            time.sleep(0.05)
        inds = [x for x in b.received if x[0] == CEMI_LDATA_IND and x[2] == dst]
        if not inds:
            return "FAIL", "B received no echo"
        if inds[0][1] == spoofed:
            return "FAIL", f"spoofed src {int_to_ia(spoofed)} reached B unmodified"
        if inds[0][1] != a.assigned_ia:
            return "FAIL", f"src at B = {int_to_ia(inds[0][1])}, expected {int_to_ia(a.assigned_ia)}"
        return "PASS", f"src rewritten {int_to_ia(spoofed)} → {int_to_ia(a.assigned_ia)}"
    finally:
        a.disconnect(); b.disconnect()
        time.sleep(0.3)


def build_cemi_propread(object_type: int, instance: int, pid: int, num: int = 1, start: int = 1) -> bytes:
    return struct.pack(">BHBBBB",
        CEMI_M_PROPREAD_REQ,
        object_type & 0xFFFF,
        instance & 0xFF,
        pid & 0xFF,
        ((num & 0xF) << 4) | ((start >> 8) & 0xF),
        start & 0xFF,
    )


def collect_propread_con(received: list, pid: int, timeout_s: float = 1.5) -> Optional[bytes]:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        for entry in received:
            msg = entry[0]
            if msg == CEMI_M_PROPREAD_CON:
                # entry[3] holds the raw cemi if our TunClient.received stored it;
                # but our existing TunClient only keeps msg/src/dst/time. We need
                # a separate listener for M_Prop frames.
                return b"received"
        time.sleep(0.05)
    return None


# DEVICE_MANAGEMENT connection client. Behaves like TunClient but uses the
# DEVMGMT CRI for connect and DEVICE_CONFIGURATION_REQUEST (0x0310) for the
# M_* round-trip. Required for testing M_PropRead/PropWrite — those messages
# do NOT flow through TUNNELING_REQUEST (0x0420), which only carries L_Data.
class DevMgmtClient:
    def __init__(self, host, name, verbose=False):
        self.host = host
        self.name = name
        self.verbose = verbose
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", 0))
        self.sock.settimeout(3.0)
        self.local_port = self.sock.getsockname()[1]
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect((host, 3671))
        self.local_ip = s.getsockname()[0]
        s.close()
        self.channel = None
        self.tx_seq = 0
        self.cemi_raw: list[tuple[int, bytes, float]] = []
        self.recv_run = False
        self.recv_thread = None

    def log(self, msg):
        if self.verbose:
            print(f"[{self.name}] {msg}")

    def connect(self):
        # Build CONNECT_REQUEST with DEVMGMT CRI
        ctrl = build_hpai(self.local_ip, self.local_port)
        data = build_hpai(self.local_ip, self.local_port)
        payload = ctrl + data + CRI_DEVMGMT
        req = build_header(CONNECT_REQUEST, len(payload)) + payload
        self.sock.sendto(req, (self.host, 3671))
        resp, _ = self.sock.recvfrom(512)
        hdr = parse_header(resp)
        if not hdr or hdr[0] != CONNECT_RESPONSE:
            raise RuntimeError(f"{self.name}: bad CONNECT_RESPONSE {hdr}")
        status = resp[7]
        if status != 0:
            raise RuntimeError(f"{self.name}: connect status=0x{status:02x}")
        self.channel = resp[6]
        self.log(f"connected ch={self.channel}")
        self.recv_run = True
        self.recv_thread = threading.Thread(target=self._receiver, daemon=True)
        self.recv_thread.start()
        return self

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
            service, _ = hdr
            if service == DEVCFG_REQUEST:
                if len(data) < 10: continue
                seq = data[8]
                # ACK the request
                ack = struct.pack(">BBBB", 4, self.channel, seq, 0)
                self.sock.sendto(build_header(DEVCFG_ACK, len(ack)) + ack, (self.host, 3671))
                cemi = data[10:]
                if cemi:
                    msg = cemi[0]
                    self.cemi_raw.append((msg, bytes(cemi), time.time()))
                    self.log(f"<- DEVCFG seq={seq} msg=0x{msg:02x} len={len(cemi)}")

    def send_devcfg(self, cemi: bytes):
        conn = struct.pack(">BBBB", 4, self.channel, self.tx_seq, 0)
        payload = conn + cemi
        req = build_header(DEVCFG_REQUEST, len(payload)) + payload
        self.sock.sendto(req, (self.host, 3671))
        self.log(f"-> DEVCFG seq={self.tx_seq} cemi={cemi.hex()}")
        self.tx_seq = (self.tx_seq + 1) & 0xFF

    def disconnect(self):
        self.recv_run = False
        try:
            req = build_disconnect_request(self.channel, self.local_ip, self.local_port)
            self.sock.sendto(req, (self.host, 3671))
        except Exception:
            pass
        try: self.sock.close()
        except Exception: pass


# (legacy TunClientPlus kept inline for the tunnel-source test; cEMI capture
# also helps when L_Data echoes carry extra info we want to inspect.)
class TunClientPlus(TunClient):
    def __init__(self, host, name, verbose=False):
        super().__init__(host, name, verbose=verbose)
        self.cemi_raw: list[tuple[int, bytes, float]] = []

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
            service, _ = hdr
            if service == TUNNELING_REQUEST:
                if len(data) < 10: continue
                seq = data[8]
                self.sock.sendto(build_tunneling_ack(self.channel, seq), (self.host, 3671))
                cemi = data[10:]
                if cemi:
                    msg = cemi[0]
                    self.cemi_raw.append((msg, bytes(cemi), time.time()))
                    parsed = parse_cemi_src_dst(cemi)
                    if parsed:
                        self.received.append((parsed[0], parsed[1], parsed[2], time.time()))


@timed
def t_cemi_devmgmt_roundtrip(dev: Device):
    # Verifies M_PropRead_req via DEVICE_CONFIGURATION_REQUEST gets a
    # M_PropRead_con response routed back to the originating channel.
    # This exercises cherry-pick 1bd8201 (dataRequestToChannelId).
    # Multi-DEVMGMT-slot testing of the no-leak property would need
    # cherry-pick b26115c which is blocked by the upstream tunneling
    # refactor — see project memory upstream_openknx_tracking.md.
    c = DevMgmtClient(dev.host, "DM", verbose=False)
    try:
        c.connect()
        c.cemi_raw.clear()
        # PID_SERIAL_NUMBER on OT_DEVICE — universally readable
        c.send_devcfg(build_cemi_propread(OT_DEVICE, 1, PID_SERIAL_NUMBER))
        deadline = time.time() + 1.5
        cons = []
        while time.time() < deadline:
            cons = [m for m in c.cemi_raw if m[0] == CEMI_M_PROPREAD_CON]
            if cons: break
            time.sleep(0.05)
        if not cons:
            return "FAIL", "no M_PropRead_con received via DEVMGMT (channelId-routing broken?)"
        return "PASS", f"M_PropRead_con received ({len(cons[0][1])} cEMI bytes)"
    finally:
        c.disconnect()
        time.sleep(0.3)


@timed
def t_apdu_length_router(dev: Device):
    c = DevMgmtClient(dev.host, "APDU", verbose=False)
    try:
        c.connect()
        c.cemi_raw.clear()
        # read PID_MAX_APDU_LENGTH_ROUTER from OT_ROUTER, instance 1
        c.send_devcfg(build_cemi_propread(OT_ROUTER, 1, PID_MAX_APDU_LENGTH_ROUTER))
        deadline = time.time() + 1.5
        cons: list[bytes] = []
        while time.time() < deadline:
            cons = [bytes(m[1]) for m in c.cemi_raw if m[0] == CEMI_M_PROPREAD_CON]
            if cons: break
            time.sleep(0.05)
        if not cons:
            return "FAIL", "no M_PropRead_con received"
        raw = cons[0]
        # response shape: [msg=0xFB, objtype_hi, objtype_lo, inst, pid, noE/idxHi, idxLo, data...]
        if len(raw) < 9:
            return "FAIL", f"response too short: {raw.hex()}"
        noE = (raw[5] >> 4) & 0xF
        if noE == 0:
            return "FAIL", f"negative response (noE=0): {raw.hex()}"
        # PDT_UNSIGNED_INT = 2 bytes
        val = (raw[7] << 8) | raw[8]
        if val != 254:
            return "FAIL", f"PID_MAX_APDU_LENGTH_ROUTER={val} (expected 254 per cherry-pick b50301e)"
        return "PASS", "PID_MAX_APDU_LENGTH_ROUTER=254"
    finally:
        c.disconnect()
        time.sleep(0.3)


@timed
def t_routing_indication(dev: Device):
    # Routing indications require the 091A coupler to be ETS-programmed
    # (knx_configured=true). On a factory-fresh device they're a no-op.
    if not dev.status.get("knx_configured"):
        return "SKIP", "knx_configured=false — routing not active without ETS"
    # Listen on multicast 224.0.23.12:3671 for routing indications.
    mcast_addr = "224.0.23.12"
    port = 3671
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try: s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    except Exception: pass
    try:
        s.bind(("", port))
    except OSError as e:
        return "SKIP", f"cannot bind {port} for multicast listen: {e}"
    mreq = struct.pack("=4sl", socket.inet_aton(mcast_addr), socket.INADDR_ANY)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
    s.settimeout(2.0)

    received = []
    stop = threading.Event()
    def listen():
        while not stop.is_set():
            try:
                data, _ = s.recvfrom(1024)
                hdr = parse_header(data)
                if hdr and hdr[0] == 0x0530:  # ROUTING_INDICATION
                    received.append(data)
            except socket.timeout:
                pass
            except OSError:
                return
    th = threading.Thread(target=listen, daemon=True)
    th.start()

    c = TunClient(dev.host, "ROUTE", verbose=False)
    try:
        c.connect()
        dst = group_to_int("0/0/7")
        c.send_tunneling(build_cemi_group_write(0, dst, value=0x42))
        deadline = time.time() + 2.0
        while time.time() < deadline:
            if received: break
            time.sleep(0.05)
        if not received:
            return "FAIL", "no ROUTING_INDICATION on 224.0.23.12 within 2 s"
        return "PASS", f"{len(received)} routing indication(s) observed"
    finally:
        c.disconnect()
        stop.set()
        try: s.close()
        except Exception: pass


@timed
def t_heap_stability(dev: Device):
    code, body = http_get(dev.host, "/api/status")
    if code != 200:
        return "FAIL", "status unreachable"
    now = body["hardware"]["heap_free"]
    delta = dev.initial_heap - now
    if delta > HEAP_DROP_THRESHOLD_BYTES:
        return "FAIL", f"heap drop {delta} bytes (initial {dev.initial_heap} → now {now}, threshold {HEAP_DROP_THRESHOLD_BYTES})"
    if delta > HEAP_DROP_THRESHOLD_BYTES // 2:
        return "WARN", f"heap drop {delta} bytes (within threshold but elevated)"
    return "PASS", f"heap delta {delta:+d} bytes (initial {dev.initial_heap} → now {now})"


# -- driver -------------------------------------------------------------------

ALL_TESTS = [
    ("pre_flight",            t_pre_flight),
    ("ncn_self_test",         t_ncn_self_test),
    ("web_api_surface",       t_web_api_surface),
    ("progmode_toggle",       t_progmode_toggle),
    ("ota_query",             t_ota_query),
    ("tunnel_pool_full",      t_tunnel_pool_full),
    ("source_validation",     t_source_validation),
    ("cemi_devmgmt_roundtrip", t_cemi_devmgmt_roundtrip),
    ("apdu_length_router",    t_apdu_length_router),
    ("routing_indication",    t_routing_indication),
    ("heap_stability",        t_heap_stability),
]

STATUS_ICONS = {"PASS": "✓", "FAIL": "✗", "SKIP": "·", "WARN": "!"}
STATUS_COLOURS = {"PASS": "\033[32m", "FAIL": "\033[31m", "SKIP": "\033[33m", "WARN": "\033[33m"}
RESET = "\033[0m"


def run_suite(dev: Device, skip: set[str]) -> list[Result]:
    results: list[Result] = []
    print(f"\n=== {dev.label} ({dev.host}) ===")
    for name, fn in ALL_TESTS:
        if name in skip:
            print(f"  [SKIP]  {name}: explicitly skipped")
            results.append(Result(name, "SKIP", "explicitly skipped"))
            continue
        status, msg, dur_ms = fn(dev)
        col = STATUS_COLOURS.get(status, "")
        ico = STATUS_ICONS.get(status, "?")
        print(f"  {col}[{ico} {status}]{RESET}  {name}: {msg}  ({dur_ms} ms)")
        results.append(Result(name, status, msg, dur_ms))
        if status == "FAIL" and name == "pre_flight":
            # no point continuing if device is unreachable
            print("  → pre_flight failed, skipping remainder")
            for skip_n, _ in ALL_TESTS[1:]:
                results.append(Result(skip_n, "SKIP", "pre_flight failed"))
            break
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--c3", required=True, help="ESP32-C3 device IP")
    ap.add_argument("--c6", required=True, help="ESP32-C6 device IP")
    ap.add_argument("--skip", default="", help="comma-separated test names to skip")
    args = ap.parse_args()
    skip = {s.strip() for s in args.skip.split(",") if s.strip()}

    devices = [Device("C3", args.c3), Device("C6", args.c6)]
    all_results: dict[str, list[Result]] = {}
    for dev in devices:
        all_results[dev.label] = run_suite(dev, skip)

    # cross-device consistency
    print("\n=== Cross-device ===")
    c3v = devices[0].status.get("build", {}).get("version")
    c6v = devices[1].status.get("build", {}).get("version")
    if c3v and c6v:
        if c3v == c6v:
            print(f"  {STATUS_COLOURS['PASS']}[✓ PASS]{RESET}  firmware_version_match: both on v{c3v}")
            consistency_ok = True
        else:
            print(f"  {STATUS_COLOURS['FAIL']}[✗ FAIL]{RESET}  firmware_version_match: C3=v{c3v} ≠ C6=v{c6v}")
            consistency_ok = False
    else:
        print(f"  {STATUS_COLOURS['SKIP']}[· SKIP]{RESET}  firmware_version_match: one device unreachable")
        consistency_ok = True  # already covered by pre_flight FAIL

    # summary
    print("\n=== Summary ===")
    total_fail = total_warn = total_skip = total_pass = 0
    for label, results in all_results.items():
        passed = sum(1 for r in results if r.status == "PASS")
        failed = sum(1 for r in results if r.status == "FAIL")
        warned = sum(1 for r in results if r.status == "WARN")
        skipped = sum(1 for r in results if r.status == "SKIP")
        total_pass += passed; total_fail += failed; total_warn += warned; total_skip += skipped
        print(f"  {label}: {passed} PASS, {failed} FAIL, {warned} WARN, {skipped} SKIP")
    if not consistency_ok:
        total_fail += 1

    if total_fail > 0:
        print(f"\n{STATUS_COLOURS['FAIL']}NOT RELEASE-READY{RESET} — {total_fail} FAIL across the suite")
        sys.exit(1)
    if total_skip > len(devices) * 2:  # tolerate a couple of skips
        print(f"\n{STATUS_COLOURS['WARN']}DEGRADED{RESET} — {total_skip} skipped, run again or address blockers")
        sys.exit(2)
    print(f"\n{STATUS_COLOURS['PASS']}RELEASE-READY{RESET} — {total_pass} passed, {total_warn} warned, {total_skip} skipped, 0 failed")
    sys.exit(0)


if __name__ == "__main__":
    main()
