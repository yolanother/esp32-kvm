# Device-owned input (BLE central) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the board pair the owner's own BLE keyboard and mouse and route their input to the selected guest without the PC app, and stop the screen from pausing when no host session is open.

**Architecture:** A new `doubtech_hid_central` component (NimBLE GATT client, registry, report parsers) feeds a pure `input_hub` component (target, arbitration against the router, chords, fail-safe release) that runs inside the existing USB worker loop beside `kvm_router_t`. The host-driven path is unchanged and wins while it is armed. The display model gains host-session and input-device facts and a dimmed "USB off" badge.

**Tech Stack:** C, ESP-IDF 5.5.1 (ESP32-S3), NimBLE (central + peripheral), LVGL, MSVC host-side tests (`run-msvc.ps1` pattern), PowerShell build script `tools/firmware.ps1`.

**Spec:** `docs/superpowers/specs/2026-11-25-device-input-central-design.md`

## Global Constraints

- Branch `feature/device-input-central`; each task ends with the firmware build green (`pwsh -NoProfile -ExecutionPolicy Bypass -File tools/firmware.ps1 build`) and the existing suites green: `firmware/tests/{display,router,pairing,integration}/run-msvc.ps1` and `firmware/components/doubtech_hid/tests/run-msvc.ps1`.
- Only the USB worker task touches `kvm_router_t` and the hub. NimBLE-host callbacks copy and enqueue; they never call router or hub functions.
- No product name ("kvm", any case) in `doubtech_hid_central` or `input_hub` shipped files, except the legacy token allowed by the existing guard; extend `check-product-names.ps1` coverage to both.
- Guest bond layout (`kvm_bonds`, magic `0x4b564d42`, version 2, 196 bytes) and the 500 ms host lease are unchanged. Input identities use NVS namespace `dthid_inputs`.
- sdkconfig: `BT_NIMBLE_ROLE_CENTRAL=y`, `BT_NIMBLE_MAX_CONNECTIONS=5`, `BT_CTRL_BLE_MAX_ACT=8`, `BT_NIMBLE_MAX_BONDS=12`, `BT_NIMBLE_MAX_CCCDS=24`. Never erase flash; flashing needs the owner's approval and the quit-the-app rule from `tools/firmware.ps1`.
- Chords: Left Ctrl + Left Alt + `1`/`2`/`3` = guest, `0` = This PC, `Esc` = release all. Input pairing window 30 s, sets `sm_mitm=0`, exclusive with guest pairing. Hub event queue depth 16; overflow drops motion before key releases.
- Deploying (flash, installer) is out of scope for the tasks; Task 12 lists owner-run checks.

## Review Focus

1. Guest pairing capacity must stay 8 guests even with input bonds present (`doubtech_hid_pairing_open` counting); test with 8 guest + 2 input bonds in the NimBLE store.
2. A key release must never be lost: queue overflow, device disconnect and chord swallow all end with an all-up report on the previous target.
3. The hub must never emit while `router.armed`, and must release its own arm before the router arms the same slot.
4. A chord must be swallowed fully (no stray `1` forwarded) and must not fire when extra modifiers are held or keys are held across a target change.
5. A mouse report with a wheel, a 12-bit axis or a report ID must parse or be rejected with a reason, never mis-scale silently.

---

### Task 1: Connection budget and library additions

**Files:**
- Modify: `firmware/sdkconfig.defaults`, `firmware/components/doubtech_hid/include/doubtech_hid.h`, `firmware/components/doubtech_hid/ble/hid_guest.c`, `firmware/components/doubtech_hid/tests/test_doubtech_hid_config.c`, `firmware/components/doubtech_hid/tests/test_hid_guest.c`, `docs/Designs/doubtech-hid/api.md`
- Modify: `firmware/main/main.c` (config field only)

**Interfaces:**
- Produces: `doubtech_hid_config_t.foreign_bond_reserve` (`uint8_t`, default 0, max 8); `bool doubtech_hid_host_synced(void)`; `pairing_open` capacity rule `nimble_bonds >= DOUBTECH_HID_MAX_BONDS + foreign_bond_reserve || pairing.bond_count >= DOUBTECH_HID_MAX_BONDS`.

- [ ] **Step 1: Failing tests.** In `test_hid_guest.c` add `pairing_open_ignores_foreign_bonds`: mock store reports 10 NimBLE bonds with `foreign_bond_reserve = 4` and `pairing.bond_count = 6`; expect `doubtech_hid_pairing_open() == ESP_OK`. Add `pairing_open_refused_at_eight_guests`: `bond_count = 8` plus 2 foreign; expect `ESP_ERR_INVALID_STATE` and a `DOUBTECH_HID_PAIRING_CAPACITY` event. In `test_doubtech_hid_config.c` add `reserve_above_8_rejected` and `host_synced_false_before_sync_true_after`.
- [ ] **Step 2: Run** `pwsh -NoProfile -ExecutionPolicy Bypass -File firmware/components/doubtech_hid/tests/run-msvc.ps1 test_hid_guest test_doubtech_hid_config`. Expected: the new cases fail.
- [ ] **Step 3: Implement** the field, `doubtech_hid_host_synced()` (set in `on_sync`, cleared in `on_reset`) and the capacity rule in `hid_guest.c` (`doubtech_hid_pairing_open`, around the `ble_store_util_bonded_peers` call). Validate the reserve in `doubtech_hid_start`.
- [ ] **Step 4: sdkconfig.** Apply the budget from Global Constraints to `firmware/sdkconfig.defaults`; delete a stale generated `firmware/sdkconfig` only if the build fails to pick them up (it is git-ignored). Set `.foreign_bond_reserve = 4` in `main.c`. Document the new names in `docs/Designs/doubtech-hid/api.md` (`check-api-docs.ps1` must pass).
- [ ] **Step 5: Verify.** Run the three doubtech_hid scripts (`run-msvc.ps1`, `check-product-names.ps1`, `check-api-docs.ps1`) and `tools/firmware.ps1 build`. Free heap cannot be measured without flashing, so record the build's size summary (flash and static RAM) before and after in the commit message and list free-heap measurement as an owner-run item in Task 12.
- [ ] **Step 6: Commit** `Add foreign-bond reserve and host-synced query; raise BLE connection budget`.

### Task 2: Pure input hub: targets, arbitration, fail-safe

**Files:**
- Create: `firmware/components/input_hub/{CMakeLists.txt,input_hub.h,input_hub.c}`, `firmware/components/input_hub/tests/{run-msvc.ps1,test_input_hub.c}`

**Interfaces:**
- Consumes: `router.h` types `kvm_router_input_t`, `kvm_router_output_t`.
- Produces:
  - `typedef enum { KVM_HUB_FORWARDED, KVM_HUB_SWALLOWED, KVM_HUB_DROPPED_HOST_ARMED, KVM_HUB_DROPPED_NOT_READY, KVM_HUB_TARGET_CHANGED } kvm_hub_result_t;`
  - `typedef struct { kvm_router_output_t output; void *context; uint8_t target; uint8_t armed_slot; uint8_t keys[8]; uint8_t buttons; } kvm_input_hub_t;`
  - `void kvm_input_hub_init(kvm_input_hub_t *hub, kvm_router_output_t output, void *context);`
  - `bool kvm_input_hub_set_target(kvm_input_hub_t *hub, uint8_t target);` (0 to 3; sends all-up to the old target first)
  - `kvm_hub_result_t kvm_input_hub_input(kvm_input_hub_t *hub, const kvm_router_input_t *input, bool router_armed);`
  - `void kvm_input_hub_yield(kvm_input_hub_t *hub);` (all-up, release, clear `armed_slot`; called when the router arms)
  - `void kvm_input_hub_device_lost(kvm_input_hub_t *hub);` (all-up to the target)
  - Target 0 sink: `output` callbacks are not called for slot 0 (stub, input returns `KVM_HUB_FORWARDED`).

- [ ] **Step 1: Tests** (mock output records calls): `forwards_to_ready_guest_after_arm`, `drops_when_router_armed` (and releases its own arm), `drops_when_target_not_ready` (target kept), `set_target_sends_all_up_then_arms_new`, `yield_releases_and_clears_arm`, `device_lost_sends_all_up_keyboard_and_zero_mouse`, `target_zero_is_noop_sink`, `invalid_target_rejected`.
- [ ] **Step 2: Run** `pwsh -NoProfile -ExecutionPolicy Bypass -File firmware/components/input_hub/tests/run-msvc.ps1`. Expected: fail (no source). Model the script on `firmware/tests/router/run-msvc.ps1`.
- [ ] **Step 3: Implement** the hub as pure C; arm lazily on first forwarded event and release on every target change, yield and device-lost-with-nothing-held.
- [ ] **Step 4: Run** the test; expected PASS. Build the firmware (component registered in `CMakeLists.txt`, `REQUIRES router`).
- [ ] **Step 5: Commit** `Add the pure input hub with host-armed arbitration`.

### Task 3: Hub chords

**Files:**
- Modify: `firmware/components/input_hub/input_hub.{h,c}`, `firmware/components/input_hub/tests/test_input_hub.c`

**Interfaces:**
- Produces: chord handling inside `kvm_input_hub_input`: returns `KVM_HUB_TARGET_CHANGED` when a chord changes the target and `KVM_HUB_SWALLOWED` for chord reports that leave it unchanged; internal `static bool hub_chord(const uint8_t keys[8], uint8_t *target, bool *release_all)`.

- [ ] **Step 1: Tests:** `ctrl_alt_1_selects_guest_1`, `ctrl_alt_0_selects_pc`, `ctrl_alt_esc_releases_all_to_pc`, `chord_with_extra_modifier_is_forwarded`, `chord_key_not_forwarded_after_target_change` (the same report held afterwards stays swallowed until all keys up), `right_ctrl_alt_is_not_a_chord`, `all_up_old_target_before_switch`.
- [ ] **Step 2: Run** the suite; new cases fail.
- [ ] **Step 3: Implement** modifier mask check (`0x05` exactly = LCtrl+LAlt), first non-modifier key decides; latch "swallow until all non-modifier keys released".
- [ ] **Step 4: Run** the suite; PASS. Build.
- [ ] **Step 5: Commit** `Add keyboard chords to switch the hub target`.

### Task 4: Report parsers

**Files:**
- Create: `firmware/components/doubtech_hid_central/{CMakeLists.txt,Kconfig,idf_component.yml,hogp_parse.h,hogp_parse.c}`, `firmware/components/doubtech_hid_central/tests/{run-msvc.ps1,test_hogp_parse.c}`

**Interfaces:**
- Produces:
  - `bool hogp_parse_boot_keyboard(const uint8_t *data, size_t len, uint8_t keys[8]);` (len 8 exact)
  - `bool hogp_parse_boot_mouse(const uint8_t *data, size_t len, uint8_t *buttons, int16_t *dx, int16_t *dy);` (len >= 3; 8-bit signed)
  - `typedef struct { uint8_t report_id; bool has_id; uint8_t button_bits, x_bits, y_bits; uint8_t x_off, y_off, wheel_off, wheel_bits; bool has_wheel; } hogp_mouse_map_t;`
  - `typedef enum { HOGP_MAP_OK, HOGP_MAP_UNSUPPORTED, HOGP_MAP_MALFORMED } hogp_map_result_t;`
  - `hogp_map_result_t hogp_mouse_map_from_descriptor(const uint8_t *map, size_t len, hogp_mouse_map_t *out);`
  - `bool hogp_parse_mouse_report(const hogp_mouse_map_t *map, const uint8_t *data, size_t len, uint8_t *buttons, int16_t *dx, int16_t *dy, int8_t *wheel);`

- [ ] **Step 1: Tests** with real descriptor byte arrays: standard 4-byte 3-button+wheel mouse; 12-bit X/Y mouse (Logitech style) with report ID 2; boot-only descriptor; descriptor with absolute axes (`HOGP_MAP_UNSUPPORTED`); truncated descriptor (`MALFORMED`); keyboard report of length 7 rejected; sign extension of 12-bit negative values; wheel -1.
- [ ] **Step 2: Run** `pwsh -NoProfile -ExecutionPolicy Bypass -File firmware/components/doubtech_hid_central/tests/run-msvc.ps1`; fails.
- [ ] **Step 3: Implement** a bounded short-item HID descriptor walker (no long items; max 16 global-stack entries; stops at 512 bytes) that only recognises Generic Desktop X/Y/Wheel, Button page, Input with relative flag.
- [ ] **Step 4: Run** PASS. If the walker proves too large, keep only the boot functions, mark the mouse-map functions as returning `HOGP_MAP_UNSUPPORTED`, and note the cut in the ledger (spec Decision 6).
- [ ] **Step 5: Commit** `Add HOGP boot and narrow report-map parsers`.

### Task 5: Input-device registry and storage

**Files:**
- Create: `firmware/components/doubtech_hid_central/{central_registry.h,central_registry.c,central_store.h,central_store.c}`, `firmware/components/doubtech_hid_central/tests/test_central_registry.c` (reuse `doubtech_hid/tests/mocks/nvs*.h` via an include path in `run-msvc.ps1`)

**Interfaces:**
- Produces:
  - `typedef enum { DTHID_INPUT_KEYBOARD = 1, DTHID_INPUT_MOUSE = 2 } dthid_input_kind_t;`
  - `typedef struct { uint8_t addr[6]; uint8_t addr_type; dthid_input_kind_t kind; char name[24]; } dthid_input_record_t;`
  - `typedef struct { dthid_input_record_t slots[2]; bool used[2]; } central_registry_t;`
  - `bool central_registry_set(central_registry_t *r, const dthid_input_record_t *rec);` (replaces the same kind)
  - `bool central_registry_get(const central_registry_t *r, dthid_input_kind_t kind, dthid_input_record_t *out);`
  - `bool central_registry_forget(central_registry_t *r, dthid_input_kind_t kind);`
  - `esp_err_t central_store_load(const char *ns, central_registry_t *r);` / `esp_err_t central_store_save(const char *ns, const central_registry_t *r);` (default `dthid_inputs`, magic + version + CRC32, corrupt blob loads as empty).

- [ ] **Step 1: Tests:** set/get/replace/forget; one record per kind; save then load round-trip; corrupt CRC loads empty; truncated blob loads empty; namespace length limit 15; saving never touches `kvm_bonds`.
- [ ] **Step 2: Run** the suite; fails. **Step 3: Implement.** **Step 4: Run** PASS and build.
- [ ] **Step 5: Commit** `Add the input-device registry and NVS store`.

### Task 6: Central BLE adapter

**Files:**
- Create: `firmware/components/doubtech_hid_central/{include/doubtech_hid_central.h,central_ble.c,central_gatt.c,central_gatt.h}`, `firmware/components/doubtech_hid_central/tests/test_central_gatt.c`
- Modify: `firmware/components/doubtech_hid_central/CMakeLists.txt` (REQUIRES `doubtech_hid`, `bt`, `nvs_flash`)

**Interfaces:**
- Consumes: Task 4 parsers, Task 5 registry, `doubtech_hid_host_synced()`.
- Produces (public header):
  - `typedef struct { uint8_t kind; uint8_t keys[8]; uint8_t buttons; int16_t dx, dy; int8_t wheel; bool lost; } doubtech_hid_central_input_t;`
  - `typedef enum { DTHID_CENTRAL_SCAN_RESULT, DTHID_CENTRAL_PASSKEY, DTHID_CENTRAL_PAIRED, DTHID_CENTRAL_PAIR_FAILED, DTHID_CENTRAL_CONNECTED, DTHID_CENTRAL_DISCONNECTED } doubtech_hid_central_event_type_t;`
  - `typedef struct { doubtech_hid_central_event_type_t type; uint8_t index; uint8_t kind; char name[24]; int8_t rssi; uint32_t passkey; uint8_t reason; } doubtech_hid_central_event_t;` (`reason`: 1 not-supported, 2 timeout, 3 auth-failed, 4 busy, 5 capacity)
  - `typedef struct { void (*on_input)(const doubtech_hid_central_input_t *, void *); void (*on_event)(const doubtech_hid_central_event_t *, void *); void *context; const char *store_namespace; } doubtech_hid_central_config_t;`
  - `esp_err_t doubtech_hid_central_start(const doubtech_hid_central_config_t *config);`
  - `esp_err_t doubtech_hid_central_pair_begin(void);` (30 s scan, `sm_mitm=0`, refused while guest pairing is open) / `void doubtech_hid_central_pair_cancel(void);` / `esp_err_t doubtech_hid_central_pair_select(uint8_t index);`
  - `esp_err_t doubtech_hid_central_forget(uint8_t kind);` / `bool doubtech_hid_central_connected(uint8_t kind);`
- Callbacks run on the NimBLE host thread and must only copy and enqueue.

- [ ] **Step 1: Tests (pure part).** `central_gatt.c` holds the discovery state machine as a pure function of events (`service found`, `characteristics found`, `protocol-mode written`, `subscribed`, `error`); `test_central_gatt.c` covers boot keyboard happy path, boot mouse, mouse with Report map accepted, device without boot and an unsupported map yields reason 1, discovery error at each state, and `lost` input emitted on disconnect. Copy the mock pattern in `doubtech_hid/tests/mocks` only if a NimBLE type is needed; prefer none.
- [ ] **Step 2: Run** the suite; fails. **Step 3: Implement** the state machine, then `central_ble.c` using `ble_gap_disc`, `ble_gap_connect`, `ble_gattc_disc_svc_by_uuid(0x1812)`, `ble_gattc_disc_all_chrs`, `ble_gattc_write_flat` for protocol mode and CCCD, `ble_gattc_read` for the report map, `ble_gap_security_initiate`, and `BLE_GAP_EVENT_PASSKEY_ACTION` (display action: `ble_sm_inject_io`). Pairing windows are mutually exclusive with `doubtech_hid_pairing_open` by checking its state; return `reason 4` otherwise.
- [ ] **Step 4: Run** tests PASS; `tools/firmware.ps1 build` green (this is the only compile check for the NimBLE calls).
- [ ] **Step 5: Commit** `Add the BLE central input client`.

### Task 7: Wire the hub into the USB worker

**Files:**
- Modify: `firmware/components/transport/transport_usb_serial_jtag.c`, `firmware/components/transport/transport_usb_serial_jtag.h`, `firmware/components/transport/CMakeLists.txt`, `firmware/main/main.c`, `firmware/components/transport/router_hid_bridge.{c,h}`
- Test: `firmware/tests/integration/test_hub_worker.c` (add to `run-msvc.ps1`)

**Interfaces:**
- Consumes: `kvm_input_hub_*`, `doubtech_hid_central_*`.
- Produces: `typedef struct { uint8_t type; kvm_router_input_t input; } kvm_hub_msg_t;` with `type` in `{KVM_HUB_MSG_INPUT, KVM_HUB_MSG_LOST}`; `void kvm_transport_hub_submit(const kvm_hub_msg_t *msg);` (queue depth 16, drop-motion-first); `uint8_t kvm_transport_hub_target(void);` the worker drains the queue each loop and calls `kvm_input_hub_yield` the loop iteration the router becomes armed.

- [ ] **Step 1: Failing integration test** `hub_input_routes_when_no_session`, `hub_dropped_while_router_armed`, `router_arm_yields_hub`, `queue_overflow_keeps_release`, `device_lost_sends_all_up` using the mocks in `firmware/tests/integration/mocks`.
- [ ] **Step 2: Run** `pwsh -NoProfile -ExecutionPolicy Bypass -File firmware/tests/integration/run-msvc.ps1`; fails.
- [ ] **Step 3: Implement** the queue, the worker drain and yield call, the central callbacks (`on_input` converts to `kvm_hub_msg_t`), and `main.c` start order: `doubtech_hid_start`, then `doubtech_hid_central_start` after `doubtech_hid_host_synced()` is true (poll from the worker, not from `app_main`). Touch and PLUS target selection set `hub.target` when `router.session_open` is false (route through the existing target callback).
- [ ] **Step 4: Run** the integration and router suites; build.
- [ ] **Step 5: Commit** `Run the input hub in the USB worker beside the router`.

### Task 8: Display model and status

**Files:**
- Modify: `firmware/components/display/display_model.{h,c}`, `firmware/components/transport/transport_display_status.{h,c}`, `firmware/components/transport/transport_usb_serial_jtag.c` (`publish_status`), `firmware/tests/display/test_display_model.c`, `firmware/tests/integration/test_transport_display_status.c`

**Interfaces:**
- Produces: `kvm_display_status_t` new fields `bool host_session; bool input_kbd; bool input_mouse; uint8_t hub_target;`; `kvm_display_view_t.usb_dim` (`bool`). `kvm_display_make_view`: when `!usb_connected && !host_session` and `ready_slots > 0`, produce `KVM_DISPLAY_ACTIVE` with `usb_dim = true`, title "DOUBTECH HID", `footer` "USB off"; with no ready guest and no session produce `KVM_DISPLAY_PAUSED` only if `recovery_required` or `fault`, otherwise the guest list (empty cards) with `detail` "No guests connected".

- [ ] **Step 1: Update tests** in `test_display_model.c`: `usb_off_with_ready_guest_shows_active_dimmed`, `usb_off_no_guest_shows_guest_view_not_pause`, `fault_still_pauses`, `recovery_still_wins`; adjust the two tests at lines 35 and 45 that expect "INPUT PAUSED" for `!usb_connected` to the new rule and keep the ones for fault and recovery.
- [ ] **Step 2: Run** `firmware/tests/display/run-msvc.ps1`, expect failures; **Step 3: implement** `display_model.c` (the `!s->usb_connected` branch at line 85), the status projection and `publish_status` (fill the new fields from the hub and central state). Keep `s->fault` and `recovery_required` precedence exactly as is.
- [ ] **Step 4: Run** display, integration suites; build. **Step 5: Commit** `Show the guest view with a dimmed USB-off badge instead of pausing`.

### Task 9: Render the badge and input-device icons

**Files:**
- Modify: `firmware/components/display/display.c`, `docs/Designs/device-screens/screen-state.md`

**Interfaces:**
- Consumes: `kvm_display_view_t.usb_dim`, status `input_kbd`, `input_mouse`.

- [ ] **Step 1:** No automated test (LVGL). Add a pure helper `kvm_display_badge_text(const kvm_display_status_t*, char *out, size_t n)` in `display_model.c` with a test `badge_text_lists_usb_and_inputs` (e.g. "USB off | KB MS").
- [ ] **Step 2: Implement** in `display.c`: dim the existing USB indicator when `usb_dim`, show the badge text in the footer label; keep the card and touch-target geometry unchanged.
- [ ] **Step 3: Run** display tests; build. Update the screen-state doc table.
- [ ] **Step 4: Commit** `Render the USB-off badge and input-device icons`.

### Task 10: Input pairing screen and touch flow

**Files:**
- Modify: `firmware/components/display/display_model.{h,c}`, `display.c`, `display_pairing.{h,c}`, `firmware/components/transport/transport_usb_serial_jtag.c`, `firmware/tests/display/test_display_model.c`, `firmware/tests/integration/test_hub_worker.c`

**Interfaces:**
- Produces: `KVM_DISPLAY_INPUT_PAIR` screen; `kvm_display_status_t.input_pair_state` (`0` closed, `1` scanning, `2` passkey, `3` result), `input_pair_rows[4][25]` with `input_pair_count`, `input_pair_passkey`, `input_pair_reason`; touch actions `KVM_DISPLAY_INPUT_PAIR_BEGIN`, `_SELECT` (row index), `_CANCEL`; `bool kvm_display_input_touch_action(const kvm_display_status_t*, uint16_t x, uint16_t y, kvm_display_input_action_t*, uint8_t *row);`

- [ ] **Step 1: Tests:** view for scanning (rows listed), passkey ("TYPE ON KEYBOARD" with the six digits), `reason` text for not-supported, touch row maps to index, pairing refused when `host_session` or guest pairing is open.
- [ ] **Step 2: Run** (fails). **Step 3: Implement** the entry (extend `guest_settings_action` menu: "Add keyboard or mouse"), worker handling of the new touch requests (call `doubtech_hid_central_pair_*`), and a 30 s deadline mirrored from the central event stream.
- [ ] **Step 4: Run** display + integration; build. **Step 5: Commit** `Add the touchscreen flow to pair a keyboard or mouse`.

### Task 11: Reconnect, forget, persistence and fail-safe

**Files:**
- Modify: `firmware/components/doubtech_hid_central/central_ble.c`, `firmware/components/doubtech_hid_central/tests/test_central_gatt.c`, `firmware/components/transport/transport_usb_serial_jtag.c`, `firmware/tests/integration/test_hub_worker.c`

**Interfaces:**
- Consumes: Task 5 registry, Task 6 state machine.
- Produces: boot reconnect to stored identities (10 s attempt, 15 s gap, none while a pairing scan is active), `DTHID_CENTRAL_CONNECTED`/`DISCONNECTED` events, forget (registry + `ble_store_util_delete_peer`), settings entry "Forget keyboard/mouse" (confirm tap).

- [ ] **Step 1: Tests:** state machine retries on failure, stops when forgotten, resumes after disconnect, never runs during scan; integration: disconnect while a key is held produces all-up on the target.
- [ ] **Step 2: Run** (fails). **Step 3: Implement.** **Step 4: Run** all suites and build.
- [ ] **Step 5: Commit** `Reconnect stored input devices and release keys on loss`.

### Task 12: Documentation, guards and the owner's checklist

**Files:**
- Create: `docs/Designs/doubtech-hid/central.md`, `docs/Designs/input-hub/README.md`
- Modify: `README.md` (root; build, test commands and a new "Pair a keyboard or mouse" section), `firmware/components/doubtech_hid/tests/check-product-names.ps1` (scan the two new components), `docs/Designs/doubtech-hid/README.md`, `docs/Designs/ble/*` (connection budget table), `docs/Designs/device-screens/*`, `.github/workflows` (add the new `run-msvc.ps1` suites only if the existing workflow already runs the C suites; otherwise state they are not in CI)

- [ ] **Step 1:** Document: architecture (central, hub, arbitration table), public API (every name in `doubtech_hid_central.h`; extend `check-api-docs.ps1` to that header), chords, supported devices and the "not supported" reasons, bond budget, security trade-off (Decision 7), limits (R6: This PC target is a stub until stage 3).
- [ ] **Step 2: Run** all test and guard scripts plus `tools/firmware.ps1 build`; paste the totals in the commit body. Report the free flash and RAM figures from the build's size summary.
- [ ] **Step 3: Owner-run hardware checklist** (not performed by the agent; each needs the board flashed, which needs the owner to quit the app and approve): (a) pair the keyboard from the touchscreen, including passkey entry if asked; (b) pair the mouse; (c) power-cycle the board and confirm both reconnect without touching the screen; (d) with the desktop app closed select guest 1 from the chord and type and move the pointer there; repeat for guests 2 and 3; (e) turn the keyboard off while holding a key and confirm nothing stays pressed on the guest; (f) open the desktop app and confirm PC capture still routes to a guest and wins while armed; (g) pair a fourth guest and confirm guest pairing still works with both input devices bonded; (h) watch the screen for 10 minutes with the app closed: no flashing, "USB off" dimmed badge only; (i) record guest notify latency while an input device is reconnecting (R3).
- [ ] **Step 4: Commit** `Document device-owned input and add the hardware checklist`.
