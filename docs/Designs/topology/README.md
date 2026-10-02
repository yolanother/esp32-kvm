# Physical monitor topology and directed portals

`esp32-kvm-topology-core` is a pure Rust model for Windows monitor snapshots.
The OS adapter supplies a stable display ID, final physical-pixel rectangle,
horizontal and vertical DPI, orientation, and primary flag for each monitor.
Rectangles and edge intervals use half-open physical coordinates, including
negative virtual-desktop origins. The crate rejects empty/overflowing bounds,
zero DPI, duplicate IDs, overlapping monitor rectangles, and snapshots without
exactly one primary monitor. It never invents guest display dimensions in
Standard BLE mode.

`Monitor::logical_to_physical` and `physical_to_logical` convert coordinates
relative to a panel's unrotated logical origin. Logical units use 96 DPI and
are scaled by the monitor's respective axis DPI. The forward transform floors
to a physical pixel, then applies the reported 0/90/180/270-degree rotation
inside the final physical rectangle. The inverse returns that pixel's logical
origin; fractional logical positions can quantize to the same pixel. The UI
and Windows adapter must agree on the orientation of panel-native coordinates
before using these methods for cursor placement.

`Topology::exposed_edges` subtracts each adjacent monitor's shared edge
interval, preserving the remaining maximal intervals. A monitor corner touch
does not hide either edge. A gap exposes both facing edges. Overlapping
physical monitor rectangles are rejected because a source pixel would be
ambiguous. Edges are recomputed from the latest snapshot after hotplug, DPI,
rotation, position, size, or primary-display changes.

`PortalGraph` binds a directed host edge interval to a persistent guest ID.
Each portal must fit entirely inside one currently exposed segment. Portals on
the same source edge cannot overlap, even if they point to different guests.
Half-open hit testing gives exactly one destination at a boundary. Graphs
retain both topology generation and the validated monitor snapshot; an
unrelated topology with the same numeric generation cannot reuse validation.
After a topology change, hit testing returns no destination until
`revalidate` succeeds. If a new monitor covers a portal edge, validation
fails and the graph stays inactive. Portal coordinates are physical runtime
intervals; persistence and normalized editor positions belong to the app
adapter.

From `crates/topology-core`, source checks are:

```text
cargo test --offline
cargo clippy --offline --all-targets -- -D warnings
cargo fmt --check
```

The crate is a member of the root Rust workspace. Windows monitor enumeration, live cursor crossing, dwell and
cooldown policy, hotplug event delivery, and physical display validation are
separate native gates. No COM port or firmware flash was used.
