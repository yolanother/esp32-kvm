"""Send a read-only HELLO for diagnosis without opening an input session."""
import serial
import struct
import time

frame = struct.pack("<HBBHHQII", 0x4B56, 1, 1, 6, 0, 0, 0, 0) + bytes(6)
crc = 0xFFFFFFFF
for byte in frame:
    crc ^= byte
    for _ in range(8):
        crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
frame += struct.pack("<I", crc ^ 0xFFFFFFFF)
encoded = bytearray([0])
start = 0
code = 1
for byte in frame:
    if byte == 0:
        encoded[start] = code
        start = len(encoded)
        encoded.append(0)
        code = 1
    else:
        encoded.append(byte)
        code += 1
encoded[start] = code
encoded.append(0)
port = serial.Serial(timeout=0.2)
port.port = "COM4"
port.baudrate = 115200
port.dtr = False
port.rts = False
with port:
    port.reset_input_buffer()
    port.write(b"\0" + encoded)
    received = bytearray()
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        received.extend(port.read(1024))
    print("HELLO:", encoded.hex())
    print("response bytes:", len(received), received.hex())
