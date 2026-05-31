import sys, time
import esptool

port = sys.argv[1] if len(sys.argv) > 1 else 'COM4'
dur = float(sys.argv[2]) if len(sys.argv) > 2 else 25.0

esp = esptool.detect_chip(port, baud=115200)
sys.stderr.write(f"[connected: {esp.CHIP_NAME}]\n"); sys.stderr.flush()

sys.stderr.write("[MARK before hard_reset]\n"); sys.stderr.flush()
esp.hard_reset()
sys.stderr.write("[MARK after hard_reset - reading app output]\n"); sys.stderr.flush()

ser = esp._port
ser.baudrate = 115200
ser.timeout = 0.2

total = 0
end = time.time() + dur
while time.time() < end:
    data = ser.read(512)
    if data:
        total += len(data)
        sys.stdout.write(data.decode('utf-8', errors='replace'))
        sys.stdout.flush()
ser.close()
sys.stderr.write(f"\n[done, {total} bytes]\n"); sys.stderr.flush()
