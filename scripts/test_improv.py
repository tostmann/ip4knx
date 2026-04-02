import serial
import time
import argparse
import sys

parser = argparse.ArgumentParser(description='Improv WiFi Provisioning Test')
parser.add_argument('--port', default='/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_10:00:3B:D0:18:2C-if00', help='Serial port')
parser.add_argument('--ssid', required=True, help='WiFi SSID')
parser.add_argument('--password', required=True, help='WiFi Password')
args = parser.parse_args()

try:
    ser = serial.Serial(args.port, 115200, timeout=0.1)
    ser.setDTR(False); ser.setRTS(True); time.sleep(0.1); ser.setDTR(False); ser.setRTS(False)
except Exception as e:
    print(f"Failed to open {args.port}: {e}"); exit(1)

print("Waiting for boot...")
start = time.time()
while time.time() - start < 15:
    try:
        if b"Starting TUL KNX/IP Gateway" in ser.readline():
            print("Boot complete.")
            break
    except:
        pass
        print("Boot complete.")
        break

def calc_checksum(data): return sum(data) % 256

def send_command(command_id, payload=b''):
    frame = bytearray(b'IMPROV\x01\x03')
    frame.append(2 + len(payload))
    frame.append(command_id)
    frame.append(len(payload))
    if payload: frame.extend(payload)
    frame.append(calc_checksum(frame))
    print(f"\n>> Sending Command 0x{command_id:02X}")
    ser.write(frame)

buffer = bytearray()
def read_packet(timeout=5):
    global buffer
    start_time = time.time()
    header = b'IMPROV'
    while time.time() - start_time < timeout:
        try:
            chunk = ser.read(1024)
            if chunk: buffer.extend(chunk)
        except:
            pass
        idx = buffer.find(header)
        while idx != -1:
            if len(buffer) >= idx + 9:
                ver = buffer[idx+6]
                typ = buffer[idx+7]
                length = buffer[idx+8]
                if ver == 1 and typ <= 4:
                    if len(buffer) >= idx + 9 + length + 1:
                        payload = buffer[idx+9 : idx+9+length]
                        chk = buffer[idx+9+length]
                        if chk == (sum(header) + ver + typ + length + sum(payload)) % 256:
                            buffer = buffer[idx + 9 + length + 1:]
                            print(f"<< Packet Type: 0x{typ:02X}, Len: {length}, Payload: {payload.hex()}")
                            return typ, payload
                        else: buffer = buffer[idx + 1:]
                    else: break
                else: buffer = buffer[idx + 1:]
            else: break
            idx = buffer.find(header)
    return None

time.sleep(1)

# Request Device Info
send_command(0x03)
read_packet()

# Scan Wi-Fi
print("\n--- Scanning WiFi ---")
send_command(0x04)
scan_timeout = time.time() + 15
while time.time() < scan_timeout:
    p = read_packet(1)
    if p and p[0] == 0x04:
        if len(p[1]) == 2 and p[1][0] == 0x04 and p[1][1] == 0x00:
            print("End of Wi-Fi scan.")
            break
        if len(p[1]) > 2:
            cmd = p[1][0]
            dlen = p[1][1]
            idx = 2
            slen = p[1][idx]
            idx += 1
            try:
                ssid = p[1][idx:idx+slen].decode()
            except:
                ssid = "unknown"
            idx += slen
            rlen = p[1][idx]
            idx += 1
            try:
                rssi = p[1][idx:idx+rlen].decode()
            except:
                rssi = ""
            idx += rlen
            alen = p[1][idx]
            idx += 1
            try:
                auth = p[1][idx:idx+alen].decode()
            except:
                auth = ""
            print(f"  [Scanned] SSID: {ssid}, RSSI: {rssi}, Auth: {auth}")

# Scan Wi-Fi timeout
p = read_packet(5)

# Send credentials
ssid = args.ssid.encode()
password = args.password.encode()
print(f"\n--- Sending Credentials (SSID: {ssid.decode()}) ---")
payload = bytes([len(ssid)]) + ssid + bytes([len(password)]) + password
send_command(0x01, payload)

print("Waiting for provisioning result...")
while True:
    p = read_packet(20)
    if not p:
        print("Timeout waiting for result")
        break
    if p[0] == 0x01: # State packet
        state = p[1][0]
        print(f"State changed to: {state}")
        if state == 4: # PROVISIONED
            print("Provisioning Successful!")
            
            # Request URL
            if len(p[1]) > 2:
                urllen = p[1][1]
                url = p[1][2:2+urllen].decode()
                print(f"Device URL: {url}")
            break

ser.close()
