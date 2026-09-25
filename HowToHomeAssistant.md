# Connecting Home Assistant to ip4knx

ip4knx is itself a full KNXnet/IP gateway (Routing **and** Tunneling). The KNX
integration that ships with Home Assistant speaks KNXnet/IP natively, so it
connects to the stick **directly — no knxd, no add-on.**

Verified end-to-end on test hardware with Home Assistant 2026.9.3 (KNX library
xknx 3.20.0) and ip4knx v1.4.29, with a TUL32 (ESP32-C6) as Home Assistant's
gateway — once over Wi-Fi, once over the W5500 ethernet module — and a TUL
(ESP32-C3) as a second participant on the same KNX line:
switch commands from Home Assistant reached the bus, and telegrams from the bus
(a switch state and a DPT 9.001 temperature) updated Home Assistant entities.

## Why no knxd?

A TUL can also be run as a plain USB serial interface behind a knxd add-on. With
the ip4knx firmware the stick *is* the KNXnet/IP interface:

* Nothing sits between Home Assistant and the bus — one less service to
  configure, restart and keep in sync.
* The stick is reached over the network (Wi-Fi, or ethernet on a TUL32 with the
  W5500 module), not over a USB cable to the Home Assistant host.
* Up to 10 KNXnet/IP clients at the same time — ETS and Home Assistant can stay
  connected together.

## Prerequisites

* The stick must be running the **ip4knx firmware**. A plain serial firmware
  speaks no KNXnet/IP, which is why such setups need knxd; this guide does not
  apply to them. Flash ip4knx straight from the browser with the web flasher at
  **<https://install.busware.de/ip4knx/>** (Chrome / Edge / Opera); it also walks
  you through Wi-Fi provisioning (Improv). Other install options are in the
  [README](README.md).
* Note the stick's IP address — from the built-in dashboard, mDNS
  (`http://tul.local`), or your router's lease list (the stick registers as
  `tul-<4 hex digits>`). Home Assistant stores this address, so give the stick a
  **DHCP reservation**.
* Home Assistant in the same network segment as the stick. The gateway scan uses
  multicast: Home Assistant OS, or a container with **host networking**, finds
  the stick. From a Docker bridge network the scan finds nothing (tested); enter
  the address manually then (Step 1, *Manual*).

## Step 1 — Add the KNX integration

**Settings → Devices & services → Add integration → KNX**

1. *KNX connection type*: choose **Tunneling**.
2. Home Assistant lists the KNXnet/IP gateways its scan found. An ip4knx stick
   appears as

   ```
   15.15.0 - busware.de TUL @ <IP-OF-IP4KNX>:3671 UDP
   ```

   `15.15.0` is the stick's own individual address — the default until it is
   assigned one with ETS. Select your stick. No further questions follow; the
   entry is created as `Tunneling @ 15.15.0 - busware.de TUL @ <IP-OF-IP4KNX>:3671`.
3. On the ip4knx dashboard, *Active Clients* goes up by one.

**Stick not in the list, or Home Assistant behind NAT:** choose **Manual** at the
end of the list and enter

| Field | Value |
|---|---|
| KNX tunneling type | **UDP (Tunneling v1)** |
| Host | `<IP-OF-IP4KNX>` |
| Port | `3671` |
| Route back / NAT mode | only if Home Assistant reaches the stick through NAT (Docker bridge network, VPN) |

ip4knx implements tunneling over UDP (Tunneling v1). *TCP (Tunneling v2)* and
*Secure Tunneling (TCP)* are not supported; Home Assistant rejects them with
*"Selected tunneling type not supported by gateway."* Route back works: a tunnel
opened with Home Assistant's KNX library from a Docker bridge network sent and
received telegrams in our test.

### Why not *Automatic*?

*Automatic* connects to the first usable gateway the scan returns. With more than
one KNXnet/IP server on the network — another interface, a knxd, a second stick —
that need not be the one you mean; in our test it connected to a different
server. *Tunneling* lets you pick the stick.

### Why not *Routing*?

ip4knx starts KNXnet/IP routing only once the stick has an individual address
other than `15.15.0`, assigned with ETS. Until then the dashboard shows
*IP Routing: Off until a physical address is set (tunnelling works)*, Home
Assistant's routing step reports *"No KNXnet/IP router was discovered on the
network."*, and a routing connection sees no telegrams. With routing active,
tunneling is still the more robust choice: it does not depend on multicast,
which switches and access points with IGMP snooping can stop delivering without
notice.

## Step 2 — Create entities

Either in the **KNX panel** — open the KNX integration under
*Settings → Devices & services*, or go to `/knx` (admin users only) — under
*Entities → Create entity*, or in YAML. The panel can also import your ETS
project (*Project*), and its *Group Monitor* lists every telegram Home Assistant
sees.

YAML example (`configuration.yaml`, restart Home Assistant afterwards):

```yaml
knx:
  switch:
    - name: "Kitchen light"
      address: "1/2/3"
  sensor:
    - name: "Living room temperature"
      state_address: "3/1/1"
      type: temperature
```

At startup Home Assistant reads every state address (GroupValueRead). If no bus
device answers — none has the read flag set for that group address — the log
shows

```
Error: KNX bus did not respond in time (2.0 secs) to GroupValueRead request for: 3/1/1
```

and the entity stays *unknown* until the next telegram on that address arrives.
This is not a gateway fault.

## Verifying it works

* **Send:** switch the entity in Home Assistant — the ip4knx dashboard's TX
  counter increments and the actuator switches.
* **Receive:** operate a physical push-button, or wait for the sensor to send —
  the entity follows and the RX counter increments.
* The KNX panel's *Group Monitor* shows each telegram with source and
  destination. Telegrams Home Assistant sends carry its tunnel address (next
  section).

## Tunnel addresses

Each tunnel gets its own individual address from the stick, and ip4knx rewrites
the source address of every telegram a tunnel client sends to that address
(KNXnet/IP Core §4.4). The stick takes these addresses from its own line:
`<area>.<line>.1` to `<area>.<line>.10` — **`15.15.1` to `15.15.10`** while the
stick still has the default address `15.15.0`.

Tunnel addresses must be unique on the bus:

* **Two unprogrammed ip4knx sticks on the same KNX line hand out the same
  addresses.** ip4knx does not forward a telegram to a tunnel whose address
  equals the telegram's source, so a client on one stick does not receive what
  a client with the same address on the other stick sends (measured). Give each
  stick its own individual address with ETS.
* **After an individual address is assigned, the tunnel addresses move to that
  line** — `1.1.1` to `1.1.10` for a stick at `1.1.250`. No device on that line
  may use these addresses.
* ETS, Home Assistant, Node-RED etc. each occupy one of the 10 tunnels; an 11th
  connection is refused.

## TUL32 on ethernet (W5500)

The KNXnet/IP side does not depend on the interface: a TUL32 on the cable
answers the gateway scan with its cable address, and the steps above apply
unchanged (tested in both directions).

The wired interface has its own MAC and therefore its own DHCP lease — a
different IP than on Wi-Fi. Home Assistant stores the IP, so after moving a
TUL32 from Wi-Fi to the cable, use **Reconfigure** on the KNX integration entry
and select the new address. The same holds for the Wi-Fi fallback: when the
cable is pulled, the stick stays reachable over Wi-Fi, but under its Wi-Fi
address, and Home Assistant's tunnel does not follow it there.

## Troubleshooting

* **The stick is not in the gateway list:** Home Assistant does not see the
  stick's multicast (e.g. Docker bridge network, different subnet). Use
  *Manual* with the IP, and check that UDP port 3671 is reachable from the Home
  Assistant host.
* **"Selected tunneling type not supported by gateway.":** choose
  *UDP (Tunneling v1)*.
* **Entities stay *unknown*:** see the GroupValueRead note in Step 2.
* **Telegrams from one device or client never arrive:** look for a tunnel
  address conflict (see *Tunnel addresses*).
* **A routing connection sees nothing:** the stick still has `15.15.0`. Use
  tunneling, or assign an individual address with ETS.
* **Connection drops after some time:** isolate the side at fault first —
  power-cycle **only** the ip4knx stick and leave Home Assistant running. If it
  recovers on its own, the fault is on the gateway/Wi-Fi side (check Wi-Fi
  signal and your router's DHCP lease time); if only a Home Assistant restart
  helps, the fault is on the host side.
* **KNX IP Secure** (secure tunneling or routing) is not supported by ip4knx.
  KNX Data Secure has not been tested with ip4knx.

## Reference

* Home Assistant: [KNX integration](https://www.home-assistant.io/integrations/knx/)
* FHEM counterpart: [HowToFHEM.md](HowToFHEM.md)
