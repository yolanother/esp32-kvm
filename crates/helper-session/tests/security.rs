// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Exercises helper-session admission and telemetry gates with a fake authenticated channel.
// These tests prove fail-closed policy decisions without opening a LAN socket or routing input.

use esp32_kvm_helper_session::{
    AuthenticatedChannel, BindingError, EdgeTelemetry, HelperSession, Reject, Route, TrustedBinding,
};

const BOND: [u8; 16] = [7; 16];
const IDENTITY: [u8; 32] = [9; 32];

struct FakeChannel {
    encrypted: bool,
    peer: [u8; 32],
    version: u16,
    session: [u8; 16],
}

impl AuthenticatedChannel for FakeChannel {
    fn is_encrypted_and_peer_verified(&self) -> bool {
        self.encrypted
    }

    fn peer_identity(&self) -> [u8; 32] {
        self.peer
    }

    fn protocol_version(&self) -> u16 {
        self.version
    }

    fn session_id(&self) -> [u8; 16] {
        self.session
    }
}

fn binding() -> TrustedBinding {
    TrustedBinding::new(42, BOND, IDENTITY, 1, 2).unwrap()
}

fn channel() -> FakeChannel {
    FakeChannel {
        encrypted: true,
        peer: IDENTITY,
        version: 2,
        session: [3; 16],
    }
}

fn telemetry() -> EdgeTelemetry {
    EdgeTelemetry {
        session_id: [3; 16],
        sequence: 1,
        sent_at_ms: 1_000,
        route_generation: 7,
        buttons_down: false,
    }
}

#[test]
fn refuses_unbound_and_invalid_trust_records() {
    assert_eq!(
        TrustedBinding::new(42, [0; 16], IDENTITY, 1, 2),
        Err(BindingError::MissingBond)
    );
    assert_eq!(
        TrustedBinding::new(0, BOND, IDENTITY, 1, 2),
        Err(BindingError::MissingBond)
    );
    assert_eq!(
        TrustedBinding::new(42, BOND, [0; 32], 1, 2),
        Err(BindingError::MissingHelperIdentity)
    );
    assert_eq!(
        TrustedBinding::new(42, BOND, IDENTITY, 2, 1),
        Err(BindingError::InvalidVersionRange)
    );
    assert!(!format!("{:?}", binding()).contains("[7, 7"));
    assert_eq!(
        HelperSession::open(None, BOND, &channel()),
        Err(Reject::Untrusted)
    );
}

#[test]
fn refuses_unverified_peer_wrong_identity_bond_and_version() {
    let trusted = binding();
    let mut peer = channel();
    peer.encrypted = false;
    assert_eq!(
        HelperSession::open(Some(&trusted), BOND, &peer),
        Err(Reject::Untrusted)
    );
    peer = channel();
    peer.peer = [8; 32];
    assert_eq!(
        HelperSession::open(Some(&trusted), BOND, &peer),
        Err(Reject::WrongHelper)
    );
    peer = channel();
    peer.version = 3;
    assert_eq!(
        HelperSession::open(Some(&trusted), BOND, &peer),
        Err(Reject::IncompatibleVersion)
    );
    assert_eq!(
        HelperSession::open(Some(&trusted), [8; 16], &channel()),
        Err(Reject::WrongBond)
    );
    peer = channel();
    peer.session = [0; 16];
    assert_eq!(
        HelperSession::open(Some(&trusted), BOND, &peer),
        Err(Reject::Untrusted)
    );
}

#[test]
fn accepts_only_current_fresh_monotonic_telemetry_for_active_profile() {
    let trusted = binding();
    let mut session = HelperSession::open(Some(&trusted), BOND, &channel()).unwrap();
    let route = Route {
        profile_id: 42,
        generation: 7,
        active: true,
    };
    assert_eq!(session.accept(telemetry(), route, 1_100), Ok(()));
    assert_eq!(
        session.accept(telemetry(), route, 1_101),
        Err(Reject::Replay)
    );
    let mut next = telemetry();
    next.sequence = 2;
    next.sent_at_ms = 1_101;
    assert_eq!(session.accept(next, route, 1_102), Ok(()));
    next.sequence = 3;
    next.sent_at_ms = 1_102;
    assert_eq!(
        session.accept(
            next,
            Route {
                generation: 8,
                ..route
            },
            1_103
        ),
        Err(Reject::WrongRoute)
    );
    assert_eq!(
        session.accept(
            next,
            Route {
                active: false,
                ..route
            },
            1_103
        ),
        Err(Reject::WrongRoute)
    );
    assert_eq!(
        session.accept(
            next,
            Route {
                profile_id: 43,
                ..route
            },
            1_103
        ),
        Err(Reject::WrongRoute)
    );
}

#[test]
fn refuses_stale_future_wrong_session_and_held_button_telemetry() {
    let trusted = binding();
    let mut session = HelperSession::open(Some(&trusted), BOND, &channel()).unwrap();
    let route = Route {
        profile_id: 42,
        generation: 7,
        active: true,
    };
    let mut event = telemetry();
    assert_eq!(session.accept(event, route, 1_251), Err(Reject::Stale));
    assert_eq!(session.accept(event, route, 899), Err(Reject::Future));
    event.session_id = [4; 16];
    assert_eq!(
        session.accept(event, route, 1_001),
        Err(Reject::WrongSession)
    );
    event.session_id = [3; 16];
    event.buttons_down = true;
    assert_eq!(
        session.accept(event, route, 1_001),
        Err(Reject::ButtonsHeld)
    );
    event.buttons_down = false;
    assert_eq!(session.accept(event, route, 1_001), Ok(()));
}

#[test]
fn freshness_limits_are_inclusive_and_rejected_events_do_not_advance_replay_state() {
    let trusted = binding();
    let mut session = HelperSession::open(Some(&trusted), BOND, &channel()).unwrap();
    let route = Route {
        profile_id: 42,
        generation: 7,
        active: true,
    };
    let event = telemetry();
    assert_eq!(session.accept(event, route, 1_250), Ok(()));
    let mut next = event;
    next.sequence = 2;
    next.sent_at_ms = 1_100;
    assert_eq!(session.accept(next, route, 999), Err(Reject::Future));
    assert_eq!(session.accept(next, route, 1_000), Ok(()));
}

#[test]
fn link_loss_revocation_and_reconnect_invalidate_old_session() {
    let trusted = binding();
    let mut session = HelperSession::open(Some(&trusted), BOND, &channel()).unwrap();
    let route = Route {
        profile_id: 42,
        generation: 7,
        active: true,
    };
    session.close();
    assert_eq!(
        session.accept(telemetry(), route, 1_001),
        Err(Reject::Closed)
    );
    let mut replacement = channel();
    replacement.session = [4; 16];
    let mut session = HelperSession::open(Some(&trusted), BOND, &replacement).unwrap();
    assert_eq!(
        session.accept(telemetry(), route, 1_001),
        Err(Reject::WrongSession)
    );
    let mut current = telemetry();
    current.session_id = [4; 16];
    assert_eq!(session.accept(current, route, 1_001), Ok(()));
    session.close();
    assert_eq!(session.accept(current, route, 1_002), Err(Reject::Closed));
}
