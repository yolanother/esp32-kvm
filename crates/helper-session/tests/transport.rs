// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Verifies bounded helper telemetry decoding and explicit two-sided enrollment approval.
// These source tests use fake secure-channel evidence and never open a network connection.

use esp32_kvm_helper_session::{
    AuthenticatedChannel, EdgeTelemetry, HelperSession, Reject, Route, TrustedBinding,
    enrollment::{Enrollment, EnrollmentError, TrustedCredentialStore},
    wire::{FRAME_LEN, FrameReject, WireError, decode_telemetry, encode_telemetry},
};

const BOND: [u8; 16] = [7; 16];
const IDENTITY: [u8; 32] = [9; 32];
const SESSION: [u8; 16] = [3; 16];

struct Channel {
    verified: bool,
    identity: [u8; 32],
    version: u16,
}

impl AuthenticatedChannel for Channel {
    fn is_encrypted_and_peer_verified(&self) -> bool {
        self.verified
    }
    fn peer_identity(&self) -> [u8; 32] {
        self.identity
    }
    fn protocol_version(&self) -> u16 {
        self.version
    }
    fn session_id(&self) -> [u8; 16] {
        SESSION
    }
}

#[derive(Default)]
struct Store {
    binding: Option<TrustedBinding>,
    revoked: bool,
}

impl TrustedCredentialStore for Store {
    type Error = ();
    fn save(&mut self, binding: TrustedBinding) -> Result<(), Self::Error> {
        self.binding = Some(binding);
        Ok(())
    }
    fn revoke(&mut self, _profile_id: u128) -> Result<(), Self::Error> {
        self.binding = None;
        self.revoked = true;
        Ok(())
    }
}

fn channel() -> Channel {
    Channel {
        verified: true,
        identity: IDENTITY,
        version: 1,
    }
}

fn event() -> EdgeTelemetry {
    EdgeTelemetry {
        session_id: SESSION,
        sequence: 1,
        sent_at_ms: 1_000,
        route_generation: 7,
        buttons_down: false,
    }
}

#[test]
fn wire_is_fixed_size_and_rejects_mutated_header_length_type_and_bool() {
    let frame = encode_telemetry(event());
    assert_eq!(frame.len(), FRAME_LEN);
    assert_eq!(decode_telemetry(&frame), Ok(event()));
    for size in [0, 1, FRAME_LEN - 1, FRAME_LEN + 1, 4096] {
        assert_eq!(decode_telemetry(&vec![0; size]), Err(WireError::Length));
    }
    for (offset, value) in [(0, b'X'), (4, 2), (5, 2), (6, 0), (7, 1), (44, 2)] {
        let mut altered = frame;
        altered[offset] = value;
        assert!(decode_telemetry(&altered).is_err(), "offset {offset}");
    }
}

#[test]
fn decoded_wire_still_requires_verified_session_route_and_freshness() {
    let binding = TrustedBinding::new(42, BOND, IDENTITY, 1, 1).unwrap();
    let mut session = HelperSession::open(Some(&binding), BOND, &channel()).unwrap();
    let route = Route {
        profile_id: 42,
        generation: 7,
        active: true,
    };
    let frame = encode_telemetry(event());
    let decoded = decode_telemetry(&frame).unwrap();
    assert_eq!(session.accept_frame(&frame, route, 1_001), Ok(()));
    assert_eq!(session.accept(decoded, route, 1_002), Err(Reject::Replay));
    let mut next = event();
    next.sequence = 2;
    next.sent_at_ms = 1_001;
    assert_eq!(
        session.accept(
            decode_telemetry(&encode_telemetry(next)).unwrap(),
            route,
            1_300
        ),
        Err(Reject::Stale)
    );
}

#[test]
fn wire_admission_rejects_a_negotiated_version_mismatch() {
    let binding = TrustedBinding::new(42, BOND, IDENTITY, 1, 2).unwrap();
    let mut channel = channel();
    channel.version = 2;
    let mut session = HelperSession::open(Some(&binding), BOND, &channel).unwrap();
    let route = Route {
        profile_id: 42,
        generation: 7,
        active: true,
    };
    let frame = encode_telemetry(event());
    assert_eq!(
        session.accept_frame(&frame, route, 1_001),
        Err(FrameReject::Version)
    );
    assert_eq!(
        session.accept_frame(&frame[..44], route, 1_001),
        Err(FrameReject::Wire(WireError::Length))
    );
}

#[test]
fn enrollment_needs_verified_channel_and_matching_two_sided_code() {
    let mut store = Store::default();
    let mut unverified = channel();
    unverified.verified = false;
    assert_eq!(
        Enrollment::begin(42, BOND, &unverified, 123_456),
        Err(EnrollmentError::Untrusted)
    );
    assert_eq!(
        Enrollment::begin(42, BOND, &channel(), 1_000_000),
        Err(EnrollmentError::InvalidCode)
    );
    let mut enrollment = Enrollment::begin(42, BOND, &channel(), 123_456).unwrap();
    assert_eq!(
        enrollment.confirm_local(123_457),
        Err(EnrollmentError::CodeMismatch)
    );
    assert_eq!(
        enrollment.commit(&mut store),
        Err(EnrollmentError::ApprovalMissing)
    );
    enrollment.confirm_local(123_456).unwrap();
    assert_eq!(
        enrollment.commit(&mut store),
        Err(EnrollmentError::ApprovalMissing)
    );
    assert_eq!(
        enrollment.confirm_remote(123_456, &unverified),
        Err(EnrollmentError::Untrusted)
    );
    let mut wrong_peer = channel();
    wrong_peer.identity = [8; 32];
    assert_eq!(
        enrollment.confirm_remote(123_456, &wrong_peer),
        Err(EnrollmentError::WrongPeer)
    );
    enrollment.confirm_remote(123_456, &channel()).unwrap();
    enrollment.commit(&mut store).unwrap();
    assert!(store.binding.is_some());
    assert!(!format!("{enrollment:?}").contains("123456"));
}

#[test]
fn revoked_enrollment_never_persists_and_store_revokes_binding() {
    let mut store = Store::default();
    let mut enrollment = Enrollment::begin(42, BOND, &channel(), 123_456).unwrap();
    enrollment.confirm_local(123_456).unwrap();
    enrollment.confirm_remote(123_456, &channel()).unwrap();
    enrollment.cancel();
    assert_eq!(enrollment.commit(&mut store), Err(EnrollmentError::Closed));
    assert!(store.binding.is_none());
    store.revoke(42).unwrap();
    assert!(store.revoked);
}
