# HID pairing and bond identity

The optional HID service remains off until firmware calls `hid_guest_start()`. It
advertises one HID identity and handles one connection. Routing still starts
disarmed. This implementation targets eight stored bonds; the separate
multi-guest workstream owns concurrent connections.

## Admission and consent

- `hid_guest_pairing_open()` opens a 60-second window when the bond store has
  capacity, no guest is connected, and an event sink is registered. A NimBLE
  host callout closes the window and cancels a pending challenge. A connection
  from an unknown resolved identity is terminated outside the window.
- The host configuration requires Secure Connections and MITM protection with
  Display Yes/No capability. Only numeric-comparison challenges are accepted.
  The status sink receives the six-digit number and connection handle;
  `hid_guest_pairing_confirm(handle, approved)` injects the answer. Any other
  pairing action is rejected. The sink must copy the short event and return
  promptly; no UI work belongs on the BLE callback thread.
- Encryption opens the report gate only after NimBLE reports encrypted,
  authenticated, bonded security. A new peer also needs an unexpired approved
  challenge. An unsupported guest pairing method has no Just Works fallback.
- NimBLE's sample round-robin store callback deletes the oldest bond on
  overflow. This service uses a callback that refuses the write. The policy
  refuses a ninth token and never silently replaces one. `forget` requires an
  explicit confirmation argument and disconnects the current guest.

## Identity and storage

NimBLE stores the BLE bond keys in NVS. The `kvm_bonds` NVS namespace stores a
versioned table of resolved peer identities and random nonzero 16-byte tokens.
The host can use the token as a stable opaque identity for friendly labels;
the token is not a BLE address or key. An absent table starts empty. A corrupt
or unknown-version table fails service startup without erasing keys. The NVS
format uses version 2 for 16-byte tokens; the earlier version 1 development
format was never shipped and is rejected without erasing it. New bond
metadata is committed before the connection's report gate opens. Save failure
deletes the new NimBLE peer and disconnects. Reboot reloads the token table.

## Thread boundary

The HID channel belongs to the NimBLE host task. The USB router can call
`hid_guest_request_ready`, `hid_guest_request_arm`, `hid_guest_request_release`,
and the three `hid_guest_request_*` report functions. They serialize through a
single NPL event slot and wait at most 20 ms. A busy slot or timeout returns
false; timeout queues a disconnect. The router should sample readiness at about
1 Hz for status and check again before a switch or arm. `request_arm` is the
authoritative transition. `hid_guest_request_disconnect()` queues a coalesced
disconnect from another task without waiting for radio teardown. Direct
`hid_channel_*` access from the USB task is not safe.

## Verification and physical gates

Run `firmware/tests/pairing/run-msvc.ps1` and
`firmware/tests/hid/run-msvc.ps1` with PowerShell and MSVC. The host tests cover
expiry, cancellation, consent, capacity, token reload/corruption, HID security
admission, and the command bridge. They do not verify ESP-IDF linking, NVS
power-loss behavior, radio timing, or interoperability with Windows, macOS, or
Linux. Board validation must check numeric comparison UI and guest capability,
rebooted bonds, a full eight-bond store, rejection of unsolicited peers, and
all-up/disconnect behavior under USB and BLE faults.
