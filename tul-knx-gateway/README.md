# TUL KNX/IP Gateway (ESP32-C3 / ESP32-C6)

This project provides a highly stable KNX/IP gateway firmware for the TUL and TUL32 busware USB sticks.

## Architecture & Improvements
* **Core Stack**: Based on `OpenKNX/knx` for superior modularity, active maintenance, and critical ETS stability fixes (e.g., correct physical address assignment for tunneling devices).
* **Hardware Support**: Uses a patched, local version of `OpenKNX/tpuart` (`lib/TPUart`) to correctly initialize the NCN5130 chip. The original bitmasks for the ACR0 register (DC2EN, V20VEN, etc.) were corrected according to the ON Semiconductor datasheet.
* **UX & Commissioning**: Implements `Improv-WiFi-Library` over the Serial/JTAG port. This allows users to easily provision WiFi credentials via a browser using WebSerial (https://www.improv-wifi.com) while flashing the factory image.

## Building & Flashing
The project is structured for PlatformIO. 
1. Build the image: `pio run -e tul_esp32c3` (for TUL) or `pio run -e tul32_esp32c6` (for TUL32).
2. The resulting `firmware.bin` (or factory image starting at `0x0`) can be flashed using any ESP-Webserial flasher. Ensure you explicitly use the module's USB JTAG port (e.g., `/dev/serial/by-id/usb-Espressif_USB_JTAG...`).

## Usage
1. Flash the firmware.
2. Connect to the device via an Improv-compatible WebSerial interface.
3. Provision your local WiFi credentials.
4. The device will connect to WiFi and start the KNX/IP gateway stack.
5. The gateway will be discoverable in ETS via IP routing/tunneling.
