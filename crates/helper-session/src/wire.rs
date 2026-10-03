// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Encodes and decodes one fixed-size helper telemetry frame with strict size and header checks.
// Decoding is allocation-free; callers must still admit frames through an authenticated session.

use crate::{EdgeTelemetry, HelperSession, Reject, Route};

/// Exact v1 telemetry frame size. Stream adapters must bound reads to this value.
pub const FRAME_LEN: usize = 45;
/// Helper protocol version encoded by this frame codec.
pub const FRAME_VERSION: u16 = 1;
const MAGIC: &[u8; 4] = b"HKV1";
const VERSION: u8 = 1;
const TELEMETRY: u8 = 1;

/// Why an untrusted helper frame cannot be decoded.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum WireError {
    /// Frame is truncated or exceeds the only supported size.
    Length,
    /// Magic, version, type, or declared size is unsupported.
    Header,
    /// A field has a noncanonical representation.
    Field,
}

/// Why a decoded frame could not be admitted by the verified session.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum FrameReject {
    /// Frame failed bounded canonical decoding.
    Wire(WireError),
    /// Negotiated helper version differs from this codec's version.
    Version,
    /// Session freshness, route, or replay policy rejected the frame.
    Policy(Reject),
}

/// Encodes one edge event in a deterministic fixed-size v1 frame.
pub fn encode_telemetry(event: EdgeTelemetry) -> [u8; FRAME_LEN] {
    let mut frame = [0; FRAME_LEN];
    frame[..4].copy_from_slice(MAGIC);
    frame[4] = VERSION;
    frame[5] = TELEMETRY;
    frame[6..8].copy_from_slice(&(FRAME_LEN as u16).to_le_bytes());
    frame[8..24].copy_from_slice(&event.session_id);
    frame[24..32].copy_from_slice(&event.sequence.to_le_bytes());
    frame[32..40].copy_from_slice(&event.sent_at_ms.to_le_bytes());
    frame[40..44].copy_from_slice(&event.route_generation.to_le_bytes());
    frame[44] = u8::from(event.buttons_down);
    frame
}

/// Decodes one exact v1 frame without trusting it to select a route or authorize input.
pub fn decode_telemetry(frame: &[u8]) -> Result<EdgeTelemetry, WireError> {
    if frame.len() != FRAME_LEN {
        return Err(WireError::Length);
    }
    if &frame[..4] != MAGIC
        || frame[4] != VERSION
        || frame[5] != TELEMETRY
        || u16::from_le_bytes([frame[6], frame[7]]) != FRAME_LEN as u16
    {
        return Err(WireError::Header);
    }
    if frame[44] > 1 {
        return Err(WireError::Field);
    }
    let mut session_id = [0; 16];
    session_id.copy_from_slice(&frame[8..24]);
    Ok(EdgeTelemetry {
        session_id,
        sequence: u64::from_le_bytes(frame[24..32].try_into().expect("fixed frame")),
        sent_at_ms: u64::from_le_bytes(frame[32..40].try_into().expect("fixed frame")),
        route_generation: u32::from_le_bytes(frame[40..44].try_into().expect("fixed frame")),
        buttons_down: frame[44] == 1,
    })
}

impl HelperSession {
    /// Decodes and admits a bounded frame through the verified session and current route.
    pub fn accept_frame(
        &mut self,
        frame: &[u8],
        route: Route,
        now_ms: u64,
    ) -> Result<(), FrameReject> {
        let event = decode_telemetry(frame).map_err(FrameReject::Wire)?;
        if self.protocol_version != FRAME_VERSION {
            return Err(FrameReject::Version);
        }
        self.accept(event, route, now_ms)
            .map_err(FrameReject::Policy)
    }
}
