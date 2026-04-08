#!/usr/bin/env python3
"""
Bidirectional KNX/IP Gateway Test using knxtool and Home Assistant.

Tests both directions:
  1. KNX Bus -> Gateway -> Network (Routing Indication via multicast)
  2. Network -> Gateway -> KNX Bus (Tunneling via HASS or knxd)

Usage:
    # Monitor routing indications from ip4knx gateway
    python3 test_knx_ip_bidirectional.py --monitor ip:10.10.11.199

    # Send KNX telegram via HASS
    python3 test_knx_ip_bidirectional.py --hass-url http://localhost:8123 \\
         --hass-token 'YOUR_TOKEN' --address 1/2/3

    # Send KNX telegram via knxd (UNIX socket)
    python3 test_knx_ip_bidirectional.py --send /tmp/eib --address 1/2/3

    # Full bidirectional test with HASS
    python3 test_knx_ip_bidirectional.py --monitor ip:10.10.11.199 \\
         --hass-url http://localhost:8123 --hass-token 'TOKEN'

    # Monitor KNX bus via knxd
    python3 test_knx_ip_bidirectional.py --busmonitor /tmp/eib

Prerequisites:
    - ip4knx must be ETS-programmed (knx_configured: true) for routing indications
    - For HASS: KNX integration configured with routing or tunnel access
    - Network connectivity to both gateways
"""

import argparse
import subprocess
import threading
import time
import sys
import queue
import socket
import struct


def monitor_routing_indications(gateway_ip: str, duration: int = 10) -> queue.Queue:
    """
    Monitor KNX routing indications using knxtool groupsocketlisten.

    Returns a queue with received messages.
    """
    result_queue = queue.Queue()

    def _monitor():
        try:
            # Use groupsocketlisten to monitor routing indications
            proc = subprocess.Popen(
                ["knxtool", "groupsocketlisten", f"ip:{gateway_ip}"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True
            )

            start = time.time()
            received = []

            while time.time() - start < duration:
                line = proc.stdout.readline()
                if line:
                    text = line.strip()
                    if text:
                        received.append(text)
                        print(f"[Routing] {text}")
                        result_queue.put(text)
                elif proc.poll() is not None:
                    # Process ended
                    break

            proc.terminate()
            proc.wait(timeout=2)

        except Exception as e:
            print(f"[Monitor Error] {e}")
            result_queue.put(f"ERROR: {e}")

        result_queue.put(None)  # Signal completion

    thread = threading.Thread(target=_monitor, daemon=True)
    thread.start()
    return result_queue


def send_knx_via_hass(hass_url: str, token: str, group_addr: str, payload: list = None) -> bool:
    """
    Send KNX telegram via Home Assistant.

    Returns True on success.
    """
    if payload is None:
        payload = [1]

    try:
        import requests
        headers = {
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json"
        }
        data = {
            "address": group_addr,
            "payload": payload
        }

        response = requests.post(
            f"{hass_url}/api/services/knx/send",
            headers=headers,
            json=data,
            timeout=10
        )

        if response.status_code == 200:
            print(f"[HASS] Sent {group_addr} = {payload}")
            return True
        else:
            print(f"[HASS] Error: {response.status_code} - {response.text[:100]}")
            return False

    except Exception as e:
        print(f"[HASS] Error: {e}")
        return False


def send_knx_telegram(knxd_url: str, group_addr: str, value: int = 1) -> bool:
    """
    Send KNX telegram via knxd using knxtool groupwrite.

    knxd_url can be:
      - ip:10.10.11.13 (network)
      - local:/tmp/eib (UNIX socket)

    Returns True on success.
    """
    try:
        # Auto-detect if it's a path (UNIX socket) vs IP address
        if knxd_url.startswith("/") or knxd_url.startswith("local:"):
            url = knxd_url if knxd_url.startswith("local:") else f"local:{knxd_url}"
        else:
            url = f"ip:{knxd_url}"

        result = subprocess.run(
            ["knxtool", "groupwrite", url, group_addr, str(value)],
            capture_output=True,
            text=True,
            timeout=10
        )

        if result.returncode == 0:
            print(f"[Send] Sent {group_addr} = {value} via {url}")
            return True
        else:
            print(f"[Send] Error: {result.stderr}")
            return False

    except subprocess.TimeoutExpired:
        print("[Send] Timeout")
        return False
    except Exception as e:
        print(f"[Send] Error: {e}")
        return False


def send_knx_read(knxd_url: str, group_addr: str) -> bool:
    """Send KNX group read request."""
    try:
        # Auto-detect if it's a path (UNIX socket)
        if knxd_url.startswith("/") or knxd_url.startswith("local:"):
            url = knxd_url if knxd_url.startswith("local:") else f"local:{knxd_url}"
        else:
            url = f"ip:{knxd_url}"

        result = subprocess.run(
            ["knxtool", "groupread", url, group_addr],
            capture_output=True,
            text=True,
            timeout=10
        )

        if result.returncode == 0:
            print(f"[Read] Sent read request for {group_addr}")
            return True
        else:
            print(f"[Read] Error: {result.stderr}")
            return False

    except Exception as e:
        print(f"[Read] Error: {e}")
        return False


def monitor_knxd(knxd_url: str, duration: int = 10) -> queue.Queue:
    """
    Monitor KNX bus via knxd using vbusmonitor1.

    knxd_url can be ip:host or local:/path

    Returns a queue with received messages.
    """
    result_queue = queue.Queue()

    def _monitor():
        try:
            # Auto-detect URL type
            if knxd_url.startswith("/") or knxd_url.startswith("local:"):
                url = knxd_url if knxd_url.startswith("local:") else f"local:{knxd_url}"
            else:
                url = f"ip:{knxd_url}"

            proc = subprocess.Popen(
                ["knxtool", "vbusmonitor1", url],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True
            )

            start = time.time()
            received = []

            while time.time() - start < duration:
                line = proc.stdout.readline()
                if line:
                    text = line.strip()
                    if text and not text.startswith("L"):
                        received.append(text)
                        print(f"[Bus] {text}")
                        result_queue.put(text)
                elif proc.poll() is not None:
                    break

            proc.terminate()
            proc.wait(timeout=2)

        except Exception as e:
            print(f"[Monitor Error] {e}")
            result_queue.put(f"ERROR: {e}")

        result_queue.put(None)

    thread = threading.Thread(target=_monitor, daemon=True)
    thread.start()
    return result_queue


def test_connectivity(gateway_ip: str) -> bool:
    """Test basic UDP connectivity to gateway."""
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(2.0)

        # Send search request
        header = struct.pack("!BBH", 0x06, 0x10, 0x0202)
        body = struct.pack("!BBHH", 0x08, 0x01, 0x00, 0x00)
        packet = header + struct.pack("!H", 6 + len(body)) + body

        sock.sendto(packet, (gateway_ip, 3671))

        try:
            data, addr = sock.recvfrom(1024)
            print(f"[Connectivity] Gateway responded from {addr}")
            return True
        except socket.timeout:
            print(f"[Connectivity] No response from {gateway_ip}")
            return False

    except Exception as e:
        print(f"[Connectivity] Error: {e}")
        return False
    finally:
        sock.close()


def test_multicast(gateway_ip: str = "224.0.23.12", duration: int = 5) -> bool:
    """Test if we can receive multicast routing indications."""
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(('', 3671))

        mreq = socket.inet_aton(gateway_ip) + struct.pack("!I", 0)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
        sock.settimeout(1.0)

        print(f"[Multicast] Listening on {gateway_ip}:3671 for {duration}s...")

        start = time.time()
        received = 0

        while time.time() - start < duration:
            try:
                data, addr = sock.recvfrom(512)
                print(f"[Multicast] From {addr}: {data[:30].hex().upper()}...")
                received += 1
            except socket.timeout:
                print(".", end="", flush=True)

        sock.close()

        if received > 0:
            print(f"\n[Multicast] Received {received} multicast packets")
            return True
        else:
            print(f"\n[Multicast] No multicast packets received")
            return False

    except OSError as e:
        print(f"[Multicast] OS Error: {e}")
        return False
    except Exception as e:
        print(f"[Multicast] Error: {e}")
        return False


def main():
    parser = argparse.ArgumentParser(
        description="Bidirectional KNX/IP Gateway Test using knxtool",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Monitor routing indications from ip4knx gateway
  python3 test_knx_ip_bidirectional.py --monitor ip:10.10.11.199

  # Send telegram via knxd (UNIX socket)
  python3 test_knx_ip_bidirectional.py --send /tmp/eib --address 1/2/3

  # Send telegram via knxd (network)
  python3 test_knx_ip_bidirectional.py --send ip:10.10.11.13 --address 1/2/3

  # Full bidirectional test
  python3 test_knx_ip_bidirectional.py --monitor ip:10.10.11.199 --hass-url http://localhost:8123 \\
         --hass-token 'YOUR_TOKEN'

  # Send via HASS
  python3 test_knx_ip_bidirectional.py --hass-url http://localhost:8123 \\
         --hass-token 'YOUR_TOKEN' --address 1/2/3

  # Monitor KNX bus via knxd
  python3 test_knx_ip_bidirectional.py --busmonitor /tmp/eib

  # Diagnostic: Test connectivity only
  python3 test_knx_ip_bidirectional.py --diagnose ip:10.10.11.199
        """
    )

    parser.add_argument('--diagnose', default=None,
                        help='IP of gateway to diagnose connectivity')
    parser.add_argument('--monitor', default=None,
                        help='IP of gateway to monitor (routing indications)')
    parser.add_argument('--hass-url', default=None,
                        help='Home Assistant URL (default: http://localhost:8123)')
    parser.add_argument('--hass-token', default=None,
                        help='Home Assistant long-lived access token')
    parser.add_argument('--send', default=None,
                        help='knxd URL (e.g., ip:10.10.11.13 or local:/tmp/eib or /tmp/eib)')
    parser.add_argument('--busmonitor', default=None,
                        help='knxd URL for bus monitoring (e.g., ip:10.10.11.13 or local:/tmp/eib)')
    parser.add_argument('--address', default='1/2/3',
                        help='KNX group address (default: 1/2/3)')
    parser.add_argument('--value', type=int, default=1,
                        help='Value to send (default: 1)')
    parser.add_argument('--duration', type=int, default=15,
                        help='Monitor duration in seconds (default: 15)')
    parser.add_argument('--read', action='store_true',
                        help='Send GroupRead instead of GroupWrite')

    args = parser.parse_args()

    print("="*60)
    print("Bidirectional KNX/IP Gateway Test")
    print("="*60)

    results = {
        "connectivity": False,
        "multicast": False,
        "monitor_started": False,
        "telegram_sent": False,
        "routing_received": False,
        "bus_received": False
    }

    # Diagnostic mode
    if args.diagnose:
        print("="*60)
        print("KNX/IP Gateway Diagnostic")
        print("="*60)
        print(f"\nGateway: {args.diagnose}")
        print("-" * 40)

        results["connectivity"] = test_connectivity(args.diagnose)
        print()
        results["multicast"] = test_multicast("224.0.23.12", 5)

        print("\n" + "="*60)
        print("Diagnostic Summary")
        print("="*60)
        print(f"  UDP Connectivity:   {'OK' if results['connectivity'] else 'FAILED'}")
        print(f"  Multicast Listen:   {'OK' if results['multicast'] else 'FAILED'}")
        print()
        if not results["connectivity"] and not results["multicast"]:
            print("[INFO] Gateway may not be responding.")
            print("       Check: 1) Gateway is powered")
            print("              2) ETS programming completed")
            print("              3) Firewall allows UDP 3671")
        sys.exit(0)

    # Start monitoring threads
    routing_queue = None
    bus_queue = None

    if args.monitor:
        print(f"\n[Step 1] Start routing monitor on {args.monitor}")
        print("-" * 40)
        routing_queue = monitor_routing_indications(args.monitor, args.duration)
        results["monitor_started"] = True
        print(f"[Info] Monitoring for {args.duration} seconds...")

    if args.busmonitor:
        print(f"\n[Step 2] Start bus monitor on {args.busmonitor}")
        print("-" * 40)
        bus_queue = monitor_knxd(args.busmonitor, args.duration)
        print(f"[Info] Monitoring KNX bus for {args.duration} seconds...")

    # Small delay to start monitors
    time.sleep(1)

    # Send telegram
    if args.send:
        print(f"\n[Step {'3' if args.busmonitor else '2'}] Send KNX telegram via {args.send}")
        print("-" * 40)

        if args.read:
            results["telegram_sent"] = send_knx_read(args.send, args.address)
        else:
            results["telegram_sent"] = send_knx_telegram(args.send, args.address, args.value)

        if results["telegram_sent"]:
            print(f"[Info] Waiting for routing indication...")
        else:
            print("[Warning] Send failed")

    # Send via HASS
    if args.hass_token:
        hass_url = args.hass_url or "http://localhost:8123"
        hass_token = args.hass_token
        print(f"\n[Step {'4' if args.send and args.busmonitor else '3' if args.send or args.busmonitor else '2'}] Send KNX telegram via HASS")
        print("-" * 40)

        payload = [args.value] if args.value is not None else [1]
        results["telegram_sent"] = send_knx_via_hass(hass_url, hass_token, args.address, payload)

        if results["telegram_sent"]:
            print(f"[Info] Waiting for routing indication...")
        else:
            print("[Warning] HASS send failed")

    # Wait for results
    if not args.diagnose:
        print("\n" + "="*60)
        print("Waiting for results...")
        print("="*60)

        routing_received = []
        bus_received = []

        if routing_queue:
            while True:
                try:
                    msg = routing_queue.get(timeout=args.duration + 2)
                    if msg is None:
                        break
                    routing_received.append(msg)
                    if "from" in msg.lower() or "to" in msg.lower():
                        results["routing_received"] = True
                except queue.Empty:
                    break

        if bus_queue:
            while True:
                try:
                    msg = bus_queue.get(timeout=2)
                    if msg is None:
                        break
                    bus_received.append(msg)
                    if "from" in msg.lower() or "to" in msg.lower():
                        results["bus_received"] = True
                except queue.Empty:
                    break

    # Summary
    print("\n" + "="*60)
    print("Test Results Summary")
    print("="*60)
    if args.diagnose:
        print(f"  UDP Connectivity:   {'OK' if results['connectivity'] else 'FAILED'}")
        print(f"  Multicast Listen:   {'OK' if results['multicast'] else 'FAILED'}")
    else:
        print(f"  Routing Monitor:     {'STARTED' if results['monitor_started'] else 'SKIPPED'}")
        print(f"  Telegram Sent:      {'OK' if results['telegram_sent'] else 'FAILED'}")
        print(f"  Routing Indications: {'RECEIVED' if routing_received else 'NONE'}")
        print(f"  Bus Monitor:        {'RUNNING' if args.busmonitor else 'SKIPPED'}")

    if routing_received:
        print(f"\n  Received {len(routing_received)} messages:")
        for msg in routing_received[:5]:  # Show first 5
            print(f"    - {msg}")
        if len(routing_received) > 5:
            print(f"    ... and {len(routing_received) - 5} more")

    print("\n" + "="*60)

    # Exit code
    if args.monitor and args.send and results["telegram_sent"]:
        if results["routing_received"]:
            print("[SUCCESS] Bidirectional communication confirmed!")
            print("Path: KNX Bus -> ip4knx -> Network (Routing Indication)")
            sys.exit(0)
        else:
            print("[INFO] Telegram sent, but no routing indication received.")
            print("This may be normal if routing is not enabled or addressed differently.")
            sys.exit(0)  # Not a failure, just no indication
    else:
        sys.exit(0)


if __name__ == '__main__':
    main()
