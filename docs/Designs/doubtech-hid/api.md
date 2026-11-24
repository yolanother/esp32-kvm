# doubtech_hid API reference

Everything here is declared in `firmware/components/doubtech_hid/include/doubtech_hid.h`
(`#include "doubtech_hid.h"`). It is the only header a user of the component
includes. Behavior below was read from `ble/hid_guest.c`, `ble/hid_guest_rpc.c`,
`core/hid_report.c` and `core/hid_pairing.c`. `tests/check-api-docs.ps1` fails
when a name declared in the header is missing from this file. See
[the architecture](README.md) for the design and [the integration guide](integration.md)
for a worked startup.

## Threads and blocking, in one place

| Group | Where it may run | Blocking |
| --- | --- | --- |
| `doubtech_hid_start`, `doubtech_hid_pairing_set_events` | The task that starts the service (normally `app_main`). | `doubtech_hid_start` runs NVS and NimBLE initialization and returns; it does not wait for the host to sync. |
| `doubtech_hid_pairing_*`, `doubtech_hid_current_bond_token`, `doubtech_hid_snapshot_slots`, `doubtech_hid_retained_bonds`, `doubtech_hid_disconnect_current`, `doubtech_hid_disconnect_slot` | The NimBLE host thread only. They read and write the pairing table and HID channels with no lock. | None, but they call NimBLE functions. |
| `doubtech_hid_request_*` (except `doubtech_hid_request_disconnect` and `_slot`) | Any task other than the NimBLE host thread. They post one event to the host loop and wait for it. | At most about 20 ms (`RPC_TIMEOUT_MS` in `hid_guest_rpc.c`). Never call them from the host thread or from the pairing event callback. |
| `doubtech_hid_request_disconnect`, `doubtech_hid_request_disconnect_slot` | Any task. | Queue one event and return; they do not wait. |

The request bridge has one request slot. A second request issued while one is in
flight returns `false` immediately (the mutex is taken without waiting); it does
not queue. A request whose 20 ms deadline has passed by the time the host loop
reaches it is not executed. When a request that targets slot 1 to 3 times out,
the library queues a disconnect of that slot (fail closed). A request that is
merely refused because the slot was busy does not.

All `slot` arguments are one-based: 1 to `DOUBTECH_HID_MAX_CONNECTIONS`. Slot 0 and
values above 3 make the HID requests (`ready`, `arm`, `release`, `keyboard`,
`mouse`, `consumer`) return `false`.

## Constants

### `DOUBTECH_HID_MAX_CONNECTIONS`

`3`. Number of simultaneous guest links and of HID channels (slots). This is the
source-side bound; the NimBLE and controller settings must allow it (see
[the integration guide](integration.md)).

### `DOUBTECH_HID_MAX_BONDS`

`8`. Number of retained bond tokens. A ninth pairing is refused; the library never
evicts an old bond.

### `DOUBTECH_HID_TOKEN_LEN`

`16`. Length in bytes of an opaque bond token. A token is random, nonzero and
unique among the retained bonds. It is not a BLE address and not key material.

### `DOUBTECH_HID_KEYBOARD_REPORT_LEN`

`8`. Length of one keyboard input report: modifier byte, reserved byte (must be 0),
six key usages. `ble/hid_guest_rpc.c` has a `_Static_assert` that it equals the
internal `HID_KEYBOARD_REPORT_LEN`.

### `DOUBTECH_HID_NAME_MAX_BYTES`

`18`. Longest device name in bytes (not characters). The 31-byte legacy advertising
payload holds flags (3), the HID UUID16 (4), appearance (4) and the name header
(2), which leaves 18 for the name. `hid_guest.c` has a `_Static_assert` for this
sum. A name that is longer returns `ESP_ERR_INVALID_ARG` from
`doubtech_hid_start`.

### `DOUBTECH_HID_DIS_STRING_MAX_BYTES`

`32`. Longest Device Information manufacturer or model string in bytes.

## Types

### `doubtech_hid_token_t`

```c
typedef struct { uint8_t bytes[DOUBTECH_HID_TOKEN_LEN]; } doubtech_hid_token_t;
```

An opaque application bond token. Used by value in `doubtech_hid_retained_bonds`
and `doubtech_hid_pairing_forget`, and inside `doubtech_hid_pairing_event_t`.

### `doubtech_hid_slot_t`

```c
typedef struct {
    uint8_t token[DOUBTECH_HID_TOKEN_LEN];
    bool ready;
    bool subscribed;
    uint32_t notify_tx_successes;
} doubtech_hid_slot_t;
```

State of one one-based slot as returned by `doubtech_hid_snapshot_slots` and
`doubtech_hid_request_slots`. A slot that has no authenticated, bonded link is all
zero. Otherwise:

- `token`: the bond token of the connected peer.
- `ready`: `true` means only that the link is encrypted, bonded and authenticated and
  that its token resolved. It does not mean the guest has subscribed.
- `subscribed`: `true` when the channel is fully ready to arm: encrypted, not marked
  for disconnect, and every input characteristic of the active protocol mode has
  been subscribed (report mode: keyboard, mouse and consumer; boot mode: boot
  keyboard and boot mouse; the keyboard-only diagnostic needs only the keyboard
  one). This is the value a router should treat as "READY".
- `notify_tx_successes`: count of notifications the controller confirmed on this
  link since it connected.

### `doubtech_hid_pairing_event_type_t`

Event kinds delivered to the event sink. The constants:

| Constant | Published when | `connection_handle` | `number` | Other fields |
| --- | --- | --- | --- | --- |
| `DOUBTECH_HID_PAIRING_OPENED` | A pairing window opened (`doubtech_hid_pairing_open`, or automatically for a guest-initiated numeric comparison). | 0 | 0 | `deadline_ms` is the window end. |
| `DOUBTECH_HID_PAIRING_CHALLENGE` | A numeric comparison challenge began. | The link. | The six-digit number (0 to 999999). | `challenge_id` is nonzero. |
| `DOUBTECH_HID_PAIRING_CLOSED` | `doubtech_hid_pairing_cancel` ran. | 0 | 0 | |
| `DOUBTECH_HID_BONDED` | A new peer finished authenticated bonding and its token was saved. | The link. | 0 | `token` is the new token. |
| `DOUBTECH_HID_PAIRING_REJECTED` | A challenge failed, was rejected, or its link dropped; or a numeric comparison could not open a window. | The link. | 0 | |
| `DOUBTECH_HID_PAIRING_CAPACITY` | A window was requested but eight bonds are already stored. | 0 | 0 | |
| `DOUBTECH_HID_PAIRING_TIMEOUT` | The window expired. | 0 | 0 | |
| `DOUBTECH_HID_DISCONNECTED` | A slot's link closed, or the NimBLE host reset. | The closed link (0 after a host reset). | The one-based slot that closed (0 after a host reset). | Header comment: the application can drop per-slot state it cached. |

`number` is the six-digit value for CHALLENGE and the one-based slot that closed for
DISCONNECTED (`publish(..., disconnected_slot, ...)` in the `BLE_GAP_EVENT_DISCONNECT`
case of `hid_guest.c`); a host reset publishes DISCONNECTED with `number` 0 (`on_reset`).
The header comment says the same. Every other event leaves `number` 0.

### `doubtech_hid_pairing_event_t`

```c
typedef struct {
    doubtech_hid_pairing_event_type_t type;
    uint16_t connection_handle;
    uint32_t challenge_id;
    uint32_t number;
    uint64_t deadline_ms;
    doubtech_hid_token_t token;
} doubtech_hid_pairing_event_t;
```

`deadline_ms` is the pairing window end in milliseconds of `esp_timer_get_time()`
(time since boot), copied from the pairing state at publish time; it is meaningful
while a window is open. `challenge_id` is set only for `DOUBTECH_HID_PAIRING_CHALLENGE`,
`token` only for `DOUBTECH_HID_BONDED`. The structure is built on the stack and is
valid only during the callback: copy what you need.

### `doubtech_hid_pairing_event_fn`

```c
typedef void (*doubtech_hid_pairing_event_fn)(const doubtech_hid_pairing_event_t *event, void *context);
```

The event sink. It runs on the NimBLE host thread, so it must be short and must not
block, must not call a `doubtech_hid_request_*` function that waits (those would
wait on the thread they are running on), and should not do UI work. Copy the event
into a queue and return. `context` is the pointer given to
`doubtech_hid_pairing_set_events`.

### `doubtech_hid_config_t`

```c
typedef struct {
    const char *device_name;
    const char *manufacturer;
    const char *model;
    const char *bond_namespace;
} doubtech_hid_config_t;
```

| Field | Valid | Default when `NULL` |
| --- | --- | --- |
| `device_name` | 1 to `DOUBTECH_HID_NAME_MAX_BYTES` (18) bytes. The limit counts UTF-8 bytes, not characters. | `"BLE HID"` |
| `manufacturer` | 1 to `DOUBTECH_HID_DIS_STRING_MAX_BYTES` (32) bytes. | `"DoubTech"` |
| `model` | 1 to 32 bytes. | `"BLE HID"` |
| `bond_namespace` | 1 to 15 characters (an NVS namespace name). | `"kvm_bonds"`, the namespace the earlier shipped firmware used |

The strings are copied during `doubtech_hid_start`; they do not need to outlive it.
A `NULL` pointer to the whole struct selects every default. An empty string is not
the same as `NULL`: it is rejected.

## Functions: lifecycle and events

### `esp_err_t doubtech_hid_start(const doubtech_hid_config_t *config)`

Validates the config, then initializes NVS (`nvs_flash_init`, never erasing),
loads the bond table, initializes NimBLE, registers the HID, Battery and Device
Information services, sets the GAP name and appearance, initializes the request
bridge and starts the NimBLE host task. Advertising starts when the host syncs.

Thread: call once, from startup code, not from the NimBLE host thread.

Returns:

- `ESP_OK`: the service is started.
- `ESP_ERR_INVALID_STATE`: it was already started successfully. The second call
  changes nothing.
- `ESP_ERR_INVALID_ARG`: a name, manufacturer, model or namespace is empty or too
  long. This is returned before NVS or NimBLE is touched.
- Any `nvs_flash_init` error, passed through. The library never erases NVS to
  recover; fix the partition or erase it deliberately.
- `ESP_ERR_INVALID_STATE` from a stored bond table that is corrupt, has the wrong
  size, magic or version, or does not validate. Keys are not erased.
- An error from `nimble_port_init`, service registration, the GAP calls or the
  request bridge; NimBLE is de-initialized again for those that occur after it was
  initialized.

Side effects: the guest NimBLE security settings are fixed (bonding, Secure
Connections, MITM, display-yes/no capability). With
`CONFIG_DOUBTECH_HID_KEYBOARD_ONLY_DIAGNOSTIC` the device name is forced to
`"DoubTech KB Test"` whatever `device_name` says (the name is still validated
first).

### `void doubtech_hid_pairing_set_events(doubtech_hid_pairing_event_fn callback, void *context)`

Stores the event sink and its context. `callback` may be `NULL` (no events). The
two values are plain variables read on the NimBLE host task when an event is
published; nothing synchronizes the write. Setting the sink before
`doubtech_hid_start` is the race-free order. Setting it after start is allowed (this
repository's transport does, with a `NULL` context): events published before the call
are dropped, and a write that overlaps a publish can pair the new callback with the old
context, so set it once and do not change it while links are active.

Required for pairing: `doubtech_hid_pairing_open` returns `ESP_ERR_INVALID_STATE`
without a sink, and a guest-initiated numeric comparison depends on it (it opens
the window itself), so a device with no sink cannot pair a new guest.

## Functions: pairing (NimBLE host thread)

### `esp_err_t doubtech_hid_pairing_open(void)`

Opens a 60-second pairing window: publishes `DOUBTECH_HID_PAIRING_OPENED`, stops the
current advertisement and schedules a restart (250 ms) that advertises
undirected and discoverable. Existing guests stay connected.

Returns `ESP_OK`; `ESP_ERR_INVALID_STATE` if the service is not started, no event
sink is set, the bond store cannot be queried, or eight bonds are stored (the last
case also publishes `DOUBTECH_HID_PAIRING_CAPACITY`); `ESP_FAIL` if the 60-second
timer cannot be armed (the window is closed again).

### `void doubtech_hid_pairing_cancel(void)`

Closes the window and any pending challenge, terminates the link that owned a
pending challenge, publishes `DOUBTECH_HID_PAIRING_CLOSED`, and restarts advertising.
Call it only after `doubtech_hid_start` returned `ESP_OK`: it does not check
`started` and uses timers that start creates.

### `esp_err_t doubtech_hid_pairing_confirm(uint32_t challenge_id, bool approved)`

Answers the pending numeric comparison for exactly `challenge_id`. There is no
application-decided comparison today. Every numeric comparison arrives as a NimBLE
passkey action; the library publishes `DOUBTECH_HID_PAIRING_CHALLENGE` and then, in the
same callback, calls this function itself with `approved` true (`hid_guest.c`), which
clears the pending challenge. A later call, from any thread, therefore finds no pending
challenge and returns `ESP_ERR_INVALID_STATE` (`tests/test_hid_guest.c` asserts this).
Calling it synchronously from inside the CHALLENGE event callback is harmful: it
consumes the challenge first, so the library's own confirmation then fails, and the
library publishes `DOUBTECH_HID_PAIRING_REJECTED` and terminates the link. The one effect
an application can have is to reject by calling it with `approved` false from within the
CHALLENGE callback, which terminates the link.

Returns `ESP_OK` when the answer was handed to NimBLE (a rejection also returns
`ESP_OK` after it terminates the link and publishes `DOUBTECH_HID_PAIRING_REJECTED`);
`ESP_ERR_INVALID_STATE` when no challenge is pending, `challenge_id` is 0 or does not
match the pending challenge, or the window has expired; `ESP_FAIL` when NimBLE
refuses the injected answer.

### `void doubtech_hid_pairing_snapshot(struct hid_pairing *output)`

Copies the current pairing state (window, challenge, bond table) into `*output`.
`output` must not be `NULL`.

Known gap: `struct hid_pairing` is an internal type, defined in
`firmware/components/doubtech_hid/core/hid_pairing.h`, which is a private header of
the component. Code outside the component cannot define a correctly typed variable
without that private header, so treat this function as unusable from application
code. The public ways to learn state are the events, `doubtech_hid_snapshot_slots`
and `doubtech_hid_retained_bonds`. A future public state type is the fix; until
then it is documented only because it is declared.

### `esp_err_t doubtech_hid_pairing_forget(doubtech_hid_token_t token, bool confirmed)`

Removes one bond: deletes the peer's NimBLE keys, saves the new token table to NVS,
and disarms and disconnects every connected link whose identity matches. Other links
stay up.

Returns `ESP_OK`; `ESP_ERR_INVALID_ARG` if the service is not started, `confirmed` is
`false` or the token is all zero; `ESP_ERR_NOT_FOUND` if no retained bond has that
token; `ESP_FAIL` if NimBLE could not delete the keys; or the NVS error when the
table could not be saved. The NimBLE keys and the NVS token table are two separate
stores. If the NVS save fails after NimBLE deleted the keys, the two are left
inconsistent and the in-memory table keeps the old entry.

### `bool doubtech_hid_current_bond_token(doubtech_hid_token_t *output)`

Looks up the token of the peer on slot 1. Clears `*output` and returns `false` when
the service is not started, slot 1 is not connected and encrypted, it is marked for
disconnect, or the link is not encrypted, bonded and authenticated, or the peer has
no token. Only slot 1 is addressable by this call; use `doubtech_hid_snapshot_slots` for
all slots.

## Functions: state snapshots (NimBLE host thread)

### `bool doubtech_hid_snapshot_slots(doubtech_hid_slot_t output[DOUBTECH_HID_MAX_CONNECTIONS])`

Fills all three slots (see `doubtech_hid_slot_t`). Returns `false` and leaves the array
zeroed if `output` is `NULL` or the service is not started. Side effect: a slot that
is armed but no longer has an authenticated bonded link is disarmed and disconnected
(`doubtech_hid_disconnect_slot`) as the snapshot visits it, so a snapshot can
repair a stale route.

### `bool doubtech_hid_retained_bonds(doubtech_hid_token_t output[DOUBTECH_HID_MAX_BONDS], size_t *count)`

Copies the retained tokens (never peer addresses) and their number. Returns `false`,
with `*count` 0 and the array zeroed, when an argument is `NULL`, the service is not
started, or the table is inconsistent (a zero or duplicate token). An empty table
returns `true` with `*count` 0.

## Functions: disconnect

### `esp_err_t doubtech_hid_disconnect_current(void)`

Same as `doubtech_hid_disconnect_slot(1)`. NimBLE host thread.

### `esp_err_t doubtech_hid_disconnect_slot(uint8_t slot)`

Disarms the channel, marks it as needing disconnect and terminates the link. A slot
that is not connected still succeeds. Returns `ESP_ERR_INVALID_ARG` for a slot outside 1
to 3, `ESP_FAIL` if NimBLE refuses the terminate, otherwise `ESP_OK`. NimBLE host
thread.

### `esp_err_t doubtech_hid_request_disconnect(void)`

Same as `doubtech_hid_request_disconnect_slot(1)`. Any task.

### `esp_err_t doubtech_hid_request_disconnect_slot(uint8_t slot)`

Queues a disconnect of the slot on NimBLE's default event queue and returns. Events
for the same slot coalesce into one pending event (the event object is reused).
Returns `ESP_ERR_INVALID_STATE` if the service is not started, `ESP_ERR_INVALID_ARG`
for a slot outside 1 to 3, otherwise `ESP_OK`. It does not wait for the radio
disconnect.

## Functions: bounded host-loop requests (any task except the host thread)

All of these return `false` when: the service is not started (the bridge is not
initialized), another request is in flight, the 20 ms wait elapses, the slot is out
of range where one is required, a required pointer is `NULL`, or the operation
itself fails. A timeout on slots 1 to 3 also queues a disconnect of that slot. An
operation that sends a report and fails marks the channel for disconnect and the host
loop disconnects it before the call returns.

The no-suffix functions (`..._ready`, `..._arm`, `..._release`, `..._keyboard`,
`..._mouse`, `..._consumer`) are the `_slot` functions with slot 1.

### `bool doubtech_hid_request_ready(void)` and `bool doubtech_hid_request_ready_slot(uint8_t slot)`

`true` when the slot's channel is ready: connected, encrypted, not marked for
disconnect and every input characteristic of the active protocol mode subscribed
(the same rule as `doubtech_hid_slot_t.subscribed`). Does not arm.

### `bool doubtech_hid_request_arm(void)` and `bool doubtech_hid_request_arm_slot(uint8_t slot)`

Sends an all-up baseline report set and then arms the channel so reports are
accepted. Returns `false` if the channel is not ready, is already armed, or any
baseline report could not be enqueued (in which case it stays disarmed). `true` means
the notifications were enqueued, not that the guest processed them.

### `bool doubtech_hid_request_release(void)` and `bool doubtech_hid_request_release_slot(uint8_t slot)`

Disarms first, then enqueues all-up reports. Returns `false`, and marks the slot for
disconnect, if the channel is not ready or any all-up report cannot be enqueued; the
host loop then terminates the link, because a key-up that may not have been delivered
could leave a key held on the guest.

### `bool doubtech_hid_request_keyboard(const uint8_t keys[DOUBTECH_HID_KEYBOARD_REPORT_LEN])` and `bool doubtech_hid_request_keyboard_slot(uint8_t slot, const uint8_t keys[...])`

Sends one complete eight-byte keyboard state (byte 0 modifiers, byte 1 reserved, bytes
2 to 7 six usages). `keys` is copied during the call; `NULL` returns `false`. Returns
`false` if the channel is not armed or not ready, or `keys[1]` is not 0. A failed
enqueue disarms the channel and marks it for disconnect. In boot protocol the report
goes to the boot keyboard characteristic.

### `bool doubtech_hid_request_mouse(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan)` and `bool doubtech_hid_request_mouse_slot(uint8_t slot, uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan)`

Sends one relative mouse report: `buttons` is the full five-button state (bits 0 to 4;
any of bits 5 to 7 makes the call return `false`), `dx` and `dy` are signed 16-bit
motion, `wheel` is vertical and `pan` horizontal. Returns `false` if the channel is not
armed or ready. In boot protocol the boot mouse report is used instead: only the low
three button bits, `dx` and `dy` clamped to -127 to 127, no wheel or pan. In the
keyboard-only diagnostic profile only the all-zero state is accepted (as a no-op).

### `bool doubtech_hid_request_consumer(uint16_t usage)` and `bool doubtech_hid_request_consumer_slot(uint8_t slot, uint16_t usage)`

Sends one consumer-control usage; 0 releases. Returns `false` if the channel is not
armed or ready, `usage` is above `0x03ff`, or the link is in boot protocol (boot
protocol has no consumer report). In the keyboard-only diagnostic profile only 0 is
accepted.

## Functions: pairing and bonds through the bridge (any task except the host thread)

### `bool doubtech_hid_request_pair_begin(void)`

Runs `doubtech_hid_pairing_open` on the host loop; `true` when it returned `ESP_OK`.

### `bool doubtech_hid_request_pair_cancel(void)`

Runs `doubtech_hid_pairing_cancel` on the host loop; `true` when the request
completed in time.

### `bool doubtech_hid_request_pair_reply(uint32_t challenge_id, bool approved)`

Runs `doubtech_hid_pairing_confirm` for exactly that challenge ID; `true` when it
returned `ESP_OK`. Because the library already confirmed every challenge when it published
it, a reply sent after a `DOUBTECH_HID_PAIRING_CHALLENGE` event finds no pending challenge
and this returns `false` in practice (see `doubtech_hid_pairing_confirm`). It does not let
an application decide a pairing.

### `bool doubtech_hid_request_current_bond_token(uint8_t output[DOUBTECH_HID_TOKEN_LEN])`

Copies the token of the peer on slot 1 into the caller's 16 bytes. `output` is cleared
first and stays cleared on any failure; `NULL` returns `false`. Never returns an
address or key material. It is a slot-1 request: a request
that times out queues a disconnect of guest 1 (the timeout path at the end of `request`
in `ble/hid_guest_rpc.c`).

### `bool doubtech_hid_request_slots(doubtech_hid_slot_t output[DOUBTECH_HID_MAX_CONNECTIONS])`

The bridged form of `doubtech_hid_snapshot_slots`: one bounded request copies all three
slots. The array is zeroed first and stays zero on failure; `NULL` returns `false`. The
snapshot side effect (disconnecting a stale armed slot) applies.

### `bool doubtech_hid_request_retained_bonds(uint8_t output[DOUBTECH_HID_MAX_BONDS][DOUBTECH_HID_TOKEN_LEN], uint8_t *count)`

Copies up to eight retained tokens and their count. `output` and `*count` are cleared
first and stay cleared on failure; `NULL` for either returns `false`. An empty table
with `true` is an authoritative "no bonds".

### `bool doubtech_hid_request_forget_bond(const uint8_t token[DOUBTECH_HID_TOKEN_LEN])`

Runs `doubtech_hid_pairing_forget` with `confirmed` set to `true` on the host loop; the
confirmation is the caller's act of calling it. `true` when it returned `ESP_OK`; `NULL`
returns `false`. The 16 token bytes are copied during the call.

## Pointer ownership summary

- `doubtech_hid_config_t` strings: borrowed during `doubtech_hid_start`, copied.
- `doubtech_hid_pairing_event_t *`: borrowed for the callback only.
- Event `context`: stored by the library, never freed or dereferenced.
- Output arrays and token pointers: written only before the call returns; cleared on
  failure.
- The library keeps no pointer to any caller buffer after a call returns.
