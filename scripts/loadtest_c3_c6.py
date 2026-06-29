#!/usr/bin/env python3
"""
ip4knx C3-vs-C6 release smoke-load test.

Release condition (decision 2026-06-09): every ip4knx release must be preceded
by a passing concurrent-tunnel smoke test on BOTH chip families — ESP32-C3 and
ESP32-C6 — running the exact firmware version being released. This catches the
class of bug where one chip's build is functionally broken (won't tunnel,
resets under a couple of concurrent connections, drops frames) while the other
is fine. It is deliberately a *smoke* gate, not a sustained 10-slot load run:
a few concurrent tunnels, one round-trip, no crash, on each chip.

Per device (C3 and C6):
  1. /api/status reachable; record version, build, mac, uptime
  2. open N concurrent tunnels (default 3); all must connect with unique IAs
  3. round-trip: a group-write from tunnel[0] must echo to another tunnel
     (proves the gateway actually moves frames; works factory-fresh, no ETS)
  4. disconnect all
  5. /api/status still reachable AND uptime did not go backwards
     (the device did not reset/crash during the test)

Cross-device: both chips report the SAME firmware version, and that version
matches the release version (src/version.h FW_VERSION_STRING unless --version
is given). This ties the pass artifact to the exact firmware being shipped.

On full pass it writes the gate artifact (atomic + fsync — the tree lives on
NFS) consumed by scripts/release.sh and .git/hooks/pre-push:
    tul-knx-gateway/.loadtest_pass.json      (gitignored — holds lab MAC/IP)

Usage:
    python3 scripts/loadtest_c3_c6.py --c3 10.10.11.156 --c6 10.10.11.30
    python3 scripts/loadtest_c3_c6.py --c3 ... --c6 ... --tunnels 3
    python3 scripts/loadtest_c3_c6.py --c3 ... --c6 ... --no-write   # report only

Exit codes:
    0  both chips PASS — artifact written (unless --no-write)
    1  one or more checks FAILed — no artifact written
"""

import argparse
import json
import os
import re
import sys
import time
import urllib.request
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_tunnel_source import (  # noqa: E402
    TunClient, build_cemi_group_write, group_to_int, int_to_ia,
    CEMI_LDATA_IND,
)

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION_H = os.path.join(PROJECT_DIR, "tul-knx-gateway", "src", "version.h")
ARTIFACT = os.path.join(PROJECT_DIR, "tul-knx-gateway", ".loadtest_pass.json")

DEFAULT_TUNNELS = 3
ROUNDTRIP_GA = "0/0/1"
ROUNDTRIP_TIMEOUT_S = 2.0

GREEN = "\033[32m"; RED = "\033[31m"; YEL = "\033[33m"; RST = "\033[0m"


def read_release_version() -> str:
    """FW_VERSION_STRING from the generated header — the version being shipped."""
    try:
        with open(VERSION_H) as f:
            for line in f:
                m = re.search(r'FW_VERSION_STRING\s+"([^"]+)"', line)
                if m:
                    return m.group(1)
    except OSError:
        pass
    return ""


def http_get_json(host: str, path: str, timeout: float = 4.0):
    req = urllib.request.Request(f"http://{host}{path}", method="GET")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.status, json.loads(resp.read().decode("utf-8"))


def _atomic_write_bytes(path: str, data: bytes):
    """Atomic + fsync write — the project tree is on NFS where a lazy
    write-back can let a reader (release.sh / pre-push) see a half-filled,
    NUL-prefixed file. os.replace also yields a fresh inode (bypasses stale
    page cache on read-back)."""
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    dfd = os.open(os.path.dirname(path) or ".", os.O_RDONLY)
    try:
        os.fsync(dfd)
    finally:
        os.close(dfd)


def test_device(label: str, host: str, n_tunnels: int) -> dict:
    """Run the smoke sequence against one device. Returns a result dict with
    pass=bool and the fields recorded into the artifact."""
    res = {"label": label, "host": host, "pass": False, "tunnels": n_tunnels,
           "mac": None, "version": None, "build": None,
           "uptime_before": None, "uptime_after": None,
           "tunnels_ok": False, "roundtrip_ok": False, "no_reset": False,
           "detail": ""}
    print(f"\n=== {label} ({host}) ===")

    # 1. pre-flight
    try:
        code, st = http_get_json(host, "/api/status")
    except Exception as e:
        res["detail"] = f"pre_flight: {type(e).__name__}: {e}"
        print(f"  {RED}[✗] pre_flight{RST}: {res['detail']}")
        return res
    if code != 200:
        res["detail"] = f"pre_flight HTTP {code}"
        print(f"  {RED}[✗] pre_flight{RST}: {res['detail']}")
        return res
    res["mac"] = st.get("mac")
    res["version"] = st.get("build", {}).get("version")
    res["build"] = st.get("build", {}).get("number")
    res["uptime_before"] = st.get("uptime")
    print(f"  {GREEN}[✓] pre_flight{RST}: v{res['version']} build={res['build']} "
          f"mac={res['mac']} uptime={res['uptime_before']}s")

    # 2. concurrent tunnels
    clients = []
    try:
        for i in range(n_tunnels):
            c = TunClient(host, f"{label}-T{i}", verbose=False)
            c.connect()
            clients.append(c)
        ias = [c.assigned_ia for c in clients]
        if len(set(ias)) != n_tunnels:
            res["detail"] = f"non-unique IAs: {[int_to_ia(i) for i in ias]}"
            print(f"  {RED}[✗] tunnels{RST}: {res['detail']}")
            return res
        res["tunnels_ok"] = True
        print(f"  {GREEN}[✓] tunnels{RST}: {n_tunnels} concurrent, IAs "
              f"{[int_to_ia(i) for i in ias]}")

        # 3. round-trip: sender -> any other tunnel sees the echo
        dst = group_to_int(ROUNDTRIP_GA)
        for c in clients:
            c.received.clear()
        clients[0].send_tunneling(build_cemi_group_write(0, dst, value=0x01))
        deadline = time.time() + ROUNDTRIP_TIMEOUT_S
        got = False
        while time.time() < deadline and not got:
            for c in clients[1:]:
                if any(m[0] == CEMI_LDATA_IND and m[2] == dst for m in c.received):
                    got = True
                    break
            time.sleep(0.05)
        if not got:
            res["detail"] = f"round-trip: no echo on {ROUNDTRIP_GA} within {ROUNDTRIP_TIMEOUT_S}s"
            print(f"  {RED}[✗] roundtrip{RST}: {res['detail']}")
            return res
        res["roundtrip_ok"] = True
        print(f"  {GREEN}[✓] roundtrip{RST}: group-write echoed across tunnels")
    finally:
        for c in clients:
            try: c.disconnect()
            except Exception: pass
        time.sleep(0.5)

    # 5. no reset/crash during the test
    try:
        code, st2 = http_get_json(host, "/api/status")
    except Exception as e:
        res["detail"] = f"post-check unreachable: {type(e).__name__}: {e}"
        print(f"  {RED}[✗] no_reset{RST}: {res['detail']}")
        return res
    res["uptime_after"] = st2.get("uptime")
    if res["uptime_before"] is not None and res["uptime_after"] is not None \
            and res["uptime_after"] < res["uptime_before"]:
        res["detail"] = (f"device reset during test "
                         f"(uptime {res['uptime_before']}s → {res['uptime_after']}s)")
        print(f"  {RED}[✗] no_reset{RST}: {res['detail']}")
        return res
    res["no_reset"] = True
    print(f"  {GREEN}[✓] no_reset{RST}: uptime {res['uptime_before']}s → {res['uptime_after']}s")

    res["pass"] = True
    res["detail"] = "all smoke checks passed"
    return res


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--c3", required=True, help="ESP32-C3 device IP")
    ap.add_argument("--c6", required=True, help="ESP32-C6 device IP")
    ap.add_argument("--tunnels", type=int, default=DEFAULT_TUNNELS,
                    help=f"concurrent tunnels per device (default {DEFAULT_TUNNELS})")
    ap.add_argument("--version", default="",
                    help="release version to assert (default: read from version.h)")
    ap.add_argument("--no-write", action="store_true",
                    help="report only, do not write the gate artifact")
    ap.add_argument("--now", default="",
                    help="ISO timestamp to stamp the artifact (default: wall clock)")
    args = ap.parse_args()

    expected = args.version or read_release_version()
    if not expected:
        print(f"{RED}ERROR{RST}: no release version (pass --version or build so "
              f"version.h exists)", file=sys.stderr)
        sys.exit(1)
    print(f"Release version under test: {expected}  (tunnels/chip: {args.tunnels})")

    r3 = test_device("C3", args.c3, args.tunnels)
    r6 = test_device("C6", args.c6, args.tunnels)

    # cross-device: both must run the exact release version
    print("\n=== Cross-device ===")
    ok = r3["pass"] and r6["pass"]
    for r in (r3, r6):
        if r["pass"] and r["version"] != expected:
            print(f"  {RED}[✗]{RST} {r['label']} runs v{r['version']}, "
                  f"release is v{expected} — flash the release firmware first")
            ok = False
    if r3["pass"] and r6["pass"] and r3["version"] != r6["version"]:
        print(f"  {RED}[✗]{RST} version mismatch C3 v{r3['version']} ≠ C6 v{r6['version']}")
        ok = False
    elif ok:
        print(f"  {GREEN}[✓]{RST} both chips on release v{expected}")

    print("\n=== Summary ===")
    for r in (r3, r6):
        ico = f"{GREEN}PASS{RST}" if r["pass"] else f"{RED}FAIL{RST}"
        print(f"  {r['label']}: {ico} — {r['detail']}")

    if not ok:
        print(f"\n{RED}SMOKE GATE: FAIL{RST} — release blocked, no artifact written")
        sys.exit(1)

    artifact = {
        "schema": 1,
        "version": expected,
        "build": r3["build"],
        "tunnels": args.tunnels,
        "timestamp": args.now or datetime.now(timezone.utc).isoformat(),
        "pass": True,
        "chips": {"C3": r3, "C6": r6},
    }
    if args.no_write:
        print(f"\n{YEL}--no-write{RST}: artifact NOT written. Would have been:")
        print(json.dumps(artifact, indent=2))
    else:
        _atomic_write_bytes(ARTIFACT, json.dumps(artifact, indent=2).encode() + b"\n")
        print(f"\n{GREEN}SMOKE GATE: PASS{RST} — artifact written: {ARTIFACT}")
        print(f"  v{expected} validated on C3 + C6 — release.sh / pre-push will now allow this version")
    sys.exit(0)


if __name__ == "__main__":
    main()
