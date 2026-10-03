// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Drives the native host actor with a fake monotonic clock and byte stream to
// verify exact control acknowledgments, retry bounds, local failover, and input safety.

use esp32_kvm_host_actor::{ForgetError, HostActor, HostFault, HostState, KeyMapper, MappedKey};
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
    forget_script: Option<ForgetScript>,
    forget_seen: bool,
    inventory_before: Vec<[u8; 16]>,
    inventory_after: Vec<[u8; 16]>,
    inventory_silent: bool,
}

#[derive(Clone, Copy)]
enum ForgetScript {
    Success,
    StaleThenSuccess,
    WrongAck,
    StillBonded,
    NoAck,
    Unplug,
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
    fn script_forget(&self, script: ForgetScript) {
        self.0.lock().unwrap().forget_script = Some(script);
    }
    fn inventory(&self, before: Vec<[u8; 16]>, after: Vec<[u8; 16]>) {
        let mut state = self.0.lock().unwrap();
        state.inventory_before = before;
        state.inventory_after = after;
    }
    fn silence_inventory(&self) {
        self.0.lock().unwrap().inventory_silent = true;
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
        let mut state = self.0.lock().unwrap();
        state.outgoing.extend_from_slice(buf);
        let mut decoder = FrameDecoder::new();
        for frame in buf.iter().filter_map(|byte| decoder.push(*byte)).flatten() {
            match (state.forget_script, frame.kind) {
                (_, MessageKind::GetBonds) if !state.inventory_silent => {
                    let tokens = if state.forget_seen {
                        &state.inventory_after
                    } else {
                        &state.inventory_before
                    };
                    let mut payload = vec![1, tokens.len() as u8];
                    for token in tokens {
                        payload.extend_from_slice(token);
                    }
                    let answer = Frame::new(
                        MessageKind::Bonds,
                        5,
                        frame.seq,
                        frame.route_generation,
                        payload,
                    );
                    if state.forget_seen
                        && matches!(state.forget_script, Some(ForgetScript::StaleThenSuccess))
                    {
                        let mut stale = answer.clone();
                        stale.seq = 0;
                        state.incoming.extend(stale.encode().unwrap());
                    }
                    state.incoming.extend(answer.encode().unwrap());
                }
                (Some(script), MessageKind::ForgetBond) if !state.forget_seen => {
                    state.forget_seen = true;
                    if matches!(script, ForgetScript::Unplug) {
                        state.eof = true;
                    } else if !matches!(script, ForgetScript::NoAck) {
                        let mut answer = ack(&frame, frame.route_generation);
                        if matches!(script, ForgetScript::WrongAck) {
                            answer.payload[0] = MessageKind::Arm as u8;
                        }
                        if matches!(script, ForgetScript::StaleThenSuccess) {
                            let mut stale = answer.clone();
                            stale.seq = 0;
                            stale.payload[1..5].copy_from_slice(&0_u32.to_le_bytes());
                            state.incoming.extend(stale.encode().unwrap());
                        }
                        state.incoming.extend(answer.encode().unwrap());
                    }
                }
                (Some(script), MessageKind::GetStatus)
                    if state.forget_seen
                        && !matches!(script, ForgetScript::NoAck | ForgetScript::Unplug) =>
                {
                    let answer = if matches!(script, ForgetScript::StillBonded) {
                        status(0, true)
                    } else {
                        status_empty()
                    };
                    state.incoming.extend(answer.encode().unwrap());
                }
                _ => {}
            }
        }
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
        firmware_version: "0.1.0-m1".into(),
        negotiated_minor: 0,
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

fn status_empty() -> Frame {
    Frame::new(
        MessageKind::Status,
        5,
        1,
        0,
        vec![0xa5, 1, 0, 2, 0, 3, 0x80, 4, 0, 5, 0],
    )
}

fn status_empty_m2() -> Frame {
    Frame::new(
        MessageKind::Status,
        5,
        1,
        0,
        vec![0xa6, 1, 0, 2, 0, 3, 0x80, 4, 0, 5, 0, 6, 0xa1, 1, 0],
    )
}

fn status_ready_m2() -> Frame {
    let mut frame = status(0, true);
    frame.payload[0] = 0xa6;
    frame.payload.extend_from_slice(&[6, 0xa1, 1, 0]);
    frame
}

fn status_three_m2(tokens: [u8; 3]) -> Frame {
    let mut payload = vec![0xa6, 1, 0, 2, 0, 3, 0x83];
    for (index, token) in tokens.into_iter().enumerate() {
        payload.extend_from_slice(&[0xa5, 1, index as u8 + 1, 2, 0x50]);
        payload.extend_from_slice(&[token; 16]);
        payload.extend_from_slice(&[3, 0xf5, 4, 0xf5, 5, 0]);
    }
    payload.extend_from_slice(&[4, 0, 5, 0, 6, 0xa1, 1, 0]);
    Frame::new(MessageKind::Status, 5, 1, 0, payload)
}

#[test]
fn three_slot_status_routes_only_with_negotiated_capacity() {
    let wire = FakePort::default();
    let (gate, _) = CaptureGate::new(32);
    let mut confirmed = device();
    confirmed.negotiated_minor = 2;
    confirmed.max_connections = 3;
    let mut actor = HostActor::from_confirmed(
        wire.clone(),
        Box::new(gate),
        confirmed,
        vec![3],
        Box::new(TestMapper),
        0,
    )
    .unwrap();
    wire.feed(status_three_m2([1, 2, 3]));
    actor.poll(1);
    assert_eq!(actor.setup_snapshot().slots.len(), 3);
    assert_eq!(actor.setup_snapshot().slots[2].bond_token, [3; 16]);
    actor.request(Action::Direct(3), 2);
    let release = wire.sent().last().unwrap().clone();
    wire.feed(ack(&release, 1));
    actor.poll(3);
    let select = wire.sent().last().unwrap().clone();
    assert_eq!(select.kind, MessageKind::Switch);
    assert_eq!(select.payload[0], 3);

    let (mut limited, limited_wire, _) = setup_inventory();
    limited_wire.feed(status_three_m2([1, 2, 3]));
    limited.poll(1);
    assert_eq!(limited.state(), HostState::Failed);
}

#[test]
fn duplicate_live_tokens_fail_closed() {
    let wire = FakePort::default();
    let (gate, _) = CaptureGate::new(32);
    let mut confirmed = device();
    confirmed.negotiated_minor = 2;
    confirmed.max_connections = 3;
    let mut actor = HostActor::from_confirmed(
        wire.clone(),
        Box::new(gate),
        confirmed,
        vec![3],
        Box::new(TestMapper),
        0,
    )
    .unwrap();
    wire.feed(status_three_m2([1, 1, 3]));
    actor.poll(1);
    assert_eq!(actor.state(), HostState::Failed);
}

#[test]
fn active_slot_identity_change_disarms_capture() {
    let (mut actor, wire, gate) = active();
    let mut replaced = status(2, true);
    replaced.payload[2] = 2;
    replaced.payload[4] = 1;
    replaced.payload[12..28].copy_from_slice(&[2; 16]);
    wire.feed(replaced);
    actor.poll(6);
    assert_eq!(actor.state(), HostState::Failed);
    assert_eq!(gate.generation(), 0);
}

fn setup_inventory() -> (HostActor<FakePort>, FakePort, Arc<CaptureGate>) {
    let wire = FakePort::default();
    let (gate, _receiver) = CaptureGate::new(32);
    let mut confirmed = device();
    confirmed.negotiated_minor = 2;
    let actor = HostActor::from_confirmed(
        wire.clone(),
        Box::new(gate.clone()),
        confirmed,
        vec![1],
        Box::new(TestMapper),
        0,
    )
    .unwrap();
    (actor, wire, gate)
}

#[test]
fn retained_inventory_distinguishes_unknown_from_authoritative_empty() {
    let (mut actor, wire, _) = setup_inventory();
    wire.feed(status_empty_m2());
    actor.poll(1);
    assert!(actor.setup_snapshot().retained_bonds.is_none());
    wire.inventory(vec![[1; 16]], vec![]);
    assert_eq!(actor.refresh_bond_inventory(2).unwrap(), vec![[1; 16]]);
    assert_eq!(actor.setup_snapshot().retained_bonds, Some(vec![[1; 16]]));
}

#[test]
fn forget_bond_fails_closed_when_inventory_is_unavailable_or_unsupported() {
    let (mut old_actor, old_wire, _) = setup();
    old_wire.feed(status(0, true));
    old_actor.poll(1);
    assert_eq!(
        old_actor.forget_bond([1; 16], 2),
        Err(ForgetError::Unsupported)
    );
    assert!(
        !old_wire
            .sent()
            .iter()
            .any(|f| f.kind == MessageKind::ForgetBond)
    );

    let (mut actor, wire, _) = setup_inventory();
    wire.feed(status_empty_m2());
    actor.poll(1);
    wire.silence_inventory();
    assert_eq!(
        actor.forget_bond([1; 16], 2),
        Err(ForgetError::Host(HostFault::Timeout))
    );
    assert!(actor.setup_snapshot().retained_bonds.is_none());
    assert!(
        !wire
            .sent()
            .iter()
            .any(|f| f.kind == MessageKind::ForgetBond)
    );
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
fn forget_bond_requires_exact_ack_and_fresh_absent_inventory() {
    let (mut actor, wire, _) = setup_inventory();
    wire.inventory(vec![[1; 16]], vec![]);
    wire.feed(status_empty_m2());
    actor.poll(1);
    wire.script_forget(ForgetScript::Success);
    assert_eq!(actor.forget_bond([1; 16], 2), Ok(()));
    let frames = wire.sent();
    let forget = frames
        .iter()
        .find(|f| f.kind == MessageKind::ForgetBond)
        .unwrap();
    assert_eq!(
        forget.payload,
        [0xa1, 1, 0x50]
            .into_iter()
            .chain([1; 16])
            .collect::<Vec<_>>()
    );
    assert!(
        frames
            .iter()
            .any(|f| f.kind == MessageKind::GetBonds && f.seq > forget.seq)
    );
    assert_eq!(actor.setup_snapshot().retained_bonds, Some(vec![]));
}

#[test]
fn forget_bond_rejects_absent_or_nonlocal_without_delete() {
    let (mut actor, wire, _) = setup_inventory();
    wire.inventory(vec![[1; 16]], vec![]);
    wire.feed(status_empty_m2());
    actor.poll(1);
    let before = wire.sent().len();
    assert_eq!(
        actor.forget_bond([2; 16], 2),
        Err(ForgetError::AlreadyAbsent)
    );
    assert!(wire.sent().len() > before);
    assert!(
        !wire
            .sent()
            .iter()
            .any(|f| f.kind == MessageKind::ForgetBond)
    );
    wire.feed(status_ready_m2());
    actor.poll(3);
    actor.request(Action::Direct(1), 3);
    let before = wire.sent().len();
    assert_eq!(actor.forget_bond([1; 16], 4), Err(ForgetError::NotLocal));
    assert_eq!(wire.sent().len(), before);
}

#[test]
fn forget_bond_wrong_ack_fails_local_and_unchanged_inventory_never_succeeds() {
    for (script, expected) in [
        (
            ForgetScript::WrongAck,
            Err(ForgetError::Host(HostFault::Acknowledgment)),
        ),
        (ForgetScript::StillBonded, Err(ForgetError::StillBonded)),
    ] {
        let (mut actor, wire, _) = setup_inventory();
        wire.inventory(
            vec![[1; 16]],
            if matches!(script, ForgetScript::StillBonded) {
                vec![[1; 16]]
            } else {
                vec![]
            },
        );
        wire.feed(status_empty_m2());
        actor.poll(1);
        wire.script_forget(script);
        assert_eq!(actor.forget_bond([1; 16], 2), expected);
        assert!(!wire.sent().is_empty());
    }
}

#[test]
fn forget_bond_without_ack_times_out_and_keeps_bond() {
    let (mut actor, wire, _) = setup_inventory();
    wire.inventory(vec![[1; 16]], vec![]);
    wire.feed(status_empty_m2());
    actor.poll(1);
    wire.script_forget(ForgetScript::NoAck);
    assert_eq!(
        actor.forget_bond([1; 16], 2),
        Err(ForgetError::Host(HostFault::Timeout))
    );
    assert!(
        wire.sent()
            .iter()
            .any(|f| f.kind == MessageKind::ForgetBond)
    );
}

#[test]
fn forget_bond_ignores_stale_ack_but_transport_loss_is_terminal() {
    let (mut actor, wire, _) = setup_inventory();
    wire.inventory(vec![[1; 16]], vec![]);
    wire.feed(status_empty_m2());
    actor.poll(1);
    wire.script_forget(ForgetScript::StaleThenSuccess);
    assert_eq!(actor.forget_bond([1; 16], 2), Ok(()));

    let (mut actor, wire, _) = setup_inventory();
    wire.inventory(vec![[1; 16]], vec![]);
    wire.feed(status_empty_m2());
    actor.poll(1);
    wire.script_forget(ForgetScript::Unplug);
    assert_eq!(
        actor.forget_bond([1; 16], 2),
        Err(ForgetError::Host(HostFault::Transport))
    );
    assert_eq!(actor.state(), HostState::Failed);
    assert!(actor.setup_snapshot().retained_bonds.is_none());
}

fn active() -> (HostActor<FakePort>, FakePort, Arc<CaptureGate>) {
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
    (actor, wire, gate)
}

#[test]
fn suspend_disarms_sends_release_and_rejects_stale_input() {
    let (mut actor, wire, gate) = active();
    actor.suspend();
    assert_eq!(gate.generation(), 0);
    assert_eq!(actor.fault(), Some(HostFault::Suspended));
    let release = wire.sent().last().unwrap().clone();
    assert_eq!(release.kind, MessageKind::ReleaseAll);
    assert_eq!(release.route_generation, 2);
    wire.feed(ack(&release, 3));
    actor.poll(6);
    actor.request(Action::Direct(1), 7);
    actor.on_capture(
        CaptureEvent {
            generation: 2,
            event: PhysicalEvent::Motion(8, 4),
        },
        8,
    );
    assert_eq!(actor.state(), HostState::Failed);
    assert_eq!(wire.sent().last().unwrap().kind, MessageKind::ReleaseAll);
}

#[test]
fn shutdown_and_drop_restore_local_capture() {
    let (mut actor, wire, gate) = active();
    actor.shutdown();
    assert_eq!(actor.fault(), Some(HostFault::Stopped));
    assert_eq!(gate.generation(), 0);
    assert_eq!(wire.sent().last().unwrap().kind, MessageKind::ReleaseAll);

    let (actor, _, gate) = active();
    drop(actor);
    assert_eq!(gate.generation(), 0);
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
    assert_eq!(actor.input_trace().key_events_seen, 1);
    assert_eq!(actor.input_trace().key_events_accepted, 0);
    assert_eq!(actor.input_trace().key_frames_written, 0);
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
    let trace = actor.input_trace();
    assert_eq!(trace.key_events_seen, 2);
    assert_eq!(trace.key_events_accepted, 1);
    assert_eq!(trace.key_frames_written, 1);
    assert_eq!(trace.last_key_seq, Some(keys[1].seq));
    let mut progress = Vec::new();
    progress.extend_from_slice(&keys[1].seq.to_le_bytes());
    progress.extend_from_slice(&keys[1].seq.to_le_bytes());
    wire.feed(Frame::new(MessageKind::InputProgress, 5, 99, 2, progress));
    actor.poll(9);
    let trace = actor.input_trace();
    assert_eq!(trace.board_accepted_seq, trace.last_key_seq);
    assert_eq!(trace.ble_enqueued_seq, trace.last_key_seq);
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
    assert_eq!(snapshot.firmware_version.as_deref(), Some("0.1.0-m1"));
    assert!(snapshot.comparison_value.is_none());
    assert!(actor.pair_begin(60, 2).is_err());
}

#[test]
fn minor_one_pairing_controls_share_verified_session() {
    let wire = FakePort::default();
    let (gate, _) = CaptureGate::new(32);
    let mut device = device();
    device.negotiated_minor = 1;
    let mut actor = HostActor::from_confirmed(
        wire.clone(),
        Box::new(gate),
        device,
        vec![1],
        Box::new(TestMapper),
        0,
    )
    .unwrap();
    let mut initial = status(0, true);
    initial.payload[0] = 0xa6;
    initial.payload.extend_from_slice(&[6, 0xa1, 1, 0]);
    wire.feed(initial);
    actor.poll(1);
    actor.pair_begin(60, 2).unwrap();
    let begin = wire.sent().last().unwrap().clone();
    assert_eq!(begin.kind, MessageKind::PairBegin);
    assert_eq!(begin.payload, [60, 0]);
    wire.feed(ack(&begin, 0));
    actor.poll(3);
    let mut pairing = status(0, true);
    pairing.payload[0] = 0xa6;
    pairing.payload[2] = 5;
    pairing
        .payload
        .extend_from_slice(&[6, 0xa2, 1, 1, 2, 0x19, 0xea, 0x60]);
    wire.feed(pairing);
    actor.poll(4);
    assert_eq!(actor.state(), HostState::Pairing);
    assert!(actor.pair_reply(7, true, 5).is_err());
    let reply = wire.sent().last().unwrap().clone();
    assert_eq!(reply.kind, MessageKind::PairBegin);
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

#[test]
fn numeric_challenge_status_exposes_value_and_rejects_stale_reply() {
    let wire = FakePort::default();
    let (gate, _) = CaptureGate::new(32);
    let mut device = device();
    device.negotiated_minor = 1;
    let mut actor = HostActor::from_confirmed(
        wire.clone(),
        Box::new(gate),
        device,
        vec![1],
        Box::new(TestMapper),
        0,
    )
    .unwrap();
    let mut initial = status(0, false);
    initial.payload[0] = 0xa6;
    initial.payload.extend_from_slice(&[6, 0xa1, 1, 0]);
    wire.feed(initial);
    actor.poll(1);
    assert_eq!(
        actor.setup_snapshot().firmware_version.as_deref(),
        Some("0.1.0-m1")
    );
    actor.pair_begin(60, 2).unwrap();
    let begin = wire.sent().last().unwrap().clone();
    wire.feed(ack(&begin, 0));
    actor.poll(3);
    let mut challenge = status(0, false);
    challenge.payload[0] = 0xa6;
    challenge.payload[2] = 5;
    challenge.payload.extend_from_slice(&[
        6, 0xa4, 1, 2, 2, 0x19, 0x03, 0xe8, 3, 7, 4, 0x1a, 0, 1, 0xe2, 0x40,
    ]);
    wire.feed(challenge);
    actor.poll(4);
    let snapshot = actor.setup_snapshot();
    assert_eq!(snapshot.challenge_id, Some(7));
    assert_eq!(snapshot.comparison_value, Some(123456));
    assert_eq!(snapshot.pairing_deadline_ms, Some(1004));
    assert!(actor.pair_reply(8, true, 5).is_err());
    actor.pair_reply(7, true, 5).unwrap();
    assert!(actor.pair_reply(7, true, 6).is_err());
    assert_eq!(actor.setup_snapshot().comparison_value, None);
}
