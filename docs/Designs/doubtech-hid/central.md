# doubtech_hid_central: device-owned keyboard and mouse

`firmware/components/doubtech_hid_central` is a BLE HID-over-GATT client (a NimBLE
central). It pairs and connects at most one Bluetooth keyboard and one Bluetooth mouse to
the board and delivers their reports to the application through a callback, while the same
NimBLE host keeps serving up to three guests through [doubtech_hid](README.md). It is a
library: it names no product, and `check-product-names.ps1` (see
[Guards](#guards-and-tests)) enforces that. What the application does with the reports (the
arbitration against the PC app, the guest chords) is in
[input_hub](../input-hub/README.md).

Status: built and covered by host tests with mocks. **None of the radio behaviour has been
run on a board.** The owner's checklist is
[owner-checklist-device-input.md](owner-checklist-device-input.md).

## Architecture

```text
 keyboard / mouse (BLE peripherals)
        |  GATT client, notifications
        v
 doubtech_hid_central   (runs on the NimBLE host thread; never owns the host)
        |  on_input / on_event: copy and enqueue only
        v
 USB worker loop        (queue of 16, then input_hub; see ../input-hub/README.md)
        |
        +--> armed host route? discard device input, hub yields (PC app wins)
        +--> otherwise the hub target: guest 1..3 through doubtech_hid output, 0 = This PC (stub)
```

- `doubtech_hid` still owns `nimble_port_init`, the host task and every sync/reset callback.
  The central never initializes the host: `doubtech_hid_central_start` returns
  `ESP_ERR_INVALID_STATE` until `doubtech_hid_host_synced()` is true. The USB worker
  polls that and retries with a doubling back-off (100 ms up to 10 s).
- Central connections use their own GAP callback (arg = link slot and generation), so the
  guest GAP callback never sees a central link and the reverse.
- All central state belongs to the host thread. The public pairing, forget and query
  functions are bounded requests (200 ms) posted to the host loop, following the
  `doubtech_hid` request-bridge pattern. They must not be called from the host thread.
  `doubtech_hid_central_connected` reads a volatile flag and `doubtech_hid_central_stored`
  a mutex-protected copy, so both are safe from any task.
- `on_input` and `on_event` run on the host thread. They must only copy and enqueue; the
  USB worker does the rest.

## Supported devices

Only HID-over-GATT (HOGP) devices that expose service `0x1812` after encryption.

| Kind | Supported | Not supported |
|---|---|---|
| Keyboard | Boot protocol keyboards: Boot Keyboard Input `0x2A22` is required, Protocol Mode `0x2A4E` is written to 0 when present. The 8-byte boot report is delivered as `keys`. | A keyboard without boot protocol. There is no report-mode keyboard parser. |
| Mouse | A device with a boot mouse (`0x2A33`: buttons, dx, dy, no wheel), or a mouse whose Report Map (read in full, at most 512 bytes) the small walker accepts: one Input report with relative X and Y of 8, 12 or 16 bits, buttons (counted up to 8 and only the low 8 reported), and an optional 8-bit wheel, with or without a report ID. | Absolute pointers (tablets), unsigned axes, axes wider than 16 bits, a wheel wider than 8 bits, non-contiguous or non-zero-start buttons, several report IDs in the mouse collection, more than 16 IDs, long items, and malformed or over-long maps (these fall back to the boot mouse when the device has one). |

Report-mode details:

- The parsers (`hogp_parse.[ch]`, pure C, tested with address sanitizer, fuzzed and
  truncated) take the **bare notification payload**: the report ID is never in it. Which
  report belongs to the mouse is decided by the Report Reference descriptor (`0x2908`)
  of the `0x2A4D` characteristic, matched to the ID the map names. A payload that
  includes an ID byte cannot be detected and would shift every field.
- A mouse with a map that parses and has a wheel uses Report mode (Protocol Mode written
  1); otherwise the boot mouse is used (Protocol Mode 0). A device with neither a usable
  map nor a boot mouse is not supported.
- A 12-bit axis is sign-extended; a 16-bit axis is delivered as `dx`/`dy` (`int16_t`).
- The default ATT MTU of 23 is kept (no MTU exchange): notifications carry at most 20 bytes,
  enough for the boot and supported report layouts.

Not supported is reported as `DOUBTECH_HID_CENTRAL_REASON_NOT_SUPPORTED` (reason 1),
whatever the cause (no HID service, a keyboard without boot input, no usable mouse
input, an unsupported or over-long map with no boot fallback, a boot input without a CCCD).

### Failure reasons (`DTHID_CENTRAL_PAIR_FAILED`)

| Reason | Macro | Meaning (as the touchscreen words it) |
|---|---|---|
| 1 | `DOUBTECH_HID_CENTRAL_REASON_NOT_SUPPORTED` | The device is not a supported keyboard or mouse (NOT SUPPORTED). |
| 2 | `DOUBTECH_HID_CENTRAL_REASON_TIMEOUT` | Connecting, encrypting or discovery timed out, or the 30 s window ended (TIMED OUT). |
| 3 | `DOUBTECH_HID_CENTRAL_REASON_AUTH_FAILED` | Encryption or bonding failed, a passkey was requested on a link that is not pairing, or an unsupported pairing method (PAIRING FAILED). |
| 4 | `DOUBTECH_HID_CENTRAL_REASON_BUSY` | Guest pairing is open or another input pairing is running (BUSY). |
| 5 | `DOUBTECH_HID_CENTRAL_REASON_CAPACITY` | A keyboard and a mouse are already stored, the kind or address is already stored, or the row is a guest's identity (NO FREE SLOT). Replace a device by forgetting it first. |

### Passkey display

The board is the initiator and has a display, so a keyboard that asks for passkey entry
works like this: the central generates a six-digit passkey (`esp_random() % 1000000`),
injects it into the security manager and emits `DTHID_CENTRAL_PASSKEY` with it. The
touchscreen shows **TYPE ON KEYBOARD** and the digits (`%06u`); the owner types them on
the keyboard and presses Enter. A numeric comparison request is accepted silently (the
owner already chose the device, and no number is emitted). Any other pairing action
terminates the link with reason 3.

## Pairing window

`doubtech_hid_central_pair_begin` opens a 30 s window (`DOUBTECH_HID_CENTRAL_PAIR_WINDOW_MS`):

- It saves `ble_hs_cfg.sm_mitm`, sets it to 0 and runs an active scan at half duty
  (60 ms interval, 30 ms window, duplicates filtered). The saved value is restored when the
  window ends (success, failure, cancel, timeout or scan end).
- The scan lists devices that advertise service `0x1812` or appearance `0x03C1`
  (keyboard), `0x03C2` (mouse) or `0x03C4` (gamepad, listed with an unknown kind;
  discovery decides). Rows are deduplicated by (address type, address), at most
  `DOUBTECH_HID_CENTRAL_MAX_SCAN_RESULTS` (8), and a row keeps its index so
  `doubtech_hid_central_pair_select(index)` stays valid; a row is reported again when its
  name or kind becomes known. The touchscreen shows the four strongest by RSSI (the spec
  said six rows; the shipped limit is the caller's four on screen from eight kept).
- `pair_select` refuses (PAIR_FAILED reason 5) a row whose address is a guest's identity
  (`doubtech_hid_peer_is_guest`), a stored device, or a kind that is already stored. Before
  connecting it deletes a stale local bond for that address (never a guest's or a stored
  device's). The connect uses 10 s, then the window timer is re-armed to 30 s so a
  passkey can be typed.
- The new device is connected with `ble_gap_security_initiate`. Encryption must come back
  encrypted and bonded, otherwise reason 3. Discovery then runs every time (no GATT cache),
  the record is saved to NVS and CONNECTED is emitted once input can flow, after PAIRED.
- An unfinished pairing whose link bonded has its bond deleted when the link closes, unless
  the address is stored. A link that is stuck closing is force-closed after a 5 s guard
  so the window ends and `sm_mitm` is restored.

### What exclusivity with guest pairing enforces, and what it does not

The guest pairing window requires an authenticated link and the input window clears
`sm_mitm`, so they must not overlap (risk R5). The enforcement is **partial**:

| Direction | Enforced by | Notes |
|---|---|---|
| Input window while a guest window is open | The central: `pair_begin` checks `doubtech_hid_pairing_active()` and returns `ESP_ERR_INVALID_STATE` with PAIR_FAILED reason 4. The USB worker also refuses to begin (BUSY) while its own copy of the guest window is open. | Enforced in two places. |
| Guest pairing started while an input window is open | The firmware application (the USB worker): the touchscreen request, and the PC app's `pair_begin` (NACK BUSY), are refused until the input window and its result screen are dismissed. A guest window that the library opens by itself during an input window (a guest-initiated numeric comparison opens the window from inside `doubtech_hid`) is cancelled by the worker when its OPENED event reaches it, and the cancel is retried once a second until it succeeds or that window's own deadline passes. | **Not** enforced in the libraries: `doubtech_hid` does not know about the input window, so there is a gap of a few milliseconds (and longer if the cancel fails) in which both are open. Guest bonding still requires an authenticated link either way. |

So the windows are not guaranteed never to overlap; the application narrows the overlap
to that gap.

## Reconnect

After `doubtech_hid_central_start` the stored devices are connected by their identity
address on **one global schedule**:

- One outgoing attempt at a time, each lasting 10 s (`ble_gap_connect` duration), keyboard
  first, then round robin between the stored kinds.
- After any attempt that ends without a link, a single gap of 15 s plus 0 to 2 s jitter
  (`esp_random()`; 0 to +2 s, not plus or minus) before the next attempt of either kind.
  With both devices absent an attempt starts about every 25 s and alternates strictly, so
  the initiator is busy at most 10 s in every 25 s and each kind is tried about every 50 s.
- A device that was working and drops (asleep, out of range) gets one **immediate**
  retry, at most once per 30 s per kind; a repeated drop within 30 s waits for the shared
  gap. A flapping device is therefore retried roughly every 17 s, not in a tight loop.
- None while an input pairing window is open (the schedule re-evaluates when it ends), none
  to an identity that is a guest's (also checked at encryption: any link whose encrypted
  identity is a guest's is aborted), and none beyond the connection budget left after the
  guests' `DOUBTECH_HID_MAX_CONNECTIONS` (5 minus 3 gives 2).
- A stored link stuck connecting or closing is dropped by guard timers (15 s and 5 s) so the
  schedule cannot wedge. A link stuck in security or discovery relies on NimBLE's own
  30 s timeouts.
- An open *guest* pairing window does not pause reconnect (only the input window does); a
  reconnect uses the existing bond, so `sm_mitm` does not matter to it.

Reconnect with a resolvable private address (RPA) depends on the controller or host
resolving it; this is unverified (see [Limits](#limits-and-known-gaps)).

## Registry and storage

The registry holds one record per kind. A record is `{address (6), address type, kind,
name (23 characters plus NUL)}`.

NVS namespace `dthid_inputs`, key `inputs` (a different namespace and key from the guests'
`kvm_bonds` store, which this component never opens: `kvm_bonds` is refused as a namespace
on load and save with `ESP_ERR_INVALID_ARG`):

| Offset | Size | Content |
|---|---|---|
| 0 | 4 | Magic "HDPI" (`0x49504448`) |
| 4 | 4 | Version (1) |
| 8 | 4 | Record count (0 to 2) |
| 12 | 4 | CRC32 over magic, version, count and every record byte |
| 16 | 32 each | Records: address 6, address type 1, kind 1, name 24 (explicitly serialized, no struct padding) |

At most 80 bytes. A missing, truncated, oversized, mis-versioned or CRC-failing blob, a
bad kind, a duplicate kind or an unterminated name loads as an empty registry, and boot
never fails because of it (only real NVS errors are returned). A save is a blob write then
a commit; a failure at either step leaves the previous blob. A combo device that is both
keyboard and mouse is two independent records with the same address; forgetting one kind
leaves the other, and the NimBLE bond is deleted only when no other record, and no guest,
uses the address.

## Connection and bond budget

| Setting | Value | Why |
|---|---|---|
| `CONFIG_BT_NIMBLE_ROLE_CENTRAL` | y | Central role (scan, initiate, GATT client). |
| `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` | 5 | 3 guest links (peripheral) + 1 keyboard + 1 mouse (central). Was 3. |
| `CONFIG_BT_CTRL_BLE_MAX_ACT` | 8 | 5 links + advertising + scan/initiate + 1 spare. Was 4. |
| `CONFIG_BT_NIMBLE_MAX_BONDS` | 12 | One NimBLE store shared by both roles: 8 guests + 4 input identities (only 2 are ever connected, the rest is room for in-progress and stale bonds). Was 8. |
| `CONFIG_BT_NIMBLE_MAX_CCCDS` | 24 | Unchanged: only guests subscribe to the board. |
| `foreign_bond_reserve` (`doubtech_hid_config_t`) | 4 | Bonds in the NimBLE store that are not guests'. |

Guest capacity stays eight. `doubtech_hid_pairing_open` used to refuse as soon as NimBLE
reported eight bonds of any kind; it now refuses when the guests' own retained table is
full (`pairing.bond_count >= 8`) or when the NimBLE store holds
`DOUBTECH_HID_MAX_BONDS + foreign_bond_reserve` (12) bonds. The central uses the shared
store but never the guests' retained table, and three library queries keep the two roles
apart:

- `doubtech_hid_host_synced()`: true after the host sync callback, so the central waits
  for it to start.
- `doubtech_hid_pairing_active()`: true while a guest window is open (host thread only),
  so the central refuses to open its own.
- `doubtech_hid_peer_is_guest(type, address)`: true when the address equals the identity of
  a retained guest bond (the address type is deliberately not compared, so a false
  positive only refuses). The central never selects, reconnects to, or deletes the bond of
  such an address.

Guest connections are reserved, not counted: NimBLE has no public way to count every
connection, so the schedule assumes the guests may use their three links and leaves the
central two. ENOMEM or EBUSY from `ble_gap_connect` is an ordinary failed attempt.

The measured controller memory use and three-guest-plus-two-input behaviour are not
established (risk R1, owner checklist).

## Security trade-off

- Input devices are bonded with `sm_mitm = 0` for the duration of an input pairing
  window: LE Secure Connections, bonding, but no man-in-the-middle protection requirement.
  Most mice are Just Works; a keyboard that wants a passkey gets the passkey display above.
  The risk is accepted because pairing starts only from the touchscreen, reconnect accepts
  only stored addresses, and `sm_mitm` is restored when the window ends.
- Guest bonding is unchanged: it still requires an authenticated link (numeric
  comparison), also during an overlap.
- `ble_hs_cfg.sm_mitm` is global in NimBLE, hence the exclusivity above.

### Residual risk: a guest that advertises as HID through an unresolved RPA

If a guest is seen during the scan under a resolvable private address that the host cannot
resolve to its identity, the row looks like an unknown HID device. The central **refuses to
adopt it**: encryption resolves the identity, the identity is a guest's, the link is
aborted with reason 5 and the guest's bond is not deleted by the central. The pairing
attempt itself, however, writes over the same peer in NimBLE's store, so the guest's keys
**can be lost** and it may need to be paired again. This is
documented rather than fixed; the guard is the identity check, not prevention.

## Public API

Header: `firmware/components/doubtech_hid_central/include/doubtech_hid_central.h`. Every
public name is listed here; `check-api-docs.ps1` fails when one is missing.

### Constants

- `DOUBTECH_HID_CENTRAL_KEYBOARD` (1) and `DOUBTECH_HID_CENTRAL_MOUSE` (2): the device
  kinds (equal to the registry's internal kind values; a compile-time assertion keeps them
  equal).
- `DOUBTECH_HID_CENTRAL_REASON_NOT_SUPPORTED` (1), `DOUBTECH_HID_CENTRAL_REASON_TIMEOUT` (2),
  `DOUBTECH_HID_CENTRAL_REASON_AUTH_FAILED` (3), `DOUBTECH_HID_CENTRAL_REASON_BUSY` (4),
  `DOUBTECH_HID_CENTRAL_REASON_CAPACITY` (5): see the table above.
- `DOUBTECH_HID_CENTRAL_MAX_SCAN_RESULTS` (8): most rows one window lists.
- `DOUBTECH_HID_CENTRAL_NAME_BYTES` (24): size of every name buffer, NUL included.
- `DOUBTECH_HID_CENTRAL_PAIR_WINDOW_MS` (30000): length of the scan window.

### Types

`doubtech_hid_central_input_t`: one report. `kind` is `DOUBTECH_HID_CENTRAL_KEYBOARD` or
`_MOUSE`. A keyboard fills `keys` (the 8-byte boot report). A mouse fills `buttons`, `dx`,
`dy` and `wheel`. `lost` is true, with all other fields zero, when a device that delivered
input disconnects (also when it is forgotten while connected); the consumer must release
every key and button it holds for that kind. It is delivered exactly once per loss.

`doubtech_hid_central_event_type_t`, with these enumerators:

| Enumerator | Meaning |
|---|---|
| `DTHID_CENTRAL_SCAN_RESULT` | A scan row appeared or changed (`index`, `kind` (0 unknown), `name`, `rssi`). A repeated index updates that row. |
| `DTHID_CENTRAL_PASSKEY` | The passkey to type on the keyboard (`passkey`, the passkey-entry method only; `kind` is the advertised kind or 0). |
| `DTHID_CENTRAL_PAIRED` | Bonded and discovered; the record is saved (`kind`, `name`). |
| `DTHID_CENTRAL_PAIR_FAILED` | Pairing failed (`reason`, `kind` when known). |
| `DTHID_CENTRAL_CONNECTED` | A device of `kind` is connected and input flows (also after every reconnect). |
| `DTHID_CENTRAL_DISCONNECTED` | The link closed, emitted once per link. |

`doubtech_hid_central_event_t`: `type`, `index`, `kind`, `name[DOUBTECH_HID_CENTRAL_NAME_BYTES]`
(NUL-terminated, possibly empty), `rssi`, `passkey` and `reason` (each valid only for its event).

`doubtech_hid_central_config_t`: `on_input`, `on_event`, `context` (passed to both) and
`store_namespace` (NULL selects `dthid_inputs`; 1 to 15 characters; `kvm_bonds` is refused).

### Functions

| Function | Thread | Behaviour |
|---|---|---|
| `esp_err_t doubtech_hid_central_start(const doubtech_hid_central_config_t *config)` | any task except the host thread | Loads the registry, prepares the host-loop bridge, starts the reconnect schedule. `ESP_ERR_INVALID_STATE` before `doubtech_hid_host_synced()` or on a second call; `ESP_ERR_INVALID_ARG` for a NULL config or invalid namespace. |
| `esp_err_t doubtech_hid_central_pair_begin(void)` | same | Opens the 30 s window. `ESP_ERR_INVALID_STATE` plus PAIR_FAILED reason 4 while guest pairing is open or another input pairing runs, and reason 5 when both kinds are stored. |
| `esp_err_t doubtech_hid_central_pair_cancel(void)` | same | Ends the window silently; a half-paired device is disconnected and its new bond removed. **Returns `esp_err_t`** (the original plan said `void`): `ESP_OK` only when the cancel actually ran on the host loop, which, because requests run one at a time in order, also proves that any earlier timed-out (abandoned) begin or select has run or been dropped. `ESP_ERR_TIMEOUT` or `ESP_ERR_INVALID_STATE` mean the window may still be open: retry. The touchscreen flow relies on this to keep guest pairing refused until the central is known to be closed. |
| `esp_err_t doubtech_hid_central_pair_select(uint8_t index)` | same | Connects and pairs scan row `index`. Result: PAIRED then CONNECTED, or PAIR_FAILED. |
| `esp_err_t doubtech_hid_central_forget(uint8_t kind)` | same | Saves the registry without that kind first, then disconnects its link (lost input once, then DISCONNECTED), stops its reconnect attempts and deletes its NimBLE bond unless the address is a guest's or still stored for the other kind. `ESP_ERR_NOT_FOUND` when none is stored. |
| `bool doubtech_hid_central_connected(uint8_t kind)` | any task | True while a device of that kind is connected and delivering input. Turns true just before CONNECTED and false as soon as the link is lost or closing (before DISCONNECTED). |
| `bool doubtech_hid_central_stored(uint8_t kind, char name[DOUBTECH_HID_CENTRAL_NAME_BYTES])` | any task | True when a device of that kind is stored; copies its name when `name` is not NULL. False before start. |

## Guards and tests

- `firmware/components/doubtech_hid_central/tests/run-msvc.ps1`: four suites under MSVC
  `/W4 /WX` with address sanitizer when the toolchain ships it: `test_hogp_parse`
  (real descriptors, every truncation, 20000 random and mutated descriptors),
  `test_central_registry` (registry and NVS store, CRC, corruption, `kvm_bonds` refusal),
  `test_central_gatt` (the HOGP discovery state machine, reason mapping, the reconnect
  schedule) and `test_central_scan` (advertisement parser, scan list, selection and bond
  rules). The NimBLE adapter `central_ble.c` is compiled only by the firmware build and is
  not run by any test.
- `check-product-names.ps1` (in `doubtech_hid/tests/` and in this component's `tests/`):
  neither `doubtech_hid` nor `doubtech_hid_central` may contain the product name or
  KVM-protocol wording in shipped files, subdirectories included. `input_hub` is not
  scanned (it is firmware-internal and shares the `kvm_*` router types).
- `doubtech_hid/tests/check-api-docs.ps1`: every function, type, enumerator and macro in
  `doubtech_hid_central.h` must appear in this file.

## Limits and known gaps

- **This PC (target 0) is a stub until stage 3**: typing at it does nothing (risk R6).
- RPA reconnect, scan airtime and its effect on guest notify latency (R3) are not measured.
- The worker stack (6144 bytes) and heap headroom are not measured (the firmware logs a
  `Health:` line for it, see the owner checklist).
- Deletes made by the guest code in the reverse direction (`doubtech_hid` forgetting a
  guest bond) are unaware of the central registry; the guest code only deletes guest
  bonds, so the exposure is the unresolved-RPA case above.
- Keyboards without boot protocol, and tablets and other non-relative pointers, are not supported.
- Stored-link discovery failures are silent (only a pairing link reports PAIR_FAILED).
- No ATT MTU exchange (20-byte notifications).
- Reconnect budget is reserved, not measured (see above).
