# Device-owned input (BLE central) design: stage 2

Date: 2026-11-25. Status: written for owner review. Builds on
`2026-11-24-doubtech-hid-library-design.md` (stage 1, merged at `d8b78e2`).

## Intent

The owner wants the board to be the center of the KVM, not an accessory that the PC
app drives. Stage 2 lets the owner pair their own Bluetooth keyboard and mouse to the
board and have the board send that input to the selected guest by itself. The board
then keeps routing when the desktop app is closed, asleep or crashed. Today a
dropped host session blanks the screen with "INPUT PAUSED" and returns input to the PC;
that behaviour caused the flashing and the random returns the owner reported.

Roadmap: stage 1 (peripheral library) is done. Stage 2 is this document. Stage 3
(TinyUSB composite HID plus CDC, so the board is also a USB keyboard and mouse to the PC)
is a separate spec. Stage 2 comes first because stage 3 replaces the USB Serial/JTAG
COM port (same GPIO19/20 pins, one Type-C) and puts flashing and recovery at risk.
BOOT-button recovery is documented but untested on this board
(`docs/hardware/board-bringup.md`).

## Goals

1. Pair up to one keyboard and one mouse (BLE HID over GATT) to the board, from its
   touchscreen. Bonds persist and the devices reconnect after a power cycle.
2. Route their input to the selected guest (slots 1 to 3) with no PC app running.
3. Select the target from the paired keyboard (a documented chord) as well as from the
   touchscreen and the PLUS button.
4. Keep the PC-app path working unchanged: PC capture remains an input source (option A).
5. Fix the screen: with no host session and guests connected, show the guest view with a
   dimmed "USB off" indicator, not a full-screen pause.
6. "This PC" is a routable target whose output is a no-op until stage 3.

## Non-goals

- USB HID, TinyUSB or any change to the USB Serial/JTAG link (stage 3).
- Control-app pairing UI for input devices (the touchscreen only; the app is a later layer).
- Generic Report-mode HID parsing. Only boot-format devices and one narrow mouse
  report-map case (see Decisions 5 and 6) are supported.
- More than one keyboard and one mouse at a time; combo receivers; classic Bluetooth.
- Changing the guest-side pairing security model or the host wire protocol.

## Constraints

- ESP-IDF 5.5.1, ESP32-S3, NimBLE host (single host task shared with `doubtech_hid`).
- Existing behaviour preserved: guest bonds, `kvm_bonds` NVS layout, 500 ms host lease,
  `kvm_router_t` semantics, wire protocol `0x12` STATUS.
- Library code carries no product name (`check-product-names.ps1` applies to any new
  component; the existing one stays green).
- Single-thread rule: only the USB worker task touches `kvm_router_t`; the new hub
  runs on that task too.

## Approaches considered

A. **Central logic inside `doubtech_hid/central/`.** One public header, one NimBLE
   owner, shared bond accounting. Cost: the peripheral library's tests and README grow
   and its name now covers a different role; a future consumer of only the peripheral
   half pulls central code.
B. **New component `doubtech_hid_central` plus a pure `input_hub` component.** Central
   scanning, GATT client and parsers are separate from the router and the guest
   library; `doubtech_hid` gets one small, library-neutral addition (Decision 2).
   Pure cores are host-testable like stage 1.
C. **Reuse `kvm_router_t` for device input by faking a host session.** Smallest code,
   but couples device routing to a lease and sequence rules meant for the PC link and
   keeps the "no host means paused" behaviour. Rejected.

**Chosen: B.** It keeps the stage 1 library focused, makes the new logic testable
without a radio, and leaves `kvm_router_t` untouched.

## Decisions

1. **Components.** `firmware/components/doubtech_hid_central/` (NimBLE GATT client,
   registry, parsers; public header `doubtech_hid_central.h`) and
   `firmware/components/input_hub/` (arbitration, chords, release; pure C, no NimBLE).
   Product-name guard extended to `doubtech_hid_central`.
2. **Shared NimBLE host.** `doubtech_hid` still owns `nimble_port_init` and the host
   task. It gains `doubtech_hid_host_synced()` (true after the sync callback) and a
   config field `foreign_bond_reserve` (default 0): NimBLE bonds that do not belong to
   guests. `doubtech_hid_pairing_open` currently refuses when NimBLE reports 8 bonds of
   any kind (`hid_guest.c`, `ble_store_util_bonded_peers`); it must compare against
   `DOUBTECH_HID_MAX_BONDS + foreign_bond_reserve` and keep guest capacity at 8 by
   `pairing.bond_count`. The KVM firmware sets the reserve to 4.
3. **Connection budget.** 3 guest links (peripheral) + 1 keyboard + 1 mouse (central) =
   5. sdkconfig: `BT_NIMBLE_ROLE_CENTRAL=y`, `BT_NIMBLE_MAX_CONNECTIONS=5`,
   `BT_CTRL_BLE_MAX_ACT=8` (5 links + advertising + scan/initiate + 1 spare),
   `BT_NIMBLE_MAX_BONDS=12` (8 guests + 4 input identities; only 2 connect at once),
   `BT_NIMBLE_MAX_CCCDS=24` unchanged (only guests subscribe to the board).
   The builder verifies the ranges in menuconfig for the S3 controller and records free
   internal RAM before and after (build gate: heap headroom reported, not assumed).
4. **Input-device registry and storage.** Up to one keyboard and one mouse are active.
   A small record `{addr, addr_type, kind, name[24]}` per identity is stored in NVS
   namespace `dthid_inputs` (separate from `kvm_bonds`; guest tokens are untouched). The
   NimBLE store holds the keys. Forgetting deletes both.
5. **HOGP client, boot first.** Discover service 0x1812. If Protocol Mode 0x2A4E exists,
   write 0 (Boot Protocol) and subscribe Boot Keyboard Input 0x2A22 or Boot Mouse Input
   0x2A33. Boot keyboard report is the 8-byte report the router already uses. Boot
   mouse is 3 bytes (buttons, dx, dy) and has no wheel.
6. **Wheel and non-boot mice.** A mouse without boot characteristics, or one the owner
   wants a wheel from, uses Report mode through a deliberately small report-map reader:
   it accepts one Input report holding buttons (up to 5), relative X and Y (8, 12 or 16
   bits) and an optional 8-bit wheel, with or without a report ID, and rejects anything
   else with a "not supported" reason shown on screen. No keyboard Report-mode parsing
   (a keyboard without boot support is "not supported"). If the report-map reader
   proves expensive, the builder may ship boot-only and records the cut in the plan
   ledger; the owner's actual devices decide (open item).
7. **Security.** SMP initiator, LE Secure Connections, bonding. Keyboards that need a
   passkey: the board has a display, so it shows the 6-digit number and the owner types
   it on the keyboard. Most mice are Just Works. `ble_hs_cfg.sm_mitm` is global in
   NimBLE, so an input-device pairing window sets `sm_mitm=0` and is mutually exclusive
   with guest pairing (the existing guest window requires MITM). Accepted risk: an
   input device may be bonded unauthenticated; mitigated because pairing starts only
   from the touchscreen and reconnect accepts only stored addresses.
8. **Scan and reconnect.** Scanning only runs inside an owner-started 30 s pairing
   window (filter: HID service UUID 0x1812 or HID appearance 0x03C1/0x03C2/0x03C4,
   max 6 rows, strongest RSSI first). Reconnect: `ble_gap_connect` to each stored
   identity in 10 s attempts, 15 s apart, so advertising and guest notifications keep
   their airtime. The builder measures guest notify latency while a reconnect attempt
   is active and reports it (risk R3).
9. **Input hub and ownership.** Targets: 0 "This PC" (stub sink), 1 to 3 guests.
   The hub runs inside the existing USB worker loop so router state is read on one
   thread. Central events arrive through a bounded queue (depth 16); overflow drops
   motion first and never drops a key release.
10. **Arbitration (host session versus device).** The host-armed route always wins:
    while `router.armed` is true (the PC app is driving a guest), device input is
    discarded and the hub releases anything it armed. When the router is not armed
    (no session, or host on "This PC"), device input goes to the hub target. The hub
    never arms a slot the router holds, and releases its arm before the router arms.
    The hub's selected target is independent of `router.slot`.
11. **Target selection.** Chord on the paired keyboard: Left Ctrl + Left Alt + 1/2/3
    selects guest 1/2/3, + 0 selects This PC, + Esc is "release all" (all up on the old
    target, target 0). Chord keys are swallowed. A switch first sends all-up to the old
    target, then arms the new one. Touchscreen cards and PLUS also set the hub target
    when no host session is open; with a host session they keep today's behaviour.
12. **Fail-safe.** Input-device disconnect, GATT error or parse failure sends an
    all-up keyboard report and zero mouse state to the target. An unready or
    disconnected target drops events and keeps the hub target.
13. **Display.** `kvm_display_status_t` gains `host_session`, `input_kbd`,
    `input_mouse` (connected flags) and the hub target. With no host session and at
    least one ready guest the active/guest view is shown with a dimmed "USB off" badge
    and the input-device icons. "LOCAL CONTROL / INPUT PAUSED" remains only for faults and the recovery
    state. With no host session and no ready guest the guest view shows "No guests
    connected" with the same badge, so the screen never flaps between views. A new screen
    handles input-device pairing (scan list, passkey, result), entered from the
    existing guest settings action (`display.c`, `guest_settings_action`).
14. **PC capture unchanged.** `crates/` host code is not modified in this stage; STATUS
    keeps its layout. The app learns about device ownership in a later change.

## Risks

- R1: Controller activity budget with 3 peripheral links, 2 central links, advertising
  and scanning may fail on a real board; fall back to one input link at a time
  (keyboard first) and report it.
- R2: Peripheral bond accounting regression (Decision 2) could stop guest pairing at 8;
  covered by a dedicated test and the owner's three-guest hardware check.
- R3: Reconnect scanning and 20 ms advertising may delay guest notifies.
- R4: Owner's keyboard or mouse may lack boot protocol and fit no Report case.
- R5: `sm_mitm` toggling races with a guest pairing attempt; windows are exclusive.
- R6: Typing at "This PC" does nothing until stage 3; documented.

## Left to the builder

Exact NimBLE call sequences, GATT discovery state machine, RSSI filtering details, LVGL
layout, report-map reader corner cases, and the heap and latency measurements. Owner-run
hardware checks are in the plan (final task).
