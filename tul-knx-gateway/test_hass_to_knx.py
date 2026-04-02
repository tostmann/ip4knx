import requests
import json
import serial
import time
import threading

HASS_URL = "http://localhost:8123/api/services/knx/send"
TOKEN = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiI0MzgzOTcxYzYyZDQ0NTM3OWNlNmQ3ZjBkYzUwNzk3NiIsImlhdCI6MTc3NDI2NTU4OCwiZXhwIjoyMDg5NjI1NTg4fQ.srn3sfwQlhOoMZy_peEVvQdHsVBfP6oJTnjvhmxU1R0"
HEADERS = {
    "Authorization": f"Bearer {TOKEN}",
    "Content-Type": "application/json"
}

PORT = '/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_10:00:3B:D0:18:2C-if00'

success = False

def monitor_serial():
    global success
    try:
        ser = serial.Serial(PORT, 115200, timeout=1)
        ser.setDTR(False); ser.setRTS(True); time.sleep(0.1); ser.setDTR(False); ser.setRTS(False)
        start = time.time()
        # wait for boot
        while time.time() - start < 15:
            line = ser.readline()
            if b"KNX Gateway running" in line:
                print("[Gateway] Booted and running.")
                break
        
        print("[Gateway] Monitoring for KNX transmission...")
        start = time.time()
        while time.time() - start < 10:
            line = ser.readline()
            if line:
                sline = line.decode(errors='ignore').strip()
                if ">>> TPUART:" in sline:
                    print(f"[Gateway] {sline}")
                    success = True
                    break
        ser.close()
    except Exception as e:
        print(f"Serial error: {e}")

t = threading.Thread(target=monitor_serial)
t.start()

time.sleep(10) # Give it time to boot

payload = {
    "address": "1/2/3",
    "payload": [1]
}

print(f"\n[HASS] Sending KNX telegram to 1/2/3...")
response = requests.post(HASS_URL, headers=HEADERS, json=payload)
print(f"[HASS] Response: {response.status_code}")

t.join()

if success:
    print("\nSUCCESS! Bidirectional connectivity confirmed (HASS -> IP -> Gateway -> TP-UART/Bus)")
else:
    print("\nFAILED! Did not see transmission on TP-UART.")
