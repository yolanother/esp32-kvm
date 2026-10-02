# ESP32 KVM — DESIGN.md
Version 1 · 2026-10-02 · implementation design baseline.
Canvas: JlbLPwOjBvnrOrZGdgDoW. Sample system names/states/measurements in mockups are illustrative, not live telemetry.

## Visual direction
Quiet desktop control panel: near-black blue background, teal actions, mint active status, silver text. Clear system identity and connection state matter more than decorative animation. Flat surfaces, restrained borders, compact readable tables.
Tokens: background #0B1116, sidebar #101A22, surface #15222D, raised #1D2D39, border #344955; text #EEF5F7, muted #B4C5CE, accent #75E2C3, teal #256980, warning #F6C66B, danger #FF9D9D. Mint buttons use #0B211B text. Never use teal text on dark surface without verified contrast.
Typography: Segoe UI/system sans; 28/34 page title, 18/24 section, 14/20 body, 12/18 metadata; tabular monospace for key chords/status values. Spacing 4/8/12/16/24/32; radii 8 controls, 12 cards. Desktop baseline 1120×760, minimum 900×640; content scrolls, sticky save bar for editors. Respect system text scaling and reduced motion. Optional light theme later using semantic tokens, not separate layouts.

## Components
AppShell: 190px navigation, header containing USB/link state, main content and persistent host-return footer.
SystemCard: OS/name, role, connected/ready/controlling state, shortcut, profile and Select action. Active mint border; offline text/disabled input action with reconnect.
StatusPill: icon+label; do not encode only color.
KeyChord: spaced keycaps with accessible expanded labels. Recorder suspends guest capture while recording; Esc cancels, conflicts inline.
MappingRow: physical source, direction arrow, emitted guest key, side, enable, edit/remove. No ambiguous 'swap Mac/Windows' label.
MonitorTile/Portal: host detected vs guest manual/helper label; exposed edge strip; keyboard/numeric positioning; directed arrows.
InlineNotice: concise limitation with a concrete next action, e.g. 'Use Ctrl+Alt+F10 to return. Install helper for automatic return.'
Transient switch overlay: compact target/status/return shortcut; dismiss after 1.5s; errors persist until local control restored.
Dialogs: focus trap, Esc cancel where safe, return focus, explicit destructive confirmation for forgetting bond.
Form state: clean, dirty, saving, saved, invalid, conflict; preserve draft if connection drops.

## Screens and acceptance
S01 Setup and pairing: stepper Connect device → Pair guest → Test controls. States: no device/data cable, handshaking, firmware mismatch, ready; pairing countdown/challenge; guest OS/name; no slots; timeout/retry. Testing is explicit and ends with all-up. Complete only after HID readiness.
S02 Systems dashboard: local host plus up to reported live guest capacity; add guest, select, reorder cycle, pause and return. Show '3 connected' only from firmware. Saved guests beyond live capacity remain visible offline. Empty state leads to setup; switching card pending until ACK.
S03 Screen layout: host monitor geometry, guest placeholders, segment inspector, dwell/corner/cooldown and drag/fullscreen guards. Standard entry/return limitations visible. Save + dry-run test. No inner-seam portals.
S04 Key mappings: guest selector, explicit source→guest table, preset preview, add/edit, conflict, save and local simulator. Show Cmd/GUI→Ctrl separately from Ctrl→Cmd. Live test never starts automatically.
S05 Shortcuts: record next/previous/local/direct-guest combos; conflict validation, test, restore defaults. Fixed both-Ctrl emergency shown separately. Avoid reserved OS combos; shortcuts independent of guest mappings.
S06 Device & diagnostics: model/variant, firmware/protocol, USB/BLE readiness, link metrics labeled by scope, redacted diagnostic export and firmware check/update/recovery. UI must say 'Not measured' until telemetry exists. Pairing/bond management accessible.
S07 Tray and switch overlay: local/guest radio choices, shortcuts, paused, settings, quit; visible notification on connection loss. Quit releases input. Overlay remains usable with full-screen host apps where OS permits.
S08 Optional seamless setup: per-guest helper status and identity trust, permission checks, automatic return toggle available only when healthy, test crossing and revoke. Standard mode works without this screen.
S09 Recovery/empty states: device unplugged → local restored, firmware incompatible → setup action, guest asleep → retry/change target, helper lost → Standard mode banner. Never show 'Controlling' without confirmed firmware state.

## 240×240 device UI
14px body, 24–28px target, 12px status; 12px outer padding; touch targets at least 44×44. Limit to one primary action plus two status lines; scroll long target names only on deliberate focus, otherwise ellipsis with full name on detail.
D01 Active/local: USB state, large target, guest count, cycle action; active signal mirrors firmware.
D02 Guest list: three 44px rows plus header/back, selected/standby/offline text. PLUS cycles; touch selects if present.
D03 Pairing: device name, dynamic code when applicable, 60-second timer, confirm/cancel appropriate to negotiated pairing. Code is illustration in mockup; never fixed in firmware.
D04 Paused / USB lost: 'Input paused', reason, reconnect hint; all reports cleared. Local selection possible only with healthy host session.
D05 Update/recovery: progress and do-not-unplug text during flash; recovery instructions on failure. Never hide USB/host state behind audio widgets.
Controls: PLUS short press cycles ready targets through host arbitration; BOOT long press ≥1s at runtime suspends; touch can select/confirm. PWR retains power semantics. Confirm exact physical button mapping at bring-up.

## Interaction details
Pairing authentication: prefer LE Secure Connections with MITM-protected passkey/numeric comparison supported by actual guest capabilities and display/input UI. Validate I/O capability selection across guests; never silently downgrade. Show a deliberate compatibility decision if a guest cannot complete the selected method.
Show the active target on both desktop and board; a pending selection is not active. UI optimistic edits may be staged, but routing waits for acknowledgment.
All firmware updates/pairing tests first pause/release. Destructive forget explains which guest must pair again.
Accessibility: keyboard-only complete setup/config, logical tab order, screen-reader labels/live status, AA text/control contrast verification, non-color states, ≥32px desktop targets, 44px touch, reduced motion.
Mockups are base visual handoff, not a connected application. Implementation must wire native status, all actions and the error states above.

## Data model
AppConfig{schemaVersion,hostId,deviceId,guestProfiles[],hotkeys,monitorLayout,preferences}.
GuestProfile{guestId,bondToken,label,os,physicalLayout,cycleOrder,directHotkey,mappingProfileId,helperTrustRef?}.
MappingProfile{id,revision,presetId?,rules[{id,sourceUsage,sourceModifiers,side,targetUsages,targetModifiers,enabled,priority}]}.
Portal{id,sourceMonitorId,edge,startNormalized,endNormalized,destinationGuestId,destinationMonitorId?,mode,dwellMs,cornerExclusionPx,cooldownMs,enabled}.
RuntimeGuest{guestId,slot,connected,encrypted,subscribed,ready,selected,lastError,connectionIntervalMs}; not persisted as true after restart.
HelperSession{trustedIdentity,guestId,sessionId,permissions,topologyRevision,lastSeen,routingGeneration}; OS-protected secret references only.
Bonds stay firmware-side; exports omit credentials, typed keys and passkeys. Configuration validation rejects duplicate shortcuts, conflicting portals and unavailable hardware assumptions.

