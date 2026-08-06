import serial, time, sys

ser = serial.Serial("COM31", 115200, timeout=1)

# Reset ESP32 via DTR/RTS
ser.setDTR(False)
ser.setRTS(True)
time.sleep(0.1)
ser.setDTR(True)
ser.setRTS(False)
time.sleep(0.1)
ser.setDTR(False)
ser.setRTS(False)

# Clear input buffer
ser.reset_input_buffer()

log = open(r"E:\ESP\dlna\esp32_log.txt", "w", encoding="utf-8", errors="replace")
print("Monitor started. Reading serial...", flush=True)

while True:
    try:
        line = ser.readline()
        if line:
            text = line.decode("utf-8", errors="replace").rstrip('\r\n')
            log.write(text + "\n")
            log.flush()
            print(text, flush=True)
    except KeyboardInterrupt:
        break
    except Exception as e:
        print(f"Error: {e}", flush=True)
        time.sleep(0.1)

ser.close()
log.close()