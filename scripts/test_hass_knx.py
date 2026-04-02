import serial
import time
import requests
import threading

PORT = '/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_10:00:3B:D0:18:2C-if00'
HASS_URL = "http://localhost:8123/api/services/knx/send"
HASS_TOKEN = "YOUR_LONG_LIVED_ACCESS_TOKEN_HERE"

def monitor_serial():
    try:
        ser = serial.Serial(PORT, 115200, timeout=1)
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.1)
        ser.setDTR(False)
        ser.setRTS(False)
        print("[Serial] Listening for TUL output...")
        start = time.time()
        while time.time() - start < 15:
            line = ser.readline()
            if line:
                try:
                    text = line.decode().strip()
                    if text:
                        print(f"[TUL] {text}")
                except:
                    pass
        ser.close()
    except Exception as e:
        print(f"[Serial Error] {e}")

t = threading.Thread(target=monitor_serial)
t.start()

time.sleep(8)

print("[HASS] Sending KNX payload to group address 1/1/1...")
headers = {
    "Authorization": f"Bearer {HASS_TOKEN}",
    "Content-Type": "application/json"
}
data = {
    "address": "1/1/1",
    "payload": [1]
}

try:
    response = requests.post(HASS_URL, headers=headers, json=data)
    print(f"[HASS] Response Status: {response.status_code}")
    print(f"[HASS] Response Body: {response.text}")
except Exception as e:
    print(f"[HASS Error] {e}")

t.join()
print("Done.")