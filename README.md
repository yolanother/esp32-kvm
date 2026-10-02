# ESP32 KVM

ESP32 KVM shares a Windows host's physical keyboard and mouse with a selected Bluetooth guest through a USB-connected ESP32-S3. The planned guest device is one composite BLE keyboard, mouse, and media controller. This repository currently contains an initial desktop and firmware build scaffold. Input capture, routing, BLE HID, and device controls are not implemented yet, so this build cannot control another computer.

Start with [the design handoff](docs/00-START-HERE.md), then [the architecture](docs/spec/02-architecture.md) and [validation plan](docs/spec/05-validation-and-delivery.md). The working hardware assumption is the Waveshare ESP32-S3-Touch-LCD-1.54. Verify the arrived variant, revision and schematic before any flashing or pin assignment.

## Layout

- `apps/desktop`: Tauri 2 native shell with a React/TypeScript status webview.
- `crates/input-core`: native input policy boundary; timing-critical routing belongs here, not in React.
- `crates/platform-windows`: Windows capture and recovery adapter boundary.
- `crates/protocol` and `protocol/schema`: protocol implementation owned by the concurrent protocol workstream.
- `firmware/main` and `firmware/components`: ESP-IDF application and separate board, transport, HID, router and display boundaries.
- `tests/vectors`: independent golden protocol vectors owned by the protocol workstream.

## Desktop source checks

Node 22.14.0 and npm 10.9.2 were found on the development host. The installed MSVC Rust channel reports 1.93.1 and built the native shell successfully; the exact toolchain version still needs a reproducible installation check before pinning. Windows 11 and WebView2 are required for a native Tauri window. From the repository root in PowerShell:

```powershell
& 'C:/Users/yolan/.cargo/bin/cargo.exe' fmt --all -- --check
& 'C:/Users/yolan/.cargo/bin/cargo.exe' check --workspace
$env:PATH = 'C:/Program Files/nodejs;' + $env:PATH
cd apps/desktop
& 'C:/Program Files/nodejs/npm.cmd' ci
& 'C:/Program Files/nodejs/npm.cmd' run build
cd ../..
& 'C:/Users/yolan/.cargo/bin/cargo.exe' build --workspace
cd apps/desktop
& 'C:/Program Files/nodejs/npm.cmd' run tauri dev
```

The Rust toolchain channel is selected in `rust-toolchain.toml`; JavaScript dependency versions and resolved packages are pinned in `package.json` and `package-lock.json`. The desktop shell intentionally reports no connected device until a native handshake exists.

## Firmware build

Install ESP-IDF for ESP32-S3 using the official Windows installer, open its ESP-IDF PowerShell environment, then run:

```powershell
cd firmware
idf.py set-target esp32s3
idf.py build
```

The firmware entry point only logs that routing is disabled. Do not flash this scaffold expecting BLE input. An exact ESP-IDF version, board pin map, boot recovery procedure, and hardware build result will be recorded by M0 board bring-up; the toolchain has not yet been validated here.

## Safety boundary

The webview is for configuration and status. Physical input capture, suppression, routing generations, held-key state, and recovery must execute in native host and firmware workers. A new session must start disarmed, and any loss of communication must restore local control and clear guest reports as described in the [protocol contract](docs/spec/04-usb-protocol.md).
