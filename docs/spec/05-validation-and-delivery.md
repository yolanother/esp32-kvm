# Validation & delivery
All performance figures are targets until measured on the shipped board. Record OS build, Bluetooth adapter, board revision, SDK versions, connection intervals, radio environment and test tools.

## Release gates
M0: inspect hardware and schematic, establish reproducible toolchains, prove CDC+BLE+display can coexist. A three-guest spike determines supported connection count before UI promises it.
M1: one Windows 11 host, Windows/macOS guest basic HID, hotkey return and all failure recovery tests pass.
M2: three concurrent mixed-OS guests sustained for 2 hours; 1,000 selection cycles with zero wrong-target input; mapping/layout/tray/device/installer complete.
M3: helper trust, stale telemetry, coordinate transfer and degraded-mode tests pass on Windows and macOS guests.

## Test matrix
Host: Windows 11 x64, multiple monitors with mixed 100/150/200% DPI, negative origins, portrait display, monitor unplug, laptop dock/undock. macOS/Linux controlling hosts are later.
Guest: Windows 11, supported macOS releases on actual available hardware, Linux BlueZ desktop as compatibility target; mobile/tablet opportunistic, not release blocking. Track actual versions, not generic 'all platforms' claims.
BLE: 1/2/3 guests; encrypted bonding; reboot/resume; bonded reconnect; full slots; renamed guest; privacy address rotation; forget/re-pair; one guest asleep; long connection intervals; all three subscribing; CCCD isolation.
Input: left/right modifiers, US/non-US layout, dead keys/IME, AltGr, consumer/media controls, 6KRO overflow, mouse 5 buttons, horizontal/vertical scroll, high-polling mouse, repeat, chords, mapping collisions.
Switch: held modifier, drag, repeated hotkey, button and UI simultaneous requests, offline selection, hotkey conflict, guest disconnect between prepare/arm, emergency release.
Recovery: kill UI, stall routing core, unplug USB, reset firmware, force protocol mismatch, overflow input queues, CRC errors, BLE backpressure, sleep/lock/UAC desktop, update interruption.
Edge: every exposed edge, inner seams, corners, dwell/cooldown, fullscreen pause, disconnected guest, no helper, helper trust loss, rotated helper display and remote local mouse.
Privacy: logs contain no key text; exports exclude bond keys/passkeys; pairing window expires; unrelated peer cannot subscribe to input; helper refuses untrusted host.

## Instrumentation
Separate host capture→USB accept, firmware BLE enqueue, and measured guest-observed latency. USB ACK cannot establish end-to-end latency. Use explicit controlled guest test harness or external camera/instrumentation; do not require it during ordinary use. Publish p50/p95/max and sample counts. Compare display enabled/disabled and 1/3 guest connections.
Targets: USB p95≤5ms, end-to-end p95≤30ms, connected selection p95≤100ms, capture callback budget comfortably below 1ms. Measure CPU/memory/queue high-water marks. Hardware tests must be marked unrun until actual execution.

## Delivery
CI: Rust format/lint/unit tests, TypeScript typecheck/UI tests, firmware build, independent protocol vector check, state-machine fault tests; Windows native integration smoke suite. Store dependency licenses, pinned toolchains, board manifest, recovery instructions, binaries and checksums. Produce Windows installer (code signing if credentials available; otherwise label unsigned), firmware image and known-limits release notes.
Upgrade must release input first, preserve profiles/bonds by default and verify reconnect/compatibility before re-arming. Provide manual BOOT USB recovery.

## Risk register
R1 True 3-guest HID support vs generic BLE limits → M0 spike; ship fewer live slots with explicit UI if tests fail.
R2 Secure desktop/global hooks limitations → fail local and tested supported-desktop boundary.
R3 Guest cursor unknowable in BLE-only mode → hotkey return by default, helper for exact crossing.
R4 Hardware variant/pins mismatch → inspect label/schematic/BSP before firmware.
R5 Lost releases on link loss → zero-state on reconnect plus disarm; delivery after radio loss cannot be guaranteed.
R6 Keymap modifier leakage/collisions → state ledger, reference counts, remap-once and test matrix.
R7 Power/boot buttons have hardware side effects → reserve PWR, runtime-only BOOT behavior.
R8 OS reconnect scheduling/connection intervals → report connecting honestly, avoid instantaneous reconnection promises.

