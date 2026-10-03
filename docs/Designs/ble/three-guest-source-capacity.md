# Three-link HID source capacity

The NimBLE HID peripheral now has three connection channels behind one composite GATT service and one BLE identity. Each channel owns its connection handle, encryption and subscription state, protocol mode, held reports, and fail-local disconnect flag. GATT reads and writes resolve the requesting connection handle; notifications use that handle rather than a broadcast. A fourth simultaneous connection is refused. Advertising resumes after an accepted connection while a free channel remains. The retained bond table remains bounded at eight opaque tokens.

`firmware/sdkconfig.defaults` requests three NimBLE connections, 24 CCCD records, and four controller BLE activities (three connections plus an advertising activity). These are source settings for ESP-IDF 5.5.1 on ESP32-S3. Existing generated `sdkconfig` files must be reconfigured so the defaults take effect before a native build. The controller's actual memory use, negotiated connection intervals, and simultaneous operation are not yet measured.

The integrated trial firmware now advertises `CAPS.max_connections = 3`, reports three live STATUS slots, and routes through slot-addressed HID RPCs. The host actor includes all advertised slots in its default selection order. Only one slot is armed for input at a time. The device screen counts ready guests and uses a separate readiness bit mask for guest rows; PLUS cycles among ready slots and Local. This enables a physical three-guest trial, but does not establish interoperability or notification isolation on three real guests.

Bond removal compares the target token's retained peer identity with each connected peer before terminating a link. It must leave unrelated channels connected. Current NimBLE bond deletion and application NVS save remain separate operations, so a storage failure can leave the two stores temporarily inconsistent; production recovery of this failure requires its own verification.

## Gates before claiming three usable guests

1. Build and inspect the resolved ESP-IDF `sdkconfig` for three NimBLE connections, 24 CCCDs, and four controller activities. Run the integrated native link gate.
2. On the physical board, connect three authenticated bonded guests together. Confirm a fourth is rejected, advertising resumes after each of the first two connections, and all three survive ordinary interval negotiation.
3. Subscribe each guest independently and verify encrypted GATT reads, LED writes, keyboard/mouse/consumer notifications, disconnection, and reconnect. Observe that no report reaches an unintended peer, including when a notify or release fails.
4. Prove switching, lease expiry, local emergency recovery, and opaque token status across three selectable slots. Keep the three-slot build in trial status until these physical checks pass.

Portable source evidence: `firmware/tests/hid/run-msvc.ps1` covers channel admission, CCCD/encryption/report isolation, first-link advertising, and unrelated-peer bond removal. It does not establish radio or guest OS behavior.
