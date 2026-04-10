# ip4knx - Universal KNXnet/IP Gateway

Custom firmware to turn the **Busware TUL (ESP32-C3)** and **TUL32 (ESP32-C6)** USB sticks into a fully featured KNXnet/IP Routing and Tunneling Gateway.

Built upon the excellent [OpenKNX](https://github.com/OpenKNX/knx) stack, highly optimized and patched for the specific hardware requirements of the NCN5130 transceiver and modern ESP32 Arduino Core 3.x frameworks.

## 🌟 Features

*   **Prio 1: Home Assistant Support:** Auto-discovery via KNXnet/IP Routing and complete multi-client support.
*   **High Performance Concurrency:** Supports up to **10 concurrent KNXnet/IP Tunneling connections** (e.g., simultaneous use of ETS, Home Assistant, Node-RED, etc.).
*   **Installer Mode (Captive Portal):** If no Wi-Fi credentials exist, the device immediately broadcasts an open Access Point (`TUL AP <MAC>`). Connecting to this network triggers a Captive Portal, instantly redirecting your smartphone or laptop to the built-in configuration dashboard.
*   **Web-Based Wi-Fi Setup:** Click the status badge in the web dashboard to open the Wi-Fi configuration modal. Perform a live scan of nearby networks, select your SSID, and enter the password. The gateway will save the credentials and seamlessly reboot into client mode.
*   **Improv-WiFi Provisioning:** Alternatively, connect via Serial (USB) and provision Wi-Fi credentials straight from your browser. ImprovSerial runs concurrently during the first 120 seconds after boot.
*   **Web-based Status Dashboard:** Built-in web server displaying system uptime, network details, active tunneling slots, and real-time KNX Bus Statistics (Bus Load, RX/TX Counters).
*   **Zero-Conf / mDNS:** Reach the gateway interface locally via `http://tul.local`.
*   **Hardware Watchdog:** Active Task Watchdog Timer (TWDT) and Wi-Fi connection monitoring for ultimate stability.
*   **Build Versioning:** Git hash and build number displayed in serial output and `/api/status` JSON.

## 🎛 Supported Hardware

### Busware TUL (ESP32-C3)
*   **MCU:** ESP32-C3
*   **Transceiver:** NCN5130 (Galvanically isolated via ISO7221)
*   **Flash:** 4MB
*   **Target Env:** `tul_esp32c3`
*   **Pins:** LED=4, Button=9, RX=20, TX=21 (UART_NUM_1)

### Busware TUL32 (ESP32-C6)
*   **MCU:** ESP32-C6-MINI-1-N4
*   **Transceiver:** NCN5130 (Galvanically isolated via ISO7221)
*   **Flash:** 4MB
*   **Target Env:** `tul32_esp32c6`
*   **Pins:** LED=8, Button=9, RX=5, TX=4 (UART_NUM_1)
*   **Note:** Requires custom 4MB partition table and NVS initialization (see CLAUDE.md)

## 🚀 Installation

### Option A: Flash Pre-compiled Factory Binaries (Easiest)
You can directly flash the combined factory images located in the `binaries/` folder using ESP Web Tools or `esptool.py`. They contain the bootloader, partition table, and firmware.

1. Locate the correct binary for your hardware in `binaries/`:
   * `factory_tul_esp32c3.bin`
   * `factory_tul32_esp32c6.bin`
2. Flash it to offset `0x0000`:
   ```bash
   esptool.py --chip <esp32c3|esp32c6> write_flash 0x0000 binaries/factory_target.bin
   ```

### Option B: Build from Source (PlatformIO)
This project uses PlatformIO. The required `knx` and `tpuart` libraries are vendored (included locally in `lib/`) to ensure the applied hardware patches remain stable.

1. Install [PlatformIO](https://platformio.org/).
2. Open the `tul-knx-gateway` folder.
3. Build and upload:
   ```bash
   pio run -e tul_esp32c3 -t upload
   # OR
   pio run -e tul32_esp32c6 -t upload
   ```

## ⚙️ Initial Setup
The firmware is designed for a seamless "Installer Mode" experience on the construction site:

1. **Plug the TUL stick into a USB port or power bank.**
2. **Connect to the Gateway:** If no Wi-Fi credentials are saved (factory state), the gateway will immediately broadcast an open Wi-Fi network named `TUL AP <MAC>`. Connect to this network with your smartphone or laptop.
3. **Captive Portal:** A sign-in prompt should automatically appear (Captive Portal), redirecting you to the gateway's web dashboard. If it doesn't, manually open `http://192.168.4.1` in your browser.
4. **Configure Wi-Fi:** Click on the blue "AP Modus Aktiv" badge in the top right corner. A modal will open. Click "WLAN Netzwerke suchen", select the target Wi-Fi, enter the password, and hit Connect. The device will save the credentials, disable the AP, and reboot into your local network.
5. **Manual AP Override:** You can force the gateway into AP Mode at any time by pressing and holding the push button on the stick for >2 seconds.

### Alternative: Improv-WiFi Provisioning via USB
During the first 120 seconds after powering on, you can also provision Wi-Fi credentials via USB:
*   **Web-Serial:** Open the official Webinstaller at [install.busware.de/TUL/](https://install.busware.de/TUL/) and connect to the device.
*   **CLI Script:** Highly useful for automated setups or debugging:
    ```bash
    pip install pyserial
    python3 scripts/test_improv.py --port /dev/ttyUSB0 --ssid 'My_WiFi_Network' --password 'SuperSecret123'
    ```

## 🔧 Utilities

### Build Factory Binary
Create combined factory images for ESP WebFlashTools:
```bash
./scripts/build_factory.sh tul_esp32c3   # For TUL (ESP32-C3)
./scripts/build_factory.sh tul32_esp32c6 # For TUL32 (ESP32-C6)
```
Output: `binaries/factory_*.bin` (ready for flashing at 0x0000)

### Verify Factory Image
Automated verification script that tests the complete deployment workflow:
```bash
./scripts/verify_factory_image.sh \
    --target tul32_esp32c6 \
    --ssid 'MyWiFi' \
    --password 'Secret123' \
    [--port /dev/ttyUSB0]  # Optional: auto-detected if not specified
```

The script performs:
1. **Flashing** - Writes factory binary to device
2. **WiFi Provisioning** - Sends credentials via ImprovSerial
3. **Web Dashboard Check** - Verifies HTTP/API endpoints
4. **KNX Test** - Sends test telegram (requires ETS-programmed device)

### Python Test Scripts
```bash
# WiFi provisioning via CLI
python3 scripts/test_improv.py --ssid 'MyWiFi' --password 'Secret123' --validate

# Scan available networks
python3 scripts/test_improv.py --scan

# Device info
python3 scripts/test_improv.py --info

# KNX/IP diagnostic & bidirectional test
python3 scripts/test_knx_ip_bidirectional.py --diagnose 10.10.11.199
```

## 🤝 Credits
This project heavily relies on the [OpenKNX](https://github.com/OpenKNX) library stack, which provides the robust KNX TP1 and IP protocol implementation.
