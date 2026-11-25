# Three-link HID source capacity

The NimBLE HID peripheral now has three connection channels behind one composite GATT service and one BLE identity. Each channel owns its connection handle, encryption and subscription state, protocol mode, held reports, and fail-local disconnect flag. GATT reads and writes resolve the requesting connection handle; notifications use that handle rather than a broadcast. A fourth simultaneous connection is refused. Advertising resumes after an accepted connection while a free channel remains. The retained bond table remains bounded at eight opaque tokens.

`firmware/sdkconfig.defaults` requests three NimBLE connections, 24 CCCD records, and four controller BLE activities (three connections plus an advertising activity). These are source settings for ESP-IDF 5.5.1 on ESP32-S3. Existing generated `sdkconfig` files must be reconfigured so the defaults take effect before a native build. The controller's actual memory use, negotiated connection intervals, and simultaneous operation are not yet measured.

The integrated trial firmware now advertises `CAPS.max_connections = 3`, reports three live STATUS slots, and routes through slot-addressed HID RPCs. The host actor includes all advertised slots in its default selection order. Only one slot is armed for input at a time. The device screen counts ready guests and uses a separate readiness bit mask for guest rows; PLUS cycles among ready slots and Local. This enables a physical three-guest trial, but does not establish interoperability or notification isolation on three real guests.

Bond removal compares the target token's retained peer identity with each connected peer before terminating a link. It must leave unrelated channels connected. Current NimBLE bond deletion and application NVS save remain separate operations, so a storage failure can leave the two stores temporarily inconsistent; production recovery of this failure requires its own verification.

## Connection budget with the keyboard and mouse central (current)

The four-activity figures above are the guests-only budget. The firmware now also runs a BLE central on the same NimBLE host for one board-owned keyboard and one mouse ([central.md](../doubtech-hid/central.md)), so `firmware/sdkconfig.defaults` is sized for both roles:

| Setting | Guests only | With the central | Reason |
|---|---:|---:|---|
| `CONFIG_BT_NIMBLE_ROLE_CENTRAL` | n | y | scan, initiate, GATT client |
| `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` | 3 | 5 | 3 guest links + 1 keyboard + 1 mouse |
| `CONFIG_BT_CTRL_BLE_MAX_ACT` | 4 | 8 | 5 links + advertising + scan/initiate + 1 spare |
| `CONFIG_BT_NIMBLE_MAX_BONDS` | 8 | 12 | one shared store: 8 guests + 4 input identities (only 2 connect at once) |
| `CONFIG_BT_NIMBLE_MAX_CCCDS` | 24 | 24 | only guests subscribe to the board |
| `doubtech_hid_config_t.foreign_bond_reserve` | 0 | 4 | bonds in the shared store that are not guests' |
| Guest bonds (retained table) | 8 | 8 | unchanged; `doubtech_hid_pairing_open` compares the NimBLE store against 8 plus the reserve |
| Persistent input records | none | 2 (NVS `dthid_inputs`) | separate from the guests' `kvm_bonds` |

The ranges were set from the ESP-IDF documentation and the NimBLE sources, not verified against the ESP32-S3 controller on a board. Free heap before and after, scan airtime and guest notify latency while an input device reconnects (risk R3), and three guests plus two input links together are owner-run gates in [the owner checklist](../doubtech-hid/owner-checklist-device-input.md). If the controller cannot sustain five links, the fallback is one input link at a time (keyboard first).

## Gates before claiming three usable guests

1. Build and inspect the resolved ESP-IDF `sdkconfig` for three NimBLE connections, 24 CCCDs, and four controller activities. Run the integrated native link gate.
2. On the physical board, connect three authenticated bonded guests together. Confirm a fourth is rejected, advertising resumes after each of the first two connections, and all three survive ordinary interval negotiation.
3. Subscribe each guest independently and verify encrypted GATT reads, LED writes, keyboard/mouse/consumer notifications, disconnection, and reconnect. Observe that no report reaches an unintended peer, including when a notify or release fails.
4. Prove switching, lease expiry, local emergency recovery, and opaque token status across three selectable slots. Keep the three-slot build in trial status until these physical checks pass.

Portable source evidence: `firmware/components/doubtech_hid/tests/run-msvc.ps1` covers channel admission, CCCD/encryption/report isolation, first-link advertising, and unrelated-peer bond removal. It does not establish radio or guest OS behavior.

The library design, API and test strategy are in [docs/Designs/doubtech-hid](../doubtech-hid/README.md).
