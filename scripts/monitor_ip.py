import serial
import time

port = '/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_10:00:3B:D0:18:2C-if00'
ser = serial.Serial(port, 115200, timeout=1)
ser.setDTR(False)
ser.setRTS(True)
time.sleep(0.1)
ser.setDTR(False)
ser.setRTS(False)

start = time.time()
while time.time() - start < 30:
    line = ser.readline()
    if line:
        try:
            print(line.decode().strip())
        except:
            pass
