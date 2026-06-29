#!/usr/bin/env python3
"""
ip4knx release gate: verify the C3-vs-C6 smoke-load artifact covers the version
being released. Shared by scripts/release.sh and .git/hooks/pre-push so both
enforce the same condition (decision 2026-06-09).

The artifact (tul-knx-gateway/.loadtest_pass.json) is produced by
scripts/loadtest_c3_c6.py on a passing run. This check passes only if:
  - the artifact exists and parses
  - artifact.pass is true
  - artifact.version == the requested release version
  - both chip entries (C3, C6) report pass=true

Usage:
    python3 scripts/check_loadtest_gate.py --version 1.4.86
Exit: 0 = gate satisfied, 1 = blocked (prints the reason + how to fix).
"""

import argparse
import json
import os
import sys

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ARTIFACT = os.path.join(PROJECT_DIR, "tul-knx-gateway", ".loadtest_pass.json")


def fail(msg: str):
    print(f"[loadtest-gate] BLOCKED: {msg}", file=sys.stderr)
    print("", file=sys.stderr)
    print("  Run the C3-vs-C6 smoke-load test against the release firmware:", file=sys.stderr)
    print("    python3 scripts/loadtest_c3_c6.py --c3 <C3-IP> --c6 <C6-IP>", file=sys.stderr)
    print("  Deliberate exception (hardware detached, emergency hotfix):", file=sys.stderr)
    print("    ALLOW_UNTESTED_RELEASE=1 <your release/push command>", file=sys.stderr)
    sys.exit(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--version", required=True, help="release version (MAJOR.MINOR.BUILD)")
    args = ap.parse_args()

    if not os.path.exists(ARTIFACT):
        fail(f"no smoke-load artifact ({os.path.relpath(ARTIFACT, PROJECT_DIR)}) — "
             f"C3/C6 test never ran for v{args.version}")
    try:
        with open(ARTIFACT) as f:
            a = json.load(f)
    except (OSError, ValueError) as e:
        fail(f"artifact unreadable ({type(e).__name__}: {e})")

    if not a.get("pass"):
        fail("artifact records a FAILED run")
    if a.get("version") != args.version:
        fail(f"artifact is for v{a.get('version')}, but release is v{args.version} — "
             f"re-run the smoke test against the release firmware")
    chips = a.get("chips", {})
    for chip in ("C3", "C6"):
        c = chips.get(chip)
        if not c or not c.get("pass"):
            fail(f"{chip} did not pass in the artifact")

    print(f"[loadtest-gate] OK: v{args.version} smoke-tested on C3 + C6 "
          f"({a.get('timestamp', 'no-ts')})")
    sys.exit(0)


if __name__ == "__main__":
    main()
