<!-- Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE. -->
# Handoff packet

Updated 2026-11-25. Snapshots of processes and device state below are dated; recheck live state before relying on them.

## Checkout and branches

- Repo `C:\Users\yolan\Workspace\Hardware\esp32-kvm`, currently on `feature/usb-hid-device` (`5588ba1`, docs only on top of stage 2).
- `main` = `d8b78e2` plus the capture-fault fix `08477be`. Stage 2 = `feature/device-input-central` head `75fc706` (32 commits over `main`; merge base `08477be`). Stage 3 design = `feature/usb-hid-device` (`5588ba1`). Nothing is pushed except what stage 1 pushed earlier (`main` `d8b78e2`, tags `checkpoint/pre-hid-library`, `checkpoint/hid-library`). `08477be` and the branches are local only.
- Untracked, not mine to commit: `AGENTS.md`, `CLAUDE.md`, `.orchestrator/`, `.codex/`, `.opencode/`, `.claude/` additions from `orch sync`; `.superpowers/` (git-ignored scratch). `firmware/dependencies.lock` manifest hash was committed on the stage 2 branch (`fea8f35`).
- Session-local `.claude/settings.local.json` (git-ignored) sets `env.PATH` so hooks find node and python; the hooks failed earlier because the session PATH lacked them. Unconfirmed that it takes effect without a session reload.

## Evidence and references

- Stage 2 spec `docs/superpowers/specs/2026-11-25-device-input-central-design.md`; plan `docs/superpowers/plans/2026-11-25-device-input-central.md` (12 tasks, all complete); docs `docs/Designs/doubtech-hid/central.md`, `docs/Designs/input-hub/README.md`; owner checklist `docs/Designs/doubtech-hid/owner-checklist-device-input.md` (NONE of it run).
- Subagent ledger with every ruling, deferred minor and unverified item: `.superpowers/sdd/2026-11-25-device-input-central/progress.md`; per-task reports `task-N-report.md`, `final-fix-report.md` in the same directory (git-ignored; copy if the workspace is cleaned).
- Stage 3 spec/plan as above. Memory notes under `C:\Users\yolan\.claude\projects\C--Users-yolan-Workspace-Hardware-esp32-kvm\memory\` (`random-return-to-pc-investigation`, `firmware-build-flash-workflow`, `system-service-architecture`, `hid-library-deferred-items`, `ci-red-baseline-2026-11-24`).

## Decisions and constraints (owner and controller)

- Owner: stage order free (controller chose 2 before 3); PC capture stays as an input source (option A); one combined device named "DoubTech HID"; never erase flash or NVS; quit the desktop app via tray "Quit and release" before flashing; flashing, merging and pushing need the owner's approval each time.
- Controller rulings on the stage 2 branch (all in the ledger): input hub is firmware-internal (no product-name guard); hub never emits while the router is armed and yields inside the router's arm callback (never answers BUSY, because host-actor treats any ARM NACK as fatal); input pairing gated on `router.armed`, not `session_open` (the PC app is always connected); no-session screen is identical to the session screen except dim and footer; only the This PC card dims; default hub target = first ready guest until an explicit choice; one global reconnect schedule (10 s attempt, 15 s gap + 0-2 s jitter, round robin).
- Windows install quirk found: reinstalling the same MSI version does not replace files, and the new MSI has a different product code, so `msiexec /x <msi>` fails with 1605; uninstall by the registered product code or bump the version.

## Commands and results (controller runs, 2026-11-25)

- Host suites (PowerShell, `pwsh -NoProfile -ExecutionPolicy Bypass -File ...`): `firmware/tests/{display,router,pairing,integration}/run-msvc.ps1`, `firmware/components/{doubtech_hid,doubtech_hid_central,input_hub}/tests/run-msvc.ps1`, the two `check-product-names.ps1`, `check-api-docs.ps1`, `tools/tests/test-firmware-script.ps1`: all exit 0.
- `tools/firmware.ps1 build`: exit 0, app image 1,044,064 bytes. Cargo test of the capture-fault fix: `cargo test -p esp32-kvm-desktop reconnect_clears` passed. Full `cargo test --workspace` and CI were not run for this work.
- Not run: any flash, any hardware behaviour, `flash -Full`, the stage 2 owner checklist, the screen-sleep recovery test of the installed fix.

## Processes and resources (dated 2026-11-25 ~12:50)

- Installed desktop app running from `C:\Program Files\ESP32 KVM\esp32-kvm-desktop.exe` (restarted 11:54:27); it holds COM4. No subagents were left running when this was written.

## Pending questions and approvals

Merge/push stage 2; flash stage 2; BOOT-button ROM download drill; `firmware/dependencies.lock` approval for esp_tinyusb; permission to edit `.claude/skills/esp32-kvm-firmware/SKILL.md`; dedicated USB PID before any release.

## Stage 3 status (2026-11-25 evening)

- Branch `feature/usb-hid-device`; tasks 1-8 of `docs/superpowers/plans/2026-11-25-usb-hid-device.md` are implemented and reviewed (each with task review and fix rounds; see ledger). Task 6's fix round was not separately re-reviewed (controller ruling, recorded in the ledger). Deferred minors and the binding carry-overs for task 10 and task 12 are in `.superpowers/sdd/2026-11-25-usb-hid-device/progress.md`; copy them out before that workspace is cleaned.
- **Stop point (owner's choice, "build it, you run the drill"):** task 9 is owner-only. Procedure: `docs/hardware/usb-recovery-drill.md` (prove BOOT-button ROM download mode and the serial-once flag evidence, read-only `espefuse summary`). No HID image may be flashed before it passes and is recorded in `docs/hardware/board-bringup.md`. Task 10 needs the owner's explicit approval of the `firmware/dependencies.lock` change (esp_tinyusb). `.claude/skills/esp32-kvm-firmware/SKILL.md` was deliberately not edited (owner has not agreed).
- Controller re-ran all 11 suites and guards plus `tools/firmware.ps1 build` on this branch (head before this commit: all exit 0, app 1,051,952 bytes), 2026-11-25 evening. Re-run before resuming.
- Stage 2 is flashed on the board (2026-11-25 17:17); owner confirmation of screen/guests and the checklist is outstanding.

## Next safe actions

1. Ask the owner for the approvals above. Completion: merge commit on `main` pushed, or a recorded refusal.
2. After approval: quit the app, `pwsh -File tools/firmware.ps1 flash -Port COM4`, relaunch the app from `C:\Program Files\ESP32 KVM\`, run the checklist item by item, record PASS/FAIL.
3. Fix the random return to the PC: make the 1 s slot sample and timed-out guest RPCs tolerant of transient `busy`/timeouts, retry ARM once, add reason logging to firmware and app; proof = a soak with a guest routed for an hour and no unexplained return.
4. Stage 3 phase A via the stage 3 plan (agent-only tasks 1-5); stop at the owner drill gate (task 9).
5. Move USB link and capture into a system service (see memory note `system-service-architecture`).
