# Physical modifier mapping presets

Mappings are saved on the Windows host by the firmware's opaque 16-byte bond
token. The unchanged preset emits identity keys. **Cmd / Windows key → Ctrl**
maps left and right GUI usages to the corresponding Ctrl modifiers; it does
not remap physical Ctrl. **Windows Ctrl → Mac Cmd** maps left and right Ctrl
to the corresponding GUI modifiers; it does not remap physical GUI. Alt maps
to Option/Alt by identity in both directions. Presets operate on HID usages,
not translated characters or typed text.

The Key mappings page shows the complete list of changed modifier bindings
for the selected guest before Apply. It offers a custom copy of a selected
preset, sided modifier edits, reset to the copied base, and discard. Applying
saves only this guest's choice and custom bindings in the crash-tolerant local
profile store. The setup service keeps saved profiles visible if the actor is
temporarily unavailable, marks their mapping installation pending, and retries
one pending mapping per status refresh. The desktop worker caches each accepted
mapping and restores it to the single host actor on reconnection. A failed
installation disarms the capture gate. The input engine validates
rules and defers a live edit until all physical keys are released. Local host
input is never remapped.

Windows can synthesize left Ctrl with right Alt for AltGr on non-US layouts.
When both physical usages are held, the mapping engine preserves Ctrl and
right Alt even under the Windows-to-Mac preset. Character output still depends
on the guest layout. The source test verifies the full held pair and an
additional printable key; a hardware test remains necessary for the transient
first Ctrl-down event and real Apple keyboards on Windows. Native capture is
currently disarmed, so saving a mapping does not claim a live guest input test.

Source verification: `cargo test --workspace --offline`,
`cargo clippy --workspace --all-targets --offline -- -D warnings`,
`cargo fmt --all -- --check`, `node --experimental-strip-types --test
src/*.test.mjs`, and `npm run build` from `apps/desktop`. The two firmware
bond-removal and input-capture hardware gates are owned separately.
