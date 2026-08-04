import serial, time, sys
ser = serial.Serial("COM31", 115200, timeout=1)
log = open(r"E:\ESP\dlna\esp32_log.txt", "a", encoding="utf-8", errors="replace")
print("Monitor started. Reading serial...", flush=True)
start = time.time()
while True:
    try:
        line = ser.readline()
        if line:
            text = line.decode("utf-8", errors="replace").rstrip()
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
