# Composite BLE HID guest contract

The original one-link bring-up sequence below is historical. The current
three-link source model and its remaining wire/physical gates are recorded in
[Three-link HID source capacity](three-guest-source-capacity.md). The host still
advertises and routes one live slot.

## Scope and wire layout

The `firmware/components/hid` component registers one HID-over-GATT service (`0x1812`) with one composite report map. It owns report values and one connection's output gate. Its optional `hid_guest_start()` initializes NVS and NimBLE, advertises one GAP identity and initiates encrypted bonding for a single guest. Firmware `main` does not call it until board recovery and toolchain validation. This M0/M1 leaf supports one connected guest; extending it to three requires connection-indexed channels and measured controller capacity.

| Report | ID | GATT Report Reference | Value bytes | Meaning |
|---|---:|---|---:|---|
| Keyboard input | 1 | `{1, input}` | 8 | Modifier bits, reserved zero, six USB usage IDs. |
| Keyboard LED output | 1 | `{1, output}` | 1 | Num, Caps, Scroll, Compose, Kana bits. |
| Relative mouse input | 2 | `{2, input}` | 7 | Five buttons and three pad bits, signed 16-bit X/Y, signed 8-bit vertical wheel and horizontal pan. |
| Consumer input | 3 | `{3, input}` | 2 | One 16-bit usage; zero releases. Supported usage range is 0..0x03ff. |

The input characteristics are readable and notify. NimBLE creates a CCCD for each notify characteristic; Report Reference descriptors are explicit. Input reads and LED/protocol writes require an encrypted link. Notifications pass the **connection handle** to `ble_gatts_notify_custom`; the component never broadcasts. The report bytes exclude report IDs because GATT Report Reference identifies each characteristic. The report map contains three application collections, with input bit lengths 64, 56, and 16 and keyboard output bit length 8.

Protocol Mode (`0x2a4e`) accepts report protocol value `1`. Boot protocol value `0` returns an ATT error; boot/Bios Bluetooth behavior is outside v1. HID Control Point (`0x2a4c`) suspend releases and disarms. Exit suspend remains disarmed until the routing actor performs a fresh arm. The component does not advertise boot input characteristics.

## Safe integration sequence

1. After board recovery verification, enable ESP-IDF NimBLE in the shared firmware configuration and explicitly call `hid_guest_start()` from the firmware owner. It initializes NVS without erasing it, configures a fixed GAP name, registers the composite HID service and starts advertising. One connection is admitted; an unexpected second connection is terminated.
2. The guest startup event callback initiates BLE security on connect, checks for both encryption and a bond on `BLE_GAP_EVENT_ENC_CHANGE`, tracks CCCD subscription events by connection and value handle, and disarms on disconnect. It refuses repeat pairing instead of silently deleting a bond.
3. Treat `hid_gatt_channel()->armed` as firmware output gate state. The routing actor calls `hid_channel_arm` only after the target is READY, the old route has released, and route generation is current. Arm sends three zero input reports first. If any notification cannot be enqueued, arm fails and remains disarmed.
4. Before switching, suspending, updating or returning local, call `hid_channel_release`. It disarms before attempting zero keyboard, mouse and consumer notifications. If this or an active send fails, the channel sets `needs_disconnect`; terminate the BLE link and keep it disarmed. A new encrypted, subscribed connection must receive the all-up baseline before arm.
5. Route USB keyboard, pointer and consumer state only through the `hid_channel_*` send functions. A callback return of success means BLE enqueue, not guest application delivery. Pointer deltas are not retained or replayed.

The one-channel implementation rejects a second simultaneous connection, and losing encryption or any input subscription immediately disarms. Its bring-up pairing requests LE Secure Connections with Just Works and no MITM verification; the SDK configuration may still allow legacy pairing. The later BLE policy task must add user-approved pairing windows, admission filtering, and passkey/numeric-comparison UX before production use. The root firmware integration owns heartbeat safety and route generation checks.

For ESP-IDF 5.5.1 on ESP32-S3, the shared `firmware/sdkconfig.defaults` needs these settings before the firmware owner invokes `hid_guest_start()`:

```text
CONFIG_BT_ENABLED=y
CONFIG_BTDM_CTRL_MODE_BLE_ONLY=y
CONFIG_BT_BLUEDROID_ENABLED=n
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y
CONFIG_BT_NIMBLE_GATT_SERVER=y
CONFIG_BT_NIMBLE_SECURITY_ENABLE=y
CONFIG_BT_NIMBLE_SM_SC=y
CONFIG_BT_NIMBLE_NVS_PERSIST=y
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1
CONFIG_BT_NIMBLE_MAX_BONDS=8
CONFIG_BT_NIMBLE_MAX_CCCDS=8
CONFIG_BT_CTRL_BLE_MAX_ACT=2
CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL=y
```

The HID component declares `REQUIRES bt nvs_flash`. These options come from the [v5.5.1 NimBLE peripheral example](https://raw.githubusercontent.com/espressif/esp-idf/v5.5.1/examples/bluetooth/nimble/bleprph/sdkconfig.defaults), [NimBLE Kconfig](https://raw.githubusercontent.com/espressif/esp-idf/v5.5.1/components/bt/host/nimble/Kconfig.in), and [ESP32-S3 controller Kconfig source](https://raw.githubusercontent.com/espressif/esp-idf/v5.5.1/components/bt/controller/esp32c3/Kconfig.in). In particular, NVS bond persistence defaults off, so enable it explicitly. The S3 controller delegates to that controller Kconfig; `MAX_ACT=2` budgets one connection and one advertising activity for this one-guest bring-up. Resolve the generated `sdkconfig` and build in the pinned SDK before flashing.

## Verification and physical gates

Portable tests: `powershell -ExecutionPolicy Bypass -File firmware/tests/hid/run-msvc.ps1`. The script compiles the descriptor/state, GATT adapter, and optional guest startup tests with `/W4 /WX`, runs all three, and writes only to the ignored `firmware/tests/hid/build/` directory. The GATT and startup tests use narrow NimBLE API stand-ins, so they do not establish SDK binary compatibility or radio behavior.

Pending before a one-guest claim: install/pin ESP-IDF and build firmware; identify the attached board revision, native USB path and BOOT recovery; verify safe flashing; pair a real Windows and macOS guest; read back the report map and report references; confirm one GAP identity, encrypted access and bond reconnect; verify keyboard LEDs, five mouse buttons, wheel/pan and consumer release; request unsupported boot protocol; interrupt a held key and force disconnect, then verify no held input after reconnect. Record OS versions, SDK version and observed report bytes. Do not treat source tests as hardware pass evidence.

Implementation API choices follow the [Apache NimBLE GATT declarations](https://github.com/apache/mynewt-nimble/blob/master/nimble/host/include/host/ble_gatt.h), [ATT error and permission definitions](https://github.com/apache/mynewt-nimble/blob/master/nimble/host/include/host/ble_att.h), and [Espressif's NimBLE GATT server example](https://github.com/espressif/esp-idf/blob/master/examples/bluetooth/ble_get_started/nimble/NimBLE_GATT_Server/README.md).
