# Integrating doubtech_hid into a firmware project

This guide takes an ESP-IDF v5.5 project for an ESP32-S3 from nothing to a device that
advertises, pairs and sends keyboard, mouse and consumer reports. It assumes you have read
nothing else in this repository. Function details are in [api.md](api.md); the design is in
[the architecture](README.md).

Current KVM firmware identity: configured in `firmware/main/main.c` as name "DoubTech HID", manufacturer "DoubTech", model "DoubTech HID", default bond namespace.

The component targets ESP-IDF 5.5 or newer (`idf_component.yml`: `idf: ">=5.5"`). This
repository builds it with v5.5.1 for the `esp32s3` target. It has not been built for any
other chip.

## 1. Add the component

The component is the directory `firmware/components/doubtech_hid/`. In this repository it is
found automatically. In another project, copy or link that directory into the project's
`components/` directory, or point CMake at it:

```cmake
# Top-level CMakeLists.txt, before project()
set(EXTRA_COMPONENT_DIRS "/path/to/esp32-kvm/firmware/components/doubtech_hid")
```

It is not published to the ESP component registry.

In the `CMakeLists.txt` of the component that calls it, require it:

```cmake
idf_component_register(SRCS "main.c" INCLUDE_DIRS "." REQUIRES doubtech_hid)
```

(`PRIV_REQUIRES doubtech_hid` works when only your `.c` files include the header, as
`firmware/components/transport` does.) The component itself requires `bt`, `nvs_flash`
and `esp_timer`.

## 2. sdkconfig values

Put these in your `sdkconfig.defaults` (this repository's file is
`firmware/sdkconfig.defaults`). The values marked `=3`, `=8`, `=24` and `=4` match the
library's three links and eight bonds.

```text
CONFIG_IDF_TARGET="esp32s3"
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

What they are for:

- `BT_ENABLED`, `BT_NIMBLE_ENABLED`, `BT_BLUEDROID_ENABLED=n`: use the NimBLE host.
- `ROLE_PERIPHERAL`, `GATT_SERVER`: the device is a peripheral serving HID.
- `SECURITY_ENABLE`, `SM_SC`: pairing with LE Secure Connections numeric comparison.
- `NVS_PERSIST`: NimBLE writes its bond keys to NVS. Off by default in ESP-IDF; without it
  bonds are lost at reboot.
- `MAX_CONNECTIONS=3`: three simultaneous guests (`DOUBTECH_HID_MAX_CONNECTIONS`).
- `MAX_BONDS=8`: eight stored bonds (`DOUBTECH_HID_MAX_BONDS`).
- `MAX_CCCDS=24`: how many client characteristic configuration records NimBLE keeps
  (the subscriptions of bonded guests). 24 is this repository's setting for three
  connections (the earlier one-connection bring-up used 8). It was not derived from a
  formula here, so keep it unless you measure otherwise.
- `BT_CTRL_BLE_MAX_ACT=4`: three connections plus one advertising activity.
- `MEM_ALLOC_MODE_INTERNAL`: keep radio buffers in internal RAM, not PSRAM.

Also needed in the partition table: a data partition of type `nvs` (this repository's
`firmware/partitions.csv` has a 16 KiB one at `0x9000`). The service initializes NVS and
never erases it.

An existing generated `sdkconfig` keeps old values: after changing the defaults, delete
`sdkconfig` (or run `idf.py reconfigure` after removing the options) so they take effect.
Do not erase the board's flash to get there.

Optional diagnostic options (both off by default, see [the architecture](README.md)):
`CONFIG_DOUBTECH_HID_GATT_TRACE` and `CONFIG_DOUBTECH_HID_KEYBOARD_ONLY_DIAGNOSTIC`. Neither
belongs in a release build.

## 3. A minimal main

```c
#include "doubtech_hid.h"
#include "esp_log.h"

static const char *tag = "app";

/* Runs on the NimBLE host thread: keep it short and never block. A real
 * application copies the event into a FreeRTOS queue with a zero timeout, as
 * firmware/components/transport does, and handles it on its own task. */
static void on_pairing_event(const doubtech_hid_pairing_event_t *event, void *context)
{
    (void)context;
    switch (event->type) {
    case DOUBTECH_HID_PAIRING_OPENED:
        ESP_LOGI(tag, "pairing window open until %llu ms", (unsigned long long)event->deadline_ms);
        break;
    case DOUBTECH_HID_PAIRING_CHALLENGE:
        /* Show this number to the user; they compare it with the guest's screen
         * and confirm on the guest. The device side is approved automatically. */
        ESP_LOGI(tag, "pairing code %06u", (unsigned)event->number);
        break;
    case DOUBTECH_HID_BONDED:
        ESP_LOGI(tag, "bonded; first token byte %02x", event->token.bytes[0]);
        break;
    case DOUBTECH_HID_PAIRING_REJECTED:
    case DOUBTECH_HID_PAIRING_CAPACITY:
    case DOUBTECH_HID_PAIRING_TIMEOUT:
    case DOUBTECH_HID_PAIRING_CLOSED:
        ESP_LOGW(tag, "pairing event %d", (int)event->type);
        break;
    case DOUBTECH_HID_DISCONNECTED:
        ESP_LOGI(tag, "slot %u disconnected", (unsigned)event->number);
        break;
    }
}

void app_main(void)
{
    const doubtech_hid_config_t config = {
        .device_name = "My Keyboard",   /* at most 18 bytes */
        .manufacturer = "Example Co",   /* at most 32 bytes */
        .model = "KB-1",                /* at most 32 bytes */
        .bond_namespace = NULL,         /* NULL keeps the default NVS namespace */
    };
    /* Set the sink before starting: pairing cannot work without one. */
    doubtech_hid_pairing_set_events(on_pairing_event, NULL);
    esp_err_t result = doubtech_hid_start(&config);
    if (result != ESP_OK) {
        ESP_LOGE(tag, "doubtech_hid_start failed: %d", (int)result);
        return;
    }
    /* From here the device advertises. Pairing opens by itself when a guest starts
     * numeric comparison, or on demand from any task other than the NimBLE host
     * thread with doubtech_hid_request_pair_begin(). */
}
```

`doubtech_hid_start` returns before the BLE host has synced; advertising begins when it
does. Passing `NULL` as the config selects every default (name "BLE HID", manufacturer
"DoubTech", model "BLE HID").

To send input, call the request functions from another task once a guest is READY:

```c
doubtech_hid_slot_t slots[DOUBTECH_HID_MAX_CONNECTIONS];
if (doubtech_hid_request_slots(slots) && slots[0].subscribed) {
    if (doubtech_hid_request_arm_slot(1)) {          /* sends the all-up baseline */
        uint8_t keys[DOUBTECH_HID_KEYBOARD_REPORT_LEN] = {0, 0, 0x04};  /* 'a' down */
        doubtech_hid_request_keyboard_slot(1, keys);
        keys[2] = 0;                                  /* all up */
        doubtech_hid_request_keyboard_slot(1, keys);
        doubtech_hid_request_release_slot(1);         /* disarm and send all-up */
    }
}
```

Each call blocks at most about 20 ms and returns `false` on any failure; see
[api.md](api.md) for what a failure does to the link.

## 4. Wiring output into a router

The component does not choose a target; it exposes three addressable slots. A routing
layer holds the selected slot and forwards. The pattern used by this repository's
`firmware/components/transport/router_hid_bridge.c`:

- ready check: `doubtech_hid_request_ready_slot(slot)`, plus `doubtech_hid_request_slots`
  sampled about once a second for status (this repository does that in
  `firmware/components/transport/transport_usb_serial_jtag.c`, not in `router_hid_bridge.c`);
- switch to a slot: release the old slot (`doubtech_hid_request_release_slot`), then arm
  the new one (`doubtech_hid_request_arm_slot`); arm is the authoritative transition and
  fails if the slot is not ready or already armed;
- each input: `doubtech_hid_request_keyboard_slot`, `_mouse_slot` or `_consumer_slot`;
- any failure: the library already queues a disconnect for a timed-out slot; a router
  should treat a failed input as "stop routing to that slot" and may call
  `doubtech_hid_request_disconnect_slot(slot)`.

Handle `DOUBTECH_HID_DISCONNECTED` events: `event->number` is the one-based slot that
closed (0 after a NimBLE host reset, meaning all slots).

Bond tokens (`doubtech_hid_request_retained_bonds`, `doubtech_hid_request_slots`) are the
stable identity for friendly names kept by the application; they are not BLE addresses.

## 5. Renaming the device

Change `device_name` (at most 18 bytes), `manufacturer` or `model` in
`doubtech_hid_config_t` and rebuild. The name is advertised and is also the GAP Device
Name. Do not change `bond_namespace` just to rename.

Name caching: Bluetooth hosts remember a device's name and its GATT data, and
macOS in particular keeps GATT data by the peripheral's identity. A host that already paired
with the device can keep showing the old name, and cached Device Information or report
handles. The library sends a GATT Service Changed on every boot to make bonded hosts
refresh their database, but it cannot force a host to relabel an entry. To see a new name
on a host that paired earlier, forget the device in that host's Bluetooth settings and
pair again. This is expected behavior from the code and the host documentation, not
something a test in this repository proves for a particular host.

## 6. What changing the bond namespace does

`bond_namespace` selects the NVS namespace that holds the token table (key `identities`).
The default is `kvm_bonds`, which is what the firmware used before this library existed, so
a device updated to it keeps its tokens and its paired hosts. Choosing another namespace
makes the token table start empty while NimBLE still holds the old keys. Previously paired
hosts are then not recognized as known bonds: they must forget the device and pair again
(and the pairing window must be open for a new bond). The old table is not erased. Use a
different namespace only for a new product that has no shipped bonds, or on purpose to
re-pair everybody. The name must be 1 to 15 characters (an NVS limit).

## 7. Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| `doubtech_hid_start` returns `ESP_ERR_INVALID_ARG` | A string is empty, `device_name` is over 18 bytes, `manufacturer` or `model` over 32, or `bond_namespace` over 15 characters. Nothing was initialized. Count bytes, not characters: a multi-byte UTF-8 name uses more than one byte per character. |
| `doubtech_hid_start` returns `ESP_ERR_INVALID_STATE` | Either it was already started (a second call), or the stored bond table is corrupt or of another version. The library will not erase it; check the log, and erase only the bond namespace deliberately. |
| It returns an NVS error such as `ESP_ERR_NVS_NO_FREE_PAGES` or `ESP_ERR_NVS_NEW_VERSION_FOUND` | The library never erases NVS. Fix the partition or erase NVS knowingly; that also deletes the bonds. |
| Log line `HID advertising fields rejected` | The advertising data did not fit. With a valid config this should not happen (the name is limited so it fits 31 bytes); check that you did not change the library source. |
| Log line `No stable BLE identity available` | The NimBLE host could not obtain an address; advertising does not start. |
| A guest cannot pair | Pairing needs the event sink set before start (`doubtech_hid_pairing_set_events`), fewer than eight bonds, and a numeric-comparison capable guest. Only numeric comparison is accepted: a guest that offers Just Works or passkey entry is rejected. The window is 60 seconds and allows three challenges. After a bond is forgotten on the device, the guest may also need to forget the device in its Bluetooth settings before pairing again. |
| Pairing event `DOUBTECH_HID_PAIRING_CAPACITY` | Eight bonds are stored. Remove one with `doubtech_hid_request_forget_bond`; nothing is evicted automatically. |
| A request function always returns `false` | You called it before `doubtech_hid_start` succeeded; the slot is out of range (slots are 1 to 3); the guest is not READY (not encrypted or not subscribed to every characteristic); another request was in flight; or the 20 ms wait elapsed. Do not call request functions from the pairing event callback or the NimBLE host thread. |
| A guest disconnects when input is released | A release that cannot be enqueued terminates the link by design. See [the architecture](README.md). |
| Paired hosts stopped working after a firmware change | A different `bond_namespace` was chosen, the NVS partition moved or was erased, or `CONFIG_BT_NIMBLE_NVS_PERSIST` was turned off. |
| A flash must not erase NVS | Flash only the application: `idf.py app-flash` (`tools/firmware.ps1 flash`). A full `idf.py flash` writes the bootloader, partition table and OTA data but this repository's script still never erases the chip. Do not use `erase-flash`, `erase_flash` or `-e` in tooling, because the NVS partition holds both the NimBLE keys and the token table. |
| Pairing with a host works but the name or report layout looks stale there | The host caches. Forget the device in the host and pair again (section 5). |
