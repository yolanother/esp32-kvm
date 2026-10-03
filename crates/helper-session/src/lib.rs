// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Gates optional seamless-helper edge telemetry against explicit trust, BLE profile identity,
// a verified encrypted channel, session freshness, and the authoritative routing generation.
// This pure policy crate neither opens sockets nor stores credentials or routes HID input.

#![forbid(unsafe_code)]

pub mod enrollment;
pub mod wire;

/// Maximum accepted age of edge telemetry in milliseconds.
pub const MAX_TELEMETRY_AGE_MS: u64 = 250;
/// Maximum accepted remote clock lead in milliseconds.
pub const MAX_FUTURE_SKEW_MS: u64 = 100;

/// A profile-to-helper association loaded from trusted, user-approved storage.
#[derive(Clone, Copy, Eq, PartialEq)]
pub struct TrustedBinding {
    profile_id: u128,
    bond_token: [u8; 16],
    helper_identity: [u8; 32],
    min_version: u16,
    max_version: u16,
}

impl std::fmt::Debug for TrustedBinding {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("TrustedBinding")
            .field("profile_id", &self.profile_id)
            .field("bond_token", &"<redacted>")
            .field("helper_identity", &self.helper_identity)
            .field("min_version", &self.min_version)
            .field("max_version", &self.max_version)
            .finish()
    }
}

impl TrustedBinding {
    /// Validates a persisted association. The caller must establish user approval and storage provenance.
    pub fn new(
        profile_id: u128,
        bond_token: [u8; 16],
        helper_identity: [u8; 32],
        min_version: u16,
        max_version: u16,
    ) -> Result<Self, BindingError> {
        if profile_id == 0 || bond_token == [0; 16] {
            return Err(BindingError::MissingBond);
        }
        if helper_identity == [0; 32] {
            return Err(BindingError::MissingHelperIdentity);
        }
        if min_version == 0 || min_version > max_version {
            return Err(BindingError::InvalidVersionRange);
        }
        Ok(Self {
            profile_id,
            bond_token,
            helper_identity,
            min_version,
            max_version,
        })
    }

    pub(crate) fn helper_identity(&self) -> [u8; 32] {
        self.helper_identity
    }

    pub(crate) fn min_version(&self) -> u16 {
        self.min_version
    }
}

/// Why a persisted helper association cannot be used.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum BindingError {
    /// Profile identity or BLE bond token is missing.
    MissingBond,
    /// No approved helper identity was stored.
    MissingHelperIdentity,
    /// The supported protocol range is empty or includes version zero.
    InvalidVersionRange,
}

/// Adapter contract for a mutually authenticated, encrypted stream.
///
/// Production implementations must derive these values from a completed TLS handshake,
/// including certificate or pinned-key verification. Discovery packets and plain UDP
/// cannot implement this contract. The profile's stored private key belongs in OS
/// credential storage; this crate receives only a public identity fingerprint.
pub trait AuthenticatedChannel {
    /// Whether encryption and peer verification both completed successfully.
    fn is_encrypted_and_peer_verified(&self) -> bool;
    /// The verified peer's stable public-key fingerprint.
    fn peer_identity(&self) -> [u8; 32];
    /// Negotiated helper protocol version, after transport handshake validation.
    fn protocol_version(&self) -> u16;
    /// Fresh, nonzero identifier for this connection, derived from the secure handshake.
    fn session_id(&self) -> [u8; 16];
}

/// Current selection from the authoritative routing actor.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Route {
    /// Stable guest profile identity, never a slot number or display name.
    pub profile_id: u128,
    /// Firmware-acknowledged routing generation.
    pub generation: u32,
    /// Whether this guest is the active recipient and routing is armed.
    pub active: bool,
}

/// Untrusted helper edge intent after decoding a bounded encrypted frame.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct EdgeTelemetry {
    /// Secure-channel session identifier echoed by the helper.
    pub session_id: [u8; 16],
    /// Strictly increasing sequence number within this session.
    pub sequence: u64,
    /// Guest monotonic timestamp translated to the host clock during handshake.
    pub sent_at_ms: u64,
    /// Routing generation observed when the helper produced this event.
    pub route_generation: u32,
    /// True if any physical guest pointer button was held.
    pub buttons_down: bool,
}

/// Why helper admission or an edge event failed closed.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Reject {
    /// No binding, verified encrypted channel, or fresh session ID.
    Untrusted,
    /// The authenticated helper identity differs from the approved identity.
    WrongHelper,
    /// The current BLE bond is not the bond associated with this profile.
    WrongBond,
    /// Negotiated helper protocol is outside the approved range.
    IncompatibleVersion,
    /// The helper stream was closed or its trust was revoked.
    Closed,
    /// An event belongs to a previous or different secure connection.
    WrongSession,
    /// No matching active, armed guest routing generation exists.
    WrongRoute,
    /// The event is older than the accepted freshness window.
    Stale,
    /// The event timestamp is implausibly ahead of the host clock.
    Future,
    /// The event sequence or timestamp was already observed.
    Replay,
    /// Automatic crossing is forbidden during a physical drag.
    ButtonsHeld,
}

/// An admitted helper connection and its anti-replay state.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct HelperSession {
    profile_id: u128,
    session_id: [u8; 16],
    protocol_version: u16,
    last_sequence: Option<u64>,
    last_timestamp_ms: Option<u64>,
    open: bool,
}

impl HelperSession {
    /// Admits only the explicitly bound BLE guest on a verified encrypted helper channel.
    pub fn open(
        binding: Option<&TrustedBinding>,
        current_bond_token: [u8; 16],
        channel: &impl AuthenticatedChannel,
    ) -> Result<Self, Reject> {
        let binding = binding.ok_or(Reject::Untrusted)?;
        if !channel.is_encrypted_and_peer_verified() || channel.session_id() == [0; 16] {
            return Err(Reject::Untrusted);
        }
        if current_bond_token != binding.bond_token {
            return Err(Reject::WrongBond);
        }
        if channel.peer_identity() != binding.helper_identity {
            return Err(Reject::WrongHelper);
        }
        if !(binding.min_version..=binding.max_version).contains(&channel.protocol_version()) {
            return Err(Reject::IncompatibleVersion);
        }
        Ok(Self {
            profile_id: binding.profile_id,
            session_id: channel.session_id(),
            protocol_version: channel.protocol_version(),
            last_sequence: None,
            last_timestamp_ms: None,
            open: true,
        })
    }

    /// Closes the session on link loss, revocation, or a failed secure-channel check.
    pub fn close(&mut self) {
        self.open = false;
    }

    /// Checks one edge intent; rejection never authorizes a route change.
    pub fn accept(
        &mut self,
        event: EdgeTelemetry,
        route: Route,
        now_ms: u64,
    ) -> Result<(), Reject> {
        if !self.open {
            return Err(Reject::Closed);
        }
        if event.session_id != self.session_id {
            return Err(Reject::WrongSession);
        }
        if !route.active
            || route.profile_id != self.profile_id
            || route.generation != event.route_generation
        {
            return Err(Reject::WrongRoute);
        }
        if event.sent_at_ms > now_ms.saturating_add(MAX_FUTURE_SKEW_MS) {
            return Err(Reject::Future);
        }
        if now_ms.saturating_sub(event.sent_at_ms) > MAX_TELEMETRY_AGE_MS {
            return Err(Reject::Stale);
        }
        if self
            .last_sequence
            .is_some_and(|last| event.sequence <= last)
            || self
                .last_timestamp_ms
                .is_some_and(|last| event.sent_at_ms <= last)
        {
            return Err(Reject::Replay);
        }
        if event.buttons_down {
            return Err(Reject::ButtonsHeld);
        }
        self.last_sequence = Some(event.sequence);
        self.last_timestamp_ms = Some(event.sent_at_ms);
        Ok(())
    }
}
