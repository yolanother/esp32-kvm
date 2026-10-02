# Host-to-guest exposed-edge crossing policy

`esp32-kvm-edge-policy` consumes validated `topology-core` monitor and portal
snapshots plus explicit physical cursor/motion samples. It emits a persistent
guest ID and portal ID as *intent* for the serialized host actor. The caller
must resolve that guest to a currently ready slot and wait for the normal
release/select/arm transaction. The policy does not move the cursor, synthesize
mouse events, or infer the guest's remote cursor position. Standard BLE entry
retains the guest cursor wherever the guest OS last placed it.

A crossing candidate starts only on outward relative movement while the
cursor is inside an exposed portal's physical activation strip. Default
settings are a one-pixel strip, 12-pixel exclusion at each portal endpoint,
200 ms continuous dwell, and 500 ms cooldown. Dwell is configurable from
0–1000 ms; the strip is limited to 1–32 physical pixels. Stationary samples
can finish an existing dwell because the host cursor is clamped at the edge.
Leaving the strip, moving inward, becoming offline, a guard, or a topology
change cancels the candidate. Two portals eligible at the same corner are
ambiguous and activate neither.

The caller supplies authoritative guards: local control, physical mouse
buttons held (from the capture ledger), user pause, fullscreen state,
shortcut recording, and switch in progress. Fullscreen blocks crossing by
default and requires explicit policy opt-in. Offline guests never produce a
crossing request. After a crossing, both cooldown time and departure from the
activation strip are required before another request can start. The policy
checks both topology generation and monitor contents; a revalidated graph
cannot inherit dwell from the prior snapshot.

The policy saves the host cursor at crossing. On local return it yields a
safe physical point 16 pixels inside the original monitor edge, clamped to
that monitor's current bounds. If the source monitor was unplugged, it yields
the center of the current primary monitor. The caller performs the actual
Windows cursor placement after local routing is confirmed. No estimated
guest-to-host crossing exists in Standard BLE mode; return uses a hotkey,
tray, or device action until an optional authenticated helper is implemented.

The Windows adapter must supply relative outward motion even when the visible
cursor is clamped at an edge. Its physical-button guard must include events
consumed by hotkeys and events observed while capture is disarmed. Native
hook timing, cursor placement, fullscreen detection, hotplug delivery, guest
readiness, and physical crossing remain pending integration/acceptance gates.
No COM port or firmware flash was used.

Source checks from `crates/edge-policy`:

```text
cargo test --offline
cargo clippy --offline --all-targets -- -D warnings
cargo fmt --check
```

The crate-local `[workspace]` entry is for isolated source verification. The
coordinator removes it and adds the crate to the root workspace on integration.
