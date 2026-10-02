# Native host I/O actor

`esp32-kvm-host-actor` owns one verified serial stream. `connect_system` filters
USB Serial/JTAG VID:PID `303A:1001`, verifies CAPS and SESSION_OPEN using
`usb-transport`, and passes the same open stream to `HostActor`. The actor
requests STATUS before allowing routing. A desktop setup flow must call the
actor's pairing methods rather than opening COM independently.

The caller owns the actor on one native thread and calls `drive(receiver,
now_ms)` with the Windows capture event receiver and a monotonic clock. Each
drive drains at most 64 capture events and polls protocol input. `request` is
the UI/device selection entry point. `observe_all_up` must be called only after
a physical key and button ledger proves all inputs released; the current
Windows capture adapter does not yet supply that ledger. The actor cannot arm
guest capture without both this observation and an exact ARM ACK.

For a guest switch, the actor serializes RELEASE_ALL, SWITCH, and ARM. It
checks each ACK's session, frame sequence, original command kind/sequence,
and resulting route generation. A matching NACK, two exhausted retries,
peer silence, serial failure, malformed frame, inconsistent STATUS, or capture
fault disarms capture and requires a fresh verified session. A local request
preempts an in-flight guest route. Pointer deltas are sent once and never
replayed by the retry path; keyboard state is an idempotent full report.

`pair_begin`, `pair_reply`, and `pair_cancel` use the same stream and ACK
tracking. `pair_reply` encodes numeric comparison only. `setup_snapshot`
exposes verified board ID, advertised capacities, routing state, and opaque
16-byte bond tokens with ready/subscribed flags from STATUS. Current CAPS and
STATUS do not carry firmware version, pairing deadline, challenge ID, or the
six-digit comparison value, so those fields remain `None`. The desktop must
not present an invented comparison value or call `pair_reply` until firmware
defines and reports the challenge. Pairing rejection is currently a generic
NACK fault; a typed full/timeout reason needs a firmware event or NACK
contract.

## Source verification

From `crates/host-actor`:

```text
cargo test --offline
cargo clippy --offline --all-targets -- -D warnings
cargo fmt --check
```

The fake byte stream and explicit clock cover exact ACKs, retry exhaustion,
local preemption, unplug, offline guest, stale hotkeys, capture queue driving,
keyboard and pointer forwarding, wheel accumulation, and pairing controls.
Native Windows capture integration, a physical all-up ledger, firmware pairing
challenge delivery, and board USB/guest switching remain pending hardware
gates. No firmware image was flashed for this task.
