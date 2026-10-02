# Physical guest mapping and held-input safety

The Windows capture hook recognizes reserved shortcuts before guest mapping.
Its `CaptureGate` ledger records every non-injected scan-code and mouse-button
transition, including consumed shortcut keys and transitions while guest
capture is disarmed. The ledger survives routing-generation changes. Guest
arming requires both this ledger to be empty and a Windows `GetAsyncKeyState`
snapshot to show no held keys; the latter covers keys pressed before the hook
was installed. The gate checks the ledger again while arming. Injected events
never make the physical ledger look held or released.

`input-core::MappingEngine` receives physical HID usages and side information
from the host scan-code translator. It applies the selected guest profile once;
there is no recursive mapping of its own output. Rules for an exact held chord
take precedence over single-key rules. Within either specificity, guest rules
take precedence over preset rules, then higher priority wins. Identical
same-priority triggers within one layer are rejected. Otherwise the physical
usage passes through unchanged. Left and right modifier usages remain distinct.

The engine recomputes the destination report from all held physical sources.
Each destination has a reference count, so releasing one of two sources mapped
to the same modifier cannot release the other. Seven or more distinct ordinary
usages emit the standard six-key rollover indication while modifiers remain
tracked. Repeat down events leave the report unchanged. A profile edit is
validated immediately and applied only after all currently held sources are
released, so key-up uses the mapping that produced the key-down. Route changes
reset the engine's guest output; the physical capture ledger prevents a new
route from arming until prior held keys and buttons are released.

The host actor selects profiles by opaque 16-byte bond token from firmware
STATUS, never by transient slot order or display name. `set_guest_profile`
can edit an active guest; the mapping engine defers the change while keys are
held. The host actor checks physical all-up after ARM ACK and again while
waiting in a disarmed guest state, sends an all-zero keyboard/pointer/consumer
baseline, then asks the capture gate to arm. Local host input is not mapped.

`SetOneKeyMapper` supplies a conservative Windows set-one scan-code-to-HID
translator for known physical keys. It distinguishes extended navigation,
keypad, and left/right modifiers; unknown and synthetic sequences are omitted
instead of guessed from virtual-key text. The actor constructor still accepts
a `KeyMapper` for devices that need another physical translator. OS keyboard
layout coverage, native hook timing, and the physical switch matrix require
Windows and board verification. Source tests do not establish those native
behaviors; no COM port or firmware flash was used.

Source checks from the repository root:

```text
cargo test --offline --workspace
cargo clippy --offline --workspace --all-targets -- -D warnings
cargo fmt --check
cargo check --offline -p esp32-kvm-platform-windows --target x86_64-pc-windows-msvc
```
