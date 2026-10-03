# Host guest profiles and live connection state

The desktop stores host-local guest profiles keyed by the firmware's opaque
16-byte bond identity token. A profile records a friendly name, OS, mapping
preference, and optional direct shortcut, mapping-profile link and layout
link. Those links are identifiers only; shortcut registration, mapping edits
and layout editing have separate native owners. The store contains no BLE
cryptographic keys, passkeys, typed input or firmware bond database.

Two alternating `guest-profiles-{0,1}.json` files carry a schema version and
monotonic revision. A write goes to the inactive slot and is synced before
it becomes the active in-memory revision. On startup the highest valid
revision wins, so a truncated newer slot falls back to the last good one.
Schema 1 labels load with empty optional fields. Migration copies the
selected schema 1 file to `guest-profiles-v1-backup.json`, verifies any
existing backup, then writes schema 2 into the other slot. An invalid pair
of slots stops writes for manual recovery instead of silently erasing data.
Duplicate bond identities and direct shortcuts are rejected.

The actor snapshot distinguishes firmware-reported bonds, connected BLE
peers, subscribed HID-ready peers and the acknowledged active route. A saved
profile remains **Offline** when the device is missing or its session has
failed. A connected peer without HID subscription is **Connected**, a
subscribed peer is **Ready**, and only the actor's confirmed guest route is
**Controlling**. Guest selection remains disabled until native capture and
the physical all-up ledger are integrated.

Existing labels can be edited offline because the token was already proven
when saved. Creating a new host profile still requires that token in current
firmware STATUS. The editor keeps mapping/layout link identifiers when
editing visible fields; its shortcut value is a saved preference and is not
registered as a hotkey in this build.

Forget is an explicitly confirmed, retry-safe two-part operation. The host
requires a verified local device session. If firmware still reports the
bond, the backend must send `FORGET_BOND` and wait for ACK plus a fresh STATUS
without that token. Only then does the host remove the local profile. A
failed request retains the profile; a retry after firmware removal finishes
local cleanup. Repeating a completed request returns `already_absent`.
The host actor does not yet expose `FORGET_BOND`, so live forget is gated and
the UI reports that precise failure. No local-only deletion is offered.

Source gates: `cargo test --workspace --offline`,
`cargo clippy --workspace --all-targets --offline -- -D warnings`, and
`cargo fmt --all -- --check` from the repository root;
`node --experimental-strip-types --test src/*.test.mjs` and `npm run build`
from `apps/desktop`. Physical BLE reconnect, guest switching and firmware
forget remain separate hardware gates.
