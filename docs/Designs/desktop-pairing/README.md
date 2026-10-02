# Desktop setup and guest pairing wizard

S01 is an explicit three-step flow in the **Systems → Add guest** and **Device →
Open setup** paths: connect and verify the board, pair the guest, then run a
bounded control test. It never flashes on connect or arms input. The design
preview toggle can show an illustrative challenge and completion flow, marked
as an example and isolated from all native calls.

## Native ownership

`apps/desktop/src-tauri/src/setup.rs` defines typed Tauri commands and a
`SetupBackend` boundary. `actor_backend.rs` starts one worker that owns the
verified host actor and its serial stream. It enumerates Espressif VID/PID
candidates, then uses the actor's HELLO/CAPS/SESSION_OPEN handshake on that
same open stream. The actor drives heartbeat and STATUS polling while Tauri
reads a cached snapshot. Begin and cancel requests go through a bounded
worker channel to that actor. Firmware board ID, capacity, bond tokens and
readiness come from its validated session and STATUS. A failed session clears
live readiness immediately; a saved profile or COM name cannot imply a live
connection.

The setup worker uses a disarmed `CaptureGate` and an inert mapper. It starts
no Windows hook, observes no physical all-up ledger, and cannot arm guest
input through this setup surface. This keeps pairing separate from guest
switching until the native capture and routing integration is complete.

The actor provides `pair_begin(60, now_ms)`, `pair_cancel(now_ms)`,
`pair_reply(challenge_id, approved, now_ms)`, and a setup snapshot. The current
wire schema omits the challenge number/ID, pairing deadline and detailed
rejection status. Firmware version is parsed by CAPS but not yet exposed by
the actor. The wizard displays unavailable states rather than inventing a
countdown, code, firmware version or success. Numeric confirmation stays
disabled even though the actor has a `pair_reply` method; protocol and
firmware must add an authenticated challenge event before that command can
be used. A native all-up HID test action is also required before **Finish
setup** can enable.

## Local identities and safety

Guest labels are keyed by exactly 16 opaque firmware bond-token bytes encoded
as 32 lowercase hex characters. The host stores names, OS and an explicit
profile preference; it never stores BLE keys, typed input or pairing codes.
The Tauri service accepts a profile only if the current firmware snapshot
reports that token. Two alternating versioned JSON files retain a last-good
profile revision if a write is interrupted. A corrupt or unsupported pair of
files is kept for recovery and prevents overwriting. Saved labels remain
visible as **Offline** when firmware does not report HID readiness.

The wizard handles missing cable, unverified USB candidate, incompatible
firmware, full bond storage, expiry/retry, unsupported pairing, cancellation,
and non-touch board guidance. It completes only after firmware reports the
bond ready, the local profile is saved, and an explicit all-up test succeeds.
All controls follow normal tab order, status changes use live regions, and
the numeric comparison includes a spaced spoken label.

## Verification

- `node --experimental-strip-types --test src/setup-model.test.mjs` from
  `apps/desktop` verifies the setup gates and offline identity behavior.
- `npm run build` from `apps/desktop` checks TypeScript and the production UI.
- `cargo test -p esp32-kvm-desktop --offline` from the repo root verifies
  snapshot mapping, fail-closed readiness, profile validation and last-good
  recovery.

The live Windows/Tauri screen, BLE pairing confirmation, guest HID test,
unplug recovery and keyboard-only visual pass remain physical gates. A
candidate USB port or passing source tests do not close those gates.
