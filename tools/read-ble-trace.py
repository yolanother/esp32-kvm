"""Read the opt-in firmware trace; releases input and closes the USB session."""
import argparse
import time
import serial

parser = argparse.ArgumentParser()
parser.add_argument("--port", default="COM4")
args = parser.parse_args()
port = serial.Serial(timeout=0.2)
port.port = args.port
port.baudrate = 115200
# Native USB Serial/JTAG interprets DTR/RTS transitions as reset commands.
port.dtr = False
port.rts = False
with port:
    port.reset_input_buffer()
    port.write(b"\0KVM-DIAG-LOGS\n")
    result = bytearray()
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        result.extend(port.read(1024))
        if b"KVM-TRACE-END" in result:
            break
    start = result.find(b"KVM-TRACE-BEGIN")
    if start < 0:
        raise SystemExit("No trace response; firmware trace must be enabled and app closed")
    print(result[start:].decode("utf-8", errors="replace"))
