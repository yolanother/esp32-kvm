# Protocol v1 design contract
Status: implementation baseline; verify golden vectors before coding both ends.
The implemented v1 payload keys, ranges, and fixed layouts are recorded in [protocol/schema/v1.md](../../protocol/schema/v1.md); the Rust codec and C constants follow that schema.
Transport: native USB CDC byte stream, binary frames only; logs are typed diagnostics, not unframed text. Use a separate debug interface or bounded protocol diagnostics. Baud setting has no assumed throughput meaning on native CDC.

## Framing
COBS encode each decoded frame, terminate with 0x00. Little-endian fields. Decoded frame: magic u16=0x4B56; major u8=1; kind u8; payloadLen u16; flags u16 (zero in v1); sessionId u64; seq u32; routeGeneration u32; payload[0..512]; CRC32C u32 over header+payload. Header is 24 bytes, maximum decoded frame 540 bytes. CRC32C Castagnoli reflected polynomial 0x82F63B78, init/xorout 0xFFFFFFFF. CRC detects corruption, not authentication.
Discard malformed length/version/CRC/COBS frames without input effects; bounded frame buffer; oversize bytes drained to delimiter. Flush partial data on disconnect. Reject unknown required capabilities. Sequence uses modular u32 ordering inside one session; resession before wrap ambiguity.

## Message vocabulary
0x01 HELLO (session=0): protocol minor u16 and feature bits u32.
0x02 CAPS: firmware version string, board ID, protocol range, maximum connections/bonds, supported report flags; response establishes random nonzero device sessionId.
0x03 SESSION_OPEN: echoes session plus host version/config revision; ACK leaves routing disarmed.
0x10 HEARTBEAT: monotonic host tick u64. Host sends every 100 ms while open; firmware expires at 500 ms using its own clock.
0x11 GET_STATUS; 0x12 STATUS: state, selected slot, per-slot bond identity token/readiness, subscription state, connection interval, errors and firmware generation. Status on changes plus 1 Hz.
0x20 SWITCH: desired slot u8 (0=local, 1..N=guest), expected old generation u32, requested new generation u32. Reliable and idempotent by session+seq.
0x21 RELEASE_ALL: clears all keyboard/button/consumer reports, disarms, advances generation; always accepted in valid session.
0x22 ARM: slot+generation; only after READY and all-up baseline. Firmware rejects stale session/generation.
0x30 KEY_STATE: modifiers u8 + reserved u8=0 + six usage bytes. Full state, not key text.
0x31 POINTER: buttons u8, dx i16, dy i16, wheel i8, pan i8. Saturating/splitting coalescer must preserve totals; never wrap.
0x32 CONSUMER_STATE: usage u16 (0=release).
0x40 PAIR_BEGIN: duration seconds u16 capped at 60; 0x41 PAIR_CANCEL; 0x42 FORGET_BOND (opaque token, explicit UI confirmation); 0x43 PAIR_REPLY (challenge ID and accept/passkey data per negotiated method).
0x50 DEVICE_SELECT_REQUEST: slot/request ID; board asks host to arbitrate. Emergency suspend clears immediately, reports status even if host absent.
0x60 UPDATE_PREPARE: enter released/suspended state; actual flashing is toolchain-specific and negotiated separately.
0x70 ACK; 0x71 NACK: original kind+seq, error code, resulting generation. Errors: VERSION, NOT_READY, STALE_SESSION, STALE_ROUTE, PAUSED, BAD_PAYLOAD, BUSY, UNSUPPORTED.
0x72 INPUT_PROGRESS: highest input sequence accepted into router plus separately tracked last BLE-enqueued sequence; neither means the guest application consumed it.
Minor 1 adds STATUS key 6, a canonical pairing map: state 0 closed, 1 waiting, 2 numeric challenge, 3 rejected, 4 bond capacity full, 5 timeout. Waiting and challenge include remaining milliseconds; challenge also includes a fresh nonzero ID and a value displayed as exactly six digits. The host derives its monotonic deadline from the remaining time, confirms only the currently displayed challenge ID, and discards the value after reply or expiry. The firmware executes pairing controls on NimBLE's host loop and sends pairing events through a bounded USB-worker queue. A queue overflow cancels pairing. Minor 0 hosts may continue routing but pairing controls are unsupported.
Encode variable control payloads as canonical CBOR with integer-key schemas checked into protocol/schema; fixed input payloads use the layouts above. Limit maps/strings/counts as well as frame length. Reject missing required keys; ignore documented optional extension keys on compatible minor versions.

## Reliability
USB itself is ordered; application framing handles boundaries/resets. Control ACK retry: 100 ms up to two retries, same session+seq; cache recent control results. Never blindly retry POINTER deltas. Full keyboard/button states can be resent only in current session/generation; transition/reconnect always resets them.
Bound outgoing input queues; coalesce adjacent motion only within same button state/generation. Preserve press/release ordering. Never drop button/key transitions silently: overflow → RELEASE_ALL/disarm/local error. Drop stale motion older than 50 ms. Firmware tracks BLE congestion; prolonged inability to enqueue input (100 ms design threshold) suspends instead of building a delayed burst. Tune thresholds with measurements.
Host watchdog detects loss of status/control ACK or worker heartbeat; return local, release cursor and show reason. BLE-not-ready input is rejected, not queued for eventual delivery.

## Routing transaction
Firmware serializes release old target, clear its state, install new generation, select slot and ACK while disarmed. Host ARM only when target readiness and release stage are established. For ordinary switching, physically-held keys are suppressed until released; destination receives an all-up baseline first. No stale input may cross generation boundaries.
Every control event from UI, hotkey or board is serialized by one host actor. Local-return/emergency preempts queued selections.
Pairing passkeys are never treated as normal forwarded keystrokes. UI logs metadata and aggregate counts only, never key contents or typed text.

## Required tests
Golden bytes/CRC/COBS for every message; split/coalesced frames; malformed length/CRC; random byte fuzzing; duplicate SWITCH; stale session; stale generation; seq boundaries; unplug in partial frame; queue overflow; heartbeat expiry; delayed ACK; board reset; no replay; delta coalescing conservation; one-target delivery.
