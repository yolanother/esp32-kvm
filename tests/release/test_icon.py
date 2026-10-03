# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Verifies the committed Windows installer icon has bounded, valid PNG-backed
# ICO entries at common small and high-resolution shell sizes.

from pathlib import Path
import struct
import unittest


ICON = Path(__file__).resolve().parents[2] / "apps" / "desktop" / "src-tauri" / "icons" / "icon.ico"


class InstallerIconTests(unittest.TestCase):
    """Check that Tauri receives a real multiresolution Windows icon."""

    def test_ico_has_four_bounded_png_entries(self):
        """Require valid directory offsets and PNG dimensions for each size."""
        data = ICON.read_bytes()
        self.assertGreater(len(data), 1024)
        self.assertEqual(struct.unpack_from("<HHH", data), (0, 1, 4))
        sizes = []
        for index in range(4):
            width, height, colors, reserved, planes, bits, length, offset = struct.unpack_from(
                "<BBBBHHII", data, 6 + 16 * index)
            size = 256 if width == 0 else width
            sizes.append(size)
            self.assertEqual(height, width)
            self.assertEqual((colors, reserved, planes, bits), (0, 0, 1, 32))
            self.assertLessEqual(offset + length, len(data))
            png = data[offset:offset + length]
            self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
            self.assertEqual(struct.unpack_from(">II", png, 16), (size, size))
        self.assertEqual(sizes, [16, 32, 48, 256])


if __name__ == "__main__":
    unittest.main()
