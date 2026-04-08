#!/usr/bin/env python3
"""
Monitor IP traffic on the ip4knx gateway via serial output.

Displays KNXnet/IP routing indications and tunneling activity.
"""

import argparse
import glob
import serial
import sys
import time


def find_serial_port() -> str:
    """Auto-detect available serial ports."""
    ports = []
    for pattern in ['/dev/ttyUSB*', '/dev/ttyACM*', '/dev/cu.usb*', '/dev/cu.usbserial*']:
        ports.extend(glob.glob(pattern))
    if ports:
        return ports[0]
    return None


def main():
    parser = argparse.ArgumentParser(description='Monitor IP traffic on ip4knx gateway')

    parser.add_argument('--port', default=None,
                        help='Serial port (default: auto-detect)')
    parser.add_argument('--baud', type=int, default=115200,
                        help='Baud rate (default: 115200)')
    parser.add_argument('--duration', type=int, default=30,
                        help='Monitoring duration in seconds (default: 30, 0 for infinite)')
    parser.add_argument('--filter', default=None,
                        help='Filter output by keyword (e.g., "KNX", "Tunnel", "Routing")')

    args = parser.parse_args()

    # Resolve port
    port = args.port or find_serial_port()
    if not port:
        print("[Error] No serial port specified and none auto-detected")
        print("[Info] Available ports:")
        for p in glob.glob('/dev/ttyUSB*') + glob.glob('/dev/ttyACM*'):
            print(f"  {p}")
        sys.exit(1)

    print(f"[Monitor] Listening on {port}...")
    print(f"[Monitor] Press Ctrl+C to stop")
    print("-" * 60)

    ser = None
    try:
        ser = serial.Serial(port, args.baud, timeout=1)

        # Reset device
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.1)
        ser.setDTR(False)
        ser.setRTS(False)

        # Wait for boot
        print("[Monitor] Waiting for boot...")
        start = time.time()
        while time.time() - start < 15:
            try:
                line = ser.readline()
                if line:
                    try:
                        text = line.decode().strip()
                        if text and "KNX Gateway running" in text:
                            print(f"[Boot] {text}")
                            print("-" * 60)
                            break
                    except UnicodeDecodeError:
                        pass
            except serial.SerialException:
                pass

        # Monitor loop
        start = time.time()
        count = 0

        while True:
            # Check duration
            if args.duration > 0:
                elapsed = time.time() - start
                if elapsed >= args.duration:
                    print(f"\n[Monitor] Duration ({args.duration}s) reached")
                    break

            try:
                line = ser.readline()
                if line:
                    try:
                        text = line.decode(errors='ignore').strip()
                        if text:
                            # Apply filter if specified
                            if args.filter is None or args.filter.upper() in text.upper():
                                print(f"[{time.strftime('%H:%M:%S')}] {text}")
                                count += 1
                    except Exception:
                        pass
            except serial.SerialException as e:
                print(f"[Error] Serial error: {e}")
                break
            except KeyboardInterrupt:
                print(f"\n[Monitor] Stopped by user")
                break

        print("-" * 60)
        print(f"[Monitor] Total lines: {count}")

    except serial.SerialException as e:
        print(f"[Error] Failed to open {port}: {e}")
        sys.exit(1)
    finally:
        if ser and ser.is_open:
            ser.close()


if __name__ == '__main__':
    main()
