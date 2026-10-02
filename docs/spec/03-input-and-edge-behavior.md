# Input interaction specification
## Switching
Global shortcuts are editable with a physical-key recorder, conflict/reserved-combo validation and test mode. Proposed defaults are in 01-product.md. A hold of both Ctrl keys for one second is a reserved emergency chord, recognized before all mappings. Reject mappings/shortcuts that consume this escape; modifier-only events already delivered may occur before a chord is recognized, but the activating F-key/digit must never leak.
Next/previous cycles configured order and skips unavailable guests. Direct guest hotkey preserves local control on failure. Host return is unconditional. Device touch selection and PLUS cycle request the same transaction through USB; BOOT long press at runtime suspends immediately. Keep boot-at-power-on recovery and PWR behavior intact.

Switch transaction: stop forwarding → release old guest keyboard/mouse/consumer states → advance routing generation and select a READY target → receive acknowledgment → arm destination → show selection in UI/board. Bound ordinary switch timeout at 500 ms; on uncertainty fail local. Link lost during release means delivery cannot be guaranteed; clear firmware state, close/suspend that link and send zero state before accepting input on reconnect.
Do not carry held keys/buttons across targets. Ignore keys that were down at switch until physically released and re-pressed. Pointer deltas during transition are discarded, never replayed. User must release a mouse drag before ordinary switching; emergency release always overrides. Intentional modifier carry/drag continuation is outside v1.

## Key mapping
Profiles bind to persistent guestId/bond identity, never slot order or display name alone. Fields: source usage and left/right location; optional source modifiers; target usage/modifiers; enabled; priority; source-device layout hint. Physical scan code → USB usage → mapping; do not map translated text characters.
Mapping directions must be explicit:
| Physical host input | Emitted guest input | Use |
|---|---|---|
| Cmd/Windows/GUI | Ctrl | User-requested Cmd → Ctrl, including Apple keyboard on Windows host |
| Ctrl | Cmd/GUI | Familiar Windows shortcuts when controlling a Mac |
| Alt | Option/Alt | Preserve macOS Option |
| Win/GUI | Cmd/GUI | Identity modifier semantic |
Provide unchanged, Windows shortcuts → Mac, and custom presets. Never silently reverse the user's requested direction.
Support one-key, sided modifier and exact chord-to-chord rules. Per-guest rule overrides chosen preset; exact chord wins over single-key rule; otherwise identity. Reject duplicate same-priority triggers. Evaluate once, no recursive remapping. Compute destination key-down state from all physical presses using reference counts so two sources mapped to Ctrl do not release each other. Remember transformed outputs per press for correct key-up even if profile changes; defer profile changes until release or explicitly clear/suppress held keys.
App-specific mapping is future work because BLE-only mode cannot know the guest's foreground app.
6KRO overflow emits standard rollover indication until keys return within capacity; modifiers remain tracked. Keyboard repeat is produced by guest OS from held state, not duplicate host repeats. Dead keys, IME and language output follow guest layout; config names physical layouts and makes no Unicode-text injection promise.
Mapping tester is local simulation by default: source keys → emitted guest labels, with clear/release and no background logging. Explicit 'Send test to guest' requires a ready guest and visible start/stop.

## Screen layout and crossing
Windows host enumerates monitors with stable IDs, physical-pixel rectangles, DPI, rotation and negative origins. Show a drag-to-arrange logical layout with numeric controls as accessible alternative. Guest displays in Standard mode are user-entered placeholders, never discovered claims. Bind directed portals from exposed host monitor segments to guest targets.
An exposed segment excludes shared monitor seams; re-evaluate topology after hotplug/DPI/rotation. Only one destination per source segment; reject ambiguous overlapping portals.
Activation: outward movement at assigned edge, dwell 200 ms; suppress at corners (12 px each), while dragging, when offline, during hotkey recording and after switch cooldown 500 ms. Configurable dwell 0–1000 ms. Cooldown also requires cursor leaving activation strip before rearming. Fullscreen auto-switch disabled by default where detection is reliable; manual pause always available.
Example: Mysiraith main display right edge → MacBook. Standard mode sends relative motion at the MacBook's last cursor position. UI says 'Return with Ctrl+Alt+F10'; no fake remote cursor preview.
Returning locally restores saved local cursor or a safe inset at the corresponding portal; monitor unplug falls back to current primary bounds. Offline destinations never trap cursor.

## Optional helper mode (M3)
Host app talks to guest helper over authenticated encrypted LAN transport; USB→BLE remains the input path. Helper observes local cursor/monitor topology and positions cursor on entry, not reinjects duplicate keyboard/mouse events.
Authenticated helper identity must be explicitly associated with a BLE guest profile. Exchange normalized edge coordinates u=clamp((position−segmentStart)/segmentLength,0,1) and convert using destination physical segment/rotation/DPI. Use helper session, timestamp and routing generation; stale or mismatched telemetry cannot switch targets.
Crossing while active: helper reports outward edge intent with no buttons held → host stops BLE motion → release/switch transaction → destination helper positions cursor and acknowledges → resume input. Time out to Standard mode/local control. Direct guest → guest crossing requires helpers on both guests for accurate topology/placement. Independent local guest input invalidates stale motion assumptions.
Helper connection/trust loss disables guest-edge portals; hotkey control remains. No clipboard/file/video transfer in this scope.

## UX states
Local, active guest, switching, connected standby, reconnecting, offline, pairing, paused, updating, incompatible firmware and helper degraded. Always show target name and text status (not color alone). Never retain 'Controlling' while firmware reports suspended.

