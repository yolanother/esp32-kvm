# Native local-control recovery

The Windows capture gate is local by default. A guest route is armed only after the firmware ARM acknowledgment, a physical all-up observation, a fresh capture-pump heartbeat, and an OS key-state snapshot. A route carries its confirmed generation; stale events are discarded.

An independent watcher checks the capture pump every 250 ms and the serialized host actor every 500 ms while a route is armed. Missing either lease records a terminal capture fault and immediately disarms the hook gate. The actor renews its lease during `poll`; the Windows timer renews the capture-pump lease. Both-Ctrl held for one second disarms inside the hook worker, without waiting for the actor to consume its emergency event.

`HostActor::suspend` and `shutdown` disarm before attempting a best-effort `RELEASE_ALL`. The actor then enters a terminal state, clears held keyboard and button state, and ignores stale ACKs or input. Drop also disarms. A new verified USB session and physical all-up observation are required after resume or reconnect. USB EOF, protocol errors, capture faults, and peer heartbeat expiry use the same fail-local path. Firmware heartbeat expiry is the final remote-output release when USB writes are impossible.

The capture service saves the host cursor position when guest capture arms. Explicit disarm, a system-transition call, drop, or the independent watcher restores that position after the gate becomes local. This service never calls `ClipCursor` or `ShowCursor`, so it does not change another application's clip region or visibility counter. The message-only capture window handles ending-session and suspend/resume messages if delivered; the native desktop lifecycle owner must also call `CaptureService::system_transition` and `HostActor::suspend` for lock, logoff, suspend, and resume notifications. A message-only window is not a reliable recipient of broadcast notifications.

The capture worker exits on `WM_QUIT` and unhooks. Service destruction waits at most 250 ms for a stalled worker, with the gate already local; process termination also removes that process's low-level hooks. Cursor restoration requires a running process and accessible desktop. If the process is killed, restart reconciliation must restore cursor state using a separately persisted snapshot; this is not implemented here.

## USB mode recovery (board side)

Separate from the host-side gate above: the board can run its USB device as serial (`303A:1001`, the default) or as a composite HID plus CDC device (interim `303A:4009`), and composite must never cost the owner the ability to reflash. Serial is the default; composite needs a touchscreen action and a build with composite support. Paths back to serial, none of which erases flash: replug three times within 30 s each; press BOOT within the 1.5 s escape window after plugging in; touchscreen **USE SERIAL MODE** or **RESTART IN SERIAL MODE** (USB MODE row of INPUT DEVICES, needs a paired guest); `tools/firmware.ps1 usb-serial`; BOOT held while plugging in (ROM download mode); a rescue build with `CONFIG_KVM_USB_FORCE_SERIAL=y`. `tools/firmware.ps1 flash` on a composite board writes a one-shot serial-once magic string and waits for the serial board before flashing. The board-side decision order is in `docs/superpowers/specs/2026-11-25-usb-hid-device-design.md` (Decisions 6 to 10), the README Recovery section lists the steps, and `docs/hardware/usb-recovery-drill.md` holds the owner drills. All of it is unverified on hardware until those drills are run and recorded in `docs/hardware/board-bringup.md`.

## Verification

- `cargo test --offline -p esp32-kvm-platform-windows --test recovery`: fake-clock actor and pump stalls, stale-pump arm guard, system transition, and both-Ctrl emergency.
- `cargo test --offline -p esp32-kvm-host-actor --test actor`: terminal suspend/shutdown/drop and existing ACK, timeout, USB EOF, and input safety cases.
- `cargo check --offline -p esp32-kvm-platform-windows --target x86_64-pc-windows-msvc`: native API compilation.

Native Windows gate remains pending: separately kill UI and core processes, stall each worker, unplug USB, lock/unlock, suspend/resume, and verify physical keyboard/mouse pass-through, cursor position, hook removal, and firmware output release. This code has not been run against the attached board.
