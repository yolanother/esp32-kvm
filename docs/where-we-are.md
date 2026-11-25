<!-- Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE. -->
# Where we are

Updated 2026-11-25 (local time of the owner's machine, Windows 11). Written on branch `feature/usb-hid-device` at the stage 3 design commit `5588ba1`. Nothing described as "built" here has run on the board unless stated.

## Objective (owner's words, condensed)

A device-centric KVM: the ESP32-S3 board is the central mechanism. The owner's own keyboard and mouse pair to the board and are shared to up to three BLE guests and to the PC over USB HID, so their Bluetooth connection is preserved in the device. The PC-app-driven path (capture on the PC, control over USB) must keep working as another input source. Standing direction: move the USB link and input capture into a system service with the desktop app as a configuration frontend. "Complete all three phases and any required fixes along the way."

Acceptance: stage 1 (library), stage 2 (device-owned BLE input), stage 3 (USB HID + control on one cable), each built and functionally installed on the hardware.

## Status by stage

| Stage | State | Evidence |
| --- | --- | --- |
| 1 `doubtech_hid` BLE HID library | Done, merged to `main` at `d8b78e2`, tags `checkpoint/pre-hid-library`, `checkpoint/hid-library`; flashed to the board 2026-11-24 and verified (both guests reattached, NVS untouched). | root README verification table |
| Desktop capture-fault fix | Committed on `main` as `08477be` and **installed** (MSI 0.1.1, exe 10,000,384 bytes, 2026-11-25 11:54). Whether it cures the failure to recover after the screens sleep is **unverified**: needs one screen-sleep test by the owner. | commit message; install logs in `%TEMP%\esp32kvm-install2.log` |
| 2 Device-owned BLE input (central + input hub + touch pairing + USB-off display) | **Implemented and reviewed, not merged, not flashed.** Branch `feature/device-input-central`, head `75fc706`, 32 commits ahead of `main`. All host-side suites and guards pass and `tools/firmware.ps1 build` succeeds (app image 1,044,064 bytes) as run by the controller on 2026-11-25. No Bluetooth code has run on a board. | spec/plan under `docs/superpowers/`; ledger `.superpowers/sdd/2026-11-25-device-input-central/progress.md` (git-ignored) |
| 3 USB HID composite + CDC | **Design and plan written only** (`feature/usb-hid-device`, `5588ba1`). No code. | `docs/superpowers/specs/2026-11-25-usb-hid-device-design.md`, `.../plans/2026-11-25-usb-hid-device.md` |

## Blocked on the owner

1. Approve merging `feature/device-input-central` into `main` and pushing.
2. Approve flashing stage 2 (quit the desktop app first; `tools/firmware.ps1 flash`, never erase), then run `docs/Designs/doubtech-hid/owner-checklist-device-input.md`.
3. Stage 3 safety gate: prove BOOT-button ROM download mode on this board (never tested) before any TinyUSB image is flashed; approve the `firmware/dependencies.lock` change for esp_tinyusb (task 10 of the stage 3 plan); decide whether the firmware skill file may be edited.

## Open defects and risks

- **Random return to the PC from a guest** (owner report): unresolved. Strong candidates in pre-existing firmware (`transport_usb_serial_jtag.c` slot-sample emergency release; `hid_guest_rpc.c` timeout disconnect; ARM answered NOT_READY ends the host session, `crates/host-actor/src/lib.rs` ~1241). Details in the memory note `random-return-to-pc-investigation`. Not fixed.
- Stage 2: an input pairing scan can run up to 30 s at 50 % duty while the PC route is armed (cancel is deferred to avoid blocking the worker). Recommended fix: fire-and-forget cancel event.
- Stage 2: a sleeping keyboard can take 25-50 s to reconnect (accepted trade-off for guest airtime); RPA reconnect, airtime, worker stack (8 KB, unmeasured) and the LVGL layout are unmeasured.
- CI: `rust` (cargo fmt) and `release-inputs` (version 0.1.0 vs 0.1.1) jobs were red before this work and are unfixed.

## Next work, in order

1. Owner decisions above; then flash stage 2 and run the checklist.
2. Fix the PC-path transient-timeout hazards (the random return) with reason logging.
3. Stage 3 phase A (agent-only tasks 1-5 of the stage 3 plan), then the recovery drill gate.
4. System-service architecture (USB link and capture out of the desktop app).
