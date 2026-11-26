# ESP32 KVM

ESP32 KVM shares a Windows host's physical keyboard and mouse with selected Bluetooth guests through a USB-connected ESP32-S3. The device presents one composite BLE keyboard, mouse and media controller that up to three guests can pair with. A Tauri desktop app on Windows configures the device, captures physical input and routes it over a framed USB protocol.

## Status

This is a working trial build, not a released product. The pieces exist and have source-level tests:

- Firmware (ESP-IDF, ESP32-S3): USB Serial/JTAG protocol transport, routing with a fail-local policy, a three-link BLE HID service with numeric-comparison pairing and eight stored bonds (the reusable [`doubtech_hid` library](firmware/components/doubtech_hid/README.md)), and a 240x240 status display (the Kconfig options default to off; `firmware/sdkconfig.defaults` turns on the Waveshare panel and CST816 touch for the validated board).
- Device-owned input (new, host-tested only, never run on a board): the board itself pairs one Bluetooth keyboard and one mouse and routes them to a chosen guest with a Left Ctrl + Left Alt chord, with no PC app running. See [Pair a keyboard or mouse](#pair-a-keyboard-or-mouse) and [the owner checklist](docs/Designs/doubtech-hid/owner-checklist-device-input.md).
- Desktop (Tauri 2, React/TypeScript): setup and guest-pairing wizard, dashboard and tray, per-guest key mapping and layout editors, a configurable switch shortcut, and the Windows capture adapter.
- Release packaging of an app-only firmware image and an MSI (`tools/package_release.py`).

What is not established: the product requirements are in [the product spec](docs/spec/01-product.md) and its acceptance targets, and the hardware record is in [the board bring-up log](docs/hardware/board-bringup.md). As of that log, keyboard input reached a Mac guest once on one Mac; persistent automatic reconnect, two or three simultaneous real guests, the physical BOOT recovery sequence, and the installer on a clean machine are still open gates. The desktop app has no firmware-update screen yet. Read [the design handoff](docs/00-START-HERE.md), [the architecture](docs/spec/02-architecture.md) and [the validation plan](docs/spec/05-validation-and-delivery.md) before relying on any behavior. The working hardware is the Waveshare ESP32-S3-Touch-LCD-1.54; verify the arrived variant and revision before flashing anything new.

## Layout

- `apps/desktop`: Tauri 2 native shell (`src-tauri`) with a React/TypeScript webview (`src`).
- `apps/mac`: macOS command-line USB host client and its Swift capture helper (builds on a Mac; see `apps/mac/README.md`).
- `crates/`: Rust libraries, each with its own tests: `input-core` (routing policy and fault handling; timing-critical routing belongs here, not in React), `platform-windows` (Windows capture and recovery adapter), `host-actor` (the verified USB session), `usb-transport`, `protocol` (the wire format; schema in `protocol/schema`), `edge-policy`, `topology-core`, `helper-session`, `update-core` and `update-runner`.
- `firmware/main`: the ESP-IDF application entry point. `firmware/components/`: `board`, `transport`, `router`, `display`, `input_hub` (device-input routing), and the reusable `doubtech_hid` (BLE HID guests) and `doubtech_hid_central` (BLE keyboard and mouse client) libraries. `firmware/tests/`: portable C tests for the router, pairing, integration and display. `firmware/partitions.csv` and `firmware/sdkconfig.defaults` are the shared configuration.
- `tools/`: `firmware.ps1` (build, list ports, flash), `package_release.py`, probes and `tests/test-firmware-script.ps1`.
- `tests/`: `vectors` (independent golden protocol vectors), `ci`, `release`, `fault`.
- `docs/`: `spec` (product, architecture, protocol, validation), `design` (tokens and screen sources), `Designs` (one folder per feature, including [`doubtech-hid`](docs/Designs/doubtech-hid/README.md), `release`, `recovery`, `update` and `pairing`), `hardware`.

## Build and install

All commands run from the repository root unless a `cd` is shown. Each command is marked with its verification status; the full table is at the end of this section. Verification date: 2026-11-24, on Windows 11 with Rust 1.94.0, Node v24.13.1 and npm 11.8.0, ESP-IDF v5.5.1.

### Prerequisites

| Need | Version and notes |
| --- | --- |
| Windows | Windows 11 x64. The desktop app needs the WebView2 runtime (present on current Windows 11). |
| Rust | The channel in `rust-toolchain.toml` is `stable` for `x86_64-pc-windows-msvc`, with the `clippy` and `rustfmt` components, so rustup installs whatever stable currently is. CI pins 1.93.1; the workspace needs Rust 1.85 or newer (`rust-version = "1.85"`, edition 2024). To match CI exactly, run `rustup toolchain install 1.93.1 --component rustfmt clippy` and put `+1.93.1` after `cargo` (for example `cargo +1.93.1 check --workspace`). |
| MSVC | Visual Studio 2022 Build Tools (or Community) with the "Desktop development with C++" workload and a Windows 10/11 SDK. Rust's MSVC target and the C test scripts both need it; the C test scripts look in `C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC` and `C:/Program Files (x86)/Windows Kits/10`. |
| Node.js | Node 22.x with npm. CI uses Node 22.13.1. Dependencies are pinned in `apps/desktop/package.json` and `package-lock.json` (React 19.2.4, Vite 7.3.6, TypeScript 5.9.3, Tauri API and CLI 2.12.0). The build also passed with Node v24.13.1 and npm 11.8.0. |
| ESP-IDF | v5.5.1 with the ESP32-S3 tools installed (run its `install.ps1 esp32s3` once). `tools/firmware.ps1` looks for it in this order: the `IDF_PATH` environment variable, `D:\esp\esp-idf-v5.5.1`, then `%USERPROFILE%\esp\esp-idf-v5.5.1`. A folder only counts if it contains `export.ps1`. |
| Python | Python 3.12, for `tools/package_release.py` and the `tests/ci` and `tests/release` suites. |
| PowerShell | PowerShell 7 (`pwsh`) for `tools/firmware.ps1` and the test scripts. |
| Hardware | The ESP32-S3 board on a USB-C data cable. Its USB Serial/JTAG port enumerates as USB id `303A:1001`. |

The MSI bundler (WiX) is fetched by Tauri on first use; that step was not run in this review.

Git Bash on Windows: the cargo and Node directories are not on its PATH by default. Add them first, and use the `.cmd`/`.exe` tools they provide:

```sh
export PATH="$PATH:$HOME/.cargo/bin:/c/Program Files/nodejs"
```

In PowerShell use `$env:PATH = "$env:USERPROFILE\.cargo\bin;C:\Program Files\nodejs;" + $env:PATH`.

### One-time setup

```powershell
rustup show                 # prints the toolchain chosen by rust-toolchain.toml
cd apps/desktop
npm ci                      # exact versions from package-lock.json; deletes and recreates node_modules
cd ../..
```

Install ESP-IDF v5.5.1 as described above. Nothing else is needed; `tools/firmware.ps1` loads the ESP-IDF environment for each run.

### Build and run the desktop app

Source checks:

```powershell
cargo fmt --all -- --check
cargo check --workspace
cd apps/desktop
npm run build        # tsc --noEmit, then vite build into apps/desktop/dist
cd ../..
```

Development run (starts Vite on `127.0.0.1:1420`, then the native shell):

```powershell
cd apps/desktop
npm run tauri dev
```

Release binary only:

```powershell
cargo build --workspace --release
```

Build the Windows installer (MSI). `--no-sign` is required today because there is no signing certificate, so the MSI is unsigned and must be described that way. Use the CLI pinned by the desktop dependencies rather than an older global one:

```powershell
cd apps/desktop
npm run tauri -- build --bundles msi --ci --no-sign
```

(`cargo tauri build --bundles msi --ci --no-sign` from `apps/desktop` is the same step if `cargo-tauri` 2.x is installed; see [the release notes](docs/Designs/release/README.md).) The installer is written under `target/release/bundle/msi/`, named like `ESP32 KVM_0.1.1_x64_en-US.msi` (the version comes from `tauri.conf.json` and the desktop crate, currently 0.1.1).

### Install the desktop app

Double-click the MSI, or run:

```powershell
msiexec /i "target\release\bundle\msi\ESP32 KVM_0.1.1_x64_en-US.msi"
```

The installer adds a per-user autostart entry (`apps/desktop/src-tauri/windows/fragments/autostart.wxs`): the registry value `ESP32 KVM` under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`, which starts `esp32-kvm-desktop.exe` from the install folder at every login of that user. To stop it, uninstall the app, or delete that registry value:

```powershell
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v "ESP32 KVM" /f
```

The autostart supports the desktop app and its optional PC route; it is not a boot-time service. Whether local profiles survive an upgrade or uninstall still needs the clean-machine test listed in [the release gates](docs/Designs/release/README.md).

### Build the firmware

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File tools/firmware.ps1 build
```

The script loads ESP-IDF, runs `idf.py -C firmware set-target esp32s3` only when `firmware/sdkconfig` does not exist yet, runs `idf.py -C firmware build`, and prints the app image path (`firmware/build/esp32_kvm.bin`) and size. The output of the command above was `App image: ...\firmware\build\esp32_kvm.bin (1010912 bytes)`. If you change `firmware/sdkconfig.defaults` on a checkout that already has a generated `firmware/sdkconfig`, delete that generated (git-ignored) file first so the new defaults apply.

### Install the firmware on the board

1. Quit the desktop app (and exit it from the tray). It owns the board's serial port, and the flash step refuses to run while the port is busy.
2. List the board:

   ```powershell
   pwsh -NoProfile -ExecutionPolicy Bypass -File tools/firmware.ps1 ports
   ```

   This opens each matching port to see whether it is free, which can reset the board and drop its Bluetooth links. Add `-DryRun` to list the ports without opening them (it prints `busy: not checked (dry run)`).
3. Flash:

   ```powershell
   pwsh -NoProfile -ExecutionPolicy Bypass -File tools/firmware.ps1 flash -Port COM4
   ```

   `-Port` can be omitted when exactly one board is attached; with none or several the script stops and says so. `flash` runs `idf.py app-flash`: it writes the application image only and never erases flash, so stored bonds (NVS) stay. Add `-DryRun` to print the `idf.py` command without running it.
4. `-Full` runs `idf.py flash` instead and also writes the bootloader, partition table and OTA data. Use it only when those must change: the first flash of a blank board, or after the partition table (`firmware/partitions.csv`) or bootloader changed. It still never erases flash, and it does not write the NVS partition (offset `0x9000`), so bonds survive it too.
5. Restart the desktop app. It starts a fresh USB session; the board reports `Local` with input disarmed.

`ports` also lists a board in USB composite mode (VID 303A, the allow-listed PID, interface `MI_02`) and prints each board's mode, `serial` or `composite`. `flash` on a composite-only board first writes the serial-once magic string (`"\0KVM-USB-SERIAL-ONCE\n"`, 21 bytes) to its port, waits up to 15 seconds for the board to return as `303A:1001`, then runs the same `app-flash`. The handoff flashes only the serial board that newly appears after the write, and it is refused while any other project board is plugged in; with a composite and a serial board, `-Port` of the serial board flashes it directly. `-DryRun` prints these steps without doing them. This path is covered by dry-run tests only; no board has run it.

The live `ports` listing and the busy check before `flash` open the port and may reset the board (BLE links drop if so). Both ran on the owner's board on 2026-11-24 with the desktop app stopped: `ports` reported `COM4  303A:1001  free (opens exclusively)`, and `flash -Port COM4` completed. Automatic port selection (no `-Port`) had a bug on that run: with exactly one board it tried to open a port called `C`. It is fixed in `tools/firmware.ps1` and covered by `tools/tests/test-firmware-script.ps1`, and has been checked only as a dry run since the fix.

In-app update: the desktop app has no firmware-update screen today. The building blocks are in the repository (`update-core` preflight, the host actor's `prepare_update`, `update-runner` for app-only flashing, and `tools/package_release.py`), but nothing in the app calls them. Packaging a release, its manifest and its remaining gates are in [docs/Designs/release](docs/Designs/release/README.md) and [the update preflight](docs/Designs/update/host-update-preflight.md). Use `tools/firmware.ps1` to install firmware.

### First run and pairing

1. Plug the board into the PC with the USB-C cable and start the desktop app.
2. Open **Systems -> Add guest** (or **Device -> Open setup**). The wizard connects to the board, verifies its ID and protocol, and never flashes or arms input.
3. Pair the guest: in the guest's Bluetooth settings choose the device. The device name is set in firmware (`firmware/main/main.c`). The firmware shows a six-digit number; compare it with the guest's and confirm on the guest. The pairing window lasts 60 seconds and allows three attempts.
4. Run the bounded control test, then finish setup.

Details: [desktop pairing wizard](docs/Designs/desktop-pairing/README.md), [pairing and bond identity](docs/Designs/pairing/README.md), and the library's [integration guide](docs/Designs/doubtech-hid/integration.md) for device naming.

Current KVM firmware identity: configured in `firmware/main/main.c` as name "DoubTech HID", manufacturer "DoubTech", model "DoubTech HID", default bond namespace.

If a guest previously paired under another name, forget the device in the guest's Bluetooth settings before pairing again; hosts cache names and GATT data.

### Run the test suites

```powershell
# Rust (what CI runs, plus the same crates locally)
cargo fmt --all -- --check
cargo clippy --workspace --all-targets -- -D warnings
cargo test -p esp32-kvm-input-core --test faults
cargo test --workspace
pwsh -File ./tests/vectors/generate.ps1; git diff --exit-code -- tests/vectors
cargo test -p esp32-kvm-protocol

# Desktop
cd apps/desktop
npx tsc --noEmit
npm run build
node --experimental-strip-types --test src/*.test.mjs      # not run by CI
cd ../..

# Python (CI: Python 3.12)
python -B -m unittest discover -s tests/ci -v
python -B -m unittest discover -s tests/release -v

# C suites (Windows, MSVC, no board needed; not run by CI)
pwsh -NoProfile -ExecutionPolicy Bypass -File firmware/components/doubtech_hid/tests/run-msvc.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File firmware/components/doubtech_hid_central/tests/run-msvc.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File firmware/components/input_hub/tests/run-msvc.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File firmware/tests/pairing/run-msvc.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File firmware/tests/router/run-msvc.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File firmware/tests/integration/run-msvc.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File firmware/tests/display/run-msvc.ps1

# Checks and script tests
pwsh -NoProfile -ExecutionPolicy Bypass -File firmware/components/doubtech_hid/tests/check-product-names.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File firmware/components/doubtech_hid/tests/check-api-docs.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File firmware/components/doubtech_hid_central/tests/check-product-names.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File tools/tests/test-firmware-script.ps1
```

What CI runs (`.github/workflows/validation.yml`, on every push and pull request): `rust` (Windows, Rust 1.93.1: `cargo fmt --check`, clippy with warnings denied, the input-core fault suite, `cargo test --workspace`); `desktop` (Windows, Node 22.13.1: `npm ci`, `npx tsc --noEmit`, `npm run build`); `protocol-vectors` (regenerates the vectors and fails if they differ, then runs the protocol tests); `release-inputs` (Python 3.12: `tests/ci` and `tests/release`); and `firmware` (Ubuntu, ESP-IDF v5.5.1 for ESP32-S3: builds `firmware/`). The C suites, the check scripts, the desktop Node tests and `tools/tests/test-firmware-script.ps1` are not in CI. The CI firmware build is the independent build gate for the firmware. The C suites run only through the Windows MSVC scripts and are mock-based: they show source logic, not radio or hardware behavior. This includes the new `doubtech_hid_central` and `input_hub` suites: they are not in CI either. `check-product-names.ps1` (under `doubtech_hid`) scans both `doubtech_hid` and `doubtech_hid_central`; `check-api-docs.ps1` checks both public headers against `docs/Designs/doubtech-hid/api.md` and `central.md`.

### Pair a keyboard or mouse

The board can own one Bluetooth keyboard and one mouse itself, so you can type into a guest with the PC app closed. **This feature is built and tested on the host only; nothing in it has been run on a board.** Design: [central.md](docs/Designs/doubtech-hid/central.md), [input hub](docs/Designs/input-hub/README.md). The hardware checklist, with a PASS/FAIL line per item, is [owner-checklist-device-input.md](docs/Designs/doubtech-hid/owner-checklist-device-input.md).

Before any of it works, **you must flash the new firmware first** (the build in `firmware/build` does not reach the board by itself):

1. Quit the desktop app from the tray (**Quit and release**); it owns the serial port.
2. `pwsh -NoProfile -ExecutionPolicy Bypass -File tools/firmware.ps1 build`, then `... tools/firmware.ps1 flash -Port COM4`. **Never erase flash**; the script never does, so your guest bonds survive. If `firmware/sdkconfig` is left from an older build, delete that generated file first so the larger BLE budget applies.

Then, on the touchscreen (needs at least one paired guest, no armed PC route and no open guest pairing window):

1. Tap the keyboard button left of the gear to open **INPUT DEVICES**, then **ADD KEYBOARD OR MOUSE**.
2. Put the keyboard or mouse in pairing mode and tap its row (the four strongest are listed) within 30 seconds. A keyboard may show **TYPE ON KEYBOARD** and six digits: type them on the keyboard and press Enter. A device the board cannot use shows **NOT SUPPORTED** (a keyboard needs boot protocol; mice are boot mice or simple wheel mice).
3. The device reconnects by itself after a reboot or a drop. Tap a paired row twice to forget it. Pairing shows **BUSY** only while the PC app has a guest armed or guest pairing is open; having the PC app connected is fine.

Select where input goes with the chord Left Ctrl + Left Alt + 1, 2 or 3 (guest), 0 (This PC, which does nothing yet) or Esc (release all). While the PC app has a guest armed, the PC app wins and the board's own devices are ignored. With no PC app the screen shows the normal view with a dimmed This PC card and a `USB off` badge.

### Recovery

If a flash is interrupted or the board no longer enumerates, do not retry blindly and do not erase flash. The documented manual route is to hold the BOOT button while connecting USB, then release it, to enter ROM download mode; see [the bring-up log](docs/hardware/board-bringup.md). That sequence has not been repeatably verified on this board, so do not rely on it as a tested procedure. The owner drill that is meant to prove it is [docs/hardware/usb-recovery-drill.md](docs/hardware/usb-recovery-drill.md).

**USB HID mode recovery (designed, not yet run on a board).** The board starts in serial mode (`303A:1001`) unless composite USB HID mode was turned on from the touchscreen, and only an image built with composite support honors that setting. If a composite board stops being usable, in this order of effort (spec: `docs/superpowers/specs/2026-11-25-usb-hid-device-design.md`, Decisions 6 to 10):

1. **Replug three times**, each within 30 seconds of the previous boot, with no button: the third start is forced back to serial mode and the screen says the USB HID mode was turned off after failed starts.
2. **Escape window:** plug in, then press BOOT within about 1.5 seconds. That boot is serial and the USB MODE screen offers to keep it. Pressing BOOT before power is ROM download mode instead (item 4).
3. **Touchscreen:** INPUT DEVICES, 4th row **USB MODE** (shown only with a paired guest), then **USE SERIAL MODE** (persists) or **RESTART IN SERIAL MODE** (one boot), each tapped twice to confirm.
4. **BOOT + plug:** hold BOOT while plugging in, release; the chip enters ROM download mode as `303A:1001` whatever the app is doing. Flash from there with `tools/firmware.ps1 flash -Port COMn`.
5. **Script:** `pwsh -NoProfile -ExecutionPolicy Bypass -File tools/firmware.ps1 usb-serial` writes the serial-mode magic string, persists serial mode and reboots the board (even one that is already serial); `serial-once` writes only the one-boot magic. Both refuse a port that is not a listed project board; add `-DryRun` to preview. With the board in composite mode the same script's `flash` first writes the serial-once magic, waits up to 15 seconds for `303A:1001`, then runs the usual `app-flash`.
6. **Rescue build:** an image built with `CONFIG_KVM_USB_FORCE_SERIAL=y` ignores the stored mode and never starts TinyUSB; flash it from ROM mode (item 4).

None of this erases flash. Run the drills in [docs/hardware/usb-recovery-drill.md](docs/hardware/usb-recovery-drill.md) before relying on any of it. Software-side local-control recovery (the capture gate, both-Ctrl emergency release, watchers) is described in [docs/Designs/recovery](docs/Designs/recovery/README.md).

### Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| `COMn is busy. Quit the ESP32 KVM desktop app (it owns the port) and try again.` | Another program has the serial port. Exit the desktop app from its tray icon and close serial monitors, then retry. |
| `No board with USB id 303A:1001 or 303A:4009 found.` | The board is not enumerating. Use a data cable, replug, check Device Manager, or pass `-Port COMn` if you know the port. See Recovery if it never appears. |
| `Several boards found: ...` | Pass `-Port COMn`. |
| `ESP-IDF not found. Set IDF_PATH or install ESP-IDF v5.5.1 at D:\esp\esp-idf-v5.5.1.` | Install v5.5.1 or set `IDF_PATH` to a folder that contains `export.ps1`. |
| Build fails, or old `sdkconfig` values appear | Re-run the build once. If you edited `firmware/sdkconfig.defaults`, delete the generated, git-ignored `firmware/sdkconfig` and build again. |
| Local GATT trace stopped logging after the Kconfig rename | The symbols were renamed from `CONFIG_KVM_HID_*` to `CONFIG_DOUBTECH_HID_*`. A local `sdkconfig` with the old names is migrated automatically through `firmware/main/sdkconfig.rename`; check with `grep DOUBTECH_HID firmware/sdkconfig`. |
| `cargo`, `npm` or `node` not found in Git Bash | Add the cargo and Node directories to PATH as shown under Prerequisites. |
| Bonds missing after flashing | `flash` and `flash -Full` do not erase NVS. Look for a changed bond namespace, an erased NVS partition (for example a manual full erase with another tool), or a guest that forgot the device: forget the device on the guest and pair again. See [the integration guide](docs/Designs/doubtech-hid/integration.md). |
| Input is local after Windows sleep or resume | By design the host fails local and needs a new verified USB session and a physical all-up observation after resume before guest routing can arm again ([recovery](docs/Designs/recovery/README.md)). Select the guest again. The native sleep/resume check on hardware is still pending. |
| `npm run tauri dev` fails because port 1420 is in use | The Vite server uses `strictPort` on `127.0.0.1:1420`. Find the other process (`netstat -ano \| findstr :1420`) and stop it, for example a previous dev server. |
| `cargo fmt --all -- --check` reports differences | See the verification table below. |

### Verification of the documented commands

Verified means run on 2026-11-24 in this review. Commands that were not run are listed with the reason, and are documented from the source, not from a run.

| Command | Status |
| --- | --- |
| `cargo fmt --all -- --check` | verified 2026-11-24, with a finding: exits 1 on this checkout with rustc 1.94.0 (formatting differences in 7 files under `crates/` that this work does not touch; one formatting slip in a regression test added with this work was fixed). CI pins 1.93.1 and reports the same failure, so it is not a local-toolchain artifact. The release test (`tests/release`) also fails in CI: it passes `--desktop-version 0.1.0` while the repository is at 0.1.1. |
| `cargo check --workspace` | verified 2026-11-24 (exit 0). |
| `npm run build` in `apps/desktop` | verified 2026-11-24 (exit 0, Node v24.13.1, npm 11.8.0). |
| `tools/firmware.ps1 build` | verified 2026-11-24 (exit 0, 1010912-byte app image). |
| `tools/firmware.ps1 ports -DryRun` | verified 2026-11-24 (listed `COM4  303A:1001  busy: not checked (dry run)`). |
| `tools/firmware.ps1 flash -DryRun -Port COM9` | verified 2026-11-24 (printed `idf.py -C firmware -p COM9 app-flash`). |
| `tools/firmware.ps1 flash -DryRun` with one board attached and no `-Port` | verified 2026-11-24 after the auto-select fix (printed `idf.py -C firmware -p COM4 app-flash`, exit 0). |
| `tools/firmware.ps1 ports` (live, desktop app stopped) | verified 2026-11-24 on the owner's board (`COM4  303A:1001  free (opens exclusively)`). |
| `tools/firmware.ps1 flash -Port COM4` (live, app partition only) | verified 2026-11-24 on the owner's board: ESP32-S3 (QFN56) rev v0.2, 1011904 bytes written at `0x20000`, `Hash of data verified`, hard reset via RTS; NVS (`0x9000`) was not written, and both paired guests reconnected afterwards without re-pairing. |
| The five C suites (`doubtech_hid/tests/run-msvc.ps1`, `firmware/tests/pairing`, `router`, `integration`, `display`) | verified 2026-11-24 (all PASS). |
| `check-product-names.ps1`, `check-api-docs.ps1`, `tools/tests/test-firmware-script.ps1` | verified 2026-11-24 (all exit 0). |
| `npm ci` | not run in this review (it deletes and recreates `node_modules` in the working tree). |
| `npm run tauri dev` | not run in this review (opens the app and a dev server). |
| `npm run tauri -- build --bundles msi --ci --no-sign` and `cargo tauri build ...` | not run in this review (builds an installer). |
| `msiexec /i ...` and `reg delete ...` | not run in this review (change the installed software and registry). |
| `tools/firmware.ps1 flash -Full` | not run in this review (rewrites the bootloader, partition table and OTA data; only the app-partition `flash` was run, see above). |
| `cargo test --workspace` | verified 2026-11-24 on the final tree (40 test suites, 0 failures). |
| `cargo build --workspace --release`, `cargo clippy ...`, the vector generator, `npx tsc --noEmit`, the Node `--test` command, the Python `unittest` commands, `rustup show`, `rustup toolchain install` | not run in this review (outside the verification set for this pass). `npm run build` covers `tsc --noEmit` for the desktop. |

## Safety boundary

The webview is for configuration and status. Physical input capture, suppression, routing generations, held-key state, and recovery must execute in native host and firmware workers. A new session must start disarmed, and any loss of communication must restore local control and clear guest reports as described in the [protocol contract](docs/spec/04-usb-protocol.md).
