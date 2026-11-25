# USB HID device (TinyUSB composite HID + CDC) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the board a composite USB device (boot keyboard, mouse with wheel, consumer control, CDC-ACM control port) so device-owned input on hub target 0 reaches the PC over USB HID, while the desktop app keeps driving guests over the same cable, and the board can always be reflashed.

**Architecture:** A new `usb_device` component holds a pure boot-mode decision, pure descriptors and HID report builders, an NVS mode store, the RTC "serial once" flag, and the esp_tinyusb glue. The USB worker reads through a byte-link (USJ today, TinyUSB CDC in composite mode) and keeps `transport_core.c` framing unchanged. The input hub gains a real PC target fed into a state-based HID sink that the worker pumps without blocking. Every boot starts on USB Serial/JTAG; composite mode is an owner-enabled NVS flag with several revert paths ending in the serial app.

**Tech Stack:** C, ESP-IDF 5.5.1 (ESP32-S3), `espressif/esp_tinyusb` (registry; 3.0.x per GitHub master, to be pinned and verified), TinyUSB HID/CDC device classes, NimBLE (unchanged), LVGL, MSVC host-side C tests (`run-msvc.ps1` pattern), Rust workspace (`serialport =4.10.1` with feature `usbportinfo-interface`), PowerShell `tools/firmware.ps1`.

**Spec:** `docs/superpowers/specs/2026-11-25-usb-hid-device-design.md`

## Global Constraints

- Branch `feature/usb-hid-device` (from `feature/device-input-central` at `75fc706`). Each agent task ends with `pwsh -NoProfile -ExecutionPolicy Bypass -File tools/firmware.ps1 build` green and the existing suites green: `firmware/tests/{display,router,pairing,integration}/run-msvc.ps1`, `firmware/components/doubtech_hid/tests/run-msvc.ps1`, `firmware/components/doubtech_hid_central/tests/run-msvc.ps1`, `firmware/components/input_hub/tests/run-msvc.ps1`, and from task 13 on `cargo test --offline --workspace` (Bash: `export PATH="$PATH:/c/Users/yolan/.cargo/bin"`). The Bash tool has no `pwsh`/`python3`: run PowerShell scripts with the PowerShell tool.
- **Who does what.** "Agent" = code, host tests, `tools/firmware.ps1 build`. "Owner" = anything that flashes, opens the board's port, presses buttons, or plugs cables. The agent never flashes, never kills the desktop app, never runs `ports` or `flash` without the owner's go-ahead for that task.
- Never erase flash or NVS; `tools/firmware.ps1 flash` stays app-only. Bootloader, partition table and OTA data are not touched in this stage.
- Until task 10, `CONFIG_KVM_USB_COMPOSITE_SUPPORT` defaults to `n` and the firmware's behaviour is byte-for-byte today's on the wire. No composite image is flashed before the task 9 drill passes and is logged.
- `firmware/dependencies.lock`: only task 10 may change it (adding esp_tinyusb), in its own commit, after the owner explicitly approves that lock change. No other task commits it. Do not commit `firmware/sdkconfig` (git-ignored) or `.claude/` files.
- Only the USB worker touches `kvm_router_t`, the hub, the HID sink and `kvm_transport_core_t`. TinyUSB and NimBLE callbacks only copy, set atomics, give semaphores or enqueue.
- Names fixed by this plan (use exactly): NVS namespace `kvm_usb`, keys `mode` (u8) and `unconf` (u8); magic strings `"\0KVM-USB-SERIAL-ONCE\n"` and `"\0KVM-USB-SERIAL-MODE\n"`; Kconfig `KVM_USB_COMPOSITE_SUPPORT` (bool, n), `KVM_USB_FORCE_SERIAL` (bool, n), `KVM_USB_COMPOSITE_PID` (hex, 0x4009), `KVM_USB_ESCAPE_WINDOW_MS` (int, 1500, 500..5000), `KVM_USB_CONFIRM_MS` (int, 30000), `KVM_USB_MAX_UNCONFIRMED` (int, 3), `KVM_USB_REMOTE_WAKEUP` (bool, y); interfaces 0 keyboard, 1 pointer+consumer, 2/3 CDC; endpoints `0x81 0x82 0x83 0x04 0x84`; report IDs 1 mouse, 2 consumer; bcdDevice `0x0100`; protocol minor 5, STATUS key 128.
- Wire compatibility: hosts negotiating minor <= 4 see no change; existing `tests/vectors/*` stay byte-identical.

## Review Focus

1. Recovery: every path in spec Decisions 7 to 10 ends in the serial app; a composite boot never starts TinyUSB before the escape window; `unconf` is written before `tinyusb_driver_install` and cleared only after 30 s mounted.
2. Release on every transition: chord PC to guest, router arm, suspend, bus reset, unmount, device loss. The PC must never keep a key down and must never receive device HID while the router is armed.
3. The worker never blocks on USB: HID uses `tud_hid_n_ready` then a non-blocking report; CDC write flushes with timeout 0 and gives up after 50 ms total like `send_binary`.
4. Descriptor budget: at most 4 IN endpoints besides EP0, keyboard at interface 0 with no report ID, IAD before interface 2, total length matches `wTotalLength`.
5. Host discovery: legacy `303A:1001` unchanged; composite PID accepted only on interface 2 or 3 and still handshaken; the update runner never runs esptool against the composite port.

---

## Phase A: groundwork, no behaviour change (all agent tasks)

### Task 1: `usb_device` component, Kconfig and the pure boot decision

**Who:** Agent (host tests + build).

**Files:**
- Create: `firmware/components/usb_device/{CMakeLists.txt,Kconfig,usb_mode.h,usb_mode.c}`, `firmware/components/usb_device/tests/{run-msvc.ps1,test_usb_mode.c}`

**Interfaces:**
- Produces:
  - `typedef enum { KVM_USB_MODE_SERIAL_JTAG = 0, KVM_USB_MODE_COMPOSITE = 1 } kvm_usb_mode_t;`
  - `typedef enum { KVM_USB_REASON_DEFAULT, KVM_USB_REASON_STORED, KVM_USB_REASON_NOT_BUILT, KVM_USB_REASON_FORCED_BY_BUILD, KVM_USB_REASON_SERIAL_ONCE, KVM_USB_REASON_ESCAPE_BUTTON, KVM_USB_REASON_ABNORMAL_RESET, KVM_USB_REASON_UNCONFIRMED_LIMIT } kvm_usb_reason_t;`
  - `typedef struct { bool composite_built; bool force_serial; bool serial_once; bool escape_held; bool abnormal_reset; uint8_t stored_mode; uint8_t unconfirmed; uint8_t max_unconfirmed; } kvm_usb_boot_inputs_t;`
  - `typedef struct { kvm_usb_mode_t mode; kvm_usb_reason_t reason; bool persist_serial; bool count_attempt; } kvm_usb_boot_decision_t;`
  - `kvm_usb_boot_decision_t kvm_usb_mode_decide(const kvm_usb_boot_inputs_t *in);` (NULL → serial, DEFAULT)
  - `bool kvm_usb_mode_needs_window(const kvm_usb_boot_inputs_t *in);` (true only when, ignoring `escape_held`, the decision would be composite)
  - `typedef struct { const char *pattern; size_t length; size_t matched; } kvm_usb_magic_t;` `void kvm_usb_magic_init(kvm_usb_magic_t *m, const char *pattern, size_t length);` `bool kvm_usb_magic_feed(kvm_usb_magic_t *m, const uint8_t *bytes, size_t n);` (same restart rule as `kvm_transport_diagnostics_requested`: a 0 byte restarts at 1)

- [ ] **Step 1: Failing tests** in `test_usb_mode.c`, one per row of spec Decision 7 in priority order: not built → serial/NOT_BUILT even with `stored_mode = 1`; force → FORCED_BY_BUILD; serial_once beats escape and stored; escape → ESCAPE_BUTTON, `persist_serial = false`; abnormal → ABNORMAL_RESET, `count_attempt = false`; `unconfirmed = 3, max 3` → UNCONFIRMED_LIMIT with `persist_serial = true`; stored 1 → COMPOSITE/STORED with `count_attempt = true`; stored 0, 2 or 255 → serial/DEFAULT. `needs_window`: true only for the stored-composite row. Magic: split across feeds, embedded in frame bytes, a 0 byte mid-pattern, two patterns fed the same bytes.
- [ ] **Step 2: Run** `pwsh -NoProfile -ExecutionPolicy Bypass -File firmware/components/usb_device/tests/run-msvc.ps1` (copy the `input_hub/tests/run-msvc.ps1` layout). Expected: compile or assertion failures.
- [ ] **Step 3: Implement** `usb_mode.c` (no ESP-IDF includes). `Kconfig` with all `KVM_USB_*` symbols from Global Constraints, `KVM_USB_COMPOSITE_SUPPORT` default n with help text pointing at the spec. `CMakeLists.txt` registers only `usb_mode.c` for now, no `REQUIRES` beyond `log`.
- [ ] **Step 4: Verify** the test script and `tools/firmware.ps1 build` (the component is built but unused; app image size unchanged within a few bytes).
- [ ] **Step 5: Commit** `Add the usb_device component with a pure USB boot-mode decision`.

### Task 2: Composite descriptors as pure data

**Who:** Agent.

**Files:**
- Create: `firmware/components/usb_device/{usb_descriptors.h,usb_descriptors.c}`, `firmware/components/usb_device/tests/test_usb_descriptors.c`
- Modify: `firmware/components/usb_device/CMakeLists.txt`, `firmware/components/usb_device/tests/run-msvc.ps1`

**Interfaces:**
- Produces: constants `KVM_USB_ITF_KEYBOARD 0`, `KVM_USB_ITF_POINTER 1`, `KVM_USB_ITF_CDC_COMM 2`, `KVM_USB_ITF_CDC_DATA 3`, `KVM_USB_ITF_COUNT 4`, `KVM_USB_EP_KEYBOARD_IN 0x81`, `KVM_USB_EP_POINTER_IN 0x82`, `KVM_USB_EP_CDC_NOTIF_IN 0x83`, `KVM_USB_EP_CDC_OUT 0x04`, `KVM_USB_EP_CDC_IN 0x84`, `KVM_USB_REPORT_ID_MOUSE 1`, `KVM_USB_REPORT_ID_CONSUMER 2`, `KVM_USB_BCD_DEVICE 0x0100`, `KVM_USB_ESPRESSIF_VID 0x303A`;
  `void kvm_usb_device_descriptor(uint16_t vid, uint16_t pid, uint8_t out[18]);`
  `extern const uint8_t kvm_usb_config_descriptor[]; extern const size_t kvm_usb_config_descriptor_len;`
  `extern const uint8_t kvm_usb_keyboard_report_desc[]; extern const size_t kvm_usb_keyboard_report_desc_len;`
  `extern const uint8_t kvm_usb_pointer_report_desc[]; extern const size_t kvm_usb_pointer_report_desc_len;`
  `typedef struct { uint8_t interfaces; uint8_t in_endpoints; uint8_t out_endpoints; bool iad_before_cdc; bool total_length_ok; } kvm_usb_config_summary_t;` `bool kvm_usb_config_parse(const uint8_t *desc, size_t len, kvm_usb_config_summary_t *out);`

- [ ] **Step 1: Failing tests:** device descriptor bytes (bcdUSB 0x0200, class EF/02/01, EP0 64, VID/PID little endian, bcdDevice, string indexes 1/2/3, one configuration); config parse: 4 interfaces, IN endpoints == 4 (assert `<= 4`), 1 OUT, IAD with first interface 2 and count 2, `wTotalLength == len`, bmAttributes `0xA0` (bus powered + remote wakeup), bMaxPower 125 (250 mA); keyboard interface 03/01/01 with HID descriptor pointing at a report descriptor of `kvm_usb_keyboard_report_desc_len`; pointer interface 03/00/00; keyboard report descriptor contains no Report ID item (`0x85`); pointer report descriptor declares IDs 1 and 2, 16-bit X/Y, wheel, AC Pan (`0x0A 0x38 0x02`), 16-bit consumer.
- [ ] **Step 2: Run** the suite; expected failures.
- [ ] **Step 3: Implement** the arrays with named byte constants (no TinyUSB headers, so MSVC compiles them; the TinyUSB glue in task 10 passes these arrays as-is). Endpoint intervals per spec Decision 1.
- [ ] **Step 4: Verify** suite + firmware build.
- [ ] **Step 5: Commit** `Add composite USB descriptors and their structural tests`.

### Task 3: HID report builders and the state-based HID sink

**Who:** Agent.

**Files:**
- Create: `firmware/components/usb_device/{usb_hid_sink.h,usb_hid_sink.c}`, `firmware/components/usb_device/tests/test_usb_hid_sink.c`
- Modify: `firmware/components/usb_device/CMakeLists.txt` (`REQUIRES router` for `kvm_router_input_t`), `tests/run-msvc.ps1`

**Interfaces:**
- Consumes: `kvm_router_input_t` (`router.h`).
- Produces:
  - `typedef struct { bool (*ready)(void *context, uint8_t itf); bool (*send)(void *context, uint8_t itf, uint8_t report_id, const uint8_t *data, size_t len); void *context; } kvm_usb_hid_port_t;`
  - `typedef struct { uint8_t keys[8]; uint8_t buttons; int32_t dx, dy; int16_t wheel, pan; uint16_t consumer; bool keys_dirty, pointer_dirty, consumer_dirty; bool consumer_next; } kvm_usb_hid_sink_t;`
  - `void kvm_usb_hid_sink_init(kvm_usb_hid_sink_t *s);`
  - `void kvm_usb_hid_sink_apply(kvm_usb_hid_sink_t *s, const kvm_router_input_t *in);` (keyboard/consumer replace state; pointer replaces buttons, adds motion)
  - `void kvm_usb_hid_sink_release_all(kvm_usb_hid_sink_t *s);` (zero keys/buttons/consumer, drop motion, mark all dirty)
  - `void kvm_usb_hid_sink_abandon(kvm_usb_hid_sink_t *s);` (zero everything, nothing dirty: host already dropped state)
  - `void kvm_usb_hid_keyboard_report(const kvm_usb_hid_sink_t *s, uint8_t out[8]);`
  - `bool kvm_usb_hid_pointer_report(kvm_usb_hid_sink_t *s, uint8_t out[7]);` (takes a clamped chunk, ±32767, wheel/pan ±127, leaves the remainder)
  - `void kvm_usb_hid_consumer_report(const kvm_usb_hid_sink_t *s, uint8_t out[2]);`
  - `bool kvm_usb_hid_sink_pump(kvm_usb_hid_sink_t *s, const kvm_usb_hid_port_t *port);` (at most one report per interface per call; on interface 1 alternates consumer and pointer when both are dirty; clears a dirty flag only after `send` returned true; returns true when nothing is pending)
  - `bool kvm_usb_hid_sink_pending(const kvm_usb_hid_sink_t *s);`

- [ ] **Step 1: Failing tests:** keyboard bytes equal the input's 8 bytes; pointer accumulation over three applies then one report; clamping of 40000 into 32767 + 7233 over two pumps; wheel saturation; consumer press then release both delivered when the port is busy for the first pump; alternation (pointer and consumer dirty, two pumps send both); busy `ready` leaves dirty set; `send` false leaves dirty set; release_all sends zeroed keyboard, buttons 0, consumer 0 and no motion; abandon sends nothing.
- [ ] **Step 2: Run** suite, expect failures.
- [ ] **Step 3: Implement** (pure C, no ESP-IDF).
- [ ] **Step 4: Verify** suite + firmware build.
- [ ] **Step 5: Commit** `Add the state-based USB HID sink and report builders`.

### Task 4: Input hub PC target and chords while the host is armed

**Who:** Agent.

**Files:**
- Modify: `firmware/components/input_hub/{input_hub.h,input_hub.c}`, `firmware/components/input_hub/tests/test_input_hub.c`

**Interfaces:**
- Produces (additions only; existing names and results unchanged):
  - new fields in `kvm_input_hub_t`: `kvm_router_output_t pc_output; void *pc_context; bool pc_enabled; bool pc_armed; bool pc_live; uint8_t host_request_target;`
  - `void kvm_input_hub_set_pc_output(kvm_input_hub_t *hub, kvm_router_output_t pc, void *pc_context);` (enables target 0 as an output; also sets target 0 with `explicit_target = true`; slot argument passed to `pc` callbacks is always 0)
  - `void kvm_input_hub_set_pc_live(kvm_input_hub_t *hub, bool live);` (false = unmounted: abandon PC held state and arm without emitting)
  - new result appended: `KVM_HUB_CHORD_HOST_ARMED` (chord recognized while `router_armed`; `host_request_target` holds 0 to 3; Esc gives 0)
- Rules: target 0 follows the guest rules (lazy arm via `pc.arm`, `send`, dirty on refusal or not-ready, flush, yield sends all-up/zero then `pc.release`). `pc_enabled == false` keeps today's stub branch exactly.

- [ ] **Step 1: Failing tests** (new cases; every existing case must stay untouched and green): `pc_target_forwards_when_enabled`; `pc_not_ready_marks_dirty_and_flush_resends_want`; `pc_unlive_abandons_without_emitting`; `chord_pc_to_guest_releases_pc_first` (order of fake calls: PC keyboard all-up, PC pointer zero, PC consumer zero, PC release, guest arm); `chord_guest_to_pc_releases_guest_first`; `yield_with_pc_armed_sends_all_up_to_pc`; `router_armed_drops_input_and_pc_gets_nothing`; `chord_while_router_armed_returns_host_request` (Ctrl+Alt+2 gives target 2, Esc gives 0, the chord keys are still swallowed, nothing emitted); `set_pc_output_starts_on_pc_and_disables_auto_target`; `stub_unchanged_without_pc_output`.
- [ ] **Step 2: Run** `pwsh -NoProfile -ExecutionPolicy Bypass -File firmware/components/input_hub/tests/run-msvc.ps1`; expect the new cases to fail.
- [ ] **Step 3: Implement.** Keep `armed_slot` for guests; `pc_armed` for the PC. Move chord detection ahead of the `router_armed` drop only to report `KVM_HUB_CHORD_HOST_ARMED`; local state is still cleared and nothing emitted. Update header comments for target 0.
- [ ] **Step 4: Verify** the hub suite, `firmware/tests/integration/run-msvc.ps1` (hub worker must stay green), firmware build.
- [ ] **Step 5: Commit** `Make This PC a real hub target when a PC output is bound`.

### Task 5: Byte-link abstraction under the USB worker (USJ only)

**Who:** Agent.

**Files:**
- Create: `firmware/components/transport/{transport_link.h,transport_link_usj.c}`, `firmware/tests/integration/test_transport_link.c`
- Modify: `firmware/components/transport/{transport_usb_serial_jtag.c,transport_usb_serial_jtag.h,transport_diagnostics.c,CMakeLists.txt}`, `firmware/tests/integration/run-msvc.ps1`

**Interfaces:**
- Produces:
  - `typedef struct { bool (*connected)(void *context); int (*read)(void *context, uint8_t *buf, size_t cap, uint32_t wait_ticks); int (*write)(void *context, const uint8_t *buf, size_t len, uint32_t wait_ticks); void *context; } kvm_transport_link_t;`
  - `const kvm_transport_link_t *kvm_transport_link_usj(void);` (wraps `usb_serial_jtag_is_connected/read_bytes/write_bytes`)
  - `esp_err_t kvm_transport_usb_start(const kvm_transport_link_t *link);` and `kvm_transport_usb_serial_jtag_start()` kept as `kvm_transport_usb_start(kvm_transport_link_usj())` after installing the USJ driver.
  - `void kvm_transport_diagnostics_dump_to(const kvm_transport_link_t *link);` (old dump calls it with the USJ link)
  - A pure helper for testing the write loop: `bool kvm_transport_link_write_all(const kvm_transport_link_t *link, const uint8_t *bytes, size_t length, uint32_t per_try_ticks, uint32_t max_tries);`

- [ ] **Step 1: Failing test** `test_transport_link.c`: `write_all` with a fake link that accepts partial writes, a link that returns 0 forever (gives up after `max_tries`, returns false), a link that errors.
- [ ] **Step 2: Run** `firmware/tests/integration/run-msvc.ps1`; expect failure.
- [ ] **Step 3: Implement**: the worker's `usb_serial_jtag_*` calls become `link->connected/read/write`; `send_binary` uses `write_all` with the same 50 ms budget and resets the core on false. No other behaviour change; worker name, priority and stack stay.
- [ ] **Step 4: Verify** all suites + firmware build; compare the app image size with task 4 (small growth expected).
- [ ] **Step 5: Commit** `Route the USB worker through a byte link (USB Serial/JTAG only)`.

## Phase B: mode switch and recovery (agent tasks 6 to 8, owner task 9)

### Task 6: Mode store, serial-once flag, escape window and boot wiring

**Who:** Agent (build + host tests). Composite support stays `n`.

**Files:**
- Create: `firmware/components/usb_device/{usb_mode_store.h,usb_mode_store.c,usb_boot.h,usb_boot.c}`
- Modify: `firmware/components/usb_device/CMakeLists.txt` (`PRIV_REQUIRES nvs_flash esp_system driver`), `firmware/main/{main.c,CMakeLists.txt}`, `firmware/components/transport/transport_usb_serial_jtag.c` (magic handling in the read path), `firmware/components/usb_device/tests/test_usb_mode.c`

**Interfaces:**
- Produces:
  - `esp_err_t kvm_usb_mode_store_load(uint8_t *mode, uint8_t *unconfirmed);` (missing keys → 0, ESP_OK), `esp_err_t kvm_usb_mode_store_set_mode(kvm_usb_mode_t mode);`, `esp_err_t kvm_usb_mode_store_set_unconfirmed(uint8_t count);`
  - `bool kvm_usb_take_serial_once(void);` and `void kvm_usb_set_serial_once(void);` (RTC_NOINIT `{uint32_t magic = 0x55534231; uint32_t inverse;}`)
  - `bool kvm_usb_reset_abnormal(void);` (maps `esp_reset_reason()`: PANIC, INT_WDT, TASK_WDT, WDT, BROWNOUT, CPU_LOCKUP → true)
  - `kvm_usb_boot_decision_t kvm_usb_boot_decide(void);` (gathers inputs, runs the escape window only when `kvm_usb_mode_needs_window`, samples BOOT GPIO0 every 10 ms, BOOT counted when held ≥ 200 ms, applies `persist_serial` and `count_attempt` to NVS before returning)
  - `kvm_usb_boot_decision_t kvm_usb_boot_last(void);` (for the screen and STATUS)
  - `void kvm_usb_reboot_serial_once(void);` and `void kvm_usb_reboot_with_mode(kvm_usb_mode_t mode);` (worker only: release, persist if asked, `esp_restart()`)
- Magic handling: the worker feeds each read into two `kvm_usb_magic_t` (patterns from Global Constraints) beside the diagnostics matcher; a match does emergency release, core reset, then the reboot call.

- [ ] **Step 1: Tests** (host side): extend `test_usb_mode.c` with a decision trace for "three short composite boots then serial and persist", using the pure function only. ESP-only files are covered by build + task 9.
- [ ] **Step 2: Implement** the store (namespace `kvm_usb`; open/close per call; commit before return), the RTC flag, `usb_boot.c`. In `main.c` call `kvm_usb_boot_decide()` before `kvm_transport_usb_serial_jtag_start()`; with composite support `n` it never waits and always returns serial (NOT_BUILT), so the boot path is unchanged. Magic strings are active in both modes.
- [ ] **Step 3: Verify** suites + firmware build with the default config; then prove the rescue variant compiles: temporarily set `CONFIG_KVM_USB_FORCE_SERIAL=y` in the git-ignored `firmware/sdkconfig`, build, and restore the file.
- [ ] **Step 4: Commit** `Add the USB mode store, serial-once restart and boot-time escape`.

### Task 7: USB mode screen and display status fields

**Who:** Agent.

**Files:**
- Modify: `firmware/components/display/{display_model.h,display_model.c,display.c,display.h}`, `firmware/components/transport/{transport_display_status.c,transport_usb_serial_jtag.c}`, `firmware/tests/display/test_display_model.c`, `firmware/tests/integration/test_transport_display_status.c`

**Interfaces:**
- Produces: `kvm_display_status_t` fields `uint8_t usb_mode; uint8_t pc_hid; uint8_t usb_reason;` (0 defaults = today); `typedef enum { KVM_DISPLAY_USB_USE_COMPOSITE = 1, KVM_DISPLAY_USB_USE_SERIAL = 2, KVM_DISPLAY_USB_SERIAL_ONCE = 3 } kvm_display_usb_request_t;` `void kvm_display_set_usb_fn(void (*fn)(kvm_display_usb_request_t));` and the worker handler `void kvm_transport_usb_request(kvm_display_usb_request_t request);` (queue to the worker; the worker refuses USE_COMPOSITE when the build lacks support and shows "Not in this build").
- Badge rules from spec Decision 22: `usb_mode == 0` → unchanged text and dim rule; `usb_mode == 1` → `PC on` / `PC asleep` / `USB off` (+ ` | App` with a host session, + ` | KB MS`), dim only when `pc_hid == 0`, This PC hint only when `pc_hid != 2`.

- [ ] **Step 1: Failing tests** in `test_display_model.c`: every badge combination above; serial-mode strings byte-identical to today; dim rule both modes; hint rule. In `test_transport_display_status.c`: new fields copied through.
- [ ] **Step 2: Run** `firmware/tests/display/run-msvc.ps1` and integration; expect failures.
- [ ] **Step 3: Implement** model + LVGL screen (entered from the existing guest settings action), showing mode, reason text per `kvm_usb_reason_t`, and the three actions with a confirmation tap. The escape-window prompt "Hold BOOT for serial mode" is drawn if the panel is already up when the window runs (builder decides the order in `app_main`; the window must not depend on the panel).
- [ ] **Step 4: Verify** suites + build.
- [ ] **Step 5: Commit** `Add the USB mode screen and composite-mode badge`.

### Task 8: Flash workflow, skill and recovery documentation

**Who:** Agent (script + script tests; no live port access).

**Files:**
- Modify: `tools/firmware.ps1`, `tools/tests/test-firmware-script.ps1`, `.claude/skills/esp32-kvm-firmware/SKILL.md` (only if the owner agrees: it is outside `firmware/` and `docs/`), `README.md` (build/flash/recovery sections), `docs/Designs/recovery/README.md`
- Create: `docs/hardware/usb-recovery-drill.md`

**Interfaces:**
- `tools/firmware.ps1`: `Get-BoardPorts` returns objects `{Port, Mode}` where Mode is `serial` (`VID_303A&PID_1001`) or `composite` (`VID_303A&PID_<allow-listed>` with `MI_02`); `ports` prints the mode; `flash` with only a composite board: busy check, write `"\0KVM-USB-SERIAL-ONCE\n"` with `System.IO.Ports.SerialPort` (DTR/RTS false), wait ≤ 15 s for a serial board, then the unchanged `idf.py -p <port> app-flash`. New action `usb-serial` writes `"\0KVM-USB-SERIAL-MODE\n"` (persists serial). Test-only env `ESP32_KVM_FAKE_PORTS` accepts `COM5:composite`.
- `docs/hardware/usb-recovery-drill.md`: the owner procedures for task 9 and task 11, each with "expected" and "record in board-bringup.md".

- [ ] **Step 1: Failing script tests:** composite-only fake port + `-DryRun flash` prints the magic write, the wait and the app-flash line; two composite boards refuse; `ports` lists the mode; no erase option in any printed command.
- [ ] **Step 2: Run** `pwsh -NoProfile -ExecutionPolicy Bypass -File tools/tests/test-firmware-script.ps1`.
- [ ] **Step 3: Implement** script + docs (README recovery: BOOT+plug ROM mode, replug three times, escape window, touchscreen, `firmware.ps1 usb-serial`, rescue build).
- [ ] **Step 4: Verify** script tests + firmware build.
- [ ] **Step 5: Commit** `Teach the flash script the composite board and document USB recovery`.

### Task 9: Owner drill 1: prove ROM download and the serial-side paths

**Who:** Owner only (physical). The agent prepares the build and the record template, then waits for the owner's results; silence is not a pass.

- [ ] **Step 1 (owner): ROM download drill, before anything new is flashed.** Quit the desktop app. Hold BOOT, plug in, release. Expect `303A:1001` in Device Manager. Run (agent may run with owner approval) `esptool.py --chip esp32s3 --port COMn --before no_reset --after no_reset chip_id`: expect "Staying in bootloader". Run `espefuse.py --port COMn summary` (read-only): record `DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE`, `DIS_DOWNLOAD_MODE`, `USB_PHY_SEL`. Unplug, replug without BOOT: the app returns, bonds intact (guests reconnect).
- [ ] **Step 2 (owner): flash from ROM mode.** BOOT+plug again, then `tools/firmware.ps1 flash -Port COMn` with the phase B image (app-only). Replug: the app runs, bonds intact. This proves the reflash floor.
- [ ] **Step 3 (owner): serial-side paths on the phase B image.** USB mode screen shows "Serial / Not in this build". Write `"\0KVM-USB-SERIAL-ONCE\n"` with `tools/firmware.ps1` (or the touchscreen action): the board restarts and comes back as `303A:1001`; the screen shows reason "Serial once". A panic is not induced.
- [ ] **Step 4 (agent, after the owner reports):** record the results with date, image hash and esptool output in `docs/hardware/board-bringup.md` ("Recovery status"); commit `Record the USB recovery drill`. **Gate:** task 10's image must not be flashed unless Steps 1 and 2 passed.

## Phase C: TinyUSB CDC transport (agent task 10, owner task 11)

### Task 10: esp_tinyusb, composite bring-up and the CDC link

**Who:** Agent (build only). Owner approval needed before committing the lock change.

**Files:**
- Create: `firmware/components/usb_device/{idf_component.yml,usb_composite.h,usb_composite.c}`, `firmware/components/transport/transport_link_cdc.c`
- Modify: `firmware/components/usb_device/{CMakeLists.txt,Kconfig}`, `firmware/components/transport/CMakeLists.txt`, `firmware/main/main.c`, `firmware/sdkconfig.defaults` (`CONFIG_KVM_USB_COMPOSITE_SUPPORT=y` and the esp_tinyusb CDC/HID counts and buffer sizes), `firmware/dependencies.lock` (separate commit, owner-approved), `crates/host-actor/examples/probe_serial.rs` (take the port from argv, default COM4)

**Interfaces:**
- `idf_component.yml`: `espressif/esp_tinyusb` pinned to the newest version that resolves for IDF 5.5.1 (expected `~3.0.1`; record the resolved version). Check whether esp_tinyusb defines `tud_mount_cb`/`tud_umount_cb`/`tud_suspend_cb`/`tud_resume_cb` or HID callbacks; only define what it does not.
- `usb_composite.h`:
  - `typedef struct { bool attached; bool mounted; bool suspended; bool remote_wakeup_allowed; uint32_t bus_resets; uint64_t mounted_since_ms; } kvm_usb_composite_state_t;`
  - `esp_err_t kvm_usb_composite_start(uint16_t pid);` (builds the device descriptor with `kvm_usb_device_descriptor`, strings "DoubTech", "DoubTech HID", MAC serial; `tinyusb_driver_install`; `tinyusb_cdcacm_init` on port 0 with `callback_rx`)
  - `void kvm_usb_composite_state(kvm_usb_composite_state_t *out);` (atomics copy; any task)
  - `kvm_usb_hid_port_t kvm_usb_composite_hid_port(void);` (`ready` = `tud_hid_n_ready(itf)` and mounted and not suspended; `send` = `tud_hid_n_report(itf, report_id, data, len)`; keyboard instance 0 report id 0)
  - `bool kvm_usb_composite_remote_wakeup(void);`
  - `void kvm_usb_composite_stop(void);` (`tinyusb_driver_uninstall`, before a reboot)
- `const kvm_transport_link_t *kvm_transport_link_cdc(void);` (semaphore-gated read, `tinyusb_cdcacm_write_queue` + `write_flush(0, 0)`, `connected` = mounted and not suspended)
- `main.c`: decision COMPOSITE → after the escape window, `usb_serial_jtag_driver_uninstall()`, `kvm_usb_composite_start(CONFIG_KVM_USB_COMPOSITE_PID)`, `kvm_transport_usb_start(kvm_transport_link_cdc())`; any start error → `kvm_usb_reboot_serial_once()`. A worker-side timer clears `unconf` after `KVM_USB_CONFIRM_MS` mounted. HID output is not wired yet (target 0 still the stub).

- [ ] **Step 1:** Add the manifest; run `tools/firmware.ps1 build`; show the owner the `firmware/dependencies.lock` diff and ask for approval to commit it. Without approval stop here and report.
- [ ] **Step 2: Implement** glue and CDC link; log TinyUSB task stack high-water into the existing Health line.
- [ ] **Step 3: Verify** build with composite support y (default) and n (temporary sdkconfig override) and all host suites. Record app size and static RAM.
- [ ] **Step 4: Commit** (two commits) `Pin esp_tinyusb for the composite USB device` (lock only, owner-approved) and `Bring up the composite USB device and carry the control protocol over CDC`.

### Task 11: Owner drill 2: first composite image

**Who:** Owner only. Agent records results.

- [ ] **Step 1 (owner): flash by the known method.** App quit; `tools/firmware.ps1 flash` (board is still `303A:1001` because `mode` is absent). Reboot: serial mode, screen reason "Default".
- [ ] **Step 2 (owner): enable composite** on the USB mode screen. Expect after the 1.5 s window: COM4 disappears; Device Manager shows a composite device with "HID Keyboard Device", "HID-compliant mouse", "HID-compliant consumer control device" and "USB Serial Device (COMx)" whose hardware ID has `MI_02`. Record VID/PID, COM number, any "unknown device" or Code 10.
- [ ] **Step 3 (owner, agent may run with approval):** `cargo run --offline -p esp32-kvm-host-actor --example probe_serial -- COMx`: expect CAPS board `esp32-kvm-s3` and SESSION_OPEN ACK over CDC.
- [ ] **Step 4 (owner): every revert path**, re-enabling composite between each: (a) replug and hold BOOT within the window → serial this boot; (b) `tools/firmware.ps1 usb-serial` → serial persisted; (c) touchscreen "Use serial mode"; (d) replug three times within 30 s each → serial and reason "3 failed starts"; (e) `tools/firmware.ps1 flash` while composite → serial-once handoff, flash, returns to composite; (f) BOOT+plug → ROM mode reachable while `mode = 1`.
- [ ] **Step 5 (owner):** leave composite on 10 minutes; read the Health line (UART not wired: use the diagnostics dump if the trace build is used) or report "not measured". Return to serial for daily use (MSI 0.1.1 cannot see composite).
- [ ] **Step 6 (agent):** record in `board-bringup.md`; commit `Record the first composite USB drill`. **Gate** for phase D: Steps 2 to 4 passed.

## Phase D: HID output for hub target 0 (agent tasks 12 and 13)

### Task 12: Wire the PC HID sink into the hub worker, suspend/resume and remote wakeup

**Who:** Agent.

**Files:**
- Create: `firmware/components/transport/{transport_pc_hid.h,transport_pc_hid.c}`
- Modify: `firmware/components/transport/{transport_hub.h,transport_hub.c,transport_usb_serial_jtag.c,CMakeLists.txt}`, `firmware/tests/integration/{test_hub_worker.c,run-msvc.ps1}`

**Interfaces:**
- Consumes: `kvm_usb_hid_sink_t`, `kvm_usb_hid_port_t` (task 3), `kvm_input_hub_set_pc_output`, `kvm_input_hub_set_pc_live`, `KVM_HUB_CHORD_HOST_ARMED` (task 4), `kvm_usb_composite_state_t` (task 10).
- Produces:
  - `typedef struct { kvm_usb_hid_sink_t sink; kvm_usb_hid_port_t port; bool mounted, suspended, rwu_allowed; uint64_t last_wake_ms; bool (*wake)(void *context); void *wake_context; } kvm_pc_hid_t;`
  - `void kvm_pc_hid_init(kvm_pc_hid_t *pc, kvm_usb_hid_port_t port, bool (*wake)(void *), void *wake_context);`
  - `kvm_router_output_t kvm_pc_hid_output(void);` (context = `kvm_pc_hid_t *`; `ready` = mounted and not suspended; `arm` = ready; `send` = ready ? apply + pump : (maybe wake, false); `release` = release_all + pump, true when unmounted)
  - `void kvm_pc_hid_set_link(kvm_pc_hid_t *pc, bool mounted, bool suspended, bool rwu_allowed, uint64_t now_ms);` (unmounted edge → sink abandon)
  - `void kvm_pc_hid_pump(kvm_pc_hid_t *pc);` (every worker iteration)
  - `void kvm_hub_worker_bind_pc(kvm_hub_worker_t *worker, kvm_pc_hid_t *pc);` (calls `kvm_input_hub_set_pc_output`); `void kvm_hub_worker_set_pc_link(kvm_hub_worker_t *worker, bool mounted, bool suspended, bool rwu_allowed);` (forwards to `kvm_pc_hid_set_link` and `kvm_input_hub_set_pc_live(mounted)`)
- Worker: in composite mode, each iteration copies `kvm_usb_composite_state`, calls `set_pc_link`, drains the hub, pumps the PC sink; a `KVM_HUB_CHORD_HOST_ARMED` result sends `kvm_transport_core_device_select_request(core, hub.host_request_target)`; the 1 s slot sample path is unchanged. Wake rule: key press or button press, target 0, suspended, `rwu_allowed`, ≥ 2000 ms since the last wake.

- [ ] **Step 1: Failing integration tests** in `test_hub_worker.c` with a fake HID port and fake wake: typing on target 0 produces keyboard reports in order; `ready` false then true resends the wanted state once; suspend while a key is held, release on the device during suspend, resume → one all-up report; unmount while held → no report, and after remount nothing is resent; router arm while the hub is on the PC → PC all-up before the router's arm call; chord while router armed → one DEVICE_SELECT_REQUEST frame with the chord's slot; wake called once for two presses 500 ms apart and again after 2 s; motion never wakes; consumer release survives a busy endpoint.
- [ ] **Step 2: Run** `firmware/tests/integration/run-msvc.ps1`; expect failures.
- [ ] **Step 3: Implement** and wire in `transport_usb_serial_jtag.c` only under `CONFIG_KVM_USB_COMPOSITE_SUPPORT` and only when the boot decision is COMPOSITE (serial mode keeps the stage-2 stub and auto-target).
- [ ] **Step 4: Verify** every suite + firmware build (composite y and n).
- [ ] **Step 5: Commit** `Send device-owned input to the PC over USB HID`.

### Task 13: Protocol minor 5: STATUS key 128

**Who:** Agent.

**Files:**
- Modify: `crates/protocol/src/{lib.rs,cbor.rs}` (`MINOR = 5`, `UsbStatus` decode), `protocol/schema/{v1.md,protocol_v1.h}` (regenerate with `protocol/schema/generate-header.ps1`), `firmware/components/transport/{transport_core.h,transport_core.c}`, `firmware/components/transport/tests/transport_core_test.c`, `firmware/tests/integration/test_transport_router.c`, `tests/vectors/generate.ps1`, `docs/spec/04-usb-protocol.md`
- Create: `tests/vectors/status-m5.cobs` (and `.hex` if the generator emits one), Rust tests in `crates/protocol/tests/`

**Interfaces:**
- C: `typedef struct { uint8_t mode; uint8_t pc_hid; uint8_t device_target; uint8_t device_inputs; } kvm_transport_usb_status_t;` `void kvm_transport_core_set_usb_status(kvm_transport_core_t *core, const kvm_transport_usb_status_t *status);` (STATUS key 128 emitted only when `core->minor >= 5`; a change marks STATUS due like slot changes)
- Rust: `pub enum UsbMode { SerialJtag, Composite }`, `pub enum PcHidState { Off, Suspended, Ready }`, `pub struct UsbStatus { pub mode: UsbMode, pub pc_hid: PcHidState, pub device_target: u8, pub device_inputs: u8 }`, `Status.usb: Option<UsbStatus>`; key 128 rejected below minor 5, values out of range rejected.

- [ ] **Step 1: Failing tests:** C: minor 4 STATUS bytes identical to the existing vectors; minor 5 STATUS contains key 128 with canonical CBOR ordering (after key 6). Rust: decode the new vector; reject key 128 at minor 4; reject `pc_hid = 3`.
- [ ] **Step 2: Run** `firmware/tests/integration/run-msvc.ps1` and `cargo test --offline -p esp32-kvm-protocol`.
- [ ] **Step 3: Implement**; the worker fills the status every iteration (mode from `kvm_usb_boot_last`, PC state, hub target, central connected flags).
- [ ] **Step 4: Verify** all C suites, `cargo test --offline --workspace`, firmware build; old vectors byte-identical (`git diff --stat tests/vectors` shows only new files).
- [ ] **Step 5: Commit** `Report USB mode and PC HID state in STATUS (protocol minor 5)`.

## Phase E: host discovery and docs (agent tasks 14 and 15, owner task 16)

### Task 14: Host discovery, update handoff and desktop display

**Who:** Agent.

**Files:**
- Modify: `crates/usb-transport/{Cargo.toml,src/lib.rs,tests/discovery.rs}`, `crates/host-actor/src/lib.rs`, `crates/update-runner/src/{flasher.rs,reconnect.rs}`, `apps/desktop/src-tauri/src/actor_backend.rs`, desktop UI file that shows device status (builder finds it; read-only USB mode and PC HID line)

**Interfaces:**
- `serialport = { version = "=4.10.1", features = ["usbportinfo-interface"] }` in usb-transport (other crates keep their own lines; one feature unification is enough, verify `Cargo.lock` only gains nothing or the feature).
- `PortIdentity { name, usb_id, interface: Option<u8> }`; `PortIdentity::usb(name, vid, pid)` unchanged (interface None); `PortIdentity::usb_interface(name, vid, pid, interface)`.
- `pub const COMPOSITE_PIDS: &[u16] = &[0x4009];` `pub enum BoardUsbMode { SerialJtag, Composite }` `pub fn usb_mode(port: &PortIdentity) -> Option<BoardUsbMode>;` `candidate_ports` = ports with `usb_mode(..).is_some()`; composite requires `interface == Some(2) || Some(3)`.
- update-runner: `pub const SERIAL_ONCE_MAGIC: &[u8] = b"\0KVM-USB-SERIAL-ONCE\n";` before reads, if the only candidate is Composite: open it, write the magic, close, poll ports every 250 ms up to 15 s for exactly one SerialJtag candidate, then the existing esptool sequence on it; never pass a Composite port to esptool. `reconnect.rs` accepts either mode after the flash.

- [ ] **Step 1: Failing tests:** discovery (`303A:1001` any interface; `303A:4009` with interface 2 and 3 accepted, with 0, 1 or None rejected; `303A:4001` rejected as today; `1234:4009` rejected); flasher fake runner: composite-only → magic written to that port, waits, esptool gets the new serial port; timeout → `FlashError::Failed` and no esptool call; two candidates → refuse; reconnect accepts a composite port.
- [ ] **Step 2: Run** `cargo test --offline -p esp32-kvm-usb-transport -p esp32-kvm-update-runner -p esp32-kvm-host-actor`.
- [ ] **Step 3: Implement**; desktop shows "USB mode: HID / Serial" and "PC HID: ready / asleep / off" from `Status.usb` when present.
- [ ] **Step 4: Verify** `cargo test --offline --workspace` and `cargo check --offline -p <desktop crate> --target x86_64-pc-windows-msvc` (name from `apps/desktop/src-tauri/Cargo.toml`).
- [ ] **Step 5: Commit** `Find composite boards and hand them to the flasher through serial-once`.

### Task 15: Documentation

**Who:** Agent.

**Files:**
- Modify: `docs/Designs/usb-transport/*`, `docs/Designs/hardware-primary/README.md`, `docs/Designs/input-hub/README.md`, `docs/Designs/device-screens/*`, `docs/Designs/recovery/README.md`, `README.md`, `docs/hardware/board-bringup.md` (status line only)
- Create: `docs/Designs/usb-device/README.md` (descriptors table, endpoint budget, mode decision table, recovery paths, PC target arbitration table, suspend rules, VID/PID status and the open PID request)

- [ ] **Step 1:** Write the docs from the spec and the code as built (actual Kconfig names, actual esp_tinyusb version, measured sizes).
- [ ] **Step 2: Verify** all suites + build; links resolve.
- [ ] **Step 3: Commit** `Document the composite USB device`.

### Task 16: Owner hardware checklist (final)

**Who:** Owner only (install the phase E desktop build, flash, physical checks). The agent records results in `board-bringup.md` and commits `Record the stage 3 hardware checklist`.

- [ ] **(a) Pairing a PC:** composite mode on; Windows shows keyboard, mouse, consumer control and COMx; the new desktop app connects over CDC and shows "USB mode: HID".
- [ ] **(b) Typing into the PC** from the paired BLE keyboard with the desktop app **closed**: text in Notepad, Shift/Ctrl combos, mouse move, wheel, horizontal scroll, volume keys; at the Windows lock screen; in BIOS/UEFI setup after a reboot (boot keyboard).
- [ ] **(c) Switching by chord:** Ctrl+Alt+1/2/3 to guests and Ctrl+Alt+0 back, five round trips; the screen highlight follows.
- [ ] **(d) Key release on switch:** hold Shift (and separately a mouse button), chord to a guest, release, return: nothing stuck on the PC or the guest.
- [ ] **(e) PC app coexistence:** app arms a guest with PC capture; the device keyboard types nowhere on the PC while armed; Ctrl+Alt+0 on the device keyboard makes the app return to Local (DEVICE_SELECT_REQUEST); app back in Local → device keyboard types on the PC again. Note whether the device mouse triggers the app's edge switching (R5).
- [ ] **(f) Cable unplug/replug** five times with a key held at unplug: board returns to composite (each run longer than 30 s confirms) and nothing is stuck.
- [ ] **(g) PC sleep/wake:** Start > Sleep; wake with the device keyboard (remote wakeup), and with the PC's own button; after wake the device keyboard types, the app reconnects, no stuck keys; display sleep only (monitor off) changes nothing.
- [ ] **(h) Flashing recovery drill repeat:** `tools/firmware.ps1 flash` in composite mode (serial-once handoff), the in-app updater in composite mode, BOOT+plug ROM mode, replug-three-times revert.
- [ ] **(i) Legacy board path:** a serial-mode board is found by the new app exactly as before.
- [ ] **(j) Soak:** 30 minutes mixed use; Health line or diagnostics dump: TinyUSB and worker stack minimum, free internal heap.

## Self-review

**Spec coverage.** Decisions 1 to 3 (composite, keyboard, reports): tasks 2, 3, 10. Decision 4 (VID/PID): Kconfig in task 1, host allow-list task 14, PID request noted in task 15. Decision 5 (Windows): task 11 step 2, task 16 (a). Decisions 6 to 9 (default, decision, escape, serial once): tasks 1, 6, 7, 8, drills 9 and 11. Decision 10 (ROM floor before any TinyUSB image): task 9 gate before task 11. Decision 11 (staged rollout): phase B images have composite `n`; task 10 turns it on. Decision 12 (flash workflow): tasks 8 and 14. Decision 13 (discovery): task 14. Decisions 14 and 15 (PC target, arbitration, chord while armed): tasks 4 and 12. Decision 16 (threading, lock approval): Global Constraints, tasks 10 and 12. Decision 17 (CDC link): tasks 5 and 10. Decisions 18 and 19 (suspend, remote wakeup): task 12, checklist (f) and (g). Decision 20 (minor 5): task 13. Decision 21 (host app): task 14. Decision 22 (display): task 7. Decisions 23 to 25 (testing): every agent task has host tests; owner tasks 9, 11, 16. Risks R1 to R8: R1 tasks 6 to 9 and 11; R2 task 2 test plus task 11 step 2; R3 tasks 11 and 14; R4 tasks 12 and 16 (f, g); R5 task 16 (e); R6 task 10 step 1; R7 task 11 step 5 and 16 (j); R8 task 11 step 4 (e).

**Type consistency.** `kvm_usb_mode_t`/`kvm_usb_reason_t`/`kvm_usb_boot_decision_t` (task 1) are used by tasks 6, 7, 10, 13. `kvm_usb_hid_port_t` and `kvm_usb_hid_sink_t` (task 3) are produced by `kvm_usb_composite_hid_port` (task 10) and consumed by `kvm_pc_hid_t` (task 12). `kvm_router_output_t` is the only output type the hub sees (task 4 `pc_output`, task 12 `kvm_pc_hid_output`). `kvm_transport_link_t` (task 5) has the USJ (task 5) and CDC (task 10) implementations. `KVM_HUB_CHORD_HOST_ARMED` and `host_request_target` (task 4) feed `kvm_transport_core_device_select_request` (task 12). `kvm_transport_usb_status_t` (task 13, C) mirrors `UsbStatus` (task 13, Rust) field for field; `pc_hid` values 0/1/2 match `kvm_display_status_t.pc_hid` (task 7). The magic strings and `0x4009` appear identically in Global Constraints, tasks 6, 8, 14. Endpoint and interface constants are defined once (task 2).

**Open items for the owner before execution.** (1) Approve the `firmware/dependencies.lock` change in task 10. (2) Whether task 8 may edit `.claude/skills/esp32-kvm-firmware/SKILL.md`. (3) Request a dedicated PID from `espressif/usb-pids` (or choose an owned VID) before any release; the interim `0x4009` is shared with other TinyUSB devices.
