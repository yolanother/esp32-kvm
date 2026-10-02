// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Drives the native host actor with a fake monotonic clock and byte stream to
// verify exact control acknowledgments, retry bounds, local failover, and input safety.

use esp32_kvm_host_actor::{HostActor, HostState, KeyMapper, MappedKey};
use esp32_kvm_input_core::{Action, Destination, MappingProfile, MappingRule, Side, SourceKey};
use esp32_kvm_platform_windows::{CaptureEvent, CaptureGate, PhysicalEvent};
use esp32_kvm_protocol::{Frame, FrameDecoder, MessageKind};
use esp32_kvm_usb_transport::ConfirmedDevice;
use std::collections::VecDeque;
use std::io::{self, Read, Write};
use std::sync::{Arc, Mutex};

#[derive(Default)]
struct WireState {
    incoming: VecDeque<u8>,
    outgoing: Vec<u8>,
    eof: bool,
}

#[derive(Clone, Default)]
struct FakePort(Arc<Mutex<WireState>>);

impl FakePort {
    fn feed(&self, frame: Frame) {
        self.0
            .lock()
            .unwrap()
            .incoming
            .extend(frame.encode().unwrap());
    }
    fn feed_raw(&self, bytes: &[u8]) {
        self.0.lock().unwrap().incoming.extend(bytes);
    }
    fn sent(&self) -> Vec<Frame> {
        let bytes = self.0.lock().unwrap().outgoing.clone();
        let mut decoder = FrameDecoder::new();
        bytes
            .into_iter()
            .filter_map(|byte| decoder.push(byte))
            .map(Result::unwrap)
            .collect()
    }
    fn unplug(&self) {
        self.0.lock().unwrap().eof = true;
    }
}

impl Read for FakePort {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let mut state = self.0.lock().unwrap();
        if state.eof {
            return Ok(0);
        }
        let mut n = 0;
        while n < buf.len() {
            let Some(byte) = state.incoming.pop_front() else {
                break;
            };
            buf[n] = byte;
            n += 1;
        }
        if n == 0 {
            Err(io::ErrorKind::WouldBlock.into())
        } else {
            Ok(n)
        }
    }
}

impl Write for FakePort {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        self.0.lock().unwrap().outgoing.extend_from_slice(buf);
        Ok(buf.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

fn device() -> ConfirmedDevice {
    ConfirmedDevice {
        board_id: "esp32-kvm".into(),
        session_id: 5,
        max_connections: 1,
        max_bonds: 1,
    }
}

struct TestMapper;
impl KeyMapper for TestMapper {
    fn map_key(&self, virtual_key: u32, _scan_code: u32, _extended: bool) -> Option<MappedKey> {
        (virtual_key == 0x41).then_some(MappedKey::Usage(0x04))
    }
}

fn status(generation: u32, ready: bool) -> Frame {
    let mut payload = vec![0xa5, 1, 0, 2, 0, 3, 0x81, 0xa5, 1, 1, 2, 0x50];
    payload.extend_from_slice(&[1; 16]);
    payload.extend_from_slice(&[
        3,
        if ready { 0xf5 } else { 0xf4 },
        4,
        if ready { 0xf5 } else { 0xf4 },
        5,
        0x18,
        24,
        4,
        0,
        5,
    ]);
    if generation < 24 {
        payload.push(generation as u8);
    } else if generation <= 255 {
        payload.extend_from_slice(&[0x18, generation as u8]);
    } else {
        payload.extend_from_slice(&[0x1a]);
        payload.extend_from_slice(&generation.to_be_bytes());
    }
    Frame::new(MessageKind::Status, 5, 1, generation, payload)
}

fn ack(original: &Frame, result_generation: u32) -> Frame {
    let mut payload = vec![original.kind as u8];
    payload.extend_from_slice(&original.seq.to_le_bytes());
    payload.push(0);
    payload.extend_from_slice(&result_generation.to_le_bytes());
    Frame::new(
        MessageKind::Ack,
        5,
        original.seq,
        result_generation,
        payload,
    )
}

fn setup() -> (HostActor<FakePort>, FakePort, Arc<CaptureGate>) {
    let wire = FakePort::default();
    let (gate, _receiver) = CaptureGate::new(32);
    let actor = HostActor::from_confirmed(
        wire.clone(),
        Box::new(gate.clone()),
        device(),
        vec![1],
        Box::new(TestMapper),
        0,
    )
    .unwrap();
    (actor, wire, gate)
}

#[test]
fn release_switch_arm_requires_exact_ack_and_all_up() {
    let (mut actor, wire, gate) = setup();
    assert_eq!(wire.sent()[0].kind, MessageKind::GetStatus);
    wire.feed(status(41, true));
    actor.poll(1);
    assert_eq!(actor.state(), HostState::Local);
    actor.request(Action::Direct(1), 2);
    let release = wire.sent().last().unwrap().clone();
    assert_eq!(release.kind, MessageKind::ReleaseAll);
    actor.observe_all_up();
    assert_eq!(gate.generation(), 0);
    wire.feed(ack(&release, 42));
    actor.poll(3);
    let select = wire.sent().last().unwrap().clone();
    assert_eq!(select.kind, MessageKind::Switch);
    assert_eq!(select.payload, [1, 42, 0, 0, 0, 43, 0, 0, 0]);
    wire.feed(ack(&select, 43));
    actor.poll(4);
    let arm = wire.sent().last().unwrap().clone();
    assert_eq!(arm.kind, MessageKind::Arm);
    assert_eq!(gate.generation(), 0);
    wire.feed(ack(&arm, 43));
    actor.poll(5);
    assert_eq!(actor.state(), HostState::Guest(1));
    assert_eq!(gate.generation(), 43);
    let tail = wire.sent();
    assert_eq!(tail[tail.len() - 3].kind, MessageKind::KeyState);
    assert_eq!(tail[tail.len() - 3].payload, [0; 8]);
    assert_eq!(tail[tail.len() - 2].kind, MessageKind::Pointer);
    assert_eq!(tail[tail.len() - 2].payload, [0; 7]);
    assert_eq!(tail[tail.len() - 1].kind, MessageKind::ConsumerState);
    assert_eq!(tail[tail.len() - 1].payload, [0; 2]);
}

#[test]
fn stale_ack_is_ignored_but_wrong_current_ack_fails_local() {
    let (mut actor, wire, gate) = setup();
    wire.feed(status(0, true));
    actor.poll(1);
    actor.request(Action::Next, 2);
    let release = wire.sent().last().unwrap().clone();
    let mut old = ack(&release, 1);
    old.seq = 0;
    old.payload[1..5].copy_from_slice(&0_u32.to_le_bytes());
    wire.feed(old);
    actor.poll(3);
    assert_eq!(actor.state(), HostState::Switching);
    assert_eq!(wire.sent().last().unwrap().seq, release.seq);
    let mut wrong = ack(&release, 9);
    wrong.payload[0] = MessageKind::Arm as u8;
    wire.feed(wrong);
    actor.poll(4);
    assert_eq!(actor.state(), HostState::Failed);
    assert_eq!(gate.generation(), 0);
}

#[test]
fn local_preempts_and_old_ack_cannot_rearm() {
    let (mut actor, wire, gate) = setup();
    wire.feed(status(0, true));
    actor.poll(1);
    actor.request(Action::Next, 2);
    let old = wire.sent().last().unwrap().clone();
    actor.request(Action::Local, 3);
    let local = wire.sent().last().unwrap().clone();
    assert_ne!(old.seq, local.seq);
    assert_eq!(gate.generation(), 0);
    wire.feed(ack(&old, 1));
    actor.poll(4);
    assert_eq!(actor.state(), HostState::Local);
    assert_eq!(wire.sent().last().unwrap().seq, local.seq);
}

#[test]
fn retries_controls_twice_then_fails_local_and_never_replays_motion() {
    let (mut actor, wire, gate) = setup();
    wire.feed(status(0, true));
    actor.poll(1);
    actor.request(Action::Direct(1), 2);
    let release = wire.sent().last().unwrap().clone();
    actor.poll(102);
    actor.poll(202);
    let retries: Vec<_> = wire
        .sent()
        .into_iter()
        .filter(|f| f.kind == MessageKind::ReleaseAll)
        .collect();
    assert_eq!(retries.len(), 3);
    assert!(retries.iter().all(|f| f.seq == release.seq));
    actor.poll(302);
    assert_eq!(actor.state(), HostState::Failed);
    assert_eq!(gate.generation(), 0);
    actor.on_capture(
        CaptureEvent {
            generation: 1,
            event: PhysicalEvent::Motion(17, -5),
        },
        303,
    );
    assert!(!wire.sent().iter().any(|f| f.kind == MessageKind::Pointer));
}

#[test]
fn unplug_and_offline_guest_fail_local() {
    let (mut actor, wire, gate) = setup();
    wire.feed(status(0, false));
    actor.poll(1);
    actor.request(Action::Direct(1), 2);
    assert_eq!(actor.state(), HostState::Local);
    wire.unplug();
    actor.poll(3);
    assert_eq!(actor.state(), HostState::Failed);
    assert_eq!(gate.generation(), 0);
}

#[test]
fn active_input_uses_current_generation_and_never_replays_deltas() {
    let (mut actor, wire, gate) = setup();
    wire.feed(status(0, true));
    actor.poll(1);
    actor.request(Action::Direct(1), 2);
    let release = wire.sent().last().unwrap().clone();
    wire.feed(ack(&release, 1));
    actor.poll(3);
    let select = wire.sent().last().unwrap().clone();
    wire.feed(ack(&select, 2));
    actor.poll(4);
    let arm = wire.sent().last().unwrap().clone();
    wire.feed(ack(&arm, 2));
    actor.poll(5);
    actor.observe_all_up();
    assert_eq!(gate.generation(), 2);
    actor.on_capture(
        CaptureEvent {
            generation: 1,
            event: PhysicalEvent::Motion(9, 0),
        },
        6,
    );
    actor.on_capture(
        CaptureEvent {
            generation: 2,
            event: PhysicalEvent::Motion(7, -3),
        },
        7,
    );
    actor.on_capture(
        CaptureEvent {
            generation: 2,
            event: PhysicalEvent::Key {
                virtual_key: 0x41,
                scan_code: 0x1e,
                extended: false,
                down: true,
                repeat: false,
            },
        },
        8,
    );
    let frames = wire.sent();
    let pointers: Vec<_> = frames
        .iter()
        .filter(|f| f.kind == MessageKind::Pointer)
        .collect();
    assert_eq!(pointers.len(), 2);
    assert_eq!(pointers[1].payload, [0, 7, 0, 253, 255, 0, 0]);
    let keys: Vec<_> = frames
        .iter()
        .filter(|f| f.kind == MessageKind::KeyState)
        .collect();
    assert_eq!(keys.len(), 2);
    assert_eq!(keys[1].payload, [0, 0, 4, 0, 0, 0, 0, 0]);
}

#[test]
fn malformed_frame_fails_local_and_periodic_status_checks_peer() {
    let (mut actor, wire, gate) = setup();
    wire.feed(status(0, true));
    actor.poll(1);
    actor.poll(251);
    assert_eq!(
        wire.sent()
            .iter()
            .filter(|f| f.kind == MessageKind::GetStatus)
            .count(),
        2
    );
    let mut malformed = status(0, true).encode().unwrap();
    let last = malformed.len() - 2;
    malformed[last] ^= 1;
    wire.feed_raw(&malformed);
    actor.poll(252);
    assert_eq!(actor.state(), HostState::Failed);
    assert_eq!(gate.generation(), 0);
}

#[test]
fn current_ack_with_mismatched_original_sequence_is_a_fault() {
    let (mut actor, wire, _) = setup();
    wire.feed(status(0, true));
    actor.poll(1);
    actor.request(Action::Direct(1), 2);
    let release = wire.sent().last().unwrap().clone();
    let mut wrong = ack(&release, 1);
    wrong.payload[1..5].copy_from_slice(&0_u32.to_le_bytes());
    wire.feed(wrong);
    actor.poll(3);
    assert_eq!(actor.state(), HostState::Failed);
}

#[test]
fn wheel_fractions_are_accumulated_within_one_route() {
    let (mut actor, wire, _) = setup();
    wire.feed(status(0, true));
    actor.poll(1);
    actor.request(Action::Direct(1), 2);
    let release = wire.sent().last().unwrap().clone();
    wire.feed(ack(&release, 1));
    actor.poll(3);
    let select = wire.sent().last().unwrap().clone();
    wire.feed(ack(&select, 2));
    actor.poll(4);
    let arm = wire.sent().last().unwrap().clone();
    wire.feed(ack(&arm, 2));
    actor.poll(5);
    actor.observe_all_up();
    for now in [6, 7] {
        actor.on_capture(
            CaptureEvent {
                generation: 2,
                event: PhysicalEvent::Wheel(esp32_kvm_platform_windows::MouseAxis::Vertical, 60),
            },
            now,
        );
    }
    let wheels: Vec<_> = wire
        .sent()
        .into_iter()
        .filter(|f| f.kind == MessageKind::Pointer && f.payload[5] != 0)
        .collect();
    assert_eq!(wheels.len(), 1);
    assert_eq!(wheels[0].payload[5], 1);
}

#[test]
fn stale_hotkey_event_cannot_start_a_new_route() {
    let (mut actor, wire, _) = setup();
    wire.feed(status(0, true));
    actor.poll(1);
    actor.on_capture(
        CaptureEvent {
            generation: 99,
            event: PhysicalEvent::Hotkey(Action::Direct(1)),
        },
        2,
    );
    assert_eq!(actor.state(), HostState::Local);
    assert!(
        !wire
            .sent()
            .iter()
            .any(|f| f.kind == MessageKind::ReleaseAll)
    );
    actor.on_capture(
        CaptureEvent {
            generation: 0,
            event: PhysicalEvent::Hotkey(Action::Direct(1)),
        },
        3,
    );
    assert_eq!(actor.state(), HostState::Switching);
}

#[test]
fn status_claiming_remote_control_while_actor_is_local_fails_safe() {
    let (mut actor, wire, gate) = setup();
    wire.feed(status(0, true));
    actor.poll(1);
    let mut inconsistent = status(0, true);
    inconsistent.payload[2] = 2;
    inconsistent.payload[4] = 1;
    wire.feed(inconsistent);
    actor.poll(2);
    assert_eq!(actor.state(), HostState::Failed);
    assert_eq!(gate.generation(), 0);
}

#[test]
fn drive_consumes_capture_hotkeys_on_the_one_actor_stream() {
    let wire = FakePort::default();
    let (gate, receiver) = CaptureGate::new(8);
    let mut actor = HostActor::from_confirmed(
        wire.clone(),
        Box::new(gate.clone()),
        device(),
        vec![1],
        Box::new(TestMapper),
        0,
    )
    .unwrap();
    wire.feed(status(0, true));
    actor.drive(&receiver, 1);
    assert!(gate.offer_hotkey(Action::Direct(1)));
    actor.drive(&receiver, 2);
    assert_eq!(actor.state(), HostState::Switching);
    assert_eq!(wire.sent().last().unwrap().kind, MessageKind::ReleaseAll);
}

#[test]
fn pairing_controls_share_session_and_snapshot_tracks_bonds() {
    let (mut actor, wire, _) = setup();
    wire.feed(status(0, true));
    actor.poll(1);
    let snapshot = actor.setup_snapshot();
    assert_eq!(snapshot.board_id, "esp32-kvm");
    assert_eq!(snapshot.max_bonds, 1);
    assert_eq!(snapshot.slots[0].bond_token, [1; 16]);
    assert!(snapshot.slots[0].ready);
    assert!(snapshot.firmware_version.is_none());
    assert!(snapshot.comparison_value.is_none());
    actor.pair_begin(60, 2).unwrap();
    let begin = wire.sent().last().unwrap().clone();
    assert_eq!(begin.kind, MessageKind::PairBegin);
    assert_eq!(begin.payload, [60, 0]);
    wire.feed(ack(&begin, 0));
    actor.poll(3);
    let mut pairing = status(0, true);
    pairing.payload[2] = 5;
    wire.feed(pairing);
    actor.poll(4);
    assert_eq!(actor.state(), HostState::Pairing);
    actor.pair_reply(7, true, 5).unwrap();
    let reply = wire.sent().last().unwrap().clone();
    assert_eq!(reply.kind, MessageKind::PairReply);
    assert_eq!(reply.payload, [0xa3, 1, 7, 2, 0, 3, 0xf5]);
    wire.feed(ack(&reply, 0));
    actor.poll(6);
    actor.pair_cancel(7).unwrap();
    assert_eq!(wire.sent().last().unwrap().kind, MessageKind::PairCancel);
}

#[test]
fn physical_hold_blocks_arm_even_if_all_up_is_claimed() {
    let (mut actor, wire, gate) = setup();
    wire.feed(status(0, true));
    actor.poll(1);
    gate.record_physical_key(0x1d, false, true);
    actor.request(Action::Direct(1), 2);
    let release = wire.sent().last().unwrap().clone();
    wire.feed(ack(&release, 1));
    actor.poll(3);
    let select = wire.sent().last().unwrap().clone();
    wire.feed(ack(&select, 2));
    actor.poll(4);
    let arm = wire.sent().last().unwrap().clone();
    wire.feed(ack(&arm, 2));
    actor.poll(5);
    actor.observe_all_up();
    assert_eq!(gate.generation(), 0);
    gate.record_physical_key(0x1d, false, false);
    actor.poll(6);
    assert_eq!(gate.generation(), 2);
}

#[test]
fn bonded_guest_profile_maps_physical_usage_before_key_state() {
    let (mut actor, wire, gate) = setup();
    let physical_a = SourceKey {
        usage: 4,
        side: Side::Unspecified,
    };
    actor
        .set_guest_profile(
            [1; 16],
            MappingProfile {
                preset: vec![],
                rules: vec![MappingRule {
                    source: vec![physical_a],
                    target: vec![Destination::Usage(5)],
                    priority: 0,
                    enabled: true,
                }],
            },
        )
        .unwrap();
    wire.feed(status(0, true));
    actor.poll(1);
    actor.request(Action::Direct(1), 2);
    let release = wire.sent().last().unwrap().clone();
    wire.feed(ack(&release, 1));
    actor.poll(3);
    let select = wire.sent().last().unwrap().clone();
    wire.feed(ack(&select, 2));
    actor.poll(4);
    let arm = wire.sent().last().unwrap().clone();
    wire.feed(ack(&arm, 2));
    actor.poll(5);
    assert_eq!(gate.generation(), 2);
    actor.on_capture(
        CaptureEvent {
            generation: 2,
            event: PhysicalEvent::Key {
                virtual_key: 0x41,
                scan_code: 0x1e,
                extended: false,
                down: true,
                repeat: false,
            },
        },
        6,
    );
    let key_state = wire.sent().last().unwrap().clone();
    assert_eq!(key_state.kind, MessageKind::KeyState);
    assert_eq!(key_state.payload[2], 5);
}
