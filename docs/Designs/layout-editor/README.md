# Windows screen layout discovery and portal preparation

The Screen layout page stores a version 1 draft in the desktop webview's local
storage. The native backend enumerates Windows monitors with physical desktop
bounds, effective per-monitor DPI, current rotation, and primary flag. The editor
offers **Use detected host monitors** to copy those values into the draft.
Manual geometry is still available for planning. Guest
rectangles are manual visual placeholders named from saved guest profiles;
they do not represent discovered guest screens or cursor positions. Removed
profiles remove their placeholders and portal destinations after the first
native setup snapshot loads.

Displays can be dragged, nudged with arrow keys, or positioned through numeric
fields. These actions edit the same physical-pixel coordinates, including
negative virtual-desktop origins. The editor previews exposed host edge
segments after subtracting shared monitor seams. A portal binds one exposed
half-open interval to a saved opaque guest identity, outward direction and a
0–1000 ms dwell. The suggested portal excludes 12 pixels at each corner.

**Dry-run and highlight** invokes `layout_validate_draft` in the desktop's
Rust backend. That command constructs the existing `Topology` and
`PortalGraph`, so overlapping monitors, shared seams, invalid intervals and
overlapping portals fail before a highlight appears. A saved draft does not
activate a portal. `layout_discover` polls the OS snapshot and advances a
generation on a monitor, bounds, DPI, rotation, or primary change. Prepared
portals are discarded on a change or enumeration failure. `layout_apply`
re-enumerates and checks draft hosts exactly against the current validated
snapshot, then swaps the portal graph and edge policy together. The current
edge policy has one dwell setting, so applied portals must share a dwell value.
The Enable control remains disabled until native capture, physical all-up,
guest readiness, and actor edge routing are connected. Preparation sends no
cursor or input events.

Windows display device names identify monitors within the current connected
arrangement. They can change after driver or cable changes; that invalidates
portal bindings. Discovery requires a per-monitor-DPI-aware Windows thread and
fails closed otherwise.

Standard BLE mode has host-to-guest crossing only. The guest retains its own
last cursor position; the proposed host-return shortcut is Ctrl+Alt+F10 when
native shortcuts become active. Guest-edge return and exact remote cursor
placement require a trusted optional helper and remain disabled. No helper
availability is inferred from a paired BLE guest.

Source checks: `cargo test --workspace --offline`,
`cargo clippy --workspace --all-targets --offline -- -D warnings`,
`cargo fmt --all -- --check`, Node tests in `apps/desktop/src/*.test.mjs`, and
`npm run build`. A physical multi-monitor gate must separately confirm native
bounds and DPI for negative-origin and rotated monitors, unplug/replug
invalidation, and a future captured host-edge crossing with safe local return.
Source tests do not claim that physical gate.
