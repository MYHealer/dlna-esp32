"""Capture ESP32 serial output to a log file with auto-reconnect."""
import serial
import sys
import time
from datetime import datetime

PORT = "COM31"
BAUD = 115200
OUTFILE = r"E:\ESP\dlna\esp32_log.txt"

def open_serial():
    while True:
        try:
            ser = serial.Serial(PORT, BAUD, timeout=1)
            print(f"[{datetime.now().strftime('%H:%M:%S')}] Connected to {PORT}")
            return ser
        except Exception as e:
            print(f"[{datetime.now().strftime('%H:%M:%S')}] Waiting for {PORT}: {e}")
            time.sleep(2)

def main():
    print(f"Logging to {OUTFILE}  (Ctrl+C to stop)")
    with open(OUTFILE, "a", encoding="utf-8", errors="replace") as f:
        f.write(f"\n{'='*60}\n")
        f.write(f"Session start: {datetime.now()}\n")
        f.write(f"{'='*60}\n")
        f.flush()
        while True:
            ser = open_serial()
            try:
                while True:
                    line = ser.readline()
                    if line:
                        text = line.decode("utf-8", errors="replace")
                        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
                        f.write(f"[{ts}] {text}")
                        f.flush()
            except Exception as e:
                print(f"[{datetime.now().strftime('%H:%M:%S')}] Disconnected: {e}, reconnecting...")
                try: ser.close()
                except: pass
                time.sleep(2)

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nStopped.")
