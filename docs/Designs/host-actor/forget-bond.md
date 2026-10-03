# Verified bond removal

`HostActor::forget_bond([u8; 16], now_ms)` runs on the dedicated native actor worker and uses its existing verified USB session. The desktop bridge decodes the user-confirmed 32-character hex token before calling it; no second serial owner is created.

The actor accepts the request only while locally routed, idle, and the token appears in the latest verified STATUS. It sends protocol `FORGET_BOND` (`0x42`) as canonical CBOR `{1: bytes16}`. The existing control machinery checks the ACK session, sequence, original kind, and generation, with at most two retries. After an exact ACK, the actor requests STATUS and returns success only when a newly processed STATUS omits the token. It then removes its cached mapping profile. A NACK, malformed or mismatched ACK, transport failure, timeout, or unchanged bond list never reports success; the desktop must retain its local profile. An ambiguous session fault returns to local control and requires a fresh handshake.

The synchronous call can occupy the native worker for up to the control/peer timeout window (about 500 ms); it must not run on the UI thread. The bridge should take a new `setup_snapshot()` after success so guest profiles reflect the firmware bond list.

Source tests use a scripted fake serial peer for exact ACK plus fresh STATUS, unknown/nonlocal rejection, stale and wrong ACKs, unchanged STATUS, missing ACK timeout, and USB EOF. A live firmware test remains pending. The current firmware tree defines the wire schema and `hid_guest_pairing_forget` but has no USB router dispatch for `FORGET_BOND`; until that handler is added, the verified host API cannot succeed on hardware.
