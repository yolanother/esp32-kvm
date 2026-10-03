# Optional helper session contract

The seamless helper is an opt-in M3 capability. Standard BLE input continues without a LAN connection. The helper supplies cursor topology and edge intent; USB to BLE remains the sole keyboard and mouse input path. Losing helper trust or connectivity disables guest-edge portals and matched placement. The host hotkey and emergency local release remain available.

## Trust and transport boundary

`crates/helper-session` is a pure admission and telemetry gate. It does not perform cryptography, discovery, OS credential storage, cursor placement, or a routing transaction. A production adapter must establish a mutually authenticated encrypted stream (for example, TLS 1.3 with pinned public-key identity), derive a fresh session ID from that handshake, and pass only its verified peer fingerprint and negotiated version to `AuthenticatedChannel`. A discovery broadcast, DNS name, IP address, BLE display name, plaintext UDP message, or caller-provided boolean is not proof of identity. The trait is a boundary contract; the fake implementation in tests is not a production adapter.

Enrollment must require an explicit user-visible code comparison or certificate approval on both machines. The host stores the helper's approved public-key fingerprint alongside the stable guest profile ID and the firmware's opaque BLE bond token. The helper stores the approved host identity. Private keys and enrollment secrets belong in each OS credential store, not in profile JSON, logs, firmware NVS export, or this crate. The concrete handshake and secure storage adapters remain separate M3 integration work.

## Session rules

1. Admit a stream only when an approved binding exists, the current BLE bond token matches it, the channel reports completed encryption and peer verification, the verified helper fingerprint matches, and the negotiated protocol version is within the approved range. A new stream needs a fresh nonzero session ID.
2. Only the authoritative routing actor may supply the active profile and firmware-acknowledged generation. The helper cannot select a target by sending its own profile or generation. Edge telemetry must echo the active generation and session ID, carry increasing sequence and translated monotonic timestamp, and be no older than 250 ms or more than 100 ms in the future. These are conservative policy limits, not measured network tolerances.
3. Any physical guest pointer button held blocks automatic crossing. Accepted intent permits the host to *begin* its existing release, switch, placement-ack, arm transaction; it never directly arms firmware or emits HID input.
4. On secure-channel loss, revocation, profile-bond mismatch, helper identity change, version incompatibility, or clock-sync loss, close the session and disable helper portals. A reconnect starts a new session and replay state. Old events do not carry over. The host still supports Standard BLE and local hotkeys.
5. Revocation removes the trusted helper identity through the credential-store owner and closes every session for that profile. Re-enrollment requires fresh explicit approval. A renamed guest, changed IP, or rotated BLE address alone never changes trust binding.

## Integration gates

- Implement and review host and Windows/macOS guest secure-channel adapters with certificate or pinned-key verification, explicit enrollment, OS credential storage, and revocation. Never route unauthenticated UDP commands.
- Bind the approved helper to the current firmware-reported opaque bond token, and invalidate it on firmware forget/re-pair. The host actor must supply generation and active state rather than trusting telemetry fields.
- Implement bounded frame decoding, clock translation, topology/coordinate validation, and placement acknowledgement before resuming input. Force Standard BLE/local fallback on timeout or any mismatch.
- Test Windows/macOS helpers and actual cross-machine network loss, revoked credentials, stale/reordered frames, clock drift, BLE re-pair, local guest mouse movement, rotation/DPI placement, and return to host. This commit proves only the pure source policy tests.

Run the source gate with `cargo test --offline --manifest-path crates/helper-session/Cargo.toml`. The standalone crate has its own workspace marker until the coordinator adds it to the root Cargo workspace and lockfile.
