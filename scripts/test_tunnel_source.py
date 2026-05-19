#!/usr/bin/env python3
"""
Regression test for KNXnet/IP tunnel source-address validation.

Opens two parallel TUNNELING connections (A and B) to the gateway, then has
client A send a TUNNELING_REQUEST with the cEMI source address spoofed to
B's assigned individual address. The gateway must rewrite the source to A's
assigned IA before broadcasting (KNXnet/IP Core §4.4, mirroring the
MDT / Weinzierl / Gira behavior).

PASS criterion:
  - A and B each receive the echo with src == IA_A
  - The spoofed source IA_B never appears on the wire

FAIL criteria:
  - Spoofed src IA_B is broadcast unchanged
  - Either connection fails to be established (e.g. tunnel pool exhausted)
  - No echo is observed within the timeout window

Usage:
    python3 test_tunnel_source.py --host 10.10.11.30
    python3 test_tunnel_source.py --host 10.10.11.30 --group 1/2/3 --verbose
"""

import argparse
import socket
import struct
import sys
import threading
import time

CONNECT_REQUEST    = 0x0205
CONNECT_RESPONSE   = 0x0206
CONNECTIONSTATE_REQ  = 0x0207
CONNECTIONSTATE_RESP = 0x0208
DISCONNECT_REQUEST = 0x0209
TUNNELING_REQUEST  = 0x0420
TUNNELING_ACK      = 0x0421

CRI_TUNNEL_LINKLAYER = bytes([0x04, 0x04, 0x02, 0x00])
HPAI_PROTO_UDP = 0x01

CEMI_LDATA_REQ = 0x11
CEMI_LDATA_IND = 0x29
CEMI_LDATA_CON = 0x2E


def ia_to_int(s):
    a, b, c = (int(x) for x in s.replace("/", ".").split("."))
    return (a << 12) | (b << 8) | c


def int_to_ia(v):
    return f"{(v >> 12) & 0xF}.{(v >> 8) & 0xF}.{v & 0xFF}"


def group_to_int(s):
    parts = s.split("/")
    if len(parts) == 3:
        a, b, c = (int(x) for x in parts)
        return (a << 11) | (b << 8) | c
    a, b = (int(x) for x in parts)
    return (a << 11) | b


def int_to_group(v):
    return f"{(v >> 11) & 0x1F}/{(v >> 8) & 0x7}/{v & 0xFF}"


def build_hpai(ip, port):
    return struct.pack(">BB", 8, HPAI_PROTO_UDP) + socket.inet_aton(ip) + struct.pack(">H", port)


def build_header(service, payload_len):
    total = 6 + payload_len
    return struct.pack(">BBHH", 0x06, 0x10, service, total)


def build_connect_request(local_ip, local_port):
    ctrl = build_hpai(local_ip, local_port)
    data = build_hpai(local_ip, local_port)
    payload = ctrl + data + CRI_TUNNEL_LINKLAYER
    return build_header(CONNECT_REQUEST, len(payload)) + payload


def build_disconnect_request(channel, local_ip, local_port):
    payload = struct.pack(">BB", channel, 0) + build_hpai(local_ip, local_port)
    return build_header(DISCONNECT_REQUEST, len(payload)) + payload


def build_connectionstate_req(channel, local_ip, local_port):
    payload = struct.pack(">BB", channel, 0) + build_hpai(local_ip, local_port)
    return build_header(CONNECTIONSTATE_REQ, len(payload)) + payload


def build_tunneling_request(channel, seq, cemi):
    conn = struct.pack(">BBBB", 4, channel, seq, 0)
    payload = conn + cemi
    return build_header(TUNNELING_REQUEST, len(payload)) + payload


def build_tunneling_ack(channel, seq):
    conn = struct.pack(">BBBB", 4, channel, seq, 0)
    return build_header(TUNNELING_ACK, len(conn)) + conn


def build_cemi_group_write(src_ia, dst_ga, value=0x01):
    return struct.pack(">BBBBHHBBB",
                       CEMI_LDATA_REQ,
                       0x00,
                       0xBC,
                       0xE0,
                       src_ia,
                       dst_ga,
                       1,
                       0x00,
                       0x80 | (value & 0x3F))


def parse_header(buf):
    if len(buf) < 6 or buf[0] != 0x06 or buf[1] != 0x10:
        return None
    service = (buf[2] << 8) | buf[3]
    total = (buf[4] << 8) | buf[5]
    return service, total


def parse_cemi_src_dst(cemi):
    if len(cemi) < 10:
        return None
    msg = cemi[0]
    addl = cemi[1]
    off = 2 + addl
    if len(cemi) < off + 8:
        return None
    src = (cemi[off + 2] << 8) | cemi[off + 3]
    dst = (cemi[off + 4] << 8) | cemi[off + 5]
    return msg, src, dst


class TunClient:
    def __init__(self, host, name, verbose=False):
        self.host = host
        self.name = name
        self.verbose = verbose
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", 0))
        self.sock.settimeout(3.0)
        sn = self.sock.getsockname()
        self.local_ip = self._discover_local_ip(host)
        self.local_port = sn[1]
        self.channel = None
        self.assigned_ia = None
        self.tx_seq = 0
        self.rx_seq_expected = 0
        self.received = []
        self.recv_thread = None
        self.recv_run = False

    def _discover_local_ip(self, peer):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect((peer, 3671))
            return s.getsockname()[0]
        finally:
            s.close()

    def log(self, msg):
        if self.verbose:
            print(f"[{self.name}] {msg}")

    def connect(self):
        req = build_connect_request(self.local_ip, self.local_port)
        self.sock.sendto(req, (self.host, 3671))
        self.log(f"-> CONNECT_REQUEST")
        data, _ = self.sock.recvfrom(512)
        hdr = parse_header(data)
        if not hdr or hdr[0] != CONNECT_RESPONSE:
            raise RuntimeError(f"{self.name}: unexpected response {hdr}")
        channel = data[6]
        status = data[7]
        if status != 0:
            raise RuntimeError(f"{self.name}: connect failed status=0x{status:02x}")
        self.channel = channel
        crd_off = 6 + 1 + 1 + 8
        ia_hi = data[crd_off + 2]
        ia_lo = data[crd_off + 3]
        self.assigned_ia = (ia_hi << 8) | ia_lo
        self.log(f"<- CONNECT_RESPONSE ch={channel} IA={int_to_ia(self.assigned_ia)}")
        self._start_receiver()
        return self

    def _start_receiver(self):
        self.recv_run = True
        self.recv_thread = threading.Thread(target=self._receiver, daemon=True)
        self.recv_thread.start()

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
                if len(data) < 10:
                    continue
                seq = data[8]
                ack = build_tunneling_ack(self.channel, seq)
                self.sock.sendto(ack, (self.host, 3671))
                cemi = data[10:]
                parsed = parse_cemi_src_dst(cemi)
                if parsed:
                    msg, src, dst = parsed
                    self.received.append((msg, src, dst, time.time()))
                    self.log(f"<- TUN.req seq={seq} msg=0x{msg:02x} src={int_to_ia(src)} dst={int_to_group(dst)}")
            elif service == TUNNELING_ACK:
                pass

    def send_tunneling(self, cemi):
        req = build_tunneling_request(self.channel, self.tx_seq, cemi)
        self.sock.sendto(req, (self.host, 3671))
        self.log(f"-> TUN.req seq={self.tx_seq} cemi={cemi.hex()}")
        self.tx_seq = (self.tx_seq + 1) & 0xFF

    def disconnect(self):
        self.recv_run = False
        try:
            req = build_disconnect_request(self.channel, self.local_ip, self.local_port)
            self.sock.sendto(req, (self.host, 3671))
        except Exception:
            pass
        try:
            self.sock.close()
        except Exception:
            pass


def run(host, group_ga, verbose):
    print(f"[*] connecting two tunnels to {host}:3671 ...")
    a = TunClient(host, "A", verbose=verbose).connect()
    b = TunClient(host, "B", verbose=verbose).connect()
    print(f"    A: ch={a.channel} IA={int_to_ia(a.assigned_ia)}")
    print(f"    B: ch={b.channel} IA={int_to_ia(b.assigned_ia)}")

    if a.assigned_ia == b.assigned_ia:
        print("[!] FAIL: both tunnels got the same IA (gateway bug — IA pool exhausted?)")
        a.disconnect(); b.disconnect()
        return 2

    dst = group_to_int(group_ga)
    spoofed_src = b.assigned_ia
    expected_src = a.assigned_ia

    cemi = build_cemi_group_write(spoofed_src, dst, value=0x01)
    print(f"[*] A sends frame with SPOOFED src={int_to_ia(spoofed_src)} (B's IA) → dst={int_to_group(dst)}")
    a.received.clear()
    b.received.clear()
    a.send_tunneling(cemi)

    # Wait for echoes; the gateway loops the frame back to A and to B.
    deadline = time.time() + 2.0
    while time.time() < deadline:
        if a.received and b.received:
            break
        time.sleep(0.05)

    def first_ind(received):
        for msg, src, d, _ in received:
            if msg == CEMI_LDATA_IND and d == dst:
                return src
        return None

    src_at_a = first_ind(a.received)
    src_at_b = first_ind(b.received)

    print(f"[*] echoes observed:")
    print(f"      at A: src={int_to_ia(src_at_a) if src_at_a is not None else 'NONE'}")
    print(f"      at B: src={int_to_ia(src_at_b) if src_at_b is not None else 'NONE'}")

    verdict = 0
    if src_at_b is None:
        print(f"[!] FAIL: B did not receive any echo (routing path broken?)")
        verdict = 2
    elif src_at_b == spoofed_src:
        print(f"[!] FAIL: spoofed src reached B unmodified — gateway is NOT rewriting")
        verdict = 1
    elif src_at_b == expected_src:
        print(f"[OK] gateway rewrote src to A's assigned IA ({int_to_ia(expected_src)})")
    else:
        print(f"[!] FAIL: src at B is {int_to_ia(src_at_b)}, expected {int_to_ia(expected_src)}")
        verdict = 1

    a.disconnect()
    b.disconnect()
    return verdict


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True, help="gateway IP (e.g. 10.10.11.30)")
    ap.add_argument("--group", default="0/0/1", help="group address to write to (default 0/0/1)")
    ap.add_argument("--verbose", "-v", action="store_true", help="trace packets")
    args = ap.parse_args()

    try:
        sys.exit(run(args.host, args.group, args.verbose))
    except KeyboardInterrupt:
        print("\n[!] interrupted")
        sys.exit(130)


if __name__ == "__main__":
    main()
