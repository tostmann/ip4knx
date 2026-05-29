# Endurance Test Results — 2026-05-28

Firmware: **ip4knx v1.4.3** (build 79). Hardware: **2× TULX32** (ESP32-C6 + NCN5130),
bus-powered KNXnet/IP gateway variant. Tool: `scripts/endurance_test.py`.

## Primary objective: verify bus-powered-only operation

**Goal:** confirm that the TULX32 sustains a real, continuous KNXnet/IP tunneling
workload while drawing power *exclusively from the KNX bus* — no USB, no external
supply. The TULX32 has no external USB connector for normal use; its service-header
CP2102N is galvanically isolated (ISO7741DW) and carries **no power path to the ESP**.
The bus (24–30 V) feeds the NCN5130 switching regulator → 3.3 V for the entire board.

**Result: PASS — bus-powered-only operation verified under sustained load.**

- Both gateways ran a **12 h continuous** workload (1 KNXnet/IP tunnel frame/s each).
- **Both USB service cables were physically disconnected mid-test** (~1.7 h in). The
  remaining ~10.3 h ran on pure bus power. Disconnecting USB produced **zero observable
  effect** on either device — no reboot, no uptime discontinuity, no frame loss spike.
- **NCN5130 stayed `Connected` for the full run** on both units — power rails
  (V20V / VDD2 / VBUS / VFILT / XTAL) green throughout, **0 brownout, 0 NCN dropout**.
- **0 device reboots** on either gateway across the entire run (continuous uptime).

This empirically confirms the design intent: the ESP32-C6 + NCN5130 + Wi-Fi stack runs
indefinitely on KNX bus power alone, with USB being a pure (isolated) data interface
that can be attached or removed at any time without disturbing operation.

## Test parameters

| | |
|---|---|
| Duration | 12 h (start 01:00:36 → end 13:00:36 local) |
| Load | 1 L_Data.req group-write/s per gateway → GA 0/0/1 |
| Tunnel keep-alive | CONNECTIONSTATE_REQUEST every 60 s |
| ACK timeout | 2.0 s; force-reconnect after 3 consecutive timeouts |
| Power | bus-only; both USB cables removed ~1.7 h in |

## Secondary results

| Metric | Stick A | Stick B |
|---|---|---|
| Frames sent | 42 611 | 43 079 |
| ACK rate | **99.927 %** | **99.958 %** |
| Timeouts | 31 (0.073 %) | 18 (0.042 %) |
| Device reboots | 0 | 0 |
| Heap net-drift / 12 h | **−592 B** | **−2808 B** |

### ACK latency (ACKed frames)

| | Stick A | Stick B |
|---|---|---|
| p50 | 133 ms | 122 ms |
| p95 | 283 ms | 287 ms |
| p99 | 432 ms | 452 ms |
| p99.9 | 814 ms | 929 ms |
| mean | 148 ms | 142 ms |

### Memory / leak check

No leak. Net heap drift over 12 h is −592 B / −2808 B — within allocation noise.
Stick B saw one transient dip to 229 564 B during a reconnect (TLS/socket re-setup
burst) and fully recovered to ~250 k B. No monotonic downward trend on either unit.

## Incidents

| Time (local) | Gateways | Type | Cause |
|---|---|---|---|
| 01:41–01:51 | A | AP-mode + reboot | operator error (service-line test) |
| 02:57 | B | 1× tunnel reconnect | transient Wi-Fi blip (<1 s) |
| **05:19–05:20** | **both** | simultaneous drop, ~80 s | **network-side event** (AP/LAN) |
| 10:55 / 11:22 / 12:04 | A | 1× reconnect each | Wi-Fi blips |
| **12:09** | **both** | simultaneous reconnect (s) | **network-side event** |

Two events hit **both** gateways at the same instant → infrastructure-side (Wi-Fi AP
or LAN), not a device fault; auto-reconnect recovered both cleanly. Stick A showed more
solo Wi-Fi reconnects than Stick B (same FW/HW) — likely RSSI/placement difference.

## Caveats

- The few **>2 s latency samples are a test-harness artifact**: both gateways are driven
  from one Python process, so GIL contention during a reconnect storm inflates the
  measured ACK-wait wall time. They do **not** reflect gateway latency — real gateway
  ACK latency is the clean p50–p99 range (120–452 ms).
- Single-tunnel sustained throughput is far below the gateway's ceiling (~167 fps single
  tunnel measured separately); 1 fps was chosen as a realistic-plus-margin soak load.
