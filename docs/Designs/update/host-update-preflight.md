# Host update preflight and recovery model

`esp32-kvm-update-core` provides a pure host-side preflight and release-first
state machine. It does not open USB, invoke a flash tool, alter bonds or local
profiles, or implement a bootloader. The production adapter must supply a
verified device identity, current app-partition capacity, firmware image bytes,
control replies, and the result of an app-only flash.

## Release manifest v1

The release artifact has one UTF-8 JSON manifest of at most 4,096 bytes. All
fields are required; unknown or duplicate keys are rejected. Example for the
three-byte test image `abc`:

```json
{
  "schema": 1,
  "board_id": "esp32-kvm-s3",
  "protocol_major": 1,
  "protocol_minor_min": 0,
  "protocol_minor_max": 1,
  "firmware_version": "0.2.0",
  "partition": "app",
  "image_size": 3,
  "image_sha256": "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
}
```

`board_id` and `firmware_version` are 1–32 ASCII letters, digits, `.`, `_`, or
`-`. The board ID must match the verified connected board; the current product
ID is `esp32-kvm-s3`. The connected major version must match and its minor
version must fall within the inclusive range. `partition` is exactly `app`.
`image_size` is nonzero, at most 8 MiB, and at most the independently verified
app-partition capacity. It must equal the supplied image byte count.
`image_sha256` is 64 lowercase hexadecimal characters and must match those
bytes. The SHA-256 check detects corruption; this schema does not authenticate
a publisher. Distribution and firmware signing remain separate requirements.

The manifest cannot request a full-chip flash, NVS erase, bond wipe, automatic
guest arm, or profile deletion. A future schema version requires a new parser
and review, rather than permissive extension of v1.

## Transaction

1. Preflight manifest, image, board identity, protocol, and app capacity.
2. Disarm local capture and send `RELEASE_ALL` in the verified session. An
   exact ACK must identify that command, sequence, session, and resulting route
   generation. An older ACK is ignored; a future or malformed current ACK,
   NACK, or timeout requires recovery.
3. Send `UPDATE_PREPARE` with the advanced generation. Require its exact ACK.
   It places firmware in released/suspended state; the USB protocol does not
   itself flash the image.
4. Recheck the original image bytes against the manifest hash immediately
   before an external flasher writes **only the app partition**. A failed or
   interrupted flash enters `RecoveryRequired`; there is no automatic retry or
   arm. The adapter must not erase NVS.
5. On flash success, wait for USB re-enumeration and a fresh verified session.
   Accept completion only when board ID, protocol, and expected firmware
   version match and STATUS reports local/disarmed. Keep input local after
   completion; ordinary explicit routing may resume later.

Control retries, if the transport adapter uses them, must retain the same
session and sequence and follow the USB protocol's bounded retry rule. A lost
connection during control or flash remains in recovery state. The physical
BOOT/manual recovery sequence and real app flash compatibility are native
hardware gates owned by the root integration task; this source model does not
claim they are proven.

## Verification

`cargo test --offline -p esp32-kvm-update-core` covers strict parsing,
identity/capacity/digest checks, exact release/prepare order, stale and future
responses, NACK/timeout, changed image bytes, and disarmed reconnect. Firmware
`UPDATE_PREPARE` support, the host actor/flasher adapter, USB re-enumeration,
preserved BLE bonds, interrupted-flash recovery, and physical BOOT recovery
still require integration and native tests.
