# DoubTech HID Library Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the BLE keyboard/mouse output code in `firmware/components/hid` into a standalone `doubtech_hid` component with a configurable device identity, without changing how it behaves.

**Architecture:** A new ESP-IDF component with `include/` (one public header), `core/` (report map, per-link output gate, pairing policy) and `ble/` (NimBLE GATT service, advertising, host-loop request bridge, bond store). The KVM firmware (`transport`, `main`) is rewired to the public API. The move happens first with tests unchanged, then the API, config, name scrub and docs are layered on in separate commits.

**Tech Stack:** C11, ESP-IDF v5.5.1 (local install at `D:\esp\esp-idf-v5.5.1`, plus CI), NimBLE, MSVC host-side tests (`run-msvc.ps1`), PowerShell, git.

**Spec:** `docs/superpowers/specs/2026-11-24-doubtech-hid-library-design.md`

## Global Constraints

- One combined keyboard + mouse BLE device, exactly as today. Never two separately named devices.
- Bond storage stays: NVS namespace `kvm_bonds`, key `identities`, magic `0x4b564d42`, table version `2`; NimBLE's own bond store untouched. A paired computer must never be forgotten.
- Device name is at most **18 bytes**; a longer name is **rejected at start, never truncated**, never moved to the scan response. Advertising content, appearance `0x03c1`, 20 ms interval, security settings, 3 links and 8 bonds are unchanged.
- Component and public prefix: `doubtech_hid`. Kconfig diagnostics become `DOUBTECH_HID_*` with the same defaults (off).
- After the name scrub, a case-insensitive search for `kvm` in the component returns only the allow-list: the legacy storage constants and the lines `Copyright (c) ESP32 KVM contributors`. Copyright headers are not rewritten.
- No changes to the wire protocol, `router`, `hardware_route`, `display`, the desktop app or the PC-capture route beyond include/prefix/REQUIRES renames.
- KVM firmware keeps passing the OLD identity ("ESP32 KVM", model "ESP32 KVM S3") through Task 6. Switching to "DoubTech HID" is Task 7, its own revertible commit.
- Reattach on macOS outranks the new name: if the owner's Mac check fails, revert Task 7 only.
- Flashing never erases stored bonds: no `erase-flash`, no `-e`/`--erase-all`. Default is app-partition-only (`app-flash`); a full flash (bootloader + partition table + app) needs an explicit `-Full` and still never erases NVS. The board is found by USB id `303A:1001`, never a hard-coded COM port. The desktop app holds the port, so it must be quit before flashing.
- Documentation is part of the deliverable: the library is documented under `docs/Designs/doubtech-hid/` (architecture, API reference, integration guide) with every public `doubtech_hid_*` symbol covered by a check script, and the root `README.md` and the component `README.md` each fully document how to build and install. Every command in them is run and marked verified, or marked not run.
- Work on branch `feature/doubtech-hid-library`; commit trailer `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`; tag `checkpoint/hid-library` only after the Mac check passes.

## Tooling Notes

- Run git in the Bash tool (PowerShell has no `git` on PATH). Run HID tests with `pwsh -NoProfile -ExecutionPolicy Bypass -File <runner> [names]`. For cargo, first `export PATH="$PATH:/c/Users/yolan/.cargo/bin"`.
- ESP-IDF v5.5.1 **is installed locally** at `D:\esp\esp-idf-v5.5.1` (tools in `%USERPROFILE%\.espressif`, Python env `idf5.5_py3.12_env`) but is not on a fresh shell's PATH and `IDF_PATH` is empty. Task 0 builds the script that loads it. Until Task 0 lands, no firmware build has been run; after it, every structural task verifies with `pwsh -NoProfile -ExecutionPolicy Bypass -File tools/firmware.ps1 build`. CI (`firmware` job) is the independent second gate. Project docs record one past flake: an Xtensa GCC segmentation fault in an unused ESP-IDF RGB LCD source on the first parallel build; a retry passed. Retry once before reporting a build failure.
- Baseline (verified 2026-11-24): HID tests `test_hid_report`, `test_hid_gatt`, `test_hid_keyboard_diag`, `test_hid_keyboard_gatt`, `test_hid_official_diag` pass; `test_hid_guest`, `test_hid_discovery`, `test_auto_pairing` fail to compile because `test_hid_guest.c` is a 0-byte file (emptied in checkpoint commit `3c2a86d`; 333 lines at `efac13a`). Suites `pairing`, `router`, `integration`, `display` pass.

## Review Focus

1. **NULL or empty config:** `doubtech_hid_start(NULL)` and NULL fields use library defaults; an empty `device_name` is rejected (Task 4).
2. **Name at the limit:** exactly 18 bytes accepted, 19 rejected; the limit counts bytes, so a multibyte name over 18 bytes is rejected, not cut mid-character (Task 4).
3. **Old bonds still load:** a table blob in the exact current on-flash format under `kvm_bonds`/`identities` loads with the same bonds after the refactor; an override namespace never reads or writes the legacy one (Task 4).
4. **Starting twice:** a second `doubtech_hid_start` must not register GATT twice or reset live links; pin whatever the current code does (Task 4).
5. **Flashing must never erase bonds:** the flash script's dry-run output for every mode contains no erase option, and it refuses a busy port instead of forcing it (Task 0).

---

### Task 0: Firmware build/flash script and project skill

No build or flash script or skill exists today (checked: repo, `~/.claude`, memory). The owner has flashed this board before, so flashing is authorized; this task makes it repeatable and safe.

**Files:**
- Create: `tools/firmware.ps1`, `tools/tests/test-firmware-script.ps1`, `.claude/skills/esp32-kvm-firmware/SKILL.md`
- Modify: `README.md` (only the stale "Firmware build" section: replace it with the script commands; the full rewrite is Task 6)

**Interfaces:**
- Produces `tools/firmware.ps1 <Action> [-Port COMn] [-Full] [-DryRun]` with `Action` in `build | flash | ports`:
  - `build`: locates ESP-IDF (`$env:IDF_PATH`, else `D:\esp\esp-idf-v5.5.1`, else `%USERPROFILE%\esp\esp-idf-v5.5.1`), loads its `export.ps1` in-process, runs `idf.py -C firmware set-target esp32s3` only when `firmware/sdkconfig` is missing, then `idf.py -C firmware build`; prints the app image path and size; exits non-zero on failure.
  - `ports`: lists serial ports with USB id `303A:1001` and, for each, whether it can be opened exclusively.
  - `flash`: auto-selects the single `303A:1001` port (error if none, or several without `-Port`), refuses with a message to quit the desktop app if the port is busy, then runs `idf.py -C firmware -p <port> app-flash`; `-Full` runs `flash` instead. Never passes an erase option.
  - `-DryRun`: prints the resolved `idf.py` command lines and exits 0 without running them.
- Produces the skill `esp32-kvm-firmware`: when to build or flash, the exact script commands, the no-erase and quit-the-app rules, the "retry the build once" flake note, pointers to `docs/Designs/recovery` and `docs/Designs/release`, and that CI is the second gate.

- [ ] **Step 1: Create the branch and commit the docs.** `git switch -c feature/doubtech-hid-library` from `main` at `3c2a86d`; `git add docs/superpowers` and commit "Add design and plan for the doubtech_hid library" (check `git status` first: only the spec and this plan should be uncommitted).
- [ ] **Step 2: Write the failing test** `tools/tests/test-firmware-script.ps1` (plain PowerShell asserts, exit 1 on failure): `build -DryRun` output contains `idf.py -C firmware build`; `flash -DryRun -Port COM9` contains `app-flash` and `COM9`; `flash -Full -DryRun -Port COM9` contains `flash` but not `app-flash`; for all three, the output matches neither `erase` nor `-e `; `flash -DryRun` with no port and no detectable board exits non-zero with a message naming `303A:1001`.
- [ ] **Step 3: Run it; expect FAIL** (script missing).
- [ ] **Step 4: Implement `tools/firmware.ps1`** as specified. Port detection uses PnP `VID_303A&PID_1001`; the busy check opens the port with .NET `System.IO.Ports.SerialPort` and releases it immediately.
- [ ] **Step 5: Run the test; expect PASS.** Then run `ports` and record what it reports for the attached board.
- [ ] **Step 6: Prove the toolchain on the unmodified code:** `tools/firmware.ps1 build`. Expected: exit 0 and an image under `firmware/build/`. This is the pre-change firmware baseline; if it fails the first time, retry once, then report the real error.
- [ ] **Step 7: Write the skill and the README firmware-section update.**
- [ ] **Step 8: Commit** "Add firmware build/flash script and project skill".

---

### Task 1: Restore the emptied adapter test and green the baseline

**Files:**
- Modify: `firmware/tests/hid/test_hid_guest.c` (restore from `efac13a`)
- Modify: `firmware/tests/hid/run-msvc.ps1` (add `test_auto_pairing` to the default list)

**Interfaces:**
- Produces: all eight HID tests (`test_hid_report`, `test_hid_gatt`, `test_hid_guest`, `test_hid_keyboard_diag`, `test_hid_keyboard_gatt`, `test_hid_discovery`, `test_hid_official_diag`, `test_auto_pairing`) pass on unmodified component code, so later tasks have a characterization net around `hid_guest.c`.

- [ ] **Step 1: Confirm the failure.** Run the runner for `test_hid_guest`; expect "Compile failed". Run `git show 3c2a86d:firmware/tests/hid/test_hid_guest.c | wc -c`; expect `0`.
- [ ] **Step 2: Restore the file** with `git show efac13a:firmware/tests/hid/test_hid_guest.c > firmware/tests/hid/test_hid_guest.c`, and add `'test_auto_pairing'` to the default `$testNames` list in the runner.
- [ ] **Step 3: Run all eight tests.** Expected: all `PASS`.
- [ ] **Step 4: If any test fails, do not touch `hid_guest.c`.** Run `git diff efac13a 3c2a86d -- firmware/components/hid/hid_guest.c firmware/components/hid/hid_guest.h`. Update the test only where the diff shows an intentional behavior change (for example the added slot snapshot fields); for any other mismatch, stop and report it to the owner as a possible real regression.
- [ ] **Step 5: Commit** "Restore truncated HID adapter test and cover it in the default runner".

---

### Task 2: Move the code into the component layout (no behavior change)

**Files:**
- Create: `firmware/components/doubtech_hid/{CMakeLists.txt,Kconfig,idf_component.yml}`
- Move (`git mv`, contents unchanged): `components/hid/hid_report.{c,h}`, `hid_pairing.{c,h}` → `components/doubtech_hid/core/`; `hid_gatt.{c,h}`, `hid_gatt_official_diag.c`, `hid_guest.{c,h}`, `hid_guest_rpc.{c,h}`, `hid_pairing_store.{c,h}` → `components/doubtech_hid/ble/`; `components/hid/Kconfig` → component root
- Move: `firmware/tests/hid/*` (including `mocks/`, `.gitignore`, `run-msvc.ps1`) → `firmware/components/doubtech_hid/tests/`
- Modify: `firmware/components/transport/CMakeLists.txt` (PRIV_REQUIRES `hid` → `doubtech_hid`), `firmware/main/CMakeLists.txt` (REQUIRES `hid` → `doubtech_hid`), `firmware/tests/pairing/run-msvc.ps1` (paths), `tools/mac-hid-map-probe.c` (build-comment paths)
- Delete: `firmware/components/hid/`

**Interfaces:**
- Consumes: Task 1's passing suite.
- Produces: the component named `doubtech_hid`, which for now exposes `core` and `ble` as public include dirs so existing includes (`hid_guest.h`) still resolve. The tests directory is named `tests` (not `test`) to avoid ESP-IDF's unit-test-app convention.

- [ ] **Step 1: `git mv` the files** as listed; `git status` must show renames, not delete/add pairs.
- [ ] **Step 2: Write `CMakeLists.txt`** registering all `.c` files from `core/` and `ble/` with `INCLUDE_DIRS core ble` and `REQUIRES bt nvs_flash esp_timer` (same as today).
- [ ] **Step 3: Update both runners** (`tests/run-msvc.ps1`, `firmware/tests/pairing/run-msvc.ps1`) so every `firmware/components/hid/...` source and include path points at the new `core/` or `ble/` location and the mocks path points at `components/doubtech_hid/tests/mocks`. Keep all test names and defines unchanged.
- [ ] **Step 4: Update the consumer CMake files and `tools/mac-hid-map-probe.c`'s comment paths.**
- [ ] **Step 5: Verify.** Run the HID runner (all eight), then the `pairing`, `router`, `integration` and `display` runners. Expected: all `PASS`. Then `git grep -n "components/hid"` outside `docs/superpowers`; expected: only `docs/Designs/ble/composite-hid.md` (handled in Task 6).
- [ ] **Step 6: Run Rust tests** (`cargo test --workspace`); expected: unchanged, all pass.
- [ ] **Step 7: Build the firmware** (`tools/firmware.ps1 build`); expected exit 0. A missed `REQUIRES` or include path shows up here.
- [ ] **Step 8: Commit** "Move HID code into the doubtech_hid component layout".

---

### Task 3: Public API header and `doubtech_hid_` prefix

**Files:**
- Create: `firmware/components/doubtech_hid/include/doubtech_hid.h`
- Modify: `ble/hid_guest.{c,h}` and `ble/hid_guest_rpc.{c,h}` (rename; the public declarations move into `doubtech_hid.h`), all tests under `tests/`, `components/transport/{router_hid_bridge.c,transport_usb_serial_jtag.c}`, `main/main.c`, `components/doubtech_hid/CMakeLists.txt`

**Interfaces:**
- Produces, in `doubtech_hid.h` (no includes of internal headers):
  - every `hid_guest_*` function becomes `doubtech_hid_*` with identical signature and semantics (for example `bool doubtech_hid_request_arm_slot(uint8_t slot)`); `hid_guest_start(void)` becomes `esp_err_t doubtech_hid_start(void)` (the config argument arrives in Task 4);
  - `hid_guest_slot_snapshot_t` → `doubtech_hid_slot_t`; `hid_guest_pairing_event_t` → `doubtech_hid_pairing_event_t`; `hid_guest_pairing_event_type_t` → `doubtech_hid_pairing_event_type_t`; `hid_guest_pairing_event_fn` → `doubtech_hid_pairing_event_fn`; enum values `HID_GUEST_*` → `DOUBTECH_HID_*` (for example `DOUBTECH_HID_DISCONNECTED`);
  - `HID_GATT_MAX_CONNECTIONS` → `DOUBTECH_HID_MAX_CONNECTIONS` (3), `HID_PAIRING_MAX_BONDS` → `DOUBTECH_HID_MAX_BONDS` (8), `HID_PAIRING_TOKEN_LEN` → `DOUBTECH_HID_TOKEN_LEN` (16), and a public `doubtech_hid_token_t` (`uint8_t bytes[DOUBTECH_HID_TOKEN_LEN]`) which internal `hid_token_t` aliases.
- Internal names (`hid_channel_*`, `hid_gatt_*`, `hid_pairing_*`, `hid_report_*`) are unchanged and private: `INCLUDE_DIRS include`, `PRIV_INCLUDE_DIRS core ble`.

- [ ] **Step 1: Write `doubtech_hid.h`** with the declarations above, moved from `hid_guest.h` (keep the doc comments).
- [ ] **Step 2: Run the compile as the failing check.** Switch the component's `INCLUDE_DIRS` to `include` only, update the runners' `/I` flags to add `include` first, and run the HID runner; expected: FAIL (consumers and tests still use old names).
- [ ] **Step 3: Apply the renames** across the component, its tests, the three consumer files and the runners. Mechanical only; no logic changes.
- [ ] **Step 4: Verify.** HID runner (all eight) and the four other suites pass; `git grep -nE "hid_guest_|HID_GUEST_|HID_GATT_MAX|HID_PAIRING_(MAX|TOKEN)"` returns nothing outside `docs/`; `tools/firmware.ps1 build` exits 0.
- [ ] **Step 5: Commit** "Expose the HID output through a doubtech_hid public API".

---

### Task 4: Configurable identity, name limit and bond-namespace override

**Files:**
- Modify: `include/doubtech_hid.h`, `ble/hid_guest.c` (advertise(), GAP name set), `ble/hid_gatt.c` and `ble/hid_gatt_official_diag.c` (DIS strings), `ble/hid_pairing_store.{c,h}`, `components/transport` and `main/main.c` (callers)
- Test: `tests/test_doubtech_hid_config.c` (new; reuse the `#define main legacy_...` include pattern from `test_auto_pairing.c`), `firmware/tests/pairing/test_pairing_store.c`

**Interfaces:**
- Produces in `doubtech_hid.h`:
  - `typedef struct { const char *device_name; const char *manufacturer; const char *model; const char *bond_namespace; } doubtech_hid_config_t;`
  - `#define DOUBTECH_HID_NAME_MAX_BYTES 18`
  - `esp_err_t doubtech_hid_start(const doubtech_hid_config_t *config);` where `NULL` config or a `NULL` field selects a library default (call: name `"BLE HID"`, manufacturer `"DoubTech"`, model `"BLE HID"`, namespace = the legacy one).
- Produces in `ble/hid_pairing_store.h`: `#define DOUBTECH_HID_LEGACY_BOND_NAMESPACE "kvm_bonds"` (allow-listed) and `esp_err_t hid_pairing_store_set_namespace(const char *name);` (`NULL` restores legacy; a name over 15 characters returns `ESP_ERR_INVALID_ARG`).
- `doubtech_hid_start` returns `ESP_ERR_INVALID_ARG` for an empty name or a name over `DOUBTECH_HID_NAME_MAX_BYTES` bytes, before touching NVS or NimBLE.
- KVM callers pass `{ "ESP32 KVM", "ESP32 KVM", "ESP32 KVM S3", NULL }` so on-air identity is unchanged.

- [ ] **Step 1: Write the failing tests** in `test_doubtech_hid_config.c`:
  - `config_null_uses_defaults`: start with `NULL`; the captured advertised name equals `"BLE HID"` and `ble_svc_gap_device_name_set` received the same string.
  - `name_exactly_18_bytes_accepted` and `name_19_bytes_rejected`: `ESP_ERR_INVALID_ARG`, and no NVS open and no `ble_gap_adv_set_fields` call happened.
  - `name_empty_rejected`; `name_multibyte_counts_bytes` (10 two-byte characters, 20 bytes, are rejected; 9 two-byte characters, 18 bytes, are accepted), and `max_length_name_fits_advertising_packet` (3 + 4 + 4 + 2 + name length is at most 31).
  - `advertised_name_and_dis_follow_config`: after `ble_hs_cfg.sync_cb()`, `fields.name`/`name_len` equal the config name; reading the manufacturer and model attributes through the GATT access callback returns the configured strings.
  - `start_twice_is_safe`: the second call returns the code the current implementation returns (read `ble/hid_guest.c` start path first and pin it), registers the service once, and leaves a connected channel connected.
  In `test_pairing_store.c` add `legacy_blob_fixture_loads` (a hand-built v2 table with magic `0x4b564d42`, two bonds, stored under namespace `kvm_bonds`, key `identities`, loads exactly those bonds) and `override_namespace_isolated` (save with namespace `"dt_bonds"`; the mock sees `"dt_bonds"` and never `"kvm_bonds"`; a corrupt blob in `"dt_bonds"` returns `ESP_ERR_INVALID_STATE` without erasing).
- [ ] **Step 2: Run them; expect FAIL** (undefined `doubtech_hid_config_t`, signature mismatch).
- [ ] **Step 3: Implement** the config struct, validation, default resolution, and use of the resolved strings in `advertise()`, the GAP name call in `on_sync`/start, and both DIS implementations; add `hid_pairing_store_set_namespace` and have `start` call it before `hid_pairing_store_load`. The diagnostic keyboard-only profile advertises the fixed name `"DoubTech KB Test"` (16 bytes) regardless of config.
- [ ] **Step 4: Update KVM callers** (`main/main.c` passes the legacy-identity config above).
- [ ] **Step 5: Run all suites; expect PASS.** Specifically all eight prior HID tests plus `test_doubtech_hid_config` (add it to the runner) and `test_pairing_store`; then `tools/firmware.ps1 build` exits 0.
- [ ] **Step 6: Commit** "Make the HID device identity and bond namespace configurable".

---

### Task 5: Scrub product names and add the guard

**Files:**
- Modify: every file under `components/doubtech_hid/` (log tags `esp32-kvm-hid`, `esp32-kvm-gatt`, `kvm-hid-trace` → `doubtech-hid`, `doubtech-hid-gatt`, `doubtech-hid-trace`; include guards `ESP32_KVM_*` → `DOUBTECH_HID_*`; comments mentioning the KVM or "protocol v1 STATUS/FORGET_BOND"), `Kconfig` (symbols → `DOUBTECH_HID_GATT_TRACE`, `DOUBTECH_HID_KEYBOARD_ONLY_DIAGNOSTIC`), `components/transport/transport_diagnostics.c` and the runner's `/DCONFIG_...` defines (follow the Kconfig rename)
- Create: `firmware/components/doubtech_hid/tests/check-product-names.ps1`

**Interfaces:**
- Produces: `check-product-names.ps1`, exit code 0 when a case-insensitive search for `kvm` over the component finds only (a) `DOUBTECH_HID_LEGACY_BOND_NAMESPACE "kvm_bonds"` in `ble/hid_pairing_store.h` and (b) lines containing `Copyright (c) ESP32 KVM contributors`; otherwise prints the offending lines and exits 1.

- [ ] **Step 1: Write the guard script** with the allow-list above.
- [ ] **Step 2: Run it; expect FAIL** listing the log tags, guards, Kconfig symbols and comments.
- [ ] **Step 3: Rename/reword** every offender (keep `0x4b564d42` as a number; it matches nothing textual).
- [ ] **Step 4: Run the guard; expect exit 0.** Run all suites; expect PASS. Add `check-product-names.ps1` to the component README's test instructions in Task 6.
- [ ] **Step 5: Pin the diagnostic identity** by adding `diagnostic_profile_name_fits_and_keeps_static_address` to `test_hid_discovery.c` (built with the diagnostic define): advertised name is `"DoubTech KB Test"`, length ≤ 18, and the static random address `51:32:84:76:5a:e1` is set.
- [ ] **Step 6: Commit** "Remove product names from the doubtech_hid component and guard against their return".

---

### Task 6: Full documentation (library, root README, build and install)

The library must be documented completely enough that someone who has never seen this repo can integrate it, and the root README must let a new developer build and install everything. Write from the code and from commands you ran, not from memory: read each header and source file before describing it, and run each documented command.

**Files:**
- Create: `firmware/components/doubtech_hid/README.md` (component quick start), `docs/Designs/doubtech-hid/README.md` (architecture), `docs/Designs/doubtech-hid/api.md` (reference), `docs/Designs/doubtech-hid/integration.md` (guide), `firmware/components/doubtech_hid/tests/check-api-docs.ps1`
- Modify: `README.md` (root; see below), `docs/Designs/ble/composite-hid.md`, `docs/spec/02-architecture.md` (component name and path), `docs/00-START-HERE.md` (index entry)

**Interfaces:**
- Produces `check-api-docs.ps1`: exit 0 when every `doubtech_hid_*` function, type and macro and every `DOUBTECH_HID_*` constant declared in `include/doubtech_hid.h` appears in `docs/Designs/doubtech-hid/api.md`; otherwise prints the missing names and exits 1.
- Required content:
  - **`docs/Designs/doubtech-hid/README.md`:** purpose and roles (today: BLE peripheral output; reserved: central input client); layering (include / core / ble) and what each file owns; the two threading models (functions that must run on the NimBLE host thread versus `doubtech_hid_request_*` calls through the bounded host-loop bridge, with the wait bounds); one combined device and the three-link, eight-bond budget; the HID report map (report IDs, lengths, boot-protocol routes) and the per-link output gate (when a link may arm, what release does, why a failed release disconnects); the pairing state machine (window, numeric comparison, challenge expiry floor, attempt limit, guest-initiated pairing) and reconnect behavior (directed advertising phases, bonded-peer private addresses); the advertising byte budget and 18-byte name rule; identity settings and defaults; bond storage and its compatibility guarantee (legacy namespace, key, magic, version 2, override rules, NimBLE's store separate); the diagnostic Kconfig options and what the keyboard-only profile changes; the test strategy (what each of the nine tests proves, mock layout, how CI differs) and known gaps; how a future central role would share the bond table and report definitions.
  - **`api.md`:** one entry per public symbol: signature, purpose, which thread may call it, blocking bounds, return values and failure behavior, pointer ownership.
  - **`integration.md`:** required `sdkconfig` values (NimBLE peripheral role, GATT server, security and SC, NVS persist, connections 3, bonds 8, CCCDs 24, controller activities 4, internal allocation); CMake `REQUIRES`; a minimal `main` that starts the library with a config and handles pairing events; wiring output into a router; renaming the device and the name-caching caveat on macOS and other hosts; what changing the bond namespace does; troubleshooting (advertising rejected, name too long, pairing window, a flash that must not erase NVS).
  - **Root `README.md`:** replace the stale status text ("not implemented yet", "do not flash") with an accurate summary checked against `docs/spec/01-product.md`, and a complete **Build and install** section: prerequisites with exact versions (Windows 11 and WebView2, Rust 1.93.1 selected by `rust-toolchain.toml`, Node 22.x with npm, MSVC build tools, ESP-IDF v5.5.1 and where `tools/firmware.ps1` looks for it, Python for packaging); one-time setup; building and running the desktop app (dev, release, and the MSI via `cargo tauri build --bundles msi` with `--no-sign` noted); installing the desktop app from the MSI, including autostart; building the firmware with `tools/firmware.ps1 build`; installing it with `tools/firmware.ps1 ports` then `flash` (app partition only, never erases bonds), `-Full` and when it is needed, quitting the desktop app first, and the in-app update path with a link to `docs/Designs/release`; first run and pairing with a link to the guides; running every test suite (Rust, desktop, the C suites, the check scripts) and what CI runs; BOOT-button recovery with a link to `docs/Designs/recovery`; a troubleshooting table (port busy, no board found, build flake, bonds after flashing, behavior after Windows sleep, the dev server on port 1420). Include the Windows-specific note that Git Bash needs the cargo and Node directories added to PATH.
  - **Component `README.md`:** a "Build and install" section for using the library in a firmware project (add the component, `sdkconfig`, build, flash) that links to the root README for the shared steps, plus the test commands (`tests/run-msvc.ps1`, `tests/check-product-names.ps1`, `tests/check-api-docs.ps1`) and `tools/firmware.ps1`.

- [ ] **Step 1: Write the failing check** `check-api-docs.ps1`; run it against an empty `api.md`; expect FAIL listing every public name.
- [ ] **Step 2: Write `api.md`** from `include/doubtech_hid.h` and the implementing sources; run the check; expect exit 0.
- [ ] **Step 3: Write the architecture document and the integration guide,** reading `ble/hid_guest.c`, `ble/hid_gatt.c`, `core/*`, `sdkconfig.defaults` and the HID tests as the source of truth. Every number stated (links, bonds, intervals, timeouts, byte budget) must match the code.
- [ ] **Step 4: Cross-check.** Spot-check at least ten stated facts by grepping the source (for example the pairing window constants and the 20 ms advertising interval) and fix any mismatch.
- [ ] **Step 5: Rewrite the root README build-and-install section and write the component README,** then run every command they document that is safe to run (`cargo fmt --check`, `cargo check --workspace`, `npm ci` and `npm run build` in `apps/desktop`, `tools/firmware.ps1 build`, `tools/firmware.ps1 ports`, all test suites). Commands that change the device or produce an installer (`flash`, `cargo tauri build`) are verified by Task 8 or marked "not run in this review". The README states which commands were verified and the date; a documented command that fails is fixed in the README or reported, never left as written.
- [ ] **Step 6: Update the three existing docs;** `git grep -n "components/hid"` outside `docs/superpowers` must return nothing.
- [ ] **Step 7: Commit** "Document the doubtech_hid library and how to build and install the project".

---

### Task 7: Switch the KVM default identity to "DoubTech HID" (own commit)

**Files:**
- Modify: `firmware/main/main.c` only

**Interfaces:**
- Consumes: `doubtech_hid_config_t` from Task 4. New values (call): `device_name = "DoubTech HID"`, `manufacturer = "DoubTech"`, `model = "DoubTech HID"`, `bond_namespace = NULL`.

- [ ] **Step 1: Change the config in `main.c`** and nothing else. This commit must be revertible alone.
- [ ] **Step 2: Run all suites; expect PASS** (no test depends on the KVM default).
- [ ] **Step 3: Commit** "Advertise the device as DoubTech HID".

---

### Task 8: Verify, flash, hand over for the Mac check, deliver

- [ ] **Step 1: Full local run:** HID runner (nine tests), `check-product-names.ps1` and `check-api-docs.ps1` (exit 0), `tools/tests/test-firmware-script.ps1`, the `pairing`, `router`, `integration` and `display` runners, `cargo test --workspace`, `tools/firmware.ps1 build`; expected all pass.
- [ ] **Step 2: Push the branch** (`git push -u origin feature/doubtech-hid-library`) and read the CI result for the `firmware` job. A red build blocks everything after this step; report it, do not tag.
- [ ] **Step 3: Flash the board.** Ask the owner to quit the desktop app (it holds the port), run `tools/firmware.ps1 ports` and confirm one board, then `tools/firmware.ps1 flash` (app partition only, no erase). Expected: success and the board re-enumerates. Confirm the bonds survived by reading the retained-bond count through the existing probe examples or the app, and report what was observed. Record in the root README that `flash` was verified, with the date.
- [ ] **Step 4: Owner's Mac check (cannot be done by the agent).** Ask the owner to confirm: (a) an already-paired Mac reattaches after a power cycle with no re-pairing; (b) a fresh pairing lists "DoubTech HID"; (c) the PC-capture route still selects a guest. A Mac that keeps showing the old name is expected until re-paired, not a bond loss. If (a) fails, `git revert` the Task 7 commit, rebuild, reflash, and re-check.
- [ ] **Step 5: Deliver.** After the owner confirms: `git switch main && git merge --ff-only feature/doubtech-hid-library`, `git tag -a checkpoint/hid-library -m "Reusable doubtech_hid component extracted"`, `git push origin main checkpoint/hid-library`; verify with `git ls-remote origin main refs/tags/checkpoint/hid-library`.
