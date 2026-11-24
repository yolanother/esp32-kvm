---
name: esp32-kvm-firmware
description: Use when building the ESP32 KVM firmware, flashing the ESP32-S3 board, or listing its serial port. Covers tools/firmware.ps1, the no-erase and quit-the-desktop-app rules, and build flakes.
---

# ESP32 KVM firmware build and flash

Use `tools/firmware.ps1` for every firmware build or flash. It finds ESP-IDF (`IDF_PATH`, else `D:\esp\esp-idf-v5.5.1`, else `%USERPROFILE%\esp\esp-idf-v5.5.1`), so no ESP-IDF shell is needed. Run it with PowerShell from any directory:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File tools/firmware.ps1 build
pwsh -NoProfile -ExecutionPolicy Bypass -File tools/firmware.ps1 ports
pwsh -NoProfile -ExecutionPolicy Bypass -File tools/firmware.ps1 flash [-Port COMn]
pwsh -NoProfile -ExecutionPolicy Bypass -File tools/firmware.ps1 flash -Full [-Port COMn]
```

Add `-DryRun` to any action to print the resolved `idf.py` command lines without running them.

## When

- Build after any change under `firmware/` and before reporting firmware work done. A successful build prints the app image path and size.
- Flash only when the change needs hardware verification and the owner has authorized it for the task.
- `ports` lists serial ports with USB id `303A:1001` and whether each can be opened exclusively.

## Rules

- Never erase flash. The script passes no erase option; do not add one, and do not run `idf.py erase-flash` or `esptool erase` by hand. Pairing and device state live in flash.
- `flash` runs `app-flash` (application image only). Use `-Full` only when the bootloader or partition table changed.
- The desktop app owns the board's serial port. If `flash` reports the port busy, ask the owner to quit the desktop app; never kill the process yourself.
- With several boards attached, pass `-Port COMn` explicitly.

## Build flake

A parallel build once hit an Xtensa GCC segmentation fault in an unused RGB LCD source. If a build fails that way, retry the build once before investigating; report the real error if the retry also fails.

## Related

- Recovery procedure: `docs/Designs/recovery`
- Release packaging: `docs/Designs/release`
- CI is the second gate: a local build passing does not replace the CI firmware build.
