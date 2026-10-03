# Dashboard, tray, and switch overlay

S02 and S07 use the host actor's route state as the only source of active
target truth. The Tauri setup snapshot carries a separate `route` field:
local, awaiting STATUS, switching, pairing, acknowledged guest slot and
opaque bond identity, or failed with a reason. A connected BLE bond is only
**Ready** until the actor reports an acknowledged guest route. Failed serial
sessions clear live readiness and show local recovery; the UI reports missing
firmware or actor status without inventing latency or cursor position.

The Systems page shows the active target above local and saved guest cards.
Its guest Select and Pause controls are visibly disabled while the desktop
actor still uses a disarmed capture gate and inert mapper. Host return goes
through the actor's sole serial stream and waits for the next actor snapshot
before changing the displayed target. Saved guests remain visible as Offline
after unplug. Design example cards are explicitly marked and never feed live
route labels or native actions.

The native Windows tray keeps host and guest choices, pause, settings, and
quit visible. Guest choices and pause stay disabled under the same capture
gate. It refreshes the radio checks and labels from the actor snapshot, with
host checked after failure. Closing the main window hides it to the tray;
Settings restores and focuses the Device page. Quit disarms the capture gate,
asks the actor to return locally, closes its serial stream, then exits. A
lost device also leaves the host gate disarmed.

A confirmed route change raises a compact overlay for 1.5 seconds without
stealing focus. Link failure raises a persistent, dismissible alert naming
the local recovery reason. The UI never claims a pending switch is active.
The proposed return shortcut is labelled inactive until native hotkey and
physical ledger integration has a separate verified handoff.

Verification: `node --experimental-strip-types --test src/*.test.mjs` and
`npm run build` from `apps/desktop`; `cargo test --workspace --offline`,
`cargo clippy --workspace --all-targets --offline -- -D warnings`, and
`cargo fmt --all -- --check` from the repository root. Native tray lifetime,
fullscreen overlay behavior, and physical switch/quit recovery require a
Windows interactive and hardware gate; source checks alone do not prove them.
