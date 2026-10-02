# ESP32 KVM — Product specification
Design baseline: 2026-10-02. Status: ready for implementation planning; hardware acceptance tests pending.
Project: P319ff29d82618. Repository: https://github.com/yolanother/esp32-kvm
Development checkout: C:/Users/yolan/workspace/hardware/esp32-kvm (Mysiraith).

## Purpose
Share the Windows host's keyboard and mouse with selected Bluetooth guests through a USB-connected ESP32-S3. Guests pair with one combined BLE keyboard/mouse/media device. The host runs the desktop app. Default guests need no app or network. The product name remains ESP32 KVM; v1 shares input, not video.

## Hardware baseline
Use the Waveshare ESP32-S3-Touch-LCD-1.54 from the most recent supplied Amazon listing (ASIN B0GV472MH4). No new hardware attachment accompanied the design request. The touch variant is the working assumption: inspect the arrived board and revision in M0 before pin assignments or flashing. Non-touch sibling uses the same core flow with buttons.
Vendor documentation identifies ESP32-S3R8, 16 MB flash, 8 MB PSRAM, 240×240 ST7789 display, optional CST816 touch, Type-C and PLUS/BOOT/PWR controls. Reserve PWR for its hardware power function; do not assume three freely programmable buttons. Exact GPIOs and USB routing require schematic/BSP verification.
Sources: https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.54 and https://www.waveshare.com/esp32-s3-lcd-1.54.htm
Audio, IMU, SD and Wi-Fi remain unused in v1. USB power is sufficient; a battery is not required.

## User requirements
R1 One combined BLE input device per guest pairing.
R2 Configurable global shortcut to cycle systems, select a guest directly and return to host.
R3 Per-guest physical key/modifier/chord mapping, including explicit Cmd/GUI → Ctrl and Windows Ctrl → macOS Cmd options.
R4 Move across configured host monitor edges to a guest when connected.
R5 Multiple remembered guests; design target three concurrently connected guests, one input recipient.
R6 Tray-first Windows app plus device screen showing the active target and connectivity.
R7 Recover local control on loss of link, app failure, suspend or emergency action.
R8 Optional guest helper enables accurate cursor placement and bidirectional edge crossing.

## Capability contract
| Mode | Guest installation | Host → guest edge | Guest → host / guest edge | Entry position |
|---|---|---|---|---|
| Standard BLE (v1 default) | None | Yes, from known host boundary | Hotkey, tray or device button | Guest's existing cursor |
| Seamless helper (later opt-in) | Small authenticated helper | Yes | Yes, using live cursor/display telemetry | Matched edge/coordinate |
| Estimated return (research only) | None | Yes | Unreliable with acceleration/local mouse | No guarantee; not enabled |

Relative BLE HID does not provide live guest cursor coordinates or monitor topology. Do not imply that accumulated deltas provide reliable seamless return. Absolute pointer HID is a compatibility experiment, not a dependency or shipped promise. A guest helper is optional; losing it immediately degrades to Standard BLE.

## Proposed defaults (editable)
Next system: Ctrl+Alt+F12. Previous: Ctrl+Alt+F11. Host: Ctrl+Alt+F10. Guests 1–3: Ctrl+Alt+1/2/3. Emergency local release: hold both Ctrl keys for 1 second; device BOOT runtime long press is a second path. Hotkeys evaluated on physical keys before guest mapping.
Only connected/ready guests participate in cycling, plus Host; explicit selection of offline guest offers reconnect and keeps local control. No broadcast-input mode.
Edge crossing is opt-in per edge; default dwell 200 ms, 12 px corner exclusion, 500 ms re-arm cooldown, and disabled while buttons are held or capture is paused. Edges mean exposed physical-pixel segments of the complete Windows desktop, not internal monitor seams.
Default mapping is unchanged physical keys. Presets are previewed and explicitly applied. Local host input is not remapped.

## Releases
M0 Foundation: board verification, repeatable builds, USB loopback, one-guest composite HID proof.
M1 Useful MVP: Windows capture/suppression, reliable one-guest routing, hotkeys, fail-local behavior, pairing wizard and tray.
M2 Full base app: three-guest capability validation, key mapping, host-edge layouts, device UI, updates and diagnostics.
M3 Optional seamless helper: Windows/macOS helpers, authenticated telemetry and bidirectional crossing.
M4 Future: macOS/Linux controlling hosts, NKRO, absolute pointer experiments. No commitment to these in v1.

## Acceptance summary
A Windows-host user pairs a macOS guest once, selects it via shortcut, types and moves the pointer without affecting host applications, applies Ctrl → Cmd to that guest, and returns locally by shortcut. Three-guest mode passes isolation and switch stress tests before being advertised. Walking off an assigned host edge activates the guest, while the UI explains hotkey return in Standard mode. USB loss restores host input and makes firmware clear held reports; reconnect never replays old input.

## Success targets — measured, not guarantees
USB acceptance p95 ≤ 5 ms; active-guest input p95 ≤ 30 ms; already-connected target switch p95 ≤ 100 ms on the declared test matrix. Reconnection has a visible state with a 10-second UI timeout, not an instant-switch guarantee. Zero wrong-target events and zero stuck keys across 1,000 switch cycles.

