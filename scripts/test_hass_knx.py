#!/usr/bin/env python3
"""
Test script for Home Assistant KNX integration with ip4knx gateway.

Sends a KNX telegram via Home Assistant and monitors the serial output
to verify the gateway receives it.
"""

import argparse
import glob
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
        print(f"[Info] Auto-detected ports: {ports}")
        return ports[0]
    return None


def monitor_serial(port: str, baudrate: int = 115200) -> threading.Event:
    """Monitor serial output for KNX activity."""
    ready_event = threading.Event()

    def _monitor():
        try:
            ser = serial.Serial(port, baudrate, timeout=1)

            # Hard reset
            ser.setDTR(False)
            ser.setRTS(True)
            time.sleep(0.1)
            ser.setDTR(False)
            ser.setRTS(False)

            print(f"[Serial] Listening on {port}...")

            # Wait for boot
            start = time.time()
            while time.time() - start < 15:
                try:
                    line = ser.readline()
                    if line:
                        try:
                            text = line.decode().strip()
                            if text:
                                print(f"[TUL] {text}")
                            if b"KNX Gateway running" in line:
                                ready_event.set()
                                print("[TUL] Gateway ready")
                                break
                        except UnicodeDecodeError:
                            pass
                except serial.SerialException:
                    pass

            # Monitor for KNX activity
            start = time.time()
            while time.time() - start < 10:
                try:
                    line = ser.readline()
                    if line:
                        try:
                            text = line.decode().strip()
                            if text:
                                # Look for KNX activity indicators
                                if b">>>" in line or b"<<<" in line or b"KNX" in line:
                                    print(f"[KNX] {text}")
                        except UnicodeDecodeError:
                            pass
                except serial.SerialException:
                    pass

            ser.close()
        except serial.SerialException as e:
            print(f"[Serial Error] {e}")
        except Exception as e:
            print(f"[Error] {e}")

    thread = threading.Thread(target=_monitor, daemon=True)
    thread.start()
    return ready_event


def main():
    parser = argparse.ArgumentParser(description='Test Home Assistant KNX integration')

    parser.add_argument('--port', default=None,
                        help='Serial port (default: auto-detect)')
    parser.add_argument('--baud', type=int, default=115200,
                        help='Baud rate (default: 115200)')
    parser.add_argument('--hass-url', default=None,
                        help='Home Assistant URL (default: env HASS_URL or http://localhost:8123)')
    parser.add_argument('--token', default=None,
                        help='HA access token (default: env HASS_TOKEN)')
    parser.add_argument('--address', default='1/1/1',
                        help='KNX group address (default: 1/1/1)')
    parser.add_argument('--payload', default='[1]',
                        help='KNX payload as JSON array (default: [1])')
    parser.add_argument('--no-wait', action='store_true',
                        help='Do not wait for gateway to boot')

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

    # Start serial monitoring
    ready_event = monitor_serial(port, args.baud)

    # Wait for gateway to boot
    if not args.no_wait:
        print("[Info] Waiting for gateway to boot...")
        if not ready_event.wait(timeout=20):
            print("[Warning] Gateway boot timeout")
        time.sleep(2)  # Give it a moment

    # Send KNX telegram
    print(f"[HA] Sending KNX telegram to {args.address}...")
    headers = {
        "Authorization": f"Bearer {token}",
        "Content-Type": "application/json"
    }

    try:
        import json
        payload_data = json.loads(args.payload)
    except json.JSONDecodeError:
        print(f"[Error] Invalid payload JSON: {args.payload}")
        sys.exit(1)

    data = {
        "address": args.address,
        "payload": payload_data
    }

    try:
        response = requests.post(hass_url, headers=headers, json=data, timeout=10)
        print(f"[HA] Response Status: {response.status_code}")
        if response.status_code == 200:
            print(f"[HA] Success")
        else:
            print(f"[HA] Response: {response.text[:200]}")
    except requests.exceptions.ConnectionError:
        print(f"[Error] Cannot connect to Home Assistant at {hass_url}")
    except requests.exceptions.Timeout:
        print(f"[Error] Request timed out")
    except Exception as e:
        print(f"[HA Error] {e}")

    # Give some time for serial output
    time.sleep(3)

    print("[Info] Done")


if __name__ == '__main__':
    main()
