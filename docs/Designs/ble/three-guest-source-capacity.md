# Three-link HID source capacity

The NimBLE HID peripheral now has three connection channels behind one composite GATT service and one BLE identity. Each channel owns its connection handle, encryption and subscription state, protocol mode, held reports, and fail-local disconnect flag. GATT reads and writes resolve the requesting connection handle; notifications use that handle rather than a broadcast. A fourth simultaneous connection is refused. Advertising resumes after an accepted connection while a free channel remains. The retained bond table remains bounded at eight opaque tokens.

`firmware/sdkconfig.defaults` requests three NimBLE connections, 24 CCCD records, and four controller BLE activities (three connections plus an advertising activity). These are source settings for ESP-IDF 5.5.1 on ESP32-S3. Existing generated `sdkconfig` files must be reconfigured so the defaults take effect before a native build. The controller's actual memory use, negotiated connection intervals, and simultaneous operation are not yet measured.

The USB protocol still advertises `CAPS.max_connections = 1`, reports one live STATUS slot, and routes input through `hid_gatt_channel()` (the first channel). Other connected guests remain disarmed. This is deliberately a partial capability: the host cannot select three guests yet. Follow-up task `T31a140d2b3092` owns the versioned multi-slot CAPS/STATUS contract, slot-addressed routing and HID RPC, and actor/UI support.

Bond removal compares the target token's retained peer identity with each connected peer before terminating a link. It must leave unrelated channels connected. Current NimBLE bond deletion and application NVS save remain separate operations, so a storage failure can leave the two stores temporarily inconsistent; production recovery of this failure requires its own verification.

## Gates before claiming three usable guests

1. Build and inspect the resolved ESP-IDF `sdkconfig` for three NimBLE connections, 24 CCCDs, and four controller activities. Run the integrated native link gate.
2. On the physical board, connect three authenticated bonded guests together. Confirm a fourth is rejected, advertising resumes after each of the first two connections, and all three survive ordinary interval negotiation.
3. Subscribe each guest independently and verify encrypted GATT reads, LED writes, keyboard/mouse/consumer notifications, disconnection, and reconnect. Observe that no report reaches an unintended peer, including when a notify or release fails.
4. Complete `T31a140d2b3092` and prove switching, lease expiry, local emergency recovery, and opaque token status across three selectable slots. Until then, CAPS and route capacity stay at one.

Portable source evidence: `firmware/tests/hid/run-msvc.ps1` covers channel admission, CCCD/encryption/report isolation, first-link advertising, and unrelated-peer bond removal. It does not establish radio or guest OS behavior.
