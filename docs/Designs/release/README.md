# Offline release packaging

`tools/package_release.py` packages an existing ESP-IDF **application** binary
and an already built Windows installer. It makes a deterministic directory; it
does not build, sign, publish, connect to USB, or flash the board. Use the
manifest v1 contract in [host update preflight](../update/host-update-preflight.md)
for the updater. A SHA-256 checksum detects changed bytes but does not establish
publisher authenticity.

## Inputs and command

Build the firmware with the pinned ESP-IDF toolchain and obtain the app binary
from `firmware/build/esp32_kvm.bin`. The Tauri desktop configuration now enables
an MSI bundle and uses the generated four-size `icons/icon.ico`. After restoring
the frontend dependencies with the project's approved build workflow, run
`cargo tauri build --bundles msi --ci --no-sign` from `apps/desktop` and locate
its `.msi` bundle. `--no-sign` must be reflected in release notes; signing
status in `release.json` remains `unverified`. The installer filename
must contain the desktop Cargo version. Then run, for example:

```powershell
& 'C:/Users/yolan/AppData/Local/Programs/Python/Python312-dropkey/python.exe' tools/package_release.py `
  --app-image firmware/build/esp32_kvm.bin `
  --partition-table firmware/partitions.csv `
  --installer 'target/release/bundle/msi/ESP32 KVM_0.1.0_x64_en-US.msi' `
  --firmware-version 0.1.0-m1 --desktop-version 0.1.0 `
  --protocol-major 1 --protocol-minor-min 0 --protocol-minor-max 2 `
  --out-dir dist/esp32-kvm-0.1.0-m1
```

Replace the input filenames and versions with the actual built release. The
script checks the desktop version against `apps/desktop/src-tauri/Cargo.toml`,
the ESP-IDF v5.5.1 image header and first-segment app descriptor, the embedded
ESP32-S3 chip ID, project name `esp32_kvm`, embedded firmware version, and
image size against both the 8 MiB update limit and the `factory` app partition
in `firmware/partitions.csv`. The checked board partition is currently offset
`0x20000`, capacity `0x650000` bytes. The descriptor check alone does not prove
a binary was built from this source; the
release operator must retain the native build log and source commit.
The firmware's USB STATUS version is read from the same ESP-IDF app descriptor,
so the packaged manifest version and device-reported version agree.

The output directory has exactly five files:

| File | Purpose |
| --- | --- |
| `esp32-kvm-s3-<version>-app.bin` | App image only; no NVS or merged flash image. |
| `esp32-kvm-s3-<version>-manifest.json` | Strict board/protocol/app/size/SHA-256 manifest v1. |
| Built `.msi` or `.exe` | Byte-for-byte copy of the supplied desktop installer. |
| `release.json` | Firmware partition coordinates and desktop build filename, version, size, checksum, and `signing_status: unverified`. |
| `SHA256SUMS.txt` | Sorted SHA-256 lines for the other four files. |

The directory is staged and renamed only after every input validates. Running
the same command again succeeds only if every output byte is identical. There
are no timestamps, machine paths, or mutable latest-release references in the
artifacts. The script never erases or packages NVS, bond storage, or desktop
profiles. A separate signing and provenance process is required before a
release can be called trusted.

The desktop pins Rust `tauri` and JavaScript `@tauri-apps/api` and
`@tauri-apps/cli` to 2.12.0. Keep the JavaScript package lock aligned with
those declarations before building an installer; the bundled CLI should come
from the desktop dependencies, not an older global installation.

## Installation and recovery gate

On a clean Windows 11 machine, install the built desktop bundle, start the app,
verify the board ID and compatible protocol over USB, then pair a guest using
the displayed numeric comparison. Explicit user confirmation and a fresh
status are required before selecting a guest. The installer/uninstaller must be
tested to confirm local profiles survive an upgrade and are handled as
documented on uninstall; this packaging script does not alter them.

Before firmware update, route input local, release all guest state, obtain the
exact `UPDATE_PREPARE` ACK, and flash only the app partition. After USB
re-enumeration, verify board/protocol/version and local disarmed status before
any explicit rearm. An interrupted flash requires a recovery decision; do not
retry blindly or erase NVS. The physical BOOT sequence and app-only flashing
path still require an on-board recovery drill by the root hardware owner. Do
not present the sequence as verified until that drill passes.

## Verification and remaining gates

`python -B -m unittest discover -s tests/release -v` runs offline packaging
tests for deterministic outputs/checksums, embedded image identity, valid ICO,
and rejected invalid inputs. An unsigned Windows MSI was built locally with
Tauri's `--ignore-version-mismatches` flag while Rust Tauri 2.12.0 and the
installed JavaScript API 2.10.1 were out of alignment; align those packages
and rebuild before any release. A real
release additionally needs: pinned ESP-IDF app build, Windows installer build
and clean-machine install/upgrade/uninstall test, code-signature verification
or an explicit unsigned label, USB update/re-enumeration with bonds preserved,
interrupted-update recovery, and physical BOOT recovery. M2 validation in
`docs/spec/05-validation-and-delivery.md` remains the release gate.
