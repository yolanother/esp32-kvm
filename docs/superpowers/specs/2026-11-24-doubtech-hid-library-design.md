# DoubTech HID library: extract the BLE keyboard/mouse output into a reusable component

Status: draft for approval. Date: 2026-11-24.
Baseline: commit `3c2a86d`, tag `checkpoint/pre-hid-library` (pushed to origin).

## Intent and why

The firmware can present itself to a computer as one Bluetooth LE keyboard + mouse
and reattach reliably after power cycles, with up to three computers at once. That
behavior works well with macOS today and is the part worth reusing.

The owner plans a new hardware variant where the board pairs the owner's own
Bluetooth keyboard and mouse directly, so input no longer depends on a PC being on.
That variant will still present the board to other computers as one keyboard/mouse,
and will also expose USB HID plus the existing control channel to the PC. This
document covers only the first step: moving today's "act as a keyboard/mouse"
(BLE peripheral) code into a standalone, reusable library without changing how it
behaves, and making the advertised identity configurable instead of hard-coded to
"ESP32 KVM".

Work is three sequential projects. Only project 1 is in scope here:

1. Extract today's BLE HID output into a library (this document).
2. Later: BLE central client that pairs the owner's keyboard and mouse on the device
   screen or through the desktop app, and feeds the existing route gate.
3. Later: TinyUSB composite HID + CDC device output to the PC.

## Goals

- A component, `doubtech_hid`, that contains no references to the KVM product, router,
  transport, display or protocol. It builds and its tests run without them.
- Behavior-preserving move: same GATT service and report map, same advertising,
  same pairing rules, same reattach behavior, same bond storage, same three-link
  budget.
- The advertised identity is a setting supplied by the integrating firmware: GAP
  device/advertised name, DIS manufacturer and model strings. The KVM firmware passes
  "DoubTech HID" as its default. The device remains ONE combined keyboard + mouse
  device, exactly as today (decided by the owner; two separately named devices were
  rejected because each computer would pair twice and use twice the connection slots).
- The KVM firmware is rewired to the library's public API with no functional change.
- Existing HID C tests move with the library and pass unchanged in behavior.
- The PC-capture route (Windows capture, USB protocol, router, selected BLE output)
  keeps working exactly as now. The owner wants it kept as a selectable input source
  so a wired keyboard/mouse on the PC still works while later layers are built.
- The layout leaves a clear place for the future central (input client) role to
  share bond storage and report definitions, without implementing it.

## Non-goals

- No BLE central/input-client code, no TinyUSB, no USB HID.
- No change to the wire protocol, router, hardware_route, display, or desktop app
  behavior, beyond renames forced by the new include names.
- No runtime UI or storage for renaming the device. The library takes the name as a
  start-up setting; making it user-editable is a later layer and the API must not
  preclude it.
- No change to connection counts, advertising timing, security settings, or the
  report descriptor.
- No separate git repository yet (see Approaches).

## Anti-goals (a failure even if everything compiles)

- Any change that makes an already-paired computer forget the device. Bond data
  must keep its current storage: NVS namespace `kvm_bonds`, key `identities`,
  magic `0x4b564d42`, table version 2, plus NimBLE's own persisted bond store.
  Names of these storage locations are not renamed.
- Any regression in reattach after power cycle on macOS.
- Leaving the strings "ESP32 KVM" or the KVM names inside the library.
- Breaking the PC-capture route or the desktop app's ability to pair and select guests.

## Constraints and facts about the code today

- `firmware/components/hid/` (2,397 lines): `hid_report` (report map and per-link output
  gate) and `hid_pairing` (policy, bond table) have no Bluetooth dependency;
  `hid_gatt`, `hid_guest`, `hid_guest_rpc`, `hid_pairing_store`, and
  `hid_gatt_official_diag` are NimBLE-bound.
- Only three files outside `hid/` use it: `components/transport/router_hid_bridge.c`,
  `components/transport/transport_usb_serial_jtag.c`, and `main/main.c`
  (`hid_guest_start`, slot request functions, pairing request/event functions,
  retained-bond inventory/forget).
- Hard-coded identity strings: `hid_guest.c` (advertised and GAP name "ESP32 KVM",
  plus a "KB Test" diagnostic name), `hid_gatt.c` (DIS model "ESP32 KVM S3",
  manufacturer "ESP32 KVM"), `hid_gatt_official_diag.c`.
- Kconfig symbols `KVM_HID_GATT_TRACE` and `KVM_HID_KEYBOARD_ONLY_DIAGNOSTIC` gate
  diagnostics; both default off and are not set in `sdkconfig.defaults`.
- `sdkconfig.defaults` enables NimBLE peripheral role and GATT server, 3 connections,
  8 bonds. The future central role needs different config and connection budget
  (documented in `docs/Designs/hardware-primary/README.md`); not touched here.
- The HID C tests (seven files under `firmware/tests/hid/`, using NimBLE/NVS mocks)
  run on Windows through `firmware/tests/hid/run-msvc.ps1` with MSVC and the Windows
  SDK. CI (`.github/workflows/validation.yml`) does not run them today.
- Rust workspace tests (all passing at baseline) are unaffected by this work.

## Approaches considered

- **A. Standalone component inside this repo (chosen by the owner).** New component
  with its own public API, no KVM references, tests moved with it, extractable to its
  own repository later by moving the folder. Trade-off: not independently versioned yet.
- B. Separate repository now. Real reuse immediately, but every API change becomes a
  cross-repo round trip while the API is still moving, and the host-side test runner
  has repo-relative paths.
- C. Leave code in place and only make the name configurable. Cheapest, not a library;
  reuse would be copy and paste.

## Design (items marked "call" are the author's, not the owner's)

Layout (call):

```
firmware/components/doubtech_hid/
  CMakeLists.txt, Kconfig, idf_component.yml
  include/doubtech_hid.h          public API
  core/   report map + per-link gate, pairing policy, bond table
  ble/    NimBLE GATT service, advertising/GAP, host-loop request bridge, bond store
  test/   (or firmware/tests/doubtech_hid/) moved HID tests and mocks
```

Public API (call): `doubtech_hid_start(const doubtech_hid_config_t *)` where the config
carries device name, manufacturer, model, and keeps today's constants as defaults
(appearance, pairing window, link and bond limits come from the existing headers).
The existing slot-addressed request functions (arm, release, ready, keyboard, mouse,
consumer, disconnect, pair begin/cancel/reply, slots snapshot, retained bonds, forget)
keep their semantics and move to the `doubtech_hid_` prefix. The host-loop request
bridge is kept as is because it is the proven serialization path. The pairing event
callback type moves with it.

Naming (call): component and prefix `doubtech_hid`; Kconfig diagnostics renamed to
`DOUBTECH_HID_*` with the same defaults. The product default name "DoubTech HID" lives
in the KVM firmware's start-up call, not in the library.

Rewiring (call): `router_hid_bridge.c`, `transport_usb_serial_jtag.c` and `main.c`
change includes/prefixes and pass the config. Their `REQUIRES` swap `hid` for
`doubtech_hid`. The old `hid` component is removed once nothing references it.

Method (call): move first, then change. Step 1 relocates files and runs the existing
tests unchanged. Step 2 introduces the config struct and removes KVM strings. Step 3
rewires KVM firmware and builds. Each step keeps the tests green.

Verification: the seven HID C tests before and after the move; a full ESP-IDF firmware
build of the KVM project; Rust workspace tests; then on hardware with a Mac, confirm
(a) an already-paired Mac reattaches after power cycle with no re-pairing, (b) a fresh
pairing shows "DoubTech HID", (c) PC-capture route still switches to a guest. Because
macOS caches names, an already-paired Mac may keep showing the old name until
re-paired; that is expected and not a bond loss.

Delivery: a second full commit, annotated tag `checkpoint/hid-library`, and push once
the port is complete and verified, mirroring the baseline checkpoint.

## Resolutions from design review (all are the author's calls)

Name length (call). The advertising payload is flags (3) + HID UUID (4) + appearance (4)
+ name header (2) = 13 bytes of overhead, so within the 31-byte payload a name may be at most 18 bytes.
"DoubTech HID" is 12 bytes (25 total, currently 22 for "ESP32 KVM"). The library rejects a longer name
at start-up with an error. It never truncates and never moves the name to the scan
response, so advertising content stays as it is today. A test covers the limit.

Rename sequencing and priority (call). Reattach on macOS outranks the new name. The
port lands first with names configurable; switching the KVM firmware's default from
"ESP32 KVM" to "DoubTech HID" is its own final, independently revertible commit. The
owner runs the Mac check (reattach after power cycle without re-pairing; a fresh
pairing shows "DoubTech HID") on a build with that commit. If it fails, that commit is
reverted and the library still ships with a configurable name. The tag
`checkpoint/hid-library` is applied after the check passes. The owner and their Mac are
part of the plan for this step. Supporting evidence: the code already notes that macOS
keys cached GATT data by the peripheral's identity, and a rename leaves the identity
address unchanged.

Storage names (call). The bond-table namespace `kvm_bonds`, key `identities`, magic and
version stay inside the library as documented legacy-compatible defaults, so a caller
cannot lose bonds by passing a wrong value. The config struct has an optional override
so a future product can use its own namespace. NimBLE's own bond store is untouched.

"No KVM names" rule (call). After step 2, a case-insensitive search for "kvm" in the
library must return only an allow-list: the legacy storage constants above and the
copyright/license headers. Log tags, include guards, Kconfig symbols and comments (for
example the "protocol v1 STATUS" wording in `hid_pairing.h`) are renamed or reworded.
Copyright headers keep "ESP32 KVM contributors" because the repository's root MIT
license uses that holder; changing attribution is the owner's decision, not part of
this port.

## Left to the builder

File-by-file move order, how mocks are shared between the old and new test layouts,
exact config struct field names and defaults handling, ESP-IDF component manifest
details, and whether the MSVC runner is generalized or duplicated. If the ESP-IDF
toolchain is not available on this machine, the build step is reported as not run
rather than assumed.
