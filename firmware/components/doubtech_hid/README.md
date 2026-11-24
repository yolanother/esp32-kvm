# doubtech_hid

An ESP-IDF component that makes an ESP32-S3 a combined Bluetooth LE HID peripheral:
one keyboard, relative mouse and consumer-control (media keys) device that up to three
hosts can pair with and use at once, with eight stored bonds. It handles the HID report
map, the GATT services, numeric-comparison pairing, bond storage and a fail-safe
per-link output gate, and exposes a small API. It does not choose which host receives
input; the embedding firmware does.

Current KVM firmware identity: configured in `firmware/main/main.c` as name "ESP32 KVM", manufacturer "ESP32 KVM", model "ESP32 KVM S3", default bond namespace.

Documentation (under `docs/Designs/doubtech-hid/` at the repository root):

- [Architecture](../../../docs/Designs/doubtech-hid/README.md): layering, threading, pairing, advertising, bond storage, tests and known gaps.
- [API reference](../../../docs/Designs/doubtech-hid/api.md): every public function, type and constant.
- [Integration guide](../../../docs/Designs/doubtech-hid/integration.md): sdkconfig, CMake, a full example, renaming, troubleshooting.

## Quick start

```c
#include "doubtech_hid.h"

static void on_event(const doubtech_hid_pairing_event_t *event, void *context)
{
    /* NimBLE host thread: copy the event to a queue and return. */
    (void)event; (void)context;
}

void app_main(void)
{
    const doubtech_hid_config_t config = {
        .device_name = "My Keyboard",  /* 1..18 bytes; NULL selects "BLE HID" */
        .manufacturer = "Example Co",  /* 1..32 bytes; NULL selects "DoubTech" */
        .model = "KB-1",               /* 1..32 bytes; NULL selects "BLE HID" */
        .bond_namespace = NULL,        /* NULL selects the default NVS namespace */
    };
    doubtech_hid_pairing_set_events(on_event, NULL);   /* required for pairing */
    if (doubtech_hid_start(&config) != ESP_OK) { /* handle the error */ }
}
```

From another task, once a guest is ready: `doubtech_hid_request_slots`,
`doubtech_hid_request_arm_slot(1)`, `doubtech_hid_request_keyboard_slot(1, keys)`,
`doubtech_hid_request_release_slot(1)`. Each blocks at most about 20 ms.

Limits that matter: the device name is at most 18 bytes (it must fit the 31-byte
advertising packet next to the flags, HID service UUID and appearance), the Device
Information strings at most 32 bytes, the bond namespace 1 to 15 characters. A second
`doubtech_hid_start` returns `ESP_ERR_INVALID_STATE`.

## Build and install

This section covers using the library in a firmware project. The toolchain, the desktop
app and the shared steps (installing ESP-IDF v5.5.1, building and flashing this
repository's firmware, tests, CI, recovery) are in the
[root README](../../../README.md#build-and-install).

1. Add the component. In this repository it is at `firmware/components/doubtech_hid/` and
   is found automatically. In another ESP-IDF 5.5 or newer project, copy or link the
   directory into the project's `components/`, or add it with
   `set(EXTRA_COMPONENT_DIRS "/path/to/firmware/components/doubtech_hid")` before
   `project()`. Then require it from the component that includes `doubtech_hid.h`:

   ```cmake
   idf_component_register(SRCS "main.c" INCLUDE_DIRS "." REQUIRES doubtech_hid)
   ```

2. Set the sdkconfig values in your `sdkconfig.defaults` (the reasons are in the
   [integration guide](../../../docs/Designs/doubtech-hid/integration.md)):

   ```text
   CONFIG_BT_ENABLED=y
   CONFIG_BT_BLUEDROID_ENABLED=n
   CONFIG_BT_NIMBLE_ENABLED=y
   CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y
   CONFIG_BT_NIMBLE_GATT_SERVER=y
   CONFIG_BT_NIMBLE_SECURITY_ENABLE=y
   CONFIG_BT_NIMBLE_SM_SC=y
   CONFIG_BT_NIMBLE_NVS_PERSIST=y
   CONFIG_BT_NIMBLE_MAX_CONNECTIONS=3
   CONFIG_BT_NIMBLE_MAX_BONDS=8
   CONFIG_BT_NIMBLE_MAX_CCCDS=24
   CONFIG_BT_CTRL_BLE_MAX_ACT=4
   CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL=y
   ```

   Keep an `nvs` data partition in the partition table. Delete a stale generated
   `sdkconfig` so the new defaults apply.

3. Build with ESP-IDF v5.5.1 for `esp32s3`. In this repository:

   ```powershell
   pwsh -NoProfile -ExecutionPolicy Bypass -File tools/firmware.ps1 build
   ```

   In your own project: `idf.py set-target esp32s3`, then `idf.py build`.

4. Flash. In this repository, quit the desktop app first (it owns the serial port), then:

   ```powershell
   pwsh -NoProfile -ExecutionPolicy Bypass -File tools/firmware.ps1 ports -DryRun
   pwsh -NoProfile -ExecutionPolicy Bypass -File tools/firmware.ps1 flash -Port COM4
   ```

   `flash` writes the application image only and never erases flash, so bonds stay
   (`-Full` also writes the bootloader, partition table and OTA data, and also never
   erases). Without `-DryRun`, `ports` opens the port and may reset the board. In your
   own project use `idf.py app-flash`; never erase flash or the NVS partition if paired
   hosts must survive.

Verification status of the commands above (2026-11-24): `tools/firmware.ps1 build`,
`ports -DryRun` and `flash -DryRun -Port COM9` were run and passed. A real `flash` and a
live `ports` were not run in this review (they open the serial port, may reset the board
and change the device). The CMake and `idf.py` forms for a separate project were not run
(this repository's own build was).

## Tests

Host-side C tests, Windows with Visual Studio 2022 and a Windows SDK, no board needed.
They compile the real component sources against stand-ins for ESP-IDF and NimBLE under
`tests/mocks/`. Run from the repository root:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File firmware/components/doubtech_hid/tests/run-msvc.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File firmware/components/doubtech_hid/tests/check-product-names.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File firmware/components/doubtech_hid/tests/check-api-docs.ps1
```

- `tests/run-msvc.ps1`: nine suites (report gate, GATT, guest and pairing flow,
  guest-initiated pairing, identity configuration with 11 cases, the keyboard-only
  diagnostic profile, and the diagnostic adapter). It accepts suite names as arguments.
- `tests/check-product-names.ps1`: fails if a product name ("kvm", any case) appears in the
  shipped part of the component (`include/`, `core/`, `ble/`, `Kconfig`, `CMakeLists.txt`,
  `idf_component.yml`). The legacy namespace token `kvm_bonds` and the copyright line are
  allowed.
- `tests/check-api-docs.ps1`: fails, listing the names, if any `doubtech_hid_*` function,
  type or macro, or any `DOUBTECH_HID_*` constant declared in `include/doubtech_hid.h` is
  missing from `docs/Designs/doubtech-hid/api.md`. It prints how many names it checked.

All three were run on 2026-11-24 and passed. The C tests are not part of CI. CI's
`firmware` job (ESP-IDF v5.5.1) builds the firmware and is the independent build gate.
The tests do not show radio behavior, ESP-IDF linking or host-OS interoperability.

## Diagnostic options

`CONFIG_DOUBTECH_HID_GATT_TRACE` (log ATT access, subscriptions and notify results) and
`CONFIG_DOUBTECH_HID_KEYBOARD_ONLY_DIAGNOSTIC` (keyboard-only profile named
"DoubTech KB Test"). Both default to off and are not for release builds; see the
[architecture](../../../docs/Designs/doubtech-hid/README.md).

## Known limitations

- `doubtech_hid_pairing_snapshot` takes an internal type (`struct hid_pairing *`) and is not
  usable from application code without the private header.
- The Device Information PnP ID is fixed to the development board's USB VID and PID.
- Only the BLE peripheral role exists; a central input client is reserved, not built.
- Behavior with three real hosts, or on macOS, Windows and Linux in general, is not
  established by this component's tests.
