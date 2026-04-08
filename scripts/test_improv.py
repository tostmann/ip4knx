#!/usr/bin/env python3
"""
Improv WiFi Provisioning Test Script

Implements the Improv Serial protocol for WiFi provisioning of ESP32 devices.
Protocol specification: https://www.improv-wifi.com/serial/

Improv Serial Packet Format:
- Header: "IMPROV" (6 bytes)
- Version: 1 byte (currently 0x01)
- Type: 1 byte (0x01=CurrentState, 0x02=Error, 0x03=RPC, 0x04=RPCResponse)
- Length: 1 byte (length of payload)
- Payload: N bytes
- Checksum: 1 byte (sum of all previous bytes mod 256)
"""

import serial
import time
import argparse
import sys
import glob
from typing import Optional, Tuple, List


def find_serial_port() -> Optional[str]:
    """Auto-detect available serial ports (ESP32 devices)."""
    ports = []
    for pattern in ['/dev/ttyUSB*', '/dev/ttyACM*', '/dev/cu.usb*', '/dev/cu.usbserial*']:
        ports.extend(glob.glob(pattern))
    return ports[0] if ports else None


def calc_checksum(data: bytes) -> int:
    """Calculate checksum: sum of all bytes mod 256."""
    return sum(data) % 256


def build_improv_packet(command_id: int, payload: bytes = b'') -> bytes:
    """
    Build a properly formatted Improv RPC packet.

    Packet format: IMPROV(6) + version(1) + type(1) + length(1) + command(1) + payload_len(1) + payload(N) + checksum(1)
    """
    header = b'IMPROV'
    version = bytes([1])
    rpc_type = bytes([0x03])  # RPC command type

    # Payload with command ID and its length
    payload_with_cmd = bytes([command_id]) + bytes([len(payload)]) + payload
    length = bytes([len(payload_with_cmd)])

    frame = header + version + rpc_type + length + payload_with_cmd
    checksum = bytes([calc_checksum(frame)])

    return frame + checksum


def parse_improv_response(data: bytes) -> Tuple[int, bytes]:
    """
    Parse an Improv response packet.

    Returns: (packet_type, payload) or raises ValueError on invalid packet
    """
    if len(data) < 9:
        raise ValueError("Packet too short")

    if data[:6] != b'IMPROV':
        raise ValueError("Invalid header")

    version = data[6]
    if version != 1:
        raise ValueError(f"Unknown version: {version}")

    packet_type = data[7]
    payload_len = data[8]

    if len(data) < 9 + payload_len + 1:
        raise ValueError("Packet truncated")

    payload = data[9:9+payload_len]
    checksum = data[9+payload_len]

    # Verify checksum (sum of all bytes except checksum itself)
    calculated = calc_checksum(data[:-1])
    if calculated != checksum:
        raise ValueError(f"Checksum mismatch: expected {calculated}, got {checksum}")

    return packet_type, payload


class ImprovClient:
    """Improv Serial Protocol Client."""

    # Command IDs
    CMD_GET_CURRENT_STATE = 0x03
    CMD_GET_DEVICE_INFO = 0x04
    CMD_GET_WIFI_NETWORKS = 0x05
    CMD_WIFI_SETTINGS = 0x01

    # Packet types
    TYPE_CURRENT_STATE = 0x01
    TYPE_ERROR = 0x02
    TYPE_RPC = 0x03
    TYPE_RPC_RESPONSE = 0x04

    # States
    STATE_AUTHORIZED = 0x02
    STATE_PROVISIONING = 0x03
    STATE_PROVISIONED = 0x04

    # Error codes
    ERROR_NONE = 0x00
    ERROR_INVALID_RPC = 0x01
    ERROR_UNKNOWN_RPC = 0x02
    ERROR_UNABLE_TO_CONNECT = 0x03

    def __init__(self, port: str, baudrate: int = 115200):
        self.port = port
        self.baudrate = baudrate
        self.ser = None
        self.buffer = bytearray()

    def connect(self, reset_device: bool = True) -> bool:
        """Connect to serial port, optionally reset device."""
        try:
            self.ser = serial.Serial(self.port, self.baudrate, timeout=1)

            if reset_device:
                # Hard reset via DTR/RTS
                self.ser.setDTR(False)
                self.ser.setRTS(True)
                time.sleep(0.1)
                self.ser.setDTR(False)
                self.ser.setRTS(False)

            return True
        except serial.SerialException as e:
            print(f"Failed to open {self.port}: {e}")
            return False

    def close(self):
        """Close serial connection."""
        if self.ser and self.ser.is_open:
            self.ser.close()

    def send_command(self, command_id: int, payload: bytes = b'') -> bool:
        """Send an Improv command."""
        packet = build_improv_packet(command_id, payload)
        self.ser.write(packet)
        return True

    def read_packet(self, timeout: float = 5.0) -> Optional[Tuple[int, bytes]]:
        """Read a complete Improv packet from the serial buffer."""
        start_time = time.time()
        header = b'IMPROV'

        while time.time() - start_time < timeout:
            try:
                chunk = self.ser.read(1024)
                if chunk:
                    self.buffer.extend(chunk)
            except serial.SerialException:
                pass

            # Look for header
            idx = self.buffer.find(header)
            if idx == -1:
                time.sleep(0.01)
                continue

            # Found header, try to parse packet
            if len(self.buffer) >= idx + 9:
                try:
                    # Extract complete packet
                    packet_len = 9 + self.buffer[idx + 8] + 1  # header + len byte + payload + checksum
                    if len(self.buffer) >= idx + packet_len:
                        packet = bytes(self.buffer[idx:idx+packet_len])
                        self.buffer = self.buffer[idx + packet_len:]
                        return parse_improv_response(packet)
                except ValueError as e:
                    # Invalid packet, remove first byte and retry
                    self.buffer = self.buffer[1:]

        return None

    def wait_for_boot(self, timeout: float = 15.0) -> bool:
        """Wait for device to boot."""
        start_time = time.time()
        boot_marker = b"Starting TUL KNX/IP Gateway"

        while time.time() - start_time < timeout:
            try:
                line = self.ser.readline()
                if line and boot_marker in line:
                    print("[Device] Boot complete.")
                    return True
            except serial.SerialException:
                pass
            time.sleep(0.1)

        print("[Device] Boot timeout (proceeding anyway)")
        return True

    def validate_wifi_credentials(self, ssid: str, password: str) -> bool:
        """Validate WiFi credentials per WPA/WPA2 requirements."""
        if not ssid or len(ssid) > 32:
            print(f"[Error] Invalid SSID: must be 1-32 characters")
            return False

        if password:
            if len(password) < 8 or len(password) > 63:
                print(f"[Error] Invalid password: must be 8-63 characters for WPA/WPA2")
                return False

        return True

    def provision_wifi(self, ssid: str, password: str, networks: List[dict] = None) -> bool:
        """
        Provision WiFi credentials via Improv protocol.

        Returns True on success, False on failure.
        """
        # Optionally use pre-fetched networks to avoid duplicate scan
        if networks:
            ssids = [n['ssid'].lower() for n in networks]

            if not networks:
                print("[Error] No WiFi networks found!")
                return False

            if ssid.lower() not in ssids:
                print(f"[Error] SSID '{ssid}' not found in scan results!")
                print("[Info] Available SSIDs:")
                for n in networks:
                    print(f"  - {n['ssid']}")
                return False

            # Find matching network details
            matching = next((n for n in networks if n['ssid'].lower() == ssid.lower()), None)
            if matching:
                print(f"[Info] Found SSID: {matching['ssid']} (RSSI: {matching['rssi']} dBm, {matching['security']})")

        # Validate credentials
        if not self.validate_wifi_credentials(ssid, password):
            return False

        # Prepare payload: SSID_LEN(1) + SSID(N) + PASS_LEN(1) + PASS(M)
        ssid_bytes = ssid.encode('utf-8')
        password_bytes = password.encode('utf-8')
        payload = bytes([len(ssid_bytes)]) + ssid_bytes + bytes([len(password_bytes)]) + password_bytes

        # Send WiFi settings command
        print(f"\n[Client] Sending WiFi credentials (SSID: {ssid})...")
        self.send_command(self.CMD_WIFI_SETTINGS, payload)

        # Wait for provisioning result
        print("[Client] Waiting for provisioning result...")
        while True:
            result = self.read_packet(timeout=20.0)
            if not result:
                print("[Error] Timeout waiting for result")
                return False

            pkt_type, data = result
            print(f"[Device] Response: type=0x{pkt_type:02X}, data={data.hex()}")

            if pkt_type == self.TYPE_CURRENT_STATE and len(data) >= 1:
                state = data[0]
                print(f"[Device] State changed to: 0x{state:02X}")

                if state == self.STATE_PROVISIONED:
                    print("[Success] WiFi provisioning successful!")

                    # Try to get assigned IP
                    assigned_ip = self.get_assigned_ip()
                    if assigned_ip != "Unknown":
                        print(f"[Device] Assigned IP: {assigned_ip}")
                        print(f"[Info] Access the gateway at: http://{assigned_ip}")
                    else:
                        # Extract URL from data if available
                        if len(data) > 2:
                            url_len = data[1] if data[1] < len(data) - 2 else len(data) - 2
                            if url_len > 0:
                                url = data[2:2+url_len].decode('utf-8', errors='ignore')
                                print(f"[Device] Access URL: {url}")

                    return True

                elif state == self.STATE_PROVISIONING:
                    print("[Device] Provisioning in progress...")
                    continue

                elif state == 0x00:  # STATE_STOPPED - sent after successful provisioning
                    # This is normal: device sends STATE_STOPPED (0x00) after STATE_PROVISIONED
                    # to indicate the provisioning cycle is complete
                    print("[Device] Provisioning cycle complete (STATE_STOPPED)")
                    # Try to get assigned IP
                    assigned_ip = self.get_assigned_ip()
                    if assigned_ip != "Unknown":
                        print(f"[Device] Assigned IP: {assigned_ip}")
                        print(f"[Info] Access the gateway at: http://{assigned_ip}")
                    return True

                else:
                    print(f"[Error] Unexpected state: 0x{state:02X}")
                    return False

            elif pkt_type == self.TYPE_ERROR and len(data) >= 1:
                error = data[0]
                error_messages = {
                    self.ERROR_NONE: "No error",
                    self.ERROR_INVALID_RPC: "Invalid RPC command",
                    self.ERROR_UNKNOWN_RPC: "Unknown RPC command",
                    self.ERROR_UNABLE_TO_CONNECT: "Unable to connect to WiFi",
                }
                if error == self.ERROR_NONE:
                    # ERROR_NONE is sent as confirmation before state change - continue waiting
                    print(f"[Device] Confirmation: {error_messages.get(error)}")
                    continue
                print(f"[Error] Improv error: {error_messages.get(error, f'Unknown (0x{error:02X})')}")
                return False

        return False

    def get_device_info(self) -> dict:
        """Request and parse device information."""
        print("[Client] Requesting device info...")
        self.send_command(self.CMD_GET_DEVICE_INFO)

        result = self.read_packet(timeout=5.0)
        if not result:
            print("[Error] Timeout waiting for device info")
            return {}

        _, data = result
        # Parse: [CMD_ID(1), TOTAL_LEN(1), STR_LEN(1), STR(N), ...]
        if len(data) < 2:
            return {}

        info = {}
        pos = 2  # Skip CMD_ID and TOTAL_LEN
        while pos < len(data):
            str_len = data[pos]
            pos += 1
            if pos + str_len <= len(data):
                value = data[pos:pos+str_len].decode('utf-8', errors='ignore')
                pos += str_len
                info[f"field_{len(info)+1}"] = value
            else:
                break

        print(f"[Device] Info: {info}")
        return info

    def scan_wifi_networks(self) -> List[dict]:
        """Scan for available WiFi networks."""
        print("[Client] Scanning WiFi networks...")
        self.send_command(self.CMD_GET_WIFI_NETWORKS)

        networks = []
        while True:
            result = self.read_packet(timeout=20.0)
            if not result:
                break

            pkt_type, data = result
            if pkt_type != self.TYPE_RPC_RESPONSE:
                continue

            # Parse networks: [CMD_ID(1), TOTAL_LEN(1), then network entries]
            # Each entry: SSID_LEN(1) + SSID(N) + RSSI_LEN(1) + RSSI(M) + SEC_LEN(1) + SEC(P)
            if len(data) < 2:
                continue

            # Empty response signals end of scan
            if data[1] == 0:
                break

            pos = 2
            while pos < len(data):
                try:
                    ssid_len = data[pos]
                    pos += 1
                    ssid = data[pos:pos+ssid_len].decode('utf-8', errors='ignore')
                    pos += ssid_len

                    rssi_len = data[pos]
                    pos += 1
                    rssi = data[pos:pos+rssi_len].decode('utf-8', errors='ignore')
                    pos += rssi_len

                    sec_len = data[pos]
                    pos += 1
                    security = data[pos:pos+sec_len].decode('utf-8', errors='ignore')
                    pos += sec_len

                    networks.append({
                        'ssid': ssid,
                        'rssi': rssi,
                        'security': security
                    })
                except (IndexError, ValueError):
                    break

        # Print all networks in sorted order (by RSSI)
        print("\n[Scan] Available networks:")
        for net in sorted(networks, key=lambda x: int(x['rssi']), reverse=True):
            print(f"  SSID: {net['ssid']:<20} RSSI: {net['rssi']:>4} dBm  Security: {net['security']}")

        return networks

    def get_assigned_ip(self) -> str:
        """Get the assigned IP address after WiFi connection."""
        print("[Client] Requesting device state for IP...")
        self.send_command(self.CMD_GET_CURRENT_STATE)

        result = self.read_packet(timeout=5.0)
        if not result:
            return "Unknown"

        pkt_type, data = result

        # STATE_PROVISIONED packet may contain URL with IP
        if pkt_type == self.TYPE_CURRENT_STATE and len(data) >= 1:
            state = data[0]
            if state == self.STATE_PROVISIONED and len(data) > 2:
                url_len = data[1] if data[1] > 0 else 0
                if url_len > 0 and len(data) >= 2 + url_len:
                    url = data[2:2+url_len].decode('utf-8', errors='ignore')
                    if "://" in url:
                        ip = url.split("://")[1].split("/")[0]
                        return ip

        # Also check RPC_RESPONSE (some implementations return URL this way)
        if pkt_type == self.TYPE_RPC_RESPONSE and len(data) > 2:
            url = data[1:].decode('utf-8', errors='ignore')
            if "://" in url:
                ip = url.split("://")[1].split("/")[0]
                return ip

        return "Unknown"


def main():
    parser = argparse.ArgumentParser(
        description='Improv WiFi Provisioning Test',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s --port /dev/ttyUSB0 --ssid 'MyWiFi' --password 'Secret123'
  %(prog)s --ssid 'MyWiFi' --password 'Secret123'  # Auto-detect port
  %(prog)s --scan  # Scan available networks
  %(prog)s --info  # Get device information
        """
    )

    parser.add_argument('--port', default=None,
                        help='Serial port (default: auto-detect first available)')
    parser.add_argument('--baud', type=int, default=115200,
                        help='Baud rate (default: 115200)')
    parser.add_argument('--ssid', default=None,
                        help='WiFi SSID')
    parser.add_argument('--password', default='',
                        help='WiFi password (empty for open networks)')
    parser.add_argument('--scan', action='store_true',
                        help='Scan available WiFi networks')
    parser.add_argument('--info', action='store_true',
                        help='Get device information')
    parser.add_argument('--validate', action='store_true',
                        help='Validate SSID exists before provisioning (auto-scans first)')
    parser.add_argument('--no-reset', action='store_true',
                        help='Do not reset device on connect')

    args = parser.parse_args()

    # Auto-detect port if not specified
    port = args.port or find_serial_port()
    if not port:
        print("[Error] No serial port specified and none auto-detected")
        print("[Info] Available ports:")
        for p in glob.glob('/dev/ttyUSB*') + glob.glob('/dev/ttyACM*'):
            print(f"  {p}")
        sys.exit(1)

    print(f"[Client] Using port: {port}")

    client = ImprovClient(port, args.baud)

    try:
        if not client.connect(reset_device=not args.no_reset):
            sys.exit(1)

        # Wait for device to boot
        if not client.wait_for_boot():
            print("[Warning] Device may not have booted properly")

        if args.info:
            client.get_device_info()

        elif args.scan:
            client.scan_wifi_networks()

        elif args.ssid:
            networks = None
            # With --validate: always scan and validate SSID first
            if args.validate:
                print("[Client] Scanning and validating SSID...")
                networks = client.scan_wifi_networks()
                ssids = [n['ssid'].lower() for n in networks]

                if not networks:
                    print("[Error] No WiFi networks found!")
                    sys.exit(1)

                if args.ssid.lower() not in ssids:
                    print(f"[Error] SSID '{args.ssid}' not found in scan results!")
                    sys.exit(1)

                # Find matching network details
                matching = next((n for n in networks if n['ssid'].lower() == args.ssid.lower()), None)
                if matching:
                    print(f"[Info] Found SSID: {matching['ssid']} (RSSI: {matching['rssi']} dBm, {matching['security']})")

                # Check current state to see if already provisioned
                print("[Client] Checking current state...")
                client.send_command(client.CMD_GET_CURRENT_STATE)
                result = client.read_packet(timeout=5.0)
                if result:
                    pkt_type, data = result
                    # Discard any RPC_RESPONSE packets (from previous scan)
                    while pkt_type == client.TYPE_RPC_RESPONSE:
                        result = client.read_packet(timeout=1.0)
                        if result:
                            pkt_type, data = result
                        else:
                            break

                    if pkt_type == client.TYPE_CURRENT_STATE and len(data) >= 1:
                        state = data[0]
                        if state == client.STATE_PROVISIONED:
                            print(f"[Info] Device already provisioned (State: 0x{state:02X})")
                            assigned_ip = client.get_assigned_ip()
                            if assigned_ip != "Unknown":
                                print(f"[Device] Current IP: {assigned_ip}")
                                print(f"[Info] Access the gateway at: http://{assigned_ip}")
                            sys.exit(0)

            # Pass networks if we already scanned them
            success = client.provision_wifi(args.ssid, args.password, networks if args.validate else None)
            sys.exit(0 if success else 1)

        else:
            parser.print_help()
            print("\n[Info] Use --ssid and --password to provision, --scan to scan, or --info for device info")

    except KeyboardInterrupt:
        print("\n[Info] Interrupted by user")
    finally:
        client.close()


if __name__ == '__main__':
    main()
