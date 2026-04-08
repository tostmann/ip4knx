#!/usr/bin/env python3
"""
Test bidirectional KNX communication through ip4knx gateway.

Sends a KNX telegram via Home Assistant and monitors the serial output
to verify it reaches the TP-UART interface (physical KNX bus).
"""

import argparse
import glob
import json
import os
import serial
import threading
import time
import sys

import requests


def find_serial_port() -> str:
    """Auto-detect available serial ports."""
    ports = []
    for pattern in ['/dev/ttyUSB*', '/dev/ttyACM*', '/dev/cu.usb*', '/dev/cu.usbserial*']:
        ports.extend(glob.glob(pattern))
    if ports:
        return ports[0]
    return None


def monitor_serial(port: str, baudrate: int = 115200) -> threading.Event:
    """
    Monitor serial output for KNX transmission to TP-UART.

    Returns Event that is set when transmission is detected.
    """
    tx_detected = threading.Event()

    def _monitor():
        try:
            ser = serial.Serial(port, baudrate, timeout=1)

            # Hard reset
            ser.setDTR(False)
            ser.setRTS(True)
            time.sleep(0.1)
            ser.setDTR(False)
            ser.setRTS(False)

            print(f"[Gateway] Listening on {port}...")

            # Wait for boot
            start = time.time()
            boot_detected = False
            while time.time() - start < 20:
                try:
                    line = ser.readline()
                    if line and b"KNX Gateway running" in line:
                        print("[Gateway] Booted and ready.")
                        boot_detected = True
                        break
                except serial.SerialException:
                    pass

            if not boot_detected:
                print("[Warning] Boot timeout, proceeding anyway...")

            # Monitor for TP-UART transmission
            print("[Gateway] Monitoring for KNX transmission to TP-UART...")
            start = time.time()
            while time.time() - start < 10:
                try:
                    line = ser.readline()
                    if line:
                        text = line.decode(errors='ignore').strip()
                        # Look for TP-UART transmission markers
                        if ">>> TPUART" in text or "<<< TPUART" in text:
                            print(f"[Gateway] {text}")
                            tx_detected.set()
                            break
                        elif text:
                            # Print other relevant output
                            if "KNX" in text or "Tunnel" in text or "WiFi" in text:
                                print(f"[Gateway] {text}")
                except serial.SerialException:
                    pass

            ser.close()
        except serial.SerialException as e:
            print(f"[Serial Error] {e}")
        except Exception as e:
            print(f"[Error] {e}")

    thread = threading.Thread(target=_monitor, daemon=True)
    thread.start()
    return tx_detected


def main():
    parser = argparse.ArgumentParser(
        description='Test bidirectional KNX: Home Assistant -> Gateway -> TP-UART'
    )

    parser.add_argument('--port', default=None,
                        help='Serial port (default: auto-detect)')
    parser.add_argument('--baud', type=int, default=115200,
                        help='Baud rate (default: 115200)')
    parser.add_argument('--hass-url', default=None,
                        help='Home Assistant URL (default: env HASS_URL)')
    parser.add_argument('--token', default=None,
                        help='HA access token (default: env HASS_TOKEN)')
    parser.add_argument('--address', default='1/2/3',
                        help='KNX group address (default: 1/2/3)')
    parser.add_argument('--payload', default='[1]',
                        help='KNX payload as JSON array (default: [1])')

    args = parser.parse_args()

    # Resolve configuration
    port = args.port or find_serial_port()
    if not port:
        print("[Error] No serial port specified and none auto-detected")
        sys.exit(1)

    hass_url = args.hass_url or os.getenv('HASS_URL', 'http://localhost:8123/api/services/knx/send')
    token = args.token or os.getenv('HASS_TOKEN')

    if not token:
        print("[Error] HA token required: use --token or set HASS_TOKEN env var")
        sys.exit(1)

    # Parse payload
    try:
        payload_data = json.loads(args.payload)
    except json.JSONDecodeError:
        print(f"[Error] Invalid payload JSON: {args.payload}")
        sys.exit(1)

    # Start serial monitoring
    tx_detected = monitor_serial(port, args.baud)

    # Wait for gateway to be ready
    if not tx_detected.wait(timeout=25):
        print("[Warning] Gateway ready timeout")

    # Give extra time for initialization
    time.sleep(2)

    # Send KNX telegram via Home Assistant
    print(f"\n[HA] Sending KNX telegram to {args.address}...")
    headers = {
        "Authorization": f"Bearer {token}",
        "Content-Type": "application/json"
    }
    data = {
        "address": args.address,
        "payload": payload_data
    }

    try:
        response = requests.post(hass_url, headers=headers, json=data, timeout=10)
        print(f"[HA] Response: {response.status_code}")
        if response.status_code != 200:
            print(f"[HA] Error response: {response.text[:200]}")
    except requests.exceptions.ConnectionError:
        print(f"[Error] Cannot connect to Home Assistant at {hass_url}")
        sys.exit(1)
    except requests.exceptions.Timeout:
        print(f"[Error] Request timed out")
        sys.exit(1)
    except Exception as e:
        print(f"[HA Error] {e}")
        sys.exit(1)

    # Wait for transmission detection
    if tx_detected.wait(timeout=10):
        print("\n[SUCCESS] Bidirectional connectivity confirmed!")
        print("Path: Home Assistant -> IP -> Gateway -> TP-UART -> KNX Bus")
        sys.exit(0)
    else:
        print("\n[FAILED] Did not detect transmission to TP-UART")
        print("[Info] Check that:")
        print("  1. Home Assistant KNX integration is configured")
        print("  2. Gateway is connected to WiFi and KNX bus")
        print("  3. Group address routing is correct")
        sys.exit(1)


if __name__ == '__main__':
    main()
