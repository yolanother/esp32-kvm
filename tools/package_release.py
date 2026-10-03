# Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
# Packages one ESP-IDF app image and one built Windows installer into a
# deterministic board-specific release with strict manifest and checksums.

"""Create reproducible app-only firmware and desktop installer release files."""

import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import tempfile
import tomllib


BOARD_ID = "esp32-kvm-s3"
PROJECT_NAME = "esp32_kvm"
ESP32_S3_CHIP_ID = 0x0009
APP_DESC_MAGIC = 0xABCD5432
MAX_IMAGE_BYTES = 8 * 1024 * 1024
MAX_MANIFEST_BYTES = 4096
REPO_ROOT = Path(__file__).resolve().parents[1]


def safe_version(value: str) -> str:
    """Require the same bounded filename-safe version shape as update-core."""
    if not 1 <= len(value) <= 32 or not re.fullmatch(r"[A-Za-z0-9._-]+", value):
        raise ValueError("version must be 1–32 ASCII letters, digits, dot, underscore, or dash")
    if value in {".", ".."} or value.startswith("."):
        raise ValueError("version cannot be a path component")
    return value


def app_partition(table: Path) -> tuple[int, int]:
    """Read the unique factory app offset and capacity from an ESP-IDF CSV."""
    rows = []
    with table.open(newline="", encoding="utf-8") as source:
        for row in csv.reader(line for line in source if line.strip() and not line.lstrip().startswith("#")):
            if len(row) >= 5 and row[0].strip() == "factory" and row[1].strip() == "app" and row[2].strip() == "factory":
                rows.append((int(row[3].strip(), 0), int(row[4].strip(), 0)))
    if len(rows) != 1 or rows[0][0] < 0 or rows[0][1] <= 0:
        raise ValueError("partition table must have exactly one positive factory app partition")
    return rows[0]


def json_bytes(value: dict) -> bytes:
    """Serialize JSON with stable key order, indentation, and final newline."""
    return (json.dumps(value, sort_keys=True, indent=2, ensure_ascii=True) + "\n").encode("ascii")


def sha256(data: bytes) -> str:
    """Return the lowercase SHA-256 required by manifest v1 and SHA256SUMS."""
    return hashlib.sha256(data).hexdigest()


def app_identity(image: bytes) -> tuple[str, str]:
    """Read the bounded first-segment IDF app descriptor for ESP32-S3."""
    # ESP-IDF v5.5.1 esp_app_format.h: 24-byte image header, then an 8-byte
    # segment header. esp_app_desc.h places a 256-byte descriptor first in DROM.
    if len(image) < 32 + 256 or image[0] != 0xE9 or not 1 <= image[1] <= 16:
        raise ValueError("missing bounded ESP-IDF application header")
    if struct.unpack_from("<H", image, 12)[0] != ESP32_S3_CHIP_ID:
        raise ValueError("image chip ID is not ESP32-S3")
    segment_len = struct.unpack_from("<I", image, 28)[0]
    if segment_len < 256 or 32 + segment_len > len(image):
        raise ValueError("first app segment is truncated")
    if struct.unpack_from("<I", image, 32)[0] != APP_DESC_MAGIC:
        raise ValueError("missing ESP-IDF app descriptor")

    def field(start: int) -> str:
        raw = image[start:start + 32]
        if b"\x00" not in raw:
            raise ValueError("app descriptor string is not terminated")
        value = raw.split(b"\x00", 1)[0]
        try:
            return value.decode("ascii")
        except UnicodeDecodeError as error:
            raise ValueError("app descriptor string is not ASCII") from error

    return field(48), field(80)


def package(args: argparse.Namespace) -> None:
    """Validate all inputs before atomically placing one immutable release directory."""
    firmware_version = safe_version(args.firmware_version)
    desktop_version = safe_version(args.desktop_version)
    if not 1 <= args.protocol_major <= 255 or not 0 <= args.protocol_minor_min <= args.protocol_minor_max <= 65535:
        raise ValueError("invalid protocol version range")
    cargo = tomllib.loads(args.desktop_cargo.read_text(encoding="utf-8"))
    if cargo["package"]["version"] != desktop_version:
        raise ValueError("desktop version differs from Cargo package version")

    image = args.app_image.read_bytes()
    offset, capacity = app_partition(args.partition_table)
    if len(image) > min(MAX_IMAGE_BYTES, capacity):
        raise ValueError("app image exceeds update or factory partition capacity")
    embedded_version, embedded_project = app_identity(image)
    if embedded_project != PROJECT_NAME or embedded_version != firmware_version:
        raise ValueError("embedded app project/version differs from release labels")
    installer = args.installer.read_bytes()
    installer_name = args.installer.name
    if not installer or args.installer.suffix.lower() not in {".msi", ".exe"}:
        raise ValueError("installer must be a nonempty .msi or .exe")
    if not re.fullmatch(r"[A-Za-z0-9._-]+", installer_name) or desktop_version not in installer_name:
        raise ValueError("installer filename must be safe and contain desktop version")

    image_name = f"{BOARD_ID}-{firmware_version}-app.bin"
    manifest_name = f"{BOARD_ID}-{firmware_version}-manifest.json"
    manifest = {
        "schema": 1, "board_id": BOARD_ID,
        "protocol_major": args.protocol_major,
        "protocol_minor_min": args.protocol_minor_min,
        "protocol_minor_max": args.protocol_minor_max,
        "firmware_version": firmware_version, "partition": "app",
        "image_size": len(image), "image_sha256": sha256(image),
    }
    manifest_data = json_bytes(manifest)
    if len(manifest_data) > MAX_MANIFEST_BYTES:
        raise ValueError("manifest exceeds update-core parser limit")
    release = {
        "schema": 1, "board_id": BOARD_ID,
        "firmware": {"version": firmware_version, "image": image_name,
                     "manifest": manifest_name, "app_partition_label": "factory",
                     "app_partition_offset": offset, "app_partition_bytes": capacity},
        "desktop": {"version": desktop_version, "file": installer_name,
                    "size": len(installer), "sha256": sha256(installer),
                    "signing_status": "unverified"},
    }
    files = {image_name: image, manifest_name: manifest_data,
             installer_name: installer, "release.json": json_bytes(release)}
    sums = "".join(f"{sha256(data)}  {name}\n" for name, data in sorted(files.items()))
    files["SHA256SUMS.txt"] = sums.encode("ascii")

    if args.out_dir.exists():
        if not args.out_dir.is_dir() or {p.name for p in args.out_dir.iterdir()} != set(files):
            raise ValueError("output directory already exists with different contents")
        if any((args.out_dir / name).read_bytes() != data for name, data in files.items()):
            raise ValueError("output directory already exists with different contents")
        return
    args.out_dir.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".esp32-kvm-release-", dir=args.out_dir.parent) as staging:
        stage = Path(staging)
        for name, data in files.items():
            (stage / name).write_bytes(data)
        stage.rename(args.out_dir)


def main() -> int:
    """Parse explicit release inputs and report validation failures without a traceback."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-image", type=Path, required=True)
    parser.add_argument("--partition-table", type=Path, default=REPO_ROOT / "firmware" / "partitions.csv")
    parser.add_argument("--installer", type=Path, required=True)
    parser.add_argument("--desktop-cargo", type=Path, default=REPO_ROOT / "apps" / "desktop" / "src-tauri" / "Cargo.toml")
    parser.add_argument("--firmware-version", required=True)
    parser.add_argument("--desktop-version", required=True)
    parser.add_argument("--protocol-major", type=int, required=True)
    parser.add_argument("--protocol-minor-min", type=int, required=True)
    parser.add_argument("--protocol-minor-max", type=int, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()
    try:
        package(args)
    except (OSError, ValueError, KeyError, tomllib.TOMLDecodeError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    sys.exit(main())
