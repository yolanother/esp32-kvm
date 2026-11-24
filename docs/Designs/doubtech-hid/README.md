# doubtech_hid: architecture

`doubtech_hid` is an ESP-IDF component that turns an ESP32-S3 into one combined
Bluetooth LE HID peripheral (keyboard, relative mouse and consumer control) that up
to three hosts can pair with and use at once. It owns the HID report definitions,
the per-link output gate, pairing and bonding, and a small thread-safe request API.
It owns no USB, display or routing logic. The firmware that embeds it decides which
guest receives input.

Quick start: [component README](../../../firmware/components/doubtech_hid/README.md).
Function reference: [api.md](api.md). Wiring it into a firmware project:
[integration.md](integration.md).

Current KVM firmware identity: configured in `firmware/main/main.c` as name "ESP32 KVM", manufacturer "ESP32 KVM", model "ESP32 KVM S3", default bond namespace.

## Roles

| Role | Status |
| --- | --- |
| BLE peripheral output (the device is the HID keyboard/mouse; hosts connect to it) | Implemented. This is all the code does today. |
| BLE central input client (the device connects to a physical Bluetooth keyboard or mouse) | Reserved. No code exists. See "Future central role". |

## Layering

```
firmware/components/doubtech_hid/
  include/doubtech_hid.h     the only public header
  core/                      portable, no Bluetooth stack, unit-testable on a PC
    hid_report.[ch]          report map, report IDs, per-link output gate
    hid_pairing.[ch]         pairing window / challenge / bond-table policy
  ble/                       NimBLE and ESP-IDF specific
    hid_gatt.[ch]            GATT database, per-link state, notifications
    hid_guest.c              startup, GAP events, advertising, public API
    hid_guest_rpc.[ch]       bounded host-loop request bridge
    hid_pairing_store.[ch]   NVS persistence of the token table
    hid_gatt_official_diag.c diagnostic adapter, compiled only by a test
  tests/                     host-side C tests with NimBLE and NVS stand-ins
  Kconfig, CMakeLists.txt, idf_component.yml
```

- `include/` is the contract. Everything else is private (`PRIV_INCLUDE_DIRS`:
  `core`, `ble`). The component `REQUIRES bt nvs_flash esp_timer`.
- `core/` has no NimBLE or ESP-IDF includes, so the report gate and the pairing policy
  run in the PC tests exactly as they run on the board. `hid_report.c` reads
  `CONFIG_DOUBTECH_HID_KEYBOARD_ONLY_DIAGNOSTIC` only to select a smaller report map.
- `ble/hid_gatt.c` defines the three GATT services and keeps one `hid_channel_t` per link.
  It never starts advertising and never arms output; the owner (`hid_guest.c`) feeds it
  link events.
- `ble/hid_guest.c` implements the public functions, owns the pairing state, handles
  every GAP event and runs advertising.
- `ble/hid_guest_rpc.c` implements all `doubtech_hid_request_*` functions.
- `ble/hid_gatt_official_diag.c` is not in `CMakeLists.txt`. Only
  `tests/test_hid_official_diag.c` compiles it, as a regression guard for an adapter
  around ESP-IDF's own NimBLE HID service builder that was used to isolate a macOS
  compatibility problem.

## Two threading models

NimBLE runs a single host thread with an event queue. All HID and pairing state lives on
that thread and is not locked.

1. Host-thread functions: `doubtech_hid_pairing_open`, `_cancel`, `_confirm`, `_snapshot`,
   `_forget`, `doubtech_hid_current_bond_token`, `doubtech_hid_snapshot_slots`,
   `doubtech_hid_retained_bonds`, `doubtech_hid_disconnect_current` and
   `doubtech_hid_disconnect_slot`. They must run on the host thread. The pairing event
   sink also runs there.
2. Request functions: every `doubtech_hid_request_*` is safe from any other task (a USB
   worker, for example). Each one posts an event to NimBLE's default event queue and
   waits for it on a semaphore. The bridge holds exactly one request at a time. Waiting
   is bounded: `RPC_TIMEOUT_MS` is 20 ms, used both as the deadline the host loop checks
   before executing and as the caller's semaphore wait. A busy bridge returns `false`
   at once. On a timeout the request is marked cancelled and, for slots 1 to 3, a
   disconnect of that slot is queued, so a request the radio side may have half done
   cannot leave a guest with held input. Only `doubtech_hid_request_disconnect` and
   `_slot` skip the wait: they queue a coalesced event and return.

## One device, three links, eight bonds

The device exposes one GATT database with one HID service, one Battery service and one
Device Information service, and uses one identity address. Up to
`DOUBTECH_HID_MAX_CONNECTIONS` (3) hosts connect at once; each gets a one-based slot
holding its own encryption state, subscriptions, protocol mode, held reports and
disconnect flag. GATT reads and writes resolve the requesting connection handle, and a
notification names exactly one handle.

Capacity: the retained bond table holds `DOUBTECH_HID_MAX_BONDS` (8) opaque tokens. These
limits require matching settings in `sdkconfig` (`firmware/sdkconfig.defaults`):
`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=3`, `CONFIG_BT_NIMBLE_MAX_BONDS=8`,
`CONFIG_BT_NIMBLE_MAX_CCCDS=24`, `CONFIG_BT_CTRL_BLE_MAX_ACT=4` (three links plus one
advertising activity). A fourth link is not accepted: the device stops advertising while
three are connected and resumes after one drops. The controller's memory use and
three-guest behavior on real hosts are gates recorded in
[three-link capacity](../ble/three-guest-source-capacity.md); source tests do not
establish them.

## HID report map

Defined in `core/hid_report.c` as `hid_report_map`. Report values exclude the report ID,
which a GATT Report Reference descriptor carries.

| Report | ID | Direction | Value bytes | Contents |
| --- | ---: | --- | ---: | --- |
| Keyboard | 1 | input | 8 | Modifier bits, a reserved zero byte, six usages. |
| Keyboard LEDs | 1 | output | 1 | Five LED bits, three pad bits. |
| Mouse | 2 | input | 7 | Five button bits plus three pad bits, signed 16-bit X, signed 16-bit Y, signed 8-bit wheel, signed 8-bit pan. |
| Consumer | 3 | input | 2 | One 16-bit usage, 0 to `0x03ff`; 0 releases. |
| Boot keyboard input | 4 (internal) | input | 8 | Same layout as report 1, on characteristic 0x2A22. |
| Boot mouse input | 5 (internal) | input | 3 | Buttons, signed 8-bit X, signed 8-bit Y, on 0x2A33. No wheel or pan. |

IDs 4 and 5 are never in the report map; they are internal route numbers for the two
Boot Protocol input characteristics (keyboard boot output, 0x2A32, carries the LED byte).
`tests/test_hid_report.c` parses the descriptor and asserts 64, 56 and 16 input bits
and 8 output bits for IDs 1, 2 and 3.

Protocol Mode (0x2A4E) accepts 1 (report, the default) and 0 (boot). A link starts in
report mode. Changing the mode releases any held input first. In boot mode only the boot
keyboard and boot mouse characteristics are used; consumer reports are refused.
`hid_channel_ready` requires, in report mode, subscriptions to keyboard, mouse and
consumer inputs, and in boot mode, to boot keyboard and boot mouse inputs. All input,
LED, Protocol Mode and Control Point accesses need an encrypted link; HID Information,
Report Map, Battery, and Device Information are readable unencrypted so discovery
works. The Battery service always reports 100 percent (the device is USB powered).
Device Information also carries a fixed PnP ID that is the development board's USB VID
and PID (0x303A, 0x1001); it is not configurable.

## The per-link output gate

`hid_channel_t` (in `core/hid_report.h`) is one link's gate. Reports flow only while
`armed`. The rules, all in `core/hid_report.c`:

- A link can arm only when it is connected, encrypted, not marked for disconnect, and
  has every subscription its protocol mode needs (`hid_channel_ready`). Arming first
  sends an all-up baseline (a zero keyboard report, zero mouse and consumer reports;
  boot mode sends the boot keyboard and a 3-byte boot mouse report) and arms only if
  every send was enqueued.
- Losing encryption, or any subscription, or a failed send disarms the link at once.
- Connect and disconnect reset all held state; reconnecting requires a fresh arm with a
  new all-up baseline.
- Release disarms first, then sends the all-up set. If the link is not ready or any
  all-up report cannot be enqueued, the channel is marked `needs_disconnect` and the
  link is terminated. The reason: a release that cannot be shown to have reached the
  guest may leave a key or button held there, and the only safe state left is no link.
- A failed keyboard, mouse or consumer send also disarms and marks the link for
  disconnect; a failed notification confirmation (`BLE_GAP_EVENT_NOTIFY_TX` with a
  nonzero status) does the same and terminates the link.
- A keyboard report with a nonzero reserved byte, a mouse report with button bits 5 to 7,
  a consumer usage above `0x03ff`, or any such report while not armed, is refused.
- A successful return means the notification was enqueued, not that the guest handled
  it.

## Pairing and reconnect

Policy lives in `core/hid_pairing.c` (constants in `core/hid_pairing.h`); the BLE event
glue is in `ble/hid_guest.c`.

- Window: `HID_PAIRING_WINDOW_MS` is 60000. Opening resets the attempt counter and clears
  any challenge. Opening is refused when eight bonds are stored.
- Method: the device sets IO capability display-yes/no, bonding, Secure Connections and
  MITM. Only numeric comparison is accepted; any other pairing action is rejected and
  the link terminated. There is no Just Works fallback.
- Guest-initiated pairing: if a guest starts numeric comparison when no window is open,
  the library opens one itself (this needs the event sink and bond capacity). It
  publishes the challenge to the sink with the six-digit number and a fresh nonzero
  32-bit challenge ID (never equal to the previous one), then immediately accepts on the
  device side. The person compares the number shown by the application with the one on the
  guest and confirms on the guest; NimBLE completes only if the guest agrees.
- Attempts: `HID_PAIRING_MAX_ATTEMPTS` is 3 numeric challenges per window.
- Challenge expiry floor: when a challenge begins, the window deadline is moved out so at
  least `HID_PAIRING_CHALLENGE_MIN_MS` (30000) remain, so a late challenge still has time.
  The timeout callout is re-armed to the new deadline.
- A peer that is already bonded is never challenged again; it must reconnect with its
  existing keys.
- A new peer is admitted only if, after encryption, the link is encrypted, bonded and
  authenticated, the window is still open, a slot in the table is free and the
  application-side approval for exactly that connection exists. The library then
  creates a random 16-byte token with `esp_random`, writes the table to NVS first and
  publishes `DOUBTECH_HID_BONDED`. If the save fails, the new NimBLE keys are deleted and
  the link is terminated.
- Bond overflow: NimBLE's sample store callback evicts the oldest bond; this component
  replaces it with one that refuses (`reject_store_overflow`), so nothing is evicted.
- Repeat pairing: a peer that forgot its keys may pair again only while a window is open,
  with an authenticated Secure Connections bonding request, and not while another link
  from the same identity is connected. The old token and keys are replaced (the peer
  gets a new token). Otherwise the link is terminated and the bond left alone.
- Disconnect of the link that owns a challenge, window expiry and cancellation each
  publish an event and clear the challenge.

Advertising (`advertise()` in `hid_guest.c`) and reconnect:

- Undirected advertising carries flags, the HID service UUID 0x1812, appearance 0x03C1
  (keyboard) and the complete device name, with a fixed 20 ms interval (32 units of
  0.625 ms, min and max equal).
- On boot, and after every disconnect, the device first does one short directed sweep: for
  each retained bond, in table order, skipping any peer already connected, it advertises
  directed to that peer's stored identity address for `HID_RECONNECT_PHASE_MS` (500 ms).
  When the sweep is exhausted it advertises undirected and discoverable without a time
  limit. The sweep does not repeat on its own. Reason: a bonded host that reconnects with
  a rotating private address never matches a directed target, and a repeating sweep would
  leave it reachable only part of the time.
- While a pairing window is open, no directed sweep runs: the device advertises
  undirected and discoverable.
- If advertising cannot start for a transient reason, it retries every
  `HID_ADVERTISING_RETRY_MS` (250 ms). Advertising stops while three links are connected.
- On every boot the library sends a GATT Service Changed for the whole handle range, so
  bonded hosts that cached an older database refresh it.

## Advertising byte budget and the 18-byte name rule

A legacy advertising payload is 31 bytes. The library advertises, in order: flags
(3 bytes), the 16-bit HID UUID list (4), the appearance (4) and the name with its
2-byte header. That is 13 bytes plus the name, so the name is at most 18 bytes.
`DOUBTECH_HID_NAME_MAX_BYTES` is 18, `doubtech_hid_start` rejects a longer name with
`ESP_ERR_INVALID_ARG` before touching anything, and `hid_guest.c` has a `_Static_assert`
on the sum. The limit is bytes: a multi-byte UTF-8 name counts each byte
(`tests/test_doubtech_hid_config.c` has cases for 18 bytes of ASCII, a multi-byte
name, and 19 bytes). The name is also the GAP Device Name.

## Identity settings and defaults

`doubtech_hid_config_t` (see [api.md](api.md)) sets the device name, the Device Information
manufacturer and model, and the NVS namespace for the token table. A `NULL` field
selects its default: name "BLE HID", manufacturer "DoubTech", model "BLE HID", namespace
`kvm_bonds`. The namespace default is the one the firmware shipped before the library
existed, so already paired hosts keep their bonds. The keyboard-only diagnostic profile
overrides the name with "DoubTech KB Test".

## Bond storage and its compatibility guarantee

Two stores are involved and they are separate. NimBLE keeps the cryptographic keys in its
own NVS store (`CONFIG_BT_NIMBLE_NVS_PERSIST=y`). This component keeps one more table in
NVS (`ble/hid_pairing_store.c`), written by `hid_pairing_store_save` and loaded by
`hid_pairing_store_load`: the opaque token for each bonded peer's identity. It holds no
keys.

Layout (so it can be read by other code or migrated): namespace (default `kvm_bonds`),
key `identities`, one blob of fixed size: `uint32` magic `0x4b564d42`, `uint32` version
`2`, `uint32` count, then eight `{type, address[6], token[16]}` slots, 196 bytes in all.
Loading accepts only a blob of exactly that size, that magic, that version 2 and a count of
at most eight with valid, unique, nonzero tokens; anything else fails
`doubtech_hid_start` with `ESP_ERR_INVALID_STATE` and erases nothing. A missing namespace
or key is an empty table. The earlier version 1 development format was never shipped.

Compatibility guarantee: with the default namespace, the key, magic and version are
unchanged from the shipped firmware, so a device upgraded to this library keeps every
token and every paired host. Overriding `bond_namespace` selects a different table:
the new table starts empty while NimBLE still holds the old keys, so previously paired
hosts are no longer admitted as known bonds until they forget the device and pair again
(the old table is left in NVS untouched). Treat a namespace change as a re-pair of
everybody. The override is process-wide and applied once at start.

## Diagnostic Kconfig options

Both default to off and are not release configurations (`Kconfig`).

- `DOUBTECH_HID_GATT_TRACE`: logs every HID ATT access, subscription and notify result
  (tag `doubtech-hid-gatt` for ATT, `doubtech-hid` for subscribe and notify). It also
  logs keyboard input bytes when the keyboard-only diagnostic is on.
- `DOUBTECH_HID_KEYBOARD_ONLY_DIAGNOSTIC`: removes the mouse and consumer reports and
  characteristics and the boot mouse characteristic, shrinking the report map to the
  keyboard collection. Other changes: the device name is "DoubTech KB Test"; the device
  uses a fixed static random address (instead of its normal identity) so a host that
  caches GATT data by peripheral identity sees a new device;
  the library does not request security on connect but asks for it when the host subscribes to keyboard input before securing the link;
  readiness needs only the keyboard subscription; mouse and consumer requests accept only
  the all-zero state.

## Test strategy and known gaps

The tests build with MSVC on Windows and run without a board (`tests/run-msvc.ps1`). They
compile the real sources of the component (`core/*`, `ble/*`) against small header-only
stand-ins for ESP-IDF and NimBLE in `tests/mocks/` (`esp_err.h`, `esp_log.h`,
`esp_random.h`, `esp_timer.h`, `nvs.h`, `nvs_flash.h`, `host/*`, `nimble/*`, `os/*`,
`services/*`) with `/W4 /WX`. Each test is its own executable. The nine suites:

| Suite | What it proves |
| --- | --- |
| `test_hid_report` | The descriptor's real bit widths (64, 56, 16, 8) and application collections; all-up baseline, arm and release gating; failed release marks the link for disconnect; protocol-mode switching. |
| `test_hid_gatt` | GATT registration; per-link encryption, subscription, held state and notification isolation across three simultaneous links. |
| `test_hid_guest` | Admission, numeric consent, authenticated bonding, token persistence, current-token and slot lookup, request-bridge timeouts and busy handling, forget, repeat pairing, multi-slot isolation. |
| `test_auto_pairing` | A guest-initiated numeric comparison becomes a bounded, device-approved challenge. |
| `test_doubtech_hid_config` | Identity configuration: defaults, the 18-byte limit (accept 18, reject 19 and empty, multi-byte counts bytes), DIS bounds, advertised name and DIS follow the config, legacy identity unchanged, a second start returns `ESP_ERR_INVALID_STATE`. It runs 11 cases, each in its own process, because the service starts once per process. |
| `test_hid_keyboard_diag` | The keyboard-only profile routes with keyboard subscriptions (built with `CONFIG_DOUBTECH_HID_KEYBOARD_ONLY_DIAGNOSTIC`). |
| `test_hid_keyboard_gatt` | The real keyboard-only GATT database: no mouse or consumer characteristics, keyboard input flags, report map readback, Device Information. |
| `test_hid_discovery` | The diagnostic profile's name ("DoubTech KB Test", within 18 bytes), static address and deferred security. |
| `test_hid_official_diag` | The diagnostic adapter around ESP-IDF's HID service builder exposes device identity and sends an exact 8-byte keyboard report. |

Pairing policy and token-store tests (`test_pairing`, `test_pairing_store`) live outside
this component in `firmware/tests/pairing/`.

What the tests do not show: ESP-IDF linking, NimBLE binary compatibility, radio timing,
NVS power-loss behavior, or behavior with Windows, macOS, Linux or other hosts. The
stand-ins model only the calls the code makes.

Known gaps:

- The C tests run only through the Windows MSVC scripts. They are not part of CI.
- CI's `firmware` job (Ubuntu, ESP-IDF v5.5.1, ESP32-S3) builds the firmware and is the
  independent build gate for the component; it runs no C tests.
- `doubtech_hid_pairing_snapshot` takes `struct hid_pairing *`, an internal type, so it
  is unusable outside the component without the private header. A future public state type
  is the fix.
- The `tests/check-api-docs.ps1` check shows only that every declared name is mentioned in
  `api.md`; it does not check the text.
- Three simultaneous real hosts, interoperability and long-run behavior are recorded as
  physical gates elsewhere and are not shown by anything in this directory.

## Future central role

A central input client (the device connecting to a physical Bluetooth keyboard or mouse
and forwarding it) does not exist; the product direction is described in
[the device-owned input path](../hardware-primary/README.md). Reserved design points, so a later role does not need a
rewrite: the opaque-token table and its store are role-independent and could be shared (a
central role would add its own bonds to the same table and the eight-entry budget would be
shared, so the limit would need revisiting); the report definitions in `core/hid_report.[ch]`
(report IDs, lengths, usage ranges) are the common vocabulary a parser would target;
the host-thread / request-bridge split would apply unchanged; and NimBLE needs its central
role enabled (`CONFIG_BT_NIMBLE_ROLE_CENTRAL`, together with more controller activities) in
addition to the peripheral role. None of this is built or tested.
