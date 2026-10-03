# Architecture
## Selected implementation baseline
Windows 11 x64 first. Rust native routing/capture core and Tauri 2 desktop shell with React/TypeScript. UI is configuration/status only: webview latency never blocks input processing. Pin exact Rust/Node/Tauri/ESP-IDF/NimBLE/LVGL versions after the M0 build spike, and check in lockfiles. ESP-IDF C/C++ firmware, NimBLE GATT HID service, TinyUSB CDC device transport, LVGL screen using verified Waveshare BSP. Toolchain versions are implementation decisions to validate, not assumed installed versions.

## Boundaries
Host physical input → Windows capture thread → reserved-hotkey recognizer → routing state machine → guest mapping → framed USB → firmware session/router → selected encrypted BLE connection → guest HID stack.
Raw Input supplies high-resolution mouse deltas; low-level keyboard/mouse hooks provide suppression. Raw Input alone does not globally suppress desktop input. Establish one authoritative event path per type, avoiding duplicate raw/hook forwarding. Hooks run on a dedicated message-loop thread and perform no I/O or heavy work. Use bounded queues. Keep a per-physical-key down ledger.
Sources: https://learn.microsoft.com/en-us/windows/win32/winmsg/lowlevelkeyboardproc and https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerrawinputdevices

## Windows capture and recovery
Capture only after firmware reports selected target READY. Suppress guest-bound keystrokes/buttons locally; local mode passes through unchanged. Ignore/mark self-injected cursor restoration events. For global capture, enumerate source devices for diagnostics, but v1 captures the combined desktop input stream; per-device suppression is not promised by low-level hooks.
Raw relative movement continues while the host cursor is parked; do not derive guest motion from a clipped desktop cursor. Save local cursor on entry and restore on return; release ClipCursor/visibility modifications on every exit path. UI process may close to tray while routing core remains healthy.
Secure attention sequences, UAC secure desktop, session lock and protected system shortcuts are outside the ordinary capture contract. Fail local on desktop/session transitions; never claim Ctrl+Alt+Delete redirection. Elevated-application behavior is tested and surfaced as a limitation.
Firmware heartbeat expiry clears output. Independent host watchdog monitors routing-worker heartbeat and restores cursor/suppression on stalls; process exit removes hooks, but cursor cleanup still needs watchdog/restart reconciliation. Do not auto-elevate.

## BLE design
One GAP identity, one HID service with report IDs for keyboard (8-byte boot-style 6KRO), mouse (buttons, signed 16-bit X/Y, signed 8-bit vertical/horizontal wheels), consumer controls (16-bit usage). Validate exact HID descriptor and report lengths across Windows/macOS/Linux. Boot protocol support is explicitly negotiated only where implemented; do not promise BIOS Bluetooth operation.
Track encryption, identity, subscriptions, protocol mode and held state per connection. Persist bonds in NVS; host holds friendly labels and preferences, never exports BLE keys. Discovery names do not identify a guest uniquely.
Target: three concurrent encrypted guests; eight stored bonds. Configure both host connection limits and controller activities including advertising. Continue controlled advertising while slots remain. Guests initiate connections; firmware cannot force an OS to reconnect instantly. An unbonded guest may initiate one bounded 60-second numeric-comparison exchange without a board tap. The board shows the six-digit code and accepts its side; the guest user must compare that code and confirm on the guest. Limit unsolicited challenges and require an authenticated bond before exposing a selectable slot. Manual Start Pairing remains available. A reconnecting guest never steals the selected target.
Standard HID libraries may assume one connection; use connection-addressed notifications and isolated CCCDs. No global notify-all call.
Source: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/ble/ble-multiconnection-guide.html — stack limits do not prove three-guest HID interoperability.

## Ownership/state
Host owns profiles, mappings, monitor layout, shortcuts and intended target. Firmware owns BLE bonds, actual connection readiness, selected slot, routing generation and the authoritative output gate. Only one routing actor mutates state. Firmware button requests and desktop hotkeys use the same switch transaction; firmware may unconditionally release/suspend, but may not silently select an unannounced target.
LOCAL → PREPARING → ACTIVE(slot,generation) → RELEASING → LOCAL. Any fault → LOCAL/paused. Firmware lease expiry → SUSPENDED and zero reports. Pairing/update use SUSPENDED. New sessions always begin disarmed.

## Repository plan
apps/desktop (Tauri + UI); crates/input-core; crates/platform-windows; crates/protocol; firmware/main; firmware/components/board, transport, hid, router, display; protocol/schema; tests/vectors; docs/spec; docs/design; optional apps/guest-helper later.
Use generated protocol constants with independently checked golden vectors. Shared policy tests exercise routing without hardware.

## Hardware bring-up checklist
Inspect board label, touch option and revision. Download the exact vendor schematic and demo through https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.54/Resources-And-Documents. Confirm USB-C data connects to native S3 USB and record GPIO19/20 routing only after inspection. Verify boot recovery, power latch, button pins, screen offset/orientation, touch address/reset/IRQ and backlight. Record pin table and license notices in repository.
Keep radio/capture buffers in suitable internal memory; PSRAM display allocations must not starve BLE. Start with two partial display buffers and cap animation/redraw under input load. Reserve bootloader/NVS/OTA partitions after measured binary sizes. Test UI disabled vs enabled.

## Persistence and updates
Versioned host JSON with atomic replacement, schema migrations, last-good backup; OS permissions restrict local profile access. Firmware NVS migrations preserve bonds unless user explicitly forgets devices. Package board ID/protocol range/hash in firmware manifest, reject wrong board/incompatible major version. Release input before update. M0 uses documented USB flashing; integrated update must be validated for bootloader/re-enumeration and interrupted-flash recovery. Never mark a device connected solely because a COM port exists.
