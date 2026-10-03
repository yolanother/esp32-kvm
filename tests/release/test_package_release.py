# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Exercises deterministic, app-only firmware and desktop installer packaging with
# strict artifact validation and checksums without requiring hardware or network.

import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "package_release.py"
REAL_PARTITIONS = Path(__file__).resolve().parents[2] / "firmware" / "partitions.csv"


def app_image(project="esp32_kvm", version="0.2.0", chip_id=9):
    """Make a bounded IDF header, first segment, and app descriptor fixture."""
    header = bytearray(24)
    header[0:2] = b"\xe9\x01"
    struct.pack_into("<H", header, 12, chip_id)
    descriptor = bytearray(256)
    struct.pack_into("<I", descriptor, 0, 0xABCD5432)
    descriptor[16:16 + len(version)] = version.encode("ascii")
    descriptor[48:48 + len(project)] = project.encode("ascii")
    return bytes(header) + struct.pack("<II", 0x3C000020, len(descriptor)) + bytes(descriptor)


class PackageReleaseTests(unittest.TestCase):
    """Verify that release inputs produce a reproducible bounded artifact set."""

    def setUp(self):
        """Create isolated source artifacts and a small app partition fixture."""
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.image = self.root / "esp32_kvm.bin"
        self.image.write_bytes(app_image())
        self.installer = self.root / "ESP32-KVM_0.1.0_x64.msi"
        self.installer.write_bytes(b"offline installer fixture")
        self.partitions = self.root / "partitions.csv"
        self.partitions.write_text("factory,app,factory,0x20000,0x1000,\n")
        self.out = self.root / "release"

    def run_package(self, *extra):
        """Run the release CLI with explicit local inputs and capture its result."""
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--app-image", str(self.image),
             "--partition-table", str(self.partitions), "--installer", str(self.installer),
             "--firmware-version", "0.2.0", "--desktop-version", "0.1.0",
             "--protocol-major", "1", "--protocol-minor-min", "0",
             "--protocol-minor-max", "1", "--out-dir", str(self.out), *extra],
            capture_output=True, text=True,
        )

    def test_produces_app_manifest_desktop_metadata_and_stable_checksums(self):
        """Ensure output is versioned, strict-schema, checksummed, and repeatable."""
        first = self.run_package()
        self.assertEqual(first.returncode, 0, first.stderr)
        names = sorted(p.name for p in self.out.iterdir())
        self.assertEqual(names, [
            "ESP32-KVM_0.1.0_x64.msi", "SHA256SUMS.txt",
            "esp32-kvm-s3-0.2.0-app.bin", "esp32-kvm-s3-0.2.0-manifest.json",
            "release.json",
        ])
        manifest = json.loads((self.out / "esp32-kvm-s3-0.2.0-manifest.json").read_text())
        self.assertEqual(set(manifest), {
            "schema", "board_id", "protocol_major", "protocol_minor_min",
            "protocol_minor_max", "firmware_version", "partition", "image_size",
            "image_sha256",
        })
        self.assertEqual(manifest["partition"], "app")
        self.assertEqual(manifest["board_id"], "esp32-kvm-s3")
        self.assertEqual(manifest["image_sha256"], hashlib.sha256(self.image.read_bytes()).hexdigest())
        metadata = json.loads((self.out / "release.json").read_text())
        self.assertEqual(metadata["desktop"]["version"], "0.1.0")
        self.assertEqual(metadata["desktop"]["signing_status"], "unverified")
        self.assertEqual(metadata["desktop"]["sha256"], hashlib.sha256(self.installer.read_bytes()).hexdigest())
        sums = (self.out / "SHA256SUMS.txt").read_text().splitlines()
        self.assertEqual(len(sums), 4)
        for line in sums:
            digest, name = line.split("  ")
            self.assertEqual(digest, hashlib.sha256((self.out / name).read_bytes()).hexdigest())
        before = {p.name: p.read_bytes() for p in self.out.iterdir()}
        second = self.run_package()
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(before, {p.name: p.read_bytes() for p in self.out.iterdir()})

    def test_rejects_non_app_or_oversized_input_without_partial_release(self):
        """Reject merged/full images and bytes larger than the app partition."""
        self.image.write_bytes(b"not-an-esp-idf-app")
        failed = self.run_package()
        self.assertNotEqual(failed.returncode, 0)
        self.assertFalse(self.out.exists())
        self.image.write_bytes(app_image() + b"x" * 0x1000)
        failed = self.run_package()
        self.assertNotEqual(failed.returncode, 0)
        self.assertFalse(self.out.exists())

    def test_accepts_tauri_msi_filename_with_product_space(self):
        """The native Tauri MSI name contains a space in the product name."""
        self.installer = self.installer.rename(self.root / "ESP32 KVM_0.1.0_x64_en-US.msi")
        result = self.run_package()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.out / self.installer.name).is_file())

    def test_rejects_unsafe_version_and_installer_mismatch(self):
        """Do not accept path-like versions or mislabeled desktop installers."""
        failed = self.run_package("--firmware-version", "../bad")
        self.assertNotEqual(failed.returncode, 0)
        self.assertFalse(self.out.exists())
        self.installer.rename(self.root / "ESP32-KVM_0.2.0_x64.msi")
        failed = self.run_package()
        self.assertNotEqual(failed.returncode, 0)
        self.assertFalse(self.out.exists())

    def test_refuses_to_overwrite_a_changed_release(self):
        """Prevent a previous artifact set from being silently replaced."""
        self.assertEqual(self.run_package().returncode, 0)
        manifest = self.out / "esp32-kvm-s3-0.2.0-manifest.json"
        manifest.write_bytes(b"changed")
        failed = self.run_package()
        self.assertNotEqual(failed.returncode, 0)
        self.assertEqual(manifest.read_bytes(), b"changed")

    def test_checked_in_board_partition_capacity_is_recorded(self):
        """Bind packaging to the checked-in factory app partition coordinates."""
        result = self.run_package("--partition-table", str(REAL_PARTITIONS))
        self.assertEqual(result.returncode, 0, result.stderr)
        firmware = json.loads((self.out / "release.json").read_text())["firmware"]
        self.assertEqual(firmware["app_partition_offset"], 0x20000)
        self.assertEqual(firmware["app_partition_bytes"], 0x650000)

    def test_rejects_embedded_project_version_chip_or_truncated_descriptor(self):
        """CLI labels cannot override the built image identity."""
        for image in (
            app_image(project="other"), app_image(version="9.9.9"),
            app_image(chip_id=0), app_image()[:90],
        ):
            with self.subTest(image=image[:20]):
                self.image.write_bytes(image)
                failed = self.run_package()
                self.assertNotEqual(failed.returncode, 0)
                self.assertFalse(self.out.exists())


if __name__ == "__main__":
    unittest.main()
