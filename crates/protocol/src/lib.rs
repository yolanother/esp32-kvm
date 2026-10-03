// Copyright (c) ESP32 KVM contributors. Use is governed by the repository LICENSE.
// This crate defines the version-one USB wire envelope, fixed payload validation,
// stream framing, bounded retained-bond inventory, and duplicate-control
// detection shared by host tooling.

mod cbor;

/// Maximum number of decoded payload bytes in one frame.
pub const MAX_PAYLOAD: usize = 512;
/// Decoded envelope size before the payload and CRC.
pub const HEADER_LEN: usize = 24;
/// Maximum decoded frame size, including CRC.
pub const MAX_FRAME: usize = HEADER_LEN + MAX_PAYLOAD + 4;
/// Maximum encoded frame size including its terminating zero byte.
pub const MAX_ENCODED: usize = MAX_FRAME + (MAX_FRAME / 254) + 2;
/// Version-one envelope magic, transmitted little endian.
pub const MAGIC: u16 = 0x4b56;
/// Current incompatible protocol version.
pub const MAJOR: u8 = 1;
/// Highest compatible minor supported by this implementation.
pub const MINOR: u16 = 2;
/// Highest live HID slot number that minor two can describe.
pub const MAX_LIVE_SLOTS: usize = 3;

/// Retained bond inventory format version for negotiated minor two.
pub const BOND_INVENTORY_VERSION: u8 = 1;
/// Maximum opaque retained bond identities in one inventory response.
pub const MAX_BONDS: usize = 8;

/// Authoritative, bounded retained identities; never BLE addresses or keys.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BondInventory {
    /// Nonzero distinct opaque bond tokens retained by firmware.
    pub tokens: Vec<[u8; 16]>,
}

impl BondInventory {
    /// Validates a retained token snapshot before it crosses the wire.
    pub fn new(tokens: Vec<[u8; 16]>) -> Result<Self, ProtocolError> {
        if tokens.len() > MAX_BONDS
            || tokens.contains(&[0; 16])
            || tokens
                .iter()
                .enumerate()
                .any(|(i, token)| tokens[..i].contains(token))
        {
            return Err(ProtocolError::Payload);
        }
        Ok(Self { tokens })
    }

    /// Encodes format version, count, then contiguous opaque tokens.
    pub fn encode(&self) -> Vec<u8> {
        let mut payload = vec![BOND_INVENTORY_VERSION, self.tokens.len() as u8];
        for token in &self.tokens {
            payload.extend_from_slice(token);
        }
        payload
    }

    /// Decodes an exact, versioned response with at most eight distinct tokens.
    pub fn decode(payload: &[u8]) -> Result<Self, ProtocolError> {
        if payload.first() != Some(&BOND_INVENTORY_VERSION) {
            return Err(ProtocolError::Version);
        }
        let count = *payload.get(1).ok_or(ProtocolError::Payload)? as usize;
        if count > MAX_BONDS || payload.len() != 2 + count * 16 {
            return Err(ProtocolError::Payload);
        }
        let tokens = payload[2..]
            .chunks_exact(16)
            .map(|bytes| bytes.try_into().unwrap())
            .collect();
        Self::new(tokens)
    }
}

/// Wire message kinds frozen for version one.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum MessageKind {
    Hello = 0x01,
    Caps = 0x02,
    SessionOpen = 0x03,
    Heartbeat = 0x10,
    GetStatus = 0x11,
    Status = 0x12,
    Switch = 0x20,
    ReleaseAll = 0x21,
    Arm = 0x22,
    KeyState = 0x30,
    Pointer = 0x31,
    ConsumerState = 0x32,
    PairBegin = 0x40,
    PairCancel = 0x41,
    ForgetBond = 0x42,
    PairReply = 0x43,
    GetBonds = 0x44,
    Bonds = 0x45,
    DeviceSelectRequest = 0x50,
    UpdatePrepare = 0x60,
    Ack = 0x70,
    Nack = 0x71,
    InputProgress = 0x72,
}

impl TryFrom<u8> for MessageKind {
    type Error = ProtocolError;

    fn try_from(value: u8) -> Result<Self, Self::Error> {
        use MessageKind::*;
        Ok(match value {
            0x01 => Hello,
            0x02 => Caps,
            0x03 => SessionOpen,
            0x10 => Heartbeat,
            0x11 => GetStatus,
            0x12 => Status,
            0x20 => Switch,
            0x21 => ReleaseAll,
            0x22 => Arm,
            0x30 => KeyState,
            0x31 => Pointer,
            0x32 => ConsumerState,
            0x40 => PairBegin,
            0x41 => PairCancel,
            0x42 => ForgetBond,
            0x43 => PairReply,
            0x44 => GetBonds,
            0x45 => Bonds,
            0x50 => DeviceSelectRequest,
            0x60 => UpdatePrepare,
            0x70 => Ack,
            0x71 => Nack,
            0x72 => InputProgress,
            _ => return Err(ProtocolError::Kind),
        })
    }
}

/// Reasons a frame or control operation is rejected without input effects.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProtocolError {
    Cobs,
    Length,
    Magic,
    Version,
    Flags,
    Kind,
    Crc,
    Payload,
    StaleSession,
    StaleRoute,
    StaleSequence,
}

/// A decoded USB message, with envelope fields kept separate from its payload.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Frame {
    /// Message vocabulary entry.
    pub kind: MessageKind,
    /// Nonzero session after CAPS, or zero for HELLO.
    pub session_id: u64,
    /// Sequence number scoped to one session.
    pub seq: u32,
    /// Routing generation assigned by firmware.
    pub route_generation: u32,
    /// Message-specific validated wire payload.
    pub payload: Vec<u8>,
}

impl Frame {
    /// Constructs a frame; call `encode` to validate and serialize it.
    pub fn new(
        kind: MessageKind,
        session_id: u64,
        seq: u32,
        route_generation: u32,
        payload: Vec<u8>,
    ) -> Self {
        Self {
            kind,
            session_id,
            seq,
            route_generation,
            payload,
        }
    }

    /// Serializes a checked frame as COBS bytes followed by a zero delimiter.
    pub fn encode(&self) -> Result<Vec<u8>, ProtocolError> {
        validate_payload(self.kind, &self.payload)?;
        validate_session(self.kind, self.session_id)?;
        if self.payload.len() > MAX_PAYLOAD {
            return Err(ProtocolError::Length);
        }
        let mut decoded = Vec::with_capacity(HEADER_LEN + self.payload.len() + 4);
        decoded.extend_from_slice(&MAGIC.to_le_bytes());
        decoded.push(MAJOR);
        decoded.push(self.kind as u8);
        decoded.extend_from_slice(&(self.payload.len() as u16).to_le_bytes());
        decoded.extend_from_slice(&0_u16.to_le_bytes());
        decoded.extend_from_slice(&self.session_id.to_le_bytes());
        decoded.extend_from_slice(&self.seq.to_le_bytes());
        decoded.extend_from_slice(&self.route_generation.to_le_bytes());
        decoded.extend_from_slice(&self.payload);
        decoded.extend_from_slice(&crc32c(&decoded).to_le_bytes());
        let mut encoded = cobs_encode(&decoded);
        encoded.push(0);
        Ok(encoded)
    }

    /// Parses one complete, COBS-decoded frame and checks its envelope and payload.
    pub fn decode_decoded(bytes: &[u8]) -> Result<Self, ProtocolError> {
        if bytes.len() < HEADER_LEN + 4 || bytes.len() > MAX_FRAME {
            return Err(ProtocolError::Length);
        }
        if u16::from_le_bytes([bytes[0], bytes[1]]) != MAGIC {
            return Err(ProtocolError::Magic);
        }
        if bytes[2] != MAJOR {
            return Err(ProtocolError::Version);
        }
        let kind = MessageKind::try_from(bytes[3])?;
        let len = u16::from_le_bytes([bytes[4], bytes[5]]) as usize;
        if len > MAX_PAYLOAD || bytes.len() != HEADER_LEN + len + 4 {
            return Err(ProtocolError::Length);
        }
        if bytes[6] != 0 || bytes[7] != 0 {
            return Err(ProtocolError::Flags);
        }
        let expected = u32::from_le_bytes(bytes[bytes.len() - 4..].try_into().unwrap());
        if crc32c(&bytes[..bytes.len() - 4]) != expected {
            return Err(ProtocolError::Crc);
        }
        let session_id = u64::from_le_bytes(bytes[8..16].try_into().unwrap());
        validate_session(kind, session_id)?;
        let seq = u32::from_le_bytes(bytes[16..20].try_into().unwrap());
        let route_generation = u32::from_le_bytes(bytes[20..24].try_into().unwrap());
        let payload = bytes[24..24 + len].to_vec();
        validate_payload(kind, &payload)?;
        Ok(Self {
            kind,
            session_id,
            seq,
            route_generation,
            payload,
        })
    }
}

/// Validates fixed layouts and version-one CBOR top-level structure.
pub fn validate_payload(kind: MessageKind, payload: &[u8]) -> Result<(), ProtocolError> {
    use MessageKind::*;
    let size = match kind {
        Hello => Some(6),
        Heartbeat => Some(8),
        GetStatus => Some(0),
        Switch => Some(9),
        ReleaseAll => Some(0),
        Arm => Some(5),
        KeyState => Some(8),
        Pointer => Some(7),
        ConsumerState => Some(2),
        PairBegin => Some(2),
        PairCancel => Some(0),
        GetBonds => Some(1),
        DeviceSelectRequest => Some(5),
        UpdatePrepare => Some(0),
        Ack | Nack => Some(10),
        InputProgress => Some(8),
        Caps | SessionOpen | Status | ForgetBond | PairReply | Bonds => None,
    };
    if let Some(size) = size {
        if payload.len() != size {
            return Err(ProtocolError::Payload);
        }
    } else if kind == Bonds {
        BondInventory::decode(payload)?;
    } else {
        cbor::validate(kind, payload)?;
    }
    match kind {
        Hello
            if u16::from_le_bytes(payload[..2].try_into().unwrap()) > MINOR
                || payload[2..] != [0, 0, 0, 0] =>
        {
            return Err(ProtocolError::Version);
        }
        KeyState if payload[1] != 0 => return Err(ProtocolError::Payload),
        PairBegin if !(1..=60).contains(&u16::from_le_bytes(payload.try_into().unwrap())) => {
            return Err(ProtocolError::Payload);
        }
        GetBonds if payload[0] != BOND_INVENTORY_VERSION => return Err(ProtocolError::Version),
        Switch => {
            let old = u32::from_le_bytes(payload[1..5].try_into().unwrap());
            let new = u32::from_le_bytes(payload[5..9].try_into().unwrap());
            if payload[0] > 3 || old == new {
                return Err(ProtocolError::Payload);
            }
        }
        Arm if payload[0] > 3 => return Err(ProtocolError::Payload),
        DeviceSelectRequest if payload[0] > 3 => return Err(ProtocolError::Payload),
        Ack if payload[5] != 0 => return Err(ProtocolError::Payload),
        Nack if !(1..=8).contains(&payload[5]) => return Err(ProtocolError::Payload),
        _ => {}
    }
    Ok(())
}

fn validate_session(kind: MessageKind, session: u64) -> Result<(), ProtocolError> {
    if (kind == MessageKind::Hello && session != 0) || (kind != MessageKind::Hello && session == 0)
    {
        return Err(ProtocolError::StaleSession);
    }
    Ok(())
}

/// CRC32C/Castagnoli, reflected, initial and final XOR of all one bits.
pub fn crc32c(bytes: &[u8]) -> u32 {
    let mut crc = !0_u32;
    for &byte in bytes {
        crc ^= u32::from(byte);
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0x82f63b78 & (0_u32.wrapping_sub(crc & 1)));
        }
    }
    !crc
}

/// COBS-encodes arbitrary bytes without the stream delimiter.
pub fn cobs_encode(bytes: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(bytes.len() + bytes.len() / 254 + 1);
    let mut code_at = 0;
    out.push(0);
    let mut code = 1_u8;
    for &byte in bytes {
        if byte == 0 {
            out[code_at] = code;
            code_at = out.len();
            out.push(0);
            code = 1;
        } else {
            out.push(byte);
            code += 1;
            if code == 0xff {
                out[code_at] = code;
                code_at = out.len();
                out.push(0);
                code = 1;
            }
        }
    }
    out[code_at] = code;
    out
}

/// COBS-decodes one delimited frame body; the delimiter is not included.
pub fn cobs_decode(bytes: &[u8]) -> Result<Vec<u8>, ProtocolError> {
    if bytes.is_empty() {
        return Err(ProtocolError::Cobs);
    }
    let mut out = Vec::with_capacity(bytes.len());
    let mut index = 0;
    while index < bytes.len() {
        let code = bytes[index] as usize;
        if code == 0 || index + code > bytes.len() + 1 {
            return Err(ProtocolError::Cobs);
        }
        index += 1;
        let count = code - 1;
        if index + count > bytes.len() {
            return Err(ProtocolError::Cobs);
        }
        out.extend_from_slice(&bytes[index..index + count]);
        index += count;
        if code != 0xff && index < bytes.len() {
            out.push(0);
        }
        if out.len() > MAX_FRAME {
            return Err(ProtocolError::Length);
        }
    }
    Ok(out)
}

/// Bounded byte-stream decoder that discards overflow through the next delimiter.
#[derive(Default)]
pub struct FrameDecoder {
    bytes: Vec<u8>,
    draining: bool,
}

impl FrameDecoder {
    /// Creates a decoder with no buffered frame.
    pub fn new() -> Self {
        Self::default()
    }

    /// Drops a partial frame on disconnect or session reset.
    pub fn reset(&mut self) {
        self.bytes.clear();
        self.draining = false;
    }

    /// Accepts one byte and returns a frame result at each delimiter.
    pub fn push(&mut self, byte: u8) -> Option<Result<Frame, ProtocolError>> {
        if byte != 0 {
            if self.draining {
                return None;
            }
            if self.bytes.len() >= MAX_ENCODED - 1 {
                self.bytes.clear();
                self.draining = true;
                return None;
            }
            self.bytes.push(byte);
            return None;
        }
        if self.draining {
            self.reset();
            return Some(Err(ProtocolError::Length));
        }
        if self.bytes.is_empty() {
            return None;
        }
        let result = cobs_decode(&self.bytes).and_then(|decoded| Frame::decode_decoded(&decoded));
        self.reset();
        Some(result)
    }
}

/// Recent control identifiers used to execute a retry only once per session.
pub struct RetryCache {
    session: Option<u64>,
    entries: Vec<(u32, MessageKind)>,
    newest: Option<u32>,
    capacity: usize,
}

impl RetryCache {
    /// Creates a cache retaining up to `capacity` control results.
    pub fn new(capacity: usize) -> Self {
        Self {
            session: None,
            entries: Vec::new(),
            newest: None,
            capacity,
        }
    }

    /// Returns true for a new control, false for a duplicate, or a stale error.
    pub fn observe(
        &mut self,
        session: u64,
        seq: u32,
        kind: MessageKind,
        expected_generation: u32,
        current_generation: u32,
    ) -> Result<bool, ProtocolError> {
        if session == 0 {
            return Err(ProtocolError::StaleSession);
        }
        match self.session {
            None => self.session = Some(session),
            Some(active) if active != session => return Err(ProtocolError::StaleSession),
            _ => {}
        }
        if let Some((_, original_kind)) = self.entries.iter().find(|(seen, _)| *seen == seq) {
            return if *original_kind == kind {
                Ok(false)
            } else {
                Err(ProtocolError::StaleSequence)
            };
        }
        if self.newest.is_some_and(|newest| {
            let distance = seq.wrapping_sub(newest);
            distance == 0 || distance >= 0x8000_0000
        }) {
            return Err(ProtocolError::StaleSequence);
        }
        if expected_generation != current_generation {
            return Err(ProtocolError::StaleRoute);
        }
        self.newest = Some(seq);
        if self.capacity > 0 {
            if self.entries.len() == self.capacity {
                self.entries.remove(0);
            }
            self.entries.push((seq, kind));
        }
        Ok(true)
    }

    /// Starts a newly negotiated session with no prior retry results.
    pub fn reset(&mut self, session: u64) {
        self.session = Some(session);
        self.entries.clear();
        self.newest = None;
    }
}
