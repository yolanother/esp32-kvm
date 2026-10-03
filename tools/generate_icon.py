# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Generates the deterministic multiresolution Windows ICO used by the Tauri
# installer and shell, with a compact geometric ESP32 KVM mark.

"""Generate a four-size PNG-backed Windows installer icon without dependencies."""

import argparse
from pathlib import Path
import struct
import zlib


SIZES = (16, 32, 48, 256)
DEFAULT_ICON = Path(__file__).resolve().parents[1] / "apps" / "desktop" / "src-tauri" / "icons" / "icon.ico"


def distance_to_segment(x: float, y: float, ax: float, ay: float, bx: float, by: float) -> float:
    """Return the normalized distance from a point to a capped line segment."""
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy)))
    return ((x - ax - t * dx) ** 2 + (y - ay - t * dy) ** 2) ** 0.5


def pixel(x: float, y: float) -> tuple[int, int, int, int]:
    """Draw a rounded navy tile with a white/cyan K mark at normalized point."""
    inset, radius = 0.055, 0.19
    cx = max(inset + radius, min(1 - inset - radius, x))
    cy = max(inset + radius, min(1 - inset - radius, y))
    if (x - cx) ** 2 + (y - cy) ** 2 > radius * radius:
        return (0, 0, 0, 0)
    color = (13, 26, 49, 255)
    if distance_to_segment(x, y, .34, .24, .34, .76) < .048:
        color = (240, 248, 255, 255)
    if (distance_to_segment(x, y, .40, .51, .69, .25) < .048 or
            distance_to_segment(x, y, .40, .51, .69, .76) < .048):
        color = (54, 215, 239, 255)
    return color


def png_chunk(kind: bytes, data: bytes) -> bytes:
    """Encode a PNG chunk with its CRC-32 checksum."""
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def png_image(size: int) -> bytes:
    """Render one RGBA PNG with four-sample antialiasing per pixel."""
    rows = bytearray()
    for iy in range(size):
        rows.append(0)
        for ix in range(size):
            samples = [pixel((ix + sx) / size, (iy + sy) / size)
                       for sy in (.25, .75) for sx in (.25, .75)]
            alpha = sum(sample[3] for sample in samples) / 4
            if alpha == 0:
                rows.extend((0, 0, 0, 0))
            else:
                rows.extend(round(sum(sample[channel] * sample[3] for sample in samples) /
                                  (4 * alpha)) for channel in range(3))
                rows.append(round(alpha))
    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", ihdr) +
            png_chunk(b"IDAT", zlib.compress(bytes(rows), level=9)) + png_chunk(b"IEND", b""))


def ico_bytes() -> bytes:
    """Assemble 16, 32, 48, and 256 pixel PNG images into an ICO container."""
    images = [png_image(size) for size in SIZES]
    directory = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    for size, image in zip(SIZES, images):
        directory.extend(struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0,
                                     1, 32, len(image), offset))
        offset += len(image)
    return bytes(directory) + b"".join(images)


def main() -> None:
    """Write the stable ICO to the Tauri icon path or an explicit output path."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=DEFAULT_ICON)
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(ico_bytes())


if __name__ == "__main__":
    main()
