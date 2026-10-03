# Manual screen layout draft and dry run

The Screen layout page stores a version 1 draft in the desktop webview's local
storage. Host display rectangles, DPI, orientation and primary status are
entered manually. The page does not claim to detect Windows monitors. Guest
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
activate a portal. The Enable control remains disabled until Windows monitor
enumeration, native capture, current topology generation and the physical
all-up ledger are connected to the routing actor.

Standard BLE mode has host-to-guest crossing only. The guest retains its own
last cursor position; the proposed host-return shortcut is Ctrl+Alt+F10 when
native shortcuts become active. Guest-edge return and exact remote cursor
placement require a trusted optional helper and remain disabled. No helper
availability is inferred from a paired BLE guest.

Source checks: `cargo test --workspace --offline`,
`cargo clippy --workspace --all-targets --offline -- -D warnings`,
`cargo fmt --all -- --check`, Node tests in `apps/desktop/src/*.test.mjs`, and
`npm run build`. A hardware gate must separately compare an authoritative
Windows monitor snapshot to the draft, exercise hotplug/DPI/rotation changes,
and verify a host-edge crossing and local return without replaying held input.
