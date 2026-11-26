# USB recovery drills (owner only)

Two physical drills for the USB HID device work (spec: `docs/superpowers/specs/2026-11-25-usb-hid-device-design.md`, Decisions 6 to 12; plan tasks 9 and 11). **Every procedure here is unverified until the owner runs it on the board.** Nothing below has been run. Do not flash a composite image until Drill 1 passes and is logged.

Ground rules for both drills:

- Never erase flash. No step below uses an erase option, and `tools/firmware.ps1` never passes one. Guest bonds live in NVS and must survive every step; check that guests reconnect.
- Quit the desktop app from the tray (**Quit and release**) before any step that opens the board's port. It owns the port.
- Replace `COMn` with the board's current port. The COM number can change when the board re-enumerates.
- Record every result, including failures and surprises, in `docs/hardware/board-bringup.md` under "Recovery status", with the date, the image SHA-256 and the exact tool output. Silence or a partial run is not a pass.

## Drill 1 (plan task 9): prove ROM download and the serial-side paths

Image under test: the phase B image (mode store, escape window, magic strings and touchscreen screen, composite support still off). The drill ends with the gate for task 10: **steps 1 and 2 must pass before any TinyUSB image is flashed.**

### Step 1: BOOT + plug reaches ROM download mode

1. Quit the desktop app. Unplug the board.
2. Hold the BOOT button, plug in USB, then release BOOT.
3. Expected in Device Manager: a USB Serial Device on a COM port with hardware id `USB\VID_303A&PID_1001` (the ROM download device). The screen is not expected to show the app.
4. Run (read-only; stays in the bootloader):

   ```powershell
   esptool --chip esp32s3 --port COMn --before no_reset --after no_reset chip_id
   ```

   Expected: chip identified and the output says it is staying in the bootloader.
5. Read the security configuration (read-only, burns nothing):

   ```powershell
   esptool --chip esp32s3 --port COMn --before no_reset --after no_reset get_security_info
   espefuse --chip esp32s3 --port COMn --before no_reset summary
   ```

   Record `DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE`, `DIS_DOWNLOAD_MODE` and `USB_PHY_SEL`, and the flash encryption and secure boot lines. Expected: USB download is not disabled (`DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE` 0, `DIS_DOWNLOAD_MODE` 0). If it is disabled, stop: the recovery floor does not exist and no TinyUSB image may be flashed. Never run an `espefuse burn` command.
6. Unplug and replug without BOOT. Expected: the app runs normally and guests reconnect (bonds intact).

Record in board-bringup.md: Device Manager id and COM number in ROM mode, the `chip_id` bootloader line, the three eFuse values, whether the app and bonds returned.

### Step 2: flash from ROM mode

1. BOOT + plug again as in step 1.
2. `pwsh -NoProfile -ExecutionPolicy Bypass -File tools/firmware.ps1 flash -Port COMn` (app-only; the script does not erase).
3. Replug without BOOT. Expected: the new app runs and bonds are intact.

Record: the image SHA-256, the esptool hash-verified lines, and the bond check.

### Step 3: serial-side paths on the phase B image

1. Open the touchscreen USB mode screen: INPUT DEVICES (keyboard button left of the gear), 4th row **USB MODE**. The row is only shown with at least one paired guest. Expected title `USB MODE: SERIAL` and reason `Not in this build` (this image has no composite support, so "not built" wins over every other reason, including serial-once).
2. Capture the boot snapshot before the restart. The board's logs go to UART0, which is not wired, so the evidence is the diagnostics dump written on request. It needs a build with `CONFIG_DOUBTECH_HID_GATT_TRACE=y` (without it the dump request is ignored). The dump is requested by the raw bytes `"\0KVM-DIAG-LOGS\n"` on the port; `tools/read-ble-trace.py` writes them, waits for `KVM-TRACE-END` and prints the result (it releases input and closes the USB session):

   ```powershell
   python tools/read-ble-trace.py --port COMn
   ```

   The header has one line `usb_boot mode= reason= once= escape= abnormal= rst= stored= unconf=`. Expected now: reason 2 (NOT_BUILT) and `once=0`.
3. Request the serial-once restart (21 bytes, leading 0 byte), by either route. `tools/firmware.ps1` writes it only as part of `flash` on a composite board, so on this serial image use one of:
   - The touchscreen row **RESTART IN SERIAL MODE** (tap twice within 5 s; the footer reads `Restarting...`, or `Not in this build` if the image refuses it; record what it says).
   - A raw write of the bytes `00 4B 56 4D 2D 55 53 42 2D 53 45 52 49 41 4C 2D 4F 4E 43 45 0A` to the port at 115200 8N1 with DTR and RTS off (the settings the script uses; `tools/firmware.ps1 flash -DryRun` with a fake composite port prints the exact write).
4. Expected: the board restarts and comes back as `303A:1001`; the screen still says `Serial / Not in this build`. Run the dump again. Expected: `usb_boot` reason still 2 (NOT_BUILT), **`once=1`** and `rst=3` (software reset). That `once=1` is the proof the RTC flag survived `esp_restart()`.
5. Unplug and replug normally, dump again. Expected: `once=0`.
6. Do not induce a panic.

Record: the `usb_boot` lines before (`once=0`) and after (`once=1 rst=3`) the restart, and the later `once=0` line, with the exact dump header text.

## Drill 2 (plan task 11): first composite image

Prerequisite: Drill 1 steps 1 and 2 passed and are recorded. Image: the phase C image (composite support on; the stored mode is absent, so it first boots serial). The default PID is the interim `303A:4009`; this is not a Windows driver-binding claim until you record it.

Revert paths use these exact names, so keep this list handy: replug three times; BOOT in the escape window; `tools/firmware.ps1 usb-serial`; touchscreen **USE SERIAL MODE**; BOOT + plug (ROM mode); rescue build (`CONFIG_KVM_USB_FORCE_SERIAL=y`) flashed from ROM mode.

### Step 1: flash by the known method

1. Quit the desktop app. `pwsh -NoProfile -ExecutionPolicy Bypass -File tools/firmware.ps1 flash`.
2. Expected: the board is still `303A:1001` (mode absent), serial mode, screen reason `Serial is the default`.

### Step 2: enable composite

1. On the USB mode screen tap **USE USB HID MODE** twice within 5 s (the second tap reads `TAP AGAIN TO CONFIRM` first).
2. Expected after the 1.5 s escape window: the old COM port disappears and Device Manager shows a composite device with "HID Keyboard Device", "HID-compliant mouse", "HID-compliant consumer control device" and "USB Serial Device (COMx)" whose hardware id contains `MI_02` (`USB\VID_303A&PID_4009&MI_02`). Screen title `USB MODE: USB HID`.
3. Record: VID/PID, the COM number, and any "unknown device" entry or Code 10.

### Step 3: CDC handshake

`cargo run --offline -p esp32-kvm-host-actor --example probe_serial -- COMx`. Expected: CAPS board `esp32-kvm-s3` and a SESSION_OPEN acknowledgement over CDC. Record the output.

### Step 4: every revert path

Re-enable composite (step 2) between each; after each expect a `303A:1001` serial board:

1. **BOOT in the escape window.** Replug, then press BOOT within 1.5 s of plugging in (plug first, press after; pressing before power is ROM mode). Expected: serial this boot, reason `BOOT button: serial this boot`.
2. **`tools/firmware.ps1 usb-serial`** (preview with `-DryRun` first). Expected: serial, persisted across a replug.
3. **Touchscreen USE SERIAL MODE** (tap twice). Expected: serial, persisted.
4. **Replug three times**, each within 30 s of the previous boot, with no button. Expected: serial and reason `Turned off after failed starts`.
5. **`tools/firmware.ps1 flash`** while composite. Expected: it prints the serial-once write and the 15 s wait, the board comes back as `303A:1001`, the flash runs, and the board returns to composite mode.
6. **BOOT + plug** while the stored mode is composite. Expected: `303A:1001` ROM mode, and an app-only flash from there works.
7. **Rescue build.** From ROM mode flash an image built with `CONFIG_KVM_USB_FORCE_SERIAL=y`. Expected: serial regardless of the stored mode, reason `Serial forced by this build`. Then flash the normal image again.

Record per path: what happened, what the screen said, and what Device Manager showed.

### Step 5: soak and daily use

Leave composite on for 10 minutes and read the Health line (UART is not wired: use the diagnostics dump if a trace build is used) or record "not measured". Return the board to serial mode for daily use: MSI 0.1.1 cannot find a composite board.

Record in board-bringup.md: the soak result, the final mode, and whether bonds survived. Gate for phase D: steps 2 to 4 passed.
