# ESP32 KVM — Start here
Design package created 2026-10-02 for [ESP32 KVM](https://orchestrator.doubtech.ai/projects/P319ff29d82618/dashboard).

## What is ready
Five engineering specifications, DESIGN.md, nine desktop base views, five 240×240 device screen states, and an architecture diagram. Nine native epics and 32 new child tasks with acceptance criteria/dependencies; existing scaffolding task reused. All new work is open and undispatched. The separate auto-generated documentation-health task remains intact.

## Design canvas
Open the project's canvas named **ESP32 KVM — Product & Base Designs**.
Workspace ID: JlbLPwOjBvnrOrZGdgDoW.
Desktop screens are arranged in three columns, with device screens and architecture below; scope/design docs on the left. Mock data is labeled.
The API's registered-design bundle currently omits API-created mockups; use docs/design/screens for full source handoff, not an empty registered-screen list. Canvas elements were saved and project association verified. Pixel rendering QA remains pending because this session lacks a browser binary; these are static base designs, not a running desktop app.

## Reading order
1. docs/spec/01-product.md — requirements, hardware assumption and release scope.
2. docs/spec/02-architecture.md — host/firmware boundaries, BLE and recovery.
3. docs/spec/03-input-and-edge-behavior.md — hotkeys, mappings, cursor handoff.
4. docs/spec/04-usb-protocol.md — framing, commands and routing generations.
5. docs/spec/05-validation-and-delivery.md — real-hardware matrix and release gates.
6. docs/design/DESIGN.md — tokens, components, interaction states and configuration model.
7. docs/design/screens/S01.md through S09.md and D01-D05.md — full HTML sources.

## Decisions to carry into implementation
- Board target is Waveshare ESP32-S3-Touch-LCD-1.54 from the last supplied hardware link. Verify arrived variant/revision/pins during M0.
- Windows host first: Rust/Tauri/React; ESP-IDF/NimBLE/TinyUSB/LVGL firmware.
- One composite BLE keyboard/mouse/media identity; three live guests is a validation target, eight saved bonds a storage target.
- Proposed configurable cycle/return defaults: Ctrl+Alt+F12 / Ctrl+Alt+F10. Emergency both-Ctrl hold takes precedence.
- Per-guest source→destination mapping explicitly supports Cmd/GUI→Ctrl and Ctrl→Cmd; no silent inversion.
- BLE-only mode supports host-edge activation; guest cursor stays at its existing position, return by hotkey.
- Optional helper enables precise entry and automatic return; no guest helper required for baseline.
- Release held inputs, serialize selection, fail local and discard stale input on every routing fault.

## Epic index
1. [Hardware & foundations](https://orchestrator.doubtech.ai/tasks/T319ff6fac40c6)
2. [BLE firmware & routing](https://orchestrator.doubtech.ai/tasks/T319ffb5ba254f)
3. [Windows capture & switching](https://orchestrator.doubtech.ai/tasks/T319ffb5fd50ab)
4. [Desktop app & profiles](https://orchestrator.doubtech.ai/tasks/T319ffb65226ab)
5. [Per-guest mapping](https://orchestrator.doubtech.ai/tasks/T319ffb69356a8)
6. [Screen-edge handoff](https://orchestrator.doubtech.ai/tasks/T319ffb6dc4349)
7. [Device UI & lifecycle](https://orchestrator.doubtech.ai/tasks/T319ffb720e012)
8. [Optional guest helper](https://orchestrator.doubtech.ai/tasks/T319ffb774d16e)
9. [Validation & release](https://orchestrator.doubtech.ai/tasks/T319ffb7fe0573)

## Existing foundation
[Set up project scaffolding](https://orchestrator.doubtech.ai/tasks/T319ff2af42536) stays standalone because parent changes are unavailable through this API. Epic 1 depends on it; child tasks reuse it rather than duplicate it.

## Implementation order
Start with scaffolding + board verification, protocol vectors, USB loopback and one-guest HID. Prove three-guest BLE as an early feasibility gate. Then capture/routing/recovery, desktop pairing/tray, mappings/host edges/device UI, packaging. Optional helper is M3 after the base product passes M2.

## Task graph
| Task | Epic | Dependencies |
|---|---|---|
| [M0 · Verify arrived board, schematic and boot recovery](https://orchestrator.doubtech.ai/tasks/T319ffd566b306) | 1 | None |
| [M0 · Freeze protocol schema and independent golden vectors](https://orchestrator.doubtech.ai/tasks/T319ffd58963ee) | 1 | [scaffold](https://orchestrator.doubtech.ai/tasks/T319ff2af42536) |
| [M0 · Prove USB CDC transport and host discovery](https://orchestrator.doubtech.ai/tasks/T319ffd5b3877d) | 1 | [hw](https://orchestrator.doubtech.ai/tasks/T319ffd566b306), [proto](https://orchestrator.doubtech.ai/tasks/T319ffd58963ee) |
| [M0/M1 · Implement one composite BLE HID guest](https://orchestrator.doubtech.ai/tasks/T319ffd5d605ee) | 2 | [hw](https://orchestrator.doubtech.ai/tasks/T319ffd566b306), [scaffold](https://orchestrator.doubtech.ai/tasks/T319ff2af42536) |
| [M1 · Implement bonding, pairing window and guest identity](https://orchestrator.doubtech.ai/tasks/T319ffd5f89457) | 2 | [hid](https://orchestrator.doubtech.ai/tasks/T319ffd5d605ee), [usb](https://orchestrator.doubtech.ai/tasks/T319ffd5b3877d) |
| [M1 · Implement routing generations and firmware fail-local lease](https://orchestrator.doubtech.ai/tasks/T319ffd61a0038) | 2 | [hid](https://orchestrator.doubtech.ai/tasks/T319ffd5d605ee), [usb](https://orchestrator.doubtech.ai/tasks/T319ffd5b3877d) |
| [M0/M2 · Validate three concurrent HID guests and isolated notifications](https://orchestrator.doubtech.ai/tasks/T319ffd63a102a) | 2 | [pair](https://orchestrator.doubtech.ai/tasks/T319ffd5f89457), [fwroute](https://orchestrator.doubtech.ai/tasks/T319ffd61a0038) |
| [M1 · Build Windows capture, suppression and raw mouse pipeline](https://orchestrator.doubtech.ai/tasks/T319ffd65a83e7) | 3 | [scaffold](https://orchestrator.doubtech.ai/tasks/T319ff2af42536), [proto](https://orchestrator.doubtech.ai/tasks/T319ffd58963ee) |
| [M1 · Implement transactional switching and physical hotkeys](https://orchestrator.doubtech.ai/tasks/T319ffd6aae57d) | 3 | [capture](https://orchestrator.doubtech.ai/tasks/T319ffd65a83e7), [fwroute](https://orchestrator.doubtech.ai/tasks/T319ffd61a0038) |
| [M1 · Restore local control on crashes, suspend and link loss](https://orchestrator.doubtech.ai/tasks/T319ffd6cf3735) | 3 | [switch](https://orchestrator.doubtech.ai/tasks/T319ffd6aae57d) |
| [M1 · Implement desktop shell, tokens and navigation](https://orchestrator.doubtech.ai/tasks/T319ffd6f1c23a) | 4 | [scaffold](https://orchestrator.doubtech.ai/tasks/T319ff2af42536) |
| [M1 · Build device setup and guest pairing wizard](https://orchestrator.doubtech.ai/tasks/T319ffd710e63d) | 4 | [shell](https://orchestrator.doubtech.ai/tasks/T319ffd6f1c23a), [pair](https://orchestrator.doubtech.ai/tasks/T319ffd5f89457) |
| [M2 · Persist guest profiles and live connection model](https://orchestrator.doubtech.ai/tasks/T319ffd730b5d0) | 4 | [shell](https://orchestrator.doubtech.ai/tasks/T319ffd6f1c23a), [pair](https://orchestrator.doubtech.ai/tasks/T319ffd5f89457) |
| [M1 · Build system dashboard, tray menu and switch overlay](https://orchestrator.doubtech.ai/tasks/T319ffd75176da) | 4 | [shell](https://orchestrator.doubtech.ai/tasks/T319ffd6f1c23a), [switch](https://orchestrator.doubtech.ai/tasks/T319ffd6aae57d), [profiles](https://orchestrator.doubtech.ai/tasks/T319ffd730b5d0) |
| [M2 · Build physical mapping engine and held-key ledger](https://orchestrator.doubtech.ai/tasks/T319ffd77513e5) | 5 | [switch](https://orchestrator.doubtech.ai/tasks/T319ffd6aae57d) |
| [M2 · Add explicit Cmd-to-Ctrl and Windows-to-Mac presets](https://orchestrator.doubtech.ai/tasks/T319ffd7a751dd) | 5 | [mapcore](https://orchestrator.doubtech.ai/tasks/T319ffd77513e5) |
| [M2 · Build keymap editor, conflict checks and simulator](https://orchestrator.doubtech.ai/tasks/T319ffd7c99279) | 5 | [profiles](https://orchestrator.doubtech.ai/tasks/T319ffd730b5d0), [presets](https://orchestrator.doubtech.ai/tasks/T319ffd7a751dd) |
| [M2 · Model monitor topology and exposed-edge portals](https://orchestrator.doubtech.ai/tasks/T319ffd7ec35d6) | 6 | [capture](https://orchestrator.doubtech.ai/tasks/T319ffd65a83e7), [profiles](https://orchestrator.doubtech.ai/tasks/T319ffd730b5d0) |
| [M2 · Implement dwell, drag guards and host-to-guest crossing](https://orchestrator.doubtech.ai/tasks/T319ffd80aa0d5) | 6 | [topology](https://orchestrator.doubtech.ai/tasks/T319ffd7ec35d6), [switch](https://orchestrator.doubtech.ai/tasks/T319ffd6aae57d) |
| [M2 · Build screen layout editor and capability explanations](https://orchestrator.doubtech.ai/tasks/T319ffd829c223) | 6 | [shell](https://orchestrator.doubtech.ai/tasks/T319ffd6f1c23a), [edge](https://orchestrator.doubtech.ai/tasks/T319ffd80aa0d5) |
| [M0/M2 · Bring up LVGL screen and runtime controls](https://orchestrator.doubtech.ai/tasks/T319ffd849934e) | 7 | [hw](https://orchestrator.doubtech.ai/tasks/T319ffd566b306), [scaffold](https://orchestrator.doubtech.ai/tasks/T319ff2af42536) |
| [M2 · Build on-device target, pairing and recovery screens](https://orchestrator.doubtech.ai/tasks/T319ffd86934b8) | 7 | [bspui](https://orchestrator.doubtech.ai/tasks/T319ffd849934e), [fwroute](https://orchestrator.doubtech.ai/tasks/T319ffd61a0038), [pair](https://orchestrator.doubtech.ai/tasks/T319ffd5f89457) |
| [M2 · Implement manifest-checked USB update and recovery](https://orchestrator.doubtech.ai/tasks/T319ffd889406d) | 7 | [usb](https://orchestrator.doubtech.ai/tasks/T319ffd5b3877d), [deviceui](https://orchestrator.doubtech.ai/tasks/T319ffd86934b8), [watchdog](https://orchestrator.doubtech.ai/tasks/T319ffd6cf3735) |
| [M3 · Design and implement authenticated helper sessions](https://orchestrator.doubtech.ai/tasks/T319ffd8b2e7fd) | 8 | [profiles](https://orchestrator.doubtech.ai/tasks/T319ffd730b5d0), [proto](https://orchestrator.doubtech.ai/tasks/T319ffd58963ee) |
| [M3 · Implement Windows/macOS cursor and topology helpers](https://orchestrator.doubtech.ai/tasks/T319ffd8d76283) | 8 | [helptrust](https://orchestrator.doubtech.ai/tasks/T319ffd8b2e7fd) |
| [M3 · Implement coordinated bidirectional portal handoff](https://orchestrator.doubtech.ai/tasks/T319ffd90396b2) | 8 | [helpobserve](https://orchestrator.doubtech.ai/tasks/T319ffd8d76283), [edge](https://orchestrator.doubtech.ai/tasks/T319ffd80aa0d5) |
| [M3 · Build seamless-mode setup and degraded states](https://orchestrator.doubtech.ai/tasks/T319ffd926a14a) | 8 | [helpcross](https://orchestrator.doubtech.ai/tasks/T319ffd90396b2), [layoutui](https://orchestrator.doubtech.ai/tasks/T319ffd829c223) |
| [M0/M1 · Add deterministic CI and state-machine fault tests](https://orchestrator.doubtech.ai/tasks/T319ffd94c0245) | 9 | [proto](https://orchestrator.doubtech.ai/tasks/T319ffd58963ee), [scaffold](https://orchestrator.doubtech.ai/tasks/T319ff2af42536) |
| [M2 · Execute mixed-guest hardware and failure matrix](https://orchestrator.doubtech.ai/tasks/T319ffd96d57fa) | 9 | [multi](https://orchestrator.doubtech.ai/tasks/T319ffd63a102a), [watchdog](https://orchestrator.doubtech.ai/tasks/T319ffd6cf3735), [mapui](https://orchestrator.doubtech.ai/tasks/T319ffd7c99279), [layoutui](https://orchestrator.doubtech.ai/tasks/T319ffd829c223), [deviceui](https://orchestrator.doubtech.ai/tasks/T319ffd86934b8) |
| [M2 · Benchmark input latency and resource usage](https://orchestrator.doubtech.ai/tasks/T319ffd98e2387) | 9 | [multi](https://orchestrator.doubtech.ai/tasks/T319ffd63a102a), [edge](https://orchestrator.doubtech.ai/tasks/T319ffd80aa0d5), [deviceui](https://orchestrator.doubtech.ai/tasks/T319ffd86934b8) |
| [M2 · Package Windows app and firmware release](https://orchestrator.doubtech.ai/tasks/T319ffd9b141bb) | 9 | [ci](https://orchestrator.doubtech.ai/tasks/T319ffd94c0245), [compat](https://orchestrator.doubtech.ai/tasks/T319ffd96d57fa), [latency](https://orchestrator.doubtech.ai/tasks/T319ffd98e2387), [update](https://orchestrator.doubtech.ai/tasks/T319ffd889406d) |
| [M3 · Validate helper trust and seamless crossing matrix](https://orchestrator.doubtech.ai/tasks/T319ffd9d44215) | 9 | [helpui](https://orchestrator.doubtech.ai/tasks/T319ffd926a14a), [compat](https://orchestrator.doubtech.ai/tasks/T319ffd96d57fa) |

## Review status
Design/specification work completed; implementation, flashing, hardware validation and release are not claimed complete. No worker sessions were dispatched and no repository commit was made by this design turn.

