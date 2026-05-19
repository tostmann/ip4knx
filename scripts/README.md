# ip4knx Test Scripts

This directory contains test and diagnostic scripts for the ip4knx KNX/IP Gateway firmware.

## Scripts Overview

| Script | Purpose |
|--------|---------|
| `test_improv.py` | WiFi provisioning via ImprovSerial |
| `test_knx_ip_bidirectional.py` | KNX/IP bidirectional communication test |
| `test_tunnel_source.py` | KNXnet/IP tunnel source-address validation regression test |
| `test_hass_knx.py` | Home Assistant KNX integration test |
| `test_hass_to_knx.py` | Home Assistant → KNX Bus test |
| `monitor_ip.py` | Serial port IP traffic monitor |
| `build_factory.sh` | Build factory binary for ESP WebFlashTools |
| `deploy_webflasher.sh` | Stage and rsync the install.busware.de/ip4knx/ web flasher site |

---

## test_improv.py

WiFi provisioning via the ImprovSerial protocol. Allows scanning for networks, validating SSID before provisioning, and displays the assigned IP address.

```bash
# Provision WiFi (auto-detect serial port)
python3 test_improv.py --ssid 'MyWiFi' --password 'Secret123'

# With explicit serial port
python3 test_improv.py --port /dev/ttyUSB0 --ssid 'MyWiFi' --password 'Secret123'

# Scan available networks
python3 test_improv.py --scan

# Validate SSID before provisioning (lists all networks, exits with error if SSID not found)
python3 test_improv.py --ssid 'MyWiFi' --password 'Secret123' --validate

# Get device information
python3 test_improv.py --info
```

**Features:**
- Auto-detects serial port
- Lists all available SSIDs with RSSI and security type
- Reports error if specified SSID is not found in scan
- Displays assigned IP address on successful provisioning

---

## test_knx_ip_bidirectional.py

Diagnostic and bidirectional test tool for KNX/IP communication. Supports testing via Home Assistant, knxd, and raw multicast monitoring.

```bash
# Diagnostic mode - test connectivity only
python3 test_knx_ip_bidirectional.py --diagnose 10.10.11.199

# Monitor routing indications (multicast from gateway)
python3 test_knx_ip_bidirectional.py --monitor ip:10.10.11.199

# Send KNX telegram via Home Assistant
python3 test_knx_ip_bidirectional.py \
  --hass-url http://localhost:8123 \
  --hass-token 'YOUR_LONG_LIVED_TOKEN' \
  --address 11/2/14

# Send KNX telegram via knxd (UNIX socket)
python3 test_knx_ip_bidirectional.py --send /tmp/eib --address 11/2/14

# Send KNX telegram via knxd (network)
python3 test_knx_ip_bidirectional.py --send ip:10.10.11.13 --address 11/2/14

# Full bidirectional test with HASS
python3 test_knx_ip_bidirectional.py \
  --monitor ip:10.10.11.199 \
  --hass-url http://localhost:8123 \
  --hass-token 'YOUR_TOKEN' \
  --address 11/2/14

# Monitor KNX bus via knxd
python3 test_knx_ip_bidirectional.py --busmonitor /tmp/eib
```

**Features:**
- Tests both directions: HASS → KNX Bus (Tunnel) and KNX Bus → HASS (Routing)
- Supports Home Assistant, knxd (UNIX socket or IP), and multicast monitoring
- Requires ip4knx with WiFi configured
- For full routing support: ip4knx needs ETS programming (`knx_configured: true`)

---

## test_tunnel_source.py

Regression test for the KNXnet/IP tunnel source-address validation feature (KNXnet/IP Core §4.4). Opens two parallel tunnel connections, has client A send a TUNNELING_REQUEST with a cEMI source address spoofed to B's assigned IA, and verifies the gateway rewrites the source to A's assigned IA before broadcasting.

```bash
# Default: writes 0/0/1 = 0x01, prints PASS/FAIL verdict
python3 test_tunnel_source.py --host 10.10.11.30

# Choose another group address
python3 test_tunnel_source.py --host 10.10.11.30 --group 1/2/3

# Show packet trace
python3 test_tunnel_source.py --host 10.10.11.30 --verbose
```

Pass criterion: B receives the echoed L_Data.ind with `src == IA_A` (not the spoofed `IA_B`). Stdlib-only, no dependencies.

---

## test_hass_knx.py

Test script for Home Assistant KNX integration. Sends KNX telegrams via Home Assistant's KNX service.

```bash
# Send KNX telegram via HASS
python3 test_hass_knx.py --token 'YOUR_TOKEN' --address 1/2/3 --payload '[1]'

# Use environment variable
export HASS_TOKEN='YOUR_TOKEN'
python3 test_hass_knx.py --address 1/2/3
```

---

## test_hass_to_knx.py

Tests bidirectional KNX communication: Home Assistant → Gateway → KNX Bus.

```bash
# Test Home Assistant to KNX Bus path
python3 test_hass_to_knx.py --token 'YOUR_TOKEN' --address 1/2/3

# With custom payload
python3 test_hass_to_knx.py --token 'YOUR_TOKEN' --address 1/2/3 --payload '[1]'
```

---

## monitor_ip.py

Serial port monitor for viewing IP traffic and debug output from the gateway.

```bash
# Monitor default serial port (30s timeout)
python3 monitor_ip.py

# Monitor specific port
python3 monitor_ip.py --port /dev/ttyUSB0

# Monitor with custom timeout
python3 monitor_ip.py --timeout 60
```

---

## build_factory.sh

Builds the factory binary for ESP WebFlashTools deployment.

```bash
# Build for TUL (ESP32-C3)
./build_factory.sh tul_esp32c3

# Build for TUL32 (ESP32-C6)
./build_factory.sh tul32_esp32c6
```

Output: `binaries/` directory with factory binary.

---

## Environment Variables

| Variable | Used By | Description |
|----------|---------|-------------|
| `HASS_URL` | test_hass_knx.py, test_knx_ip_bidirectional.py | Home Assistant URL (default: http://localhost:8123) |
| `HASS_TOKEN` | test_hass_knx.py, test_hass_to_knx.py | Home Assistant access token |

---

## Prerequisites

- Python 3.6+
- `requests` library (`pip install requests`)
- `knxtool` (for knxd-based tests)
- Serial port access (for ImprovSerial tests)
