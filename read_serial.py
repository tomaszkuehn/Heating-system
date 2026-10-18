import serial, time, sys

port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
s = serial.Serial(port, 115200, timeout=1)
time.sleep(0.3)
s.reset_input_buffer()
end = time.time() + 12
buf = b""
while time.time() < end:
    chunk = s.read(256)
    if chunk:
        buf += chunk
s.close()
try:
    text = buf.decode("utf-8", errors="replace")
except Exception:
    text = repr(buf)
print("=== RAW (%d bytes) ===" % len(buf))
print(text[:4000])
