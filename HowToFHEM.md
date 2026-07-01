# Connecting FHEM to ip4knx — without knxd

ip4knx is itself a full KNXnet/IP gateway (Routing **and** Tunneling). FHEM speaks
KNXnet/IP natively through its `KNXIO` I/O module, so it can talk to ip4knx
**directly — no `knxd` in between.**

The FHEM wiki puts it plainly: *"If a KNX-Gateway supports multicast, you don't
need a knxd installation."* The same holds for tunneling.

## Why drop knxd?

* One less service and one less failure layer between FHEM and the bus.
* `KNXIO` has its own connection-state heartbeat + auto-reconnect (tunneling) and
  needs no extra daemon.
* Fewer moving parts to configure, restart, and keep in sync.

## Prerequisites

* The stick must be running the **ip4knx firmware** — that is what turns it into
  a KNXnet/IP gateway. A plain serial-bridge firmware will not work here: it
  speaks no KNXnet/IP, which is exactly why such setups need `knxd`. Flash ip4knx
  straight from the browser with the web flasher at
  **<https://install.busware.de/ip4knx/>** (Chrome / Edge / Opera); it also walks
  you through Wi-Fi provisioning (Improv) in the same flow, and can update an
  existing device later. Other install options (esptool, PlatformIO) are in the
  [README](README.md).
* Once the stick is on Wi-Fi, note its IP address — from the built-in dashboard,
  mDNS (`http://tul.local`), or your router's lease list.
* A current FHEM. The required modules — `KNXIO` (I/O, file `00_KNXIO.pm`) and
  `KNX` (device, `10_KNX.pm`) — ship with FHEM out of the box.

  > The legacy `EIB` / `TUL` / `KNXTUL` modules are **not** needed (and `EIB` no
  > longer exists in current FHEM). Use `KNX` + `KNXIO`.

## Step 1 — Define the I/O (KNXIO)

Recommended: **Mode H (Host / Tunneling)** — unicast UDP, the same protocol ETS
uses.

```
define KNXGW KNXIO H <IP-OF-IP4KNX>:3671 <FREE-IA>
```

* `<IP-OF-IP4KNX>` — the address shown on the dashboard; `3671` is the standard
  KNXnet/IP port.
* `<FREE-IA>` — an individual address for FHEM, e.g. `1.1.250`. With tunneling
  this is uncritical: ip4knx assigns each tunnel its own individual address and
  **rewrites** the source address of outgoing frames (KNXnet/IP Core §4.4), so it
  cannot clash with real bus devices. It only has to be a syntactically valid
  address.

Check it came up:

```
list KNXGW
```

`STATE` should read `connected`. On the ip4knx dashboard the active tunnel/client
count goes to 1.

**Alternative — Mode M (Multicast / Routing):**

```
define KNXGW KNXIO M 224.0.23.12:3671 <FREE-IA>
```

`224.0.23.12:3671` is the standard KNX routing multicast group. Caveat: routing
does **not** rewrite the source address, so `<FREE-IA>` must be a genuinely free,
unique address in your ETS project — otherwise you create a bus address conflict.
Multicast delivery to a Wi-Fi client can also be less reliable than a unicast
tunnel, so **Mode H is the recommended default** for a Wi-Fi gateway.

> Timing note (from the FHEM KNXIO wiki, applies to Mode H): the tunneling
> protocol is timing-sensitive — FHEM delays larger than ~1 second can drop the
> connection. Recovery is automatic (KNXIO reconnects), though a few messages may
> be lost across the reconnect.

## Step 2 — Define a KNX device

Point a device at a group address and its datapoint type. For a simple switch
(1-bit, DPT 1):

```
define myLight KNX <MAIN>/<MIDDLE>/<SUB>:dpt1
```

Example:

```
define myLight KNX 1/2/3:dpt1
```

Now control it:

```
set myLight on
set myLight off
set myLight toggle
```

If you run more than one `KNXIO` I/O, pin the device explicitly:
`attr myLight IODev KNXGW`.

## Verifying it works

* **Send:** `set myLight on` — the ip4knx dashboard's TX counter increments (each
  group write is one L_Data frame on the bus).
* **Receive:** operate the load from a physical KNX switch/actuator — the RX
  counter increments and the FHEM device state follows automatically.

That round-trip — command out, external change reflected back — confirms the
direct, knxd-free path works in both directions.

## Mode reference

| KNXIO mode | Connects to | knxd needed? |
|---|---|---|
| **H** (Host) | KNXnet/IP **Tunneling** (unicast) → **ip4knx directly** | No |
| **M** (Multicast) | KNXnet/IP **Routing** (multicast) → **ip4knx directly** | No |
| T (TCP) | knxd via TCP (`:6720`) | Yes |
| S (Socket) | knxd via UNIX socket | Yes |

Only **H** and **M** talk to a gateway directly; **T** and **S** are knxd
back-ends.

## Troubleshooting

* **`KNXIO` STATE not `connected`:** verify the ip4knx IP and that UDP port 3671
  is reachable from the FHEM host (same subnet / no firewall in between).
* **Address conflict on the bus (Mode M):** pick a `<FREE-IA>` that no real
  device uses. Mode H avoids this via source-address rewrite.
* **Connection drops after some time:** isolate the side at fault first —
  power-cycle **only** the ip4knx stick and leave FHEM running. If it recovers on
  its own, the fault is on the gateway/Wi-Fi side (check Wi-Fi signal and your
  router's DHCP lease time); if only a FHEM restart helps, the fault is on the
  host side.

## Reference

* FHEM wiki: [KNXIO](https://wiki.fhem.de/wiki/KNXIO)
