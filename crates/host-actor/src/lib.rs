// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Owns the serialized native host USB session and routes capture events through
// a verified protocol session, input policy, fail-local capture gate, and
// numeric pairing state reported by negotiated firmware STATUS. Terminal
// suspend and shutdown end the session without input replay.

#![forbid(unsafe_code)]

mod set1;
pub use set1::SetOneKeyMapper;

use esp32_kvm_input_core::{
    Action, Command, MappingEngine, MappingError, MappingProfile, RequestActor, Side, SourceKey,
    State,
};
#[cfg(windows)]
use esp32_kvm_platform_windows::CaptureService;
use esp32_kvm_platform_windows::{
    CaptureEvent, CaptureFault, CaptureGate, MouseAxis, MouseButton, PhysicalEvent,
};
use esp32_kvm_protocol::{Frame, FrameDecoder, MessageKind, ProtocolError};
use esp32_kvm_usb_transport::{
    ConfirmedDevice, ProbeError, available_usb_ports, candidate_ports, probe_stream,
};
use std::collections::BTreeMap;
use std::io::{self, Read, Write};
use std::sync::{
    Arc,
    mpsc::{Receiver, TryRecvError},
};

const HEARTBEAT_MS: u64 = 100;
const STATUS_REQUEST_MS: u64 = 250;
const CONTROL_RETRY_MS: u64 = 100;
const PEER_TIMEOUT_MS: u64 = 500;

/// A translated keyboard output supplied by the active guest profile.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MappedKey {
    /// One USB keyboard usage; 0 is never a press.
    Usage(u8),
    /// One modifier bit in the USB keyboard report.
    Modifier(u8),
}

/// Maps physical Windows keys to HID usages without changing routing order.
pub trait KeyMapper: Send {
    /// Returns the output for one physical press; the actor remembers it for release.
    fn map_key(&self, virtual_key: u32, scan_code: u32, extended: bool) -> Option<MappedKey>;
}

/// Capture controls needed by the serialized actor; the production service
/// and deterministic test gate share this boundary.
pub trait CaptureControl {
    /// Arms local suppression for a confirmed nonzero route generation.
    fn arm(&self, generation: u32) -> bool;
    /// Restores local Windows input delivery immediately.
    fn disarm(&self);
    /// Returns the generation currently captured, or zero while local.
    fn generation(&self) -> u32;
    /// Reports a capture queue, hook, or input-device fault.
    fn fault(&self) -> Option<CaptureFault>;
    /// Proves all non-injected physical keys and buttons are released.
    fn physical_all_up(&self) -> bool;
    /// Renews the independent routing actor watchdog while this actor advances.
    fn actor_heartbeat(&self) {}
}

impl CaptureControl for CaptureGate {
    fn arm(&self, generation: u32) -> bool {
        CaptureGate::arm(self, generation)
    }
    fn disarm(&self) {
        CaptureGate::disarm(self);
    }
    fn generation(&self) -> u32 {
        CaptureGate::generation(self)
    }
    fn fault(&self) -> Option<CaptureFault> {
        CaptureGate::fault(self)
    }
    fn physical_all_up(&self) -> bool {
        CaptureGate::physical_all_up(self)
    }
}

#[cfg(windows)]
impl CaptureControl for CaptureService {
    fn arm(&self, generation: u32) -> bool {
        CaptureService::arm(self, generation)
    }
    fn disarm(&self) {
        CaptureService::disarm(self);
    }
    fn generation(&self) -> u32 {
        CaptureService::generation(self)
    }
    fn fault(&self) -> Option<CaptureFault> {
        CaptureService::fault(self)
    }
    fn physical_all_up(&self) -> bool {
        CaptureService::physical_all_up(self)
    }
    fn actor_heartbeat(&self) {
        CaptureService::actor_heartbeat(self);
    }
}

impl<T: CaptureControl + ?Sized> CaptureControl for Arc<T> {
    fn arm(&self, generation: u32) -> bool {
        (**self).arm(generation)
    }
    fn disarm(&self) {
        (**self).disarm();
    }
    fn generation(&self) -> u32 {
        (**self).generation()
    }
    fn fault(&self) -> Option<CaptureFault> {
        (**self).fault()
    }
    fn physical_all_up(&self) -> bool {
        (**self).physical_all_up()
    }
    fn actor_heartbeat(&self) {
        (**self).actor_heartbeat();
    }
}

/// Observable host routing status; Failed requires a new verified USB session.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum HostState {
    /// Waiting for an initial firmware STATUS with its current generation.
    AwaitStatus,
    /// Host receives input locally.
    Local,
    /// Firmware is pairing while the host remains local.
    Pairing,
    /// Release, select, or arm awaits an exact ACK.
    Switching,
    /// An acknowledged guest route is active.
    Guest(u8),
    /// Capture is disarmed after a transport or protocol fault.
    Failed,
}

/// Pairing state reported by a minor-one firmware STATUS.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PairingStatus {
    /// The device negotiated an older minor version.
    Unsupported,
    /// No pairing window is open.
    Closed,
    /// A bounded pairing window is open.
    Waiting,
    /// A fresh numeric comparison needs user approval.
    Challenge,
    /// The peer or user rejected pairing.
    Rejected,
    /// Bond storage has reached capacity.
    Capacity,
    /// The window or comparison expired.
    Timeout,
}

/// Reason the host stopped trusting a USB session.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum HostFault {
    /// Serial read/write or EOF ended the session.
    Transport,
    /// A frame failed COBS, CRC, version, payload, or STATUS validation.
    Protocol,
    /// A live frame claimed a different device session.
    Session,
    /// Current ACK had the wrong kind, sequence, or resulting generation.
    Acknowledgment,
    /// A NACK refused a control command.
    Rejected,
    /// Firmware did not respond within the allowed watchdog/retry window.
    Timeout,
    /// Capture was faulted or could not be armed safely.
    Capture,
    /// The operating system is suspending or ending the interactive session.
    Suspended,
    /// The host actor was explicitly shut down.
    Stopped,
}

/// One bonded slot reported by firmware STATUS.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SlotSnapshot {
    /// Firmware slot index.
    pub slot: u8,
    /// Opaque bond identity; never a Bluetooth address.
    pub bond_token: [u8; 16],
    /// Whether the peer is currently connected.
    pub ready: bool,
    /// Whether its HID notifications are subscribed.
    pub subscribed: bool,
}

/// Setup state available from the verified session and latest STATUS.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SetupSnapshot {
    /// Board ID verified by USB handshake.
    pub board_id: String,
    /// Firmware advertised simultaneous connections.
    pub max_connections: u8,
    /// Firmware advertised bond capacity.
    pub max_bonds: u8,
    /// Firmware version reported by verified CAPS.
    pub firmware_version: Option<String>,
    /// Pairing state reported by STATUS after minor-one negotiation.
    pub pairing_status: PairingStatus,
    /// Current actor state.
    pub state: HostState,
    /// Bond slots from the latest STATUS.
    pub slots: Vec<SlotSnapshot>,
    /// Host monotonic deadline derived from STATUS remaining time.
    pub pairing_deadline_ms: Option<u64>,
    /// Fresh challenge ID when pairing status is Challenge.
    pub challenge_id: Option<u32>,
    /// Six-digit numeric comparison when pairing status is Challenge.
    pub comparison_value: Option<u32>,
    /// Terminal connection or protocol fault.
    pub fault: Option<HostFault>,
}

/// A pairing command could not be safely sent.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PairingError {
    /// This device did not negotiate the pairing-status minor version.
    Unsupported,
    /// A route or control transaction is already active.
    Busy,
    /// The actor is not in a local pairing-capable state.
    NotLocal,
    /// Duration or challenge ID is outside the protocol range.
    InvalidArgument,
    /// Sending the command ended the verified session.
    Transport,
}

#[derive(Clone)]
struct Pending {
    frame: Frame,
    expected_generation: u32,
    retries: u8,
    deadline: u64,
}

/// One serialized host-side protocol and input actor for a confirmed USB stream.
pub struct HostActor<S: Read + Write> {
    stream: S,
    capture: Box<dyn CaptureControl>,
    mapper: Box<dyn KeyMapper>,
    device: ConfirmedDevice,
    decoder: FrameDecoder,
    router: Option<RequestActor>,
    order: Vec<u8>,
    pending: Option<Pending>,
    next_seq: u32,
    last_heartbeat: u64,
    last_status_request: u64,
    last_peer: u64,
    confirmed_generation: u32,
    all_up: bool,
    baseline_sent: bool,
    fault: Option<HostFault>,
    pairing: bool,
    pairing_status: PairingStatus,
    pairing_deadline_ms: Option<u64>,
    challenge_id: Option<u32>,
    comparison_value: Option<u32>,
    slots: Vec<SlotSnapshot>,
    keys: BTreeMap<(u32, bool), SourceKey>,
    mapping: MappingEngine,
    profiles: BTreeMap<[u8; 16], MappingProfile>,
    buttons: u8,
    wheel_residual: i32,
    pan_residual: i32,
}

impl<S: Read + Write> HostActor<S> {
    /// Starts a disarmed actor after HELLO/CAPS/SESSION_OPEN was verified.
    /// It requests STATUS before allowing any routing command.
    pub fn from_confirmed(
        stream: S,
        capture: Box<dyn CaptureControl>,
        device: ConfirmedDevice,
        order: Vec<u8>,
        mapper: Box<dyn KeyMapper>,
        now_ms: u64,
    ) -> Result<Self, HostFault> {
        if device.session_id == 0 {
            return Err(HostFault::Session);
        }
        let pairing_supported = device.negotiated_minor >= 1;
        capture.disarm();
        let mut actor = Self {
            stream,
            capture,
            mapper,
            device,
            decoder: FrameDecoder::new(),
            router: None,
            order,
            pending: None,
            next_seq: 2,
            last_heartbeat: now_ms,
            last_status_request: now_ms,
            last_peer: now_ms,
            confirmed_generation: 0,
            all_up: false,
            baseline_sent: false,
            fault: None,
            pairing: false,
            pairing_status: if pairing_supported {
                PairingStatus::Closed
            } else {
                PairingStatus::Unsupported
            },
            pairing_deadline_ms: None,
            challenge_id: None,
            comparison_value: None,
            slots: Vec::new(),
            keys: BTreeMap::new(),
            mapping: MappingEngine::new(MappingProfile::default()).expect("empty profile is valid"),
            profiles: BTreeMap::new(),
            buttons: 0,
            wheel_residual: 0,
            pan_residual: 0,
        };
        actor.send(MessageKind::GetStatus, 0, vec![])?;
        Ok(actor)
    }

    /// Returns the current local, transitional, guest, or failed state.
    pub fn state(&self) -> HostState {
        if self.fault.is_some() {
            return HostState::Failed;
        }
        match self.router.as_ref().map(RequestActor::state) {
            None => HostState::AwaitStatus,
            Some(State::Local) if self.pairing => HostState::Pairing,
            Some(State::Local) => HostState::Local,
            Some(State::Switching) => HostState::Switching,
            Some(State::Guest(slot)) => HostState::Guest(slot),
        }
    }

    /// Returns the terminal fault, if this session must be renegotiated.
    pub fn fault(&self) -> Option<HostFault> {
        self.fault
    }

    /// Installs a validated guest profile by its persistent opaque bond token.
    /// Edits to the active guest defer until every physical key is released.
    pub fn set_guest_profile(
        &mut self,
        bond_token: [u8; 16],
        profile: MappingProfile,
    ) -> Result<(), MappingError> {
        MappingEngine::new(profile.clone())?;
        if let HostState::Guest(slot) = self.state()
            && self
                .slots
                .iter()
                .any(|seen| seen.slot == slot && seen.bond_token == bond_token)
        {
            self.mapping.set_profile(profile.clone())?;
        }
        self.profiles.insert(bond_token, profile);
        Ok(())
    }

    /// Returns the latest setup data without opening another serial connection.
    pub fn setup_snapshot(&self) -> SetupSnapshot {
        SetupSnapshot {
            board_id: self.device.board_id.clone(),
            max_connections: self.device.max_connections,
            max_bonds: self.device.max_bonds,
            firmware_version: Some(self.device.firmware_version.clone()),
            pairing_status: self.pairing_status,
            state: self.state(),
            slots: self.slots.clone(),
            pairing_deadline_ms: self.pairing_deadline_ms,
            challenge_id: self.challenge_id,
            comparison_value: self.comparison_value,
            fault: self.fault,
        }
    }

    /// Begins a local pairing window through the actor's verified USB session.
    pub fn pair_begin(&mut self, duration_seconds: u16, now_ms: u64) -> Result<(), PairingError> {
        if self.device.negotiated_minor < 1 {
            return Err(PairingError::Unsupported);
        }
        if duration_seconds != 60 {
            return Err(PairingError::InvalidArgument);
        }
        if self.state() != HostState::Local {
            return Err(PairingError::NotLocal);
        }
        if self.pending.is_some() {
            return Err(PairingError::Busy);
        }
        self.issue_aux(
            MessageKind::PairBegin,
            duration_seconds.to_le_bytes().to_vec(),
            now_ms,
        )
    }

    /// Approves or rejects one numeric-comparison challenge.
    pub fn pair_reply(
        &mut self,
        challenge_id: u32,
        approved: bool,
        now_ms: u64,
    ) -> Result<(), PairingError> {
        if self.device.negotiated_minor < 1 {
            return Err(PairingError::Unsupported);
        }
        if challenge_id == 0
            || self.challenge_id != Some(challenge_id)
            || self.pairing_status != PairingStatus::Challenge
            || self
                .pairing_deadline_ms
                .is_none_or(|deadline| now_ms >= deadline)
        {
            return Err(PairingError::InvalidArgument);
        }
        if self.state() != HostState::Pairing {
            return Err(PairingError::NotLocal);
        }
        if self.pending.is_some() {
            return Err(PairingError::Busy);
        }
        let mut payload = vec![0xa3, 1];
        encode_cbor_u32(challenge_id, &mut payload);
        payload.extend_from_slice(&[2, 0, 3, if approved { 0xf5 } else { 0xf4 }]);
        self.issue_aux(MessageKind::PairReply, payload, now_ms)?;
        self.challenge_id = None;
        self.comparison_value = None;
        Ok(())
    }

    /// Cancels the current pairing window on the same session.
    pub fn pair_cancel(&mut self, now_ms: u64) -> Result<(), PairingError> {
        if self.device.negotiated_minor < 1 {
            return Err(PairingError::Unsupported);
        }
        if self.state() != HostState::Pairing {
            return Err(PairingError::NotLocal);
        }
        if self.pending.is_some() {
            return Err(PairingError::Busy);
        }
        self.issue_aux(MessageKind::PairCancel, vec![], now_ms)
    }

    /// Drains a bounded batch from the capture hook and advances serial I/O.
    pub fn drive(&mut self, receiver: &Receiver<CaptureEvent>, now_ms: u64) {
        for _ in 0..64 {
            match receiver.try_recv() {
                Ok(event) => self.on_capture(event, now_ms),
                Err(TryRecvError::Empty) => break,
                Err(TryRecvError::Disconnected) => {
                    self.fail(HostFault::Capture);
                    return;
                }
            }
        }
        self.poll(now_ms);
    }

    /// Sends or queues one UI, device, or physical-hotkey selection request.
    pub fn request(&mut self, action: Action, now_ms: u64) {
        if self.fault.is_some() {
            return;
        }
        if self.pending.as_ref().is_some_and(|p| {
            matches!(
                p.frame.kind,
                MessageKind::PairBegin | MessageKind::PairReply | MessageKind::PairCancel
            )
        }) {
            return;
        }
        if self.pairing {
            if action == Action::Local {
                let _ = self.pair_cancel(now_ms);
            }
            return;
        }
        let Some(router) = self.router.as_mut() else {
            return;
        };
        let command = router.request(action, now_ms);
        if let Some(command) = command {
            self.capture.disarm();
            self.all_up = false;
            self.baseline_sent = false;
            self.clear_input();
            self.issue(command, now_ms);
        }
    }

    /// Reports a verified physical all-up baseline from the Windows input ledger.
    /// Guest capture remains disarmed until an exact ARM ACK has also arrived.
    pub fn observe_all_up(&mut self) {
        if self.fault.is_some() {
            return;
        }
        self.all_up = self.capture.physical_all_up();
        self.arm_capture_if_ready();
    }

    /// Processes one bounded capture event without replaying pointer deltas.
    pub fn on_capture(&mut self, event: CaptureEvent, now_ms: u64) {
        if let PhysicalEvent::Hotkey(action) = event.event {
            if action == Action::Local || event.generation == self.capture.generation() {
                self.request(action, now_ms);
            }
            return;
        }
        if self.fault.is_some()
            || event.generation == 0
            || event.generation != self.confirmed_generation
            || self.capture.generation() != event.generation
            || !self
                .router
                .as_ref()
                .is_some_and(RequestActor::can_forward_input)
        {
            return;
        }
        match event.event {
            PhysicalEvent::Key {
                virtual_key,
                scan_code,
                extended,
                down,
                repeat,
            } => {
                if !repeat && self.update_key(virtual_key, scan_code, extended, down) {
                    self.send_key_state();
                }
            }
            PhysicalEvent::Motion(dx, dy) => self.send_motion(dx, dy, 0, 0),
            PhysicalEvent::Button(button, down) => {
                let bit = match button {
                    MouseButton::Left => 1,
                    MouseButton::Right => 2,
                    MouseButton::Middle => 4,
                    MouseButton::X1 => 8,
                    MouseButton::X2 => 16,
                };
                if down {
                    self.buttons |= bit;
                } else {
                    self.buttons &= !bit;
                }
                self.send_motion(0, 0, 0, 0);
            }
            PhysicalEvent::Wheel(axis, units) => {
                let residual = match axis {
                    MouseAxis::Vertical => &mut self.wheel_residual,
                    MouseAxis::Horizontal => &mut self.pan_residual,
                };
                *residual += i32::from(units);
                let mut steps = *residual / 120;
                *residual %= 120;
                while steps != 0 {
                    let chunk = steps.clamp(i8::MIN as i32, i8::MAX as i32) as i8;
                    match axis {
                        MouseAxis::Vertical => self.send_motion(0, 0, chunk, 0),
                        MouseAxis::Horizontal => self.send_motion(0, 0, 0, chunk),
                    }
                    if self.fault.is_some() {
                        return;
                    }
                    steps -= i32::from(chunk);
                }
            }
            PhysicalEvent::Hotkey(_) => {}
        }
    }

    /// Advances heartbeat, bounded control retry, route timeout, and serial input.
    pub fn poll(&mut self, now_ms: u64) {
        if self.fault.is_some() {
            return;
        }
        self.capture.actor_heartbeat();
        if self
            .pairing_deadline_ms
            .is_some_and(|deadline| now_ms >= deadline)
        {
            self.pairing_status = PairingStatus::Timeout;
            self.pairing_deadline_ms = None;
            self.challenge_id = None;
            self.comparison_value = None;
            self.pairing = false;
        }
        if self.capture.fault().is_some() {
            self.fail(HostFault::Capture);
            return;
        }
        if now_ms.saturating_sub(self.last_peer) >= PEER_TIMEOUT_MS {
            self.fail(HostFault::Timeout);
            return;
        }
        if let Some(router) = self.router.as_mut()
            && router.tick(now_ms).is_some()
        {
            self.fail(HostFault::Timeout);
            return;
        }
        if now_ms.saturating_sub(self.last_heartbeat) >= HEARTBEAT_MS {
            if self
                .send(
                    MessageKind::Heartbeat,
                    self.confirmed_generation,
                    now_ms.to_le_bytes().to_vec(),
                )
                .is_err()
            {
                self.fail(HostFault::Transport);
                return;
            }
            self.last_heartbeat = now_ms;
        }
        if now_ms.saturating_sub(self.last_status_request) >= STATUS_REQUEST_MS {
            if self
                .send(MessageKind::GetStatus, self.confirmed_generation, vec![])
                .is_err()
            {
                self.fail(HostFault::Transport);
                return;
            }
            self.last_status_request = now_ms;
        }
        if self.pending.as_ref().is_some_and(|p| now_ms >= p.deadline) {
            let Some(mut pending) = self.pending.take() else {
                unreachable!()
            };
            if pending.retries == 2 {
                self.fail(HostFault::Timeout);
                return;
            }
            pending.retries += 1;
            pending.deadline = now_ms.saturating_add(CONTROL_RETRY_MS);
            if self.write_frame(&pending.frame).is_err() {
                self.fail(HostFault::Transport);
                return;
            }
            self.pending = Some(pending);
        }
        let mut bytes = [0_u8; 512];
        for _ in 0..8 {
            match self.stream.read(&mut bytes) {
                Ok(0) => {
                    self.fail(HostFault::Transport);
                    return;
                }
                Ok(count) => {
                    for byte in bytes[..count].iter().copied() {
                        if let Some(result) = self.decoder.push(byte) {
                            match result {
                                Ok(frame) => self.handle_frame(frame, now_ms),
                                Err(_) => self.fail(HostFault::Protocol),
                            }
                            if self.fault.is_some() {
                                return;
                            }
                        }
                    }
                }
                Err(error)
                    if matches!(
                        error.kind(),
                        io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut
                    ) =>
                {
                    break;
                }
                Err(_) => {
                    self.fail(HostFault::Transport);
                    return;
                }
            }
        }
        if matches!(self.state(), HostState::Guest(_)) && self.capture.generation() == 0 {
            self.observe_all_up();
        }
    }

    fn handle_frame(&mut self, frame: Frame, now_ms: u64) {
        if frame.session_id != self.device.session_id {
            self.fail(HostFault::Session);
            return;
        }
        match frame.kind {
            MessageKind::Status => self.handle_status(frame, now_ms),
            MessageKind::Ack | MessageKind::Nack => self.handle_ack(frame, now_ms),
            MessageKind::InputProgress => self.last_peer = now_ms,
            MessageKind::DeviceSelectRequest => {
                self.last_peer = now_ms;
                let action = if frame.payload[0] == 0 {
                    Action::Local
                } else {
                    Action::Direct(frame.payload[0])
                };
                self.request(action, now_ms);
            }
            _ => self.fail(HostFault::Protocol),
        }
    }

    fn handle_status(&mut self, frame: Frame, now_ms: u64) {
        let Ok(status) = Status::parse(&frame.payload, self.device.negotiated_minor) else {
            self.fail(HostFault::Protocol);
            return;
        };
        if frame.route_generation != status.generation {
            self.fail(HostFault::Protocol);
            return;
        }
        self.last_peer = now_ms;
        if self.router.is_none() {
            if !matches!(status.state, 0 | 5) || status.selected != 0 {
                self.fail(HostFault::Protocol);
                return;
            }
            self.confirmed_generation = status.generation;
            self.router = Some(RequestActor::with_generation(
                self.order.clone(),
                status.generation,
            ));
        } else if self.pending.is_none() && status.generation != self.confirmed_generation {
            self.fail(HostFault::Acknowledgment);
            return;
        }
        if self.pending.is_none() {
            let agrees = match self.router.as_ref().map(RequestActor::state) {
                Some(State::Local) => matches!(status.state, 0 | 5) && status.selected == 0,
                Some(State::Guest(slot)) => status.state == 2 && status.selected == slot,
                _ => false,
            };
            if !agrees {
                self.fail(HostFault::Protocol);
                return;
            }
        }
        self.pairing = status.state == 5;
        self.pairing_status = status.pairing_status;
        self.pairing_deadline_ms = status
            .pairing_remaining_ms
            .map(|remaining| now_ms.saturating_add(u64::from(remaining)));
        self.challenge_id = status.challenge_id;
        self.comparison_value = status.comparison_value;
        self.slots = status.slots.clone();
        let mut next = None;
        if let Some(router) = self.router.as_mut() {
            for slot in self.order.iter().copied() {
                if slot <= self.device.max_connections {
                    let ready = status
                        .slots
                        .iter()
                        .any(|seen| seen.slot == slot && seen.ready && seen.subscribed);
                    next = router.set_ready(slot, ready).or(next);
                }
            }
            if matches!(router.state(), State::Guest(_)) && status.state != 2 {
                self.fail(HostFault::Protocol);
                return;
            }
        }
        if let Some(command) = next {
            self.capture.disarm();
            self.all_up = false;
            self.baseline_sent = false;
            self.clear_input();
            self.issue(command, now_ms);
        }
    }

    fn handle_ack(&mut self, frame: Frame, now_ms: u64) {
        let Some(pending) = self.pending.as_ref() else {
            return;
        };
        let original_kind = frame.payload[0];
        let original_seq = u32::from_le_bytes(frame.payload[1..5].try_into().unwrap());
        let result_generation = u32::from_le_bytes(frame.payload[6..10].try_into().unwrap());
        if frame.seq != pending.frame.seq {
            return;
        }
        if original_seq != pending.frame.seq {
            self.fail(HostFault::Acknowledgment);
            return;
        }
        if original_kind != pending.frame.kind as u8
            || result_generation != pending.expected_generation
            || frame.route_generation != result_generation
        {
            self.fail(HostFault::Acknowledgment);
            return;
        }
        if frame.kind == MessageKind::Nack {
            self.fail(HostFault::Rejected);
            return;
        }
        let kind = pending.frame.kind;
        self.pending = None;
        self.confirmed_generation = result_generation;
        self.last_peer = now_ms;
        let next = match (self.router.as_mut(), kind) {
            (Some(router), MessageKind::ReleaseAll) => router.release_ack(result_generation),
            (Some(router), MessageKind::Switch) => router.switch_ack(result_generation),
            (Some(router), MessageKind::Arm) => router.arm_ack(result_generation, now_ms),
            (_, MessageKind::PairBegin | MessageKind::PairReply | MessageKind::PairCancel) => None,
            _ => {
                self.fail(HostFault::Protocol);
                return;
            }
        };
        if let Some(command) = next {
            self.issue(command, now_ms);
        } else if kind == MessageKind::Arm {
            self.load_profile_for_active_guest();
            self.observe_all_up();
        }
    }

    fn issue(&mut self, command: Command, now_ms: u64) {
        let (kind, header_generation, payload, expected_generation) = match command {
            Command::ReleaseAll { generation } => (
                MessageKind::ReleaseAll,
                generation.wrapping_sub(1),
                vec![],
                generation,
            ),
            Command::Select {
                slot,
                expected_generation,
                new_generation,
            } => {
                let mut payload = vec![slot];
                payload.extend_from_slice(&expected_generation.to_le_bytes());
                payload.extend_from_slice(&new_generation.to_le_bytes());
                (
                    MessageKind::Switch,
                    expected_generation,
                    payload,
                    new_generation,
                )
            }
            Command::Arm { slot, generation } => {
                let mut payload = vec![slot];
                payload.extend_from_slice(&generation.to_le_bytes());
                (MessageKind::Arm, generation, payload, generation)
            }
        };
        let frame = self.make_frame(kind, header_generation, payload);
        if self.write_frame(&frame).is_err() {
            self.fail(HostFault::Transport);
            return;
        }
        self.pending = Some(Pending {
            frame,
            expected_generation,
            retries: 0,
            deadline: now_ms.saturating_add(CONTROL_RETRY_MS),
        });
    }

    fn issue_aux(
        &mut self,
        kind: MessageKind,
        payload: Vec<u8>,
        now_ms: u64,
    ) -> Result<(), PairingError> {
        let frame = self.make_frame(kind, self.confirmed_generation, payload);
        if self.write_frame(&frame).is_err() {
            self.fail(HostFault::Transport);
            return Err(PairingError::Transport);
        }
        self.pending = Some(Pending {
            frame,
            expected_generation: self.confirmed_generation,
            retries: 0,
            deadline: now_ms.saturating_add(CONTROL_RETRY_MS),
        });
        Ok(())
    }

    fn send(
        &mut self,
        kind: MessageKind,
        generation: u32,
        payload: Vec<u8>,
    ) -> Result<(), HostFault> {
        let frame = self.make_frame(kind, generation, payload);
        self.write_frame(&frame).map_err(|_| HostFault::Transport)
    }

    fn make_frame(&mut self, kind: MessageKind, generation: u32, payload: Vec<u8>) -> Frame {
        let seq = self.next_seq;
        self.next_seq = self.next_seq.wrapping_add(1);
        Frame::new(kind, self.device.session_id, seq, generation, payload)
    }

    fn write_frame(&mut self, frame: &Frame) -> Result<(), HostFault> {
        let bytes = frame.encode().map_err(|_| HostFault::Protocol)?;
        self.stream
            .write_all(&bytes)
            .map_err(|_| HostFault::Transport)?;
        self.stream.flush().map_err(|_| HostFault::Transport)
    }

    fn arm_capture_if_ready(&mut self) {
        if !self.all_up
            || !self.capture.physical_all_up()
            || !matches!(self.state(), HostState::Guest(_))
        {
            return;
        }
        if !self.baseline_sent {
            for (kind, payload) in [
                (MessageKind::KeyState, vec![0; 8]),
                (MessageKind::Pointer, vec![0; 7]),
                (MessageKind::ConsumerState, vec![0; 2]),
            ] {
                if self.send(kind, self.confirmed_generation, payload).is_err() {
                    self.fail(HostFault::Transport);
                    return;
                }
            }
            self.baseline_sent = true;
        }
        let Some(router) = self.router.as_mut() else {
            return;
        };
        router.observe_all_released();
        if self.capture.generation() == 0 && !self.capture.arm(self.confirmed_generation) {
            if self.capture.physical_all_up() {
                self.fail(HostFault::Capture);
            } else {
                self.all_up = false;
            }
        }
    }

    fn load_profile_for_active_guest(&mut self) {
        let profile = if let HostState::Guest(slot) = self.state() {
            self.slots
                .iter()
                .find(|seen| seen.slot == slot)
                .and_then(|seen| self.profiles.get(&seen.bond_token))
                .cloned()
                .unwrap_or_default()
        } else {
            MappingProfile::default()
        };
        // Every profile was validated before storage, and clear_input ran at route start.
        self.mapping
            .set_profile(profile)
            .expect("stored profile is valid");
    }

    fn update_key(&mut self, virtual_key: u32, scan_code: u32, extended: bool, down: bool) -> bool {
        let identity = (scan_code, extended);
        if down {
            if self.keys.contains_key(&identity) {
                return false;
            }
            let Some(mapped) = self.mapper.map_key(virtual_key, scan_code, extended) else {
                return false;
            };
            let usage = match mapped {
                MappedKey::Usage(usage) if usage != 0 => usage,
                MappedKey::Modifier(bit) if bit.is_power_of_two() => {
                    0xe0 + bit.trailing_zeros() as u8
                }
                _ => return false,
            };
            let side = match usage {
                0xe0..=0xe3 => Side::Left,
                0xe4..=0xe7 => Side::Right,
                _ => Side::Unspecified,
            };
            let source = SourceKey { usage, side };
            self.keys.insert(identity, source);
            self.mapping.press(source);
            true
        } else if let Some(source) = self.keys.remove(&identity) {
            self.mapping.release(source);
            true
        } else {
            false
        }
    }

    fn send_key_state(&mut self) {
        let payload = self.mapping.report().bytes().to_vec();
        if self
            .send(MessageKind::KeyState, self.confirmed_generation, payload)
            .is_err()
        {
            self.fail(HostFault::Transport);
        }
    }

    fn send_motion(&mut self, dx: i32, dy: i32, wheel: i8, pan: i8) {
        let (mut x, mut y) = (dx, dy);
        let mut first = true;
        loop {
            let chunk_x = x.clamp(i16::MIN as i32, i16::MAX as i32) as i16;
            let chunk_y = y.clamp(i16::MIN as i32, i16::MAX as i32) as i16;
            let mut payload = vec![self.buttons];
            payload.extend_from_slice(&chunk_x.to_le_bytes());
            payload.extend_from_slice(&chunk_y.to_le_bytes());
            payload.push(if first { wheel as u8 } else { 0 });
            payload.push(if first { pan as u8 } else { 0 });
            if self
                .send(MessageKind::Pointer, self.confirmed_generation, payload)
                .is_err()
            {
                self.fail(HostFault::Transport);
                return;
            }
            x -= i32::from(chunk_x);
            y -= i32::from(chunk_y);
            if x == 0 && y == 0 {
                return;
            }
            first = false;
        }
    }

    fn clear_input(&mut self) {
        self.keys.clear();
        self.mapping.reset_for_route();
        self.buttons = 0;
        self.wheel_residual = 0;
        self.pan_residual = 0;
    }

    fn fail(&mut self, fault: HostFault) {
        self.capture.disarm();
        self.clear_input();
        self.all_up = false;
        self.baseline_sent = false;
        self.pending = None;
        self.decoder.reset();
        if let Some(router) = self.router.as_mut() {
            router.link_lost();
        }
        self.fault = Some(fault);
    }

    /// Ends the current session for sleep or desktop-session changes. A fresh
    /// verified actor is required after resume; queued input is discarded.
    pub fn suspend(&mut self) {
        self.stop(HostFault::Suspended);
    }

    /// Ends the current session for a clean application exit.
    pub fn shutdown(&mut self) {
        self.stop(HostFault::Stopped);
    }

    fn stop(&mut self, reason: HostFault) {
        if self.fault.is_some() {
            self.capture.disarm();
            return;
        }
        self.capture.disarm();
        if self.router.is_some() {
            let _ = self.send(MessageKind::ReleaseAll, self.confirmed_generation, vec![]);
        }
        self.fail(reason);
    }
}

impl<S: Read + Write> Drop for HostActor<S> {
    fn drop(&mut self) {
        self.capture.disarm();
    }
}

/// Finds the verified ESP32 KVM USB identity, negotiates the protocol on that
/// same open serial stream, and requests firmware STATUS before routing.
/// The caller must retain and drive the returned actor on one native thread.
pub fn connect_system(
    expected_board_id: &str,
    host_version: &str,
    capture: Box<dyn CaptureControl>,
    order: Vec<u8>,
    mapper: Box<dyn KeyMapper>,
    now_ms: u64,
) -> Result<HostActor<Box<dyn serialport::SerialPort>>, ConnectError> {
    let ports = available_usb_ports().map_err(ConnectError::Enumeration)?;
    let mut last_probe_error = None;
    for name in candidate_ports(&ports) {
        let Ok(mut stream) = serialport::new(name, 115_200)
            .timeout(std::time::Duration::from_millis(20))
            .open()
        else {
            continue;
        };
        let device = match probe_stream(&mut *stream, expected_board_id, host_version) {
            Ok(device) => device,
            Err(error) => {
                last_probe_error = Some(error);
                continue;
            }
        };
        return HostActor::from_confirmed(stream, capture, device, order, mapper, now_ms)
            .map_err(ConnectError::Actor);
    }
    Err(last_probe_error.map_or(ConnectError::NoConfirmedDevice, ConnectError::Probe))
}

/// Discovery or actor startup failure before a routing session exists.
#[derive(Debug)]
pub enum ConnectError {
    /// The OS failed to list serial devices.
    Enumeration(serialport::Error),
    /// No USB identity candidate completed the verified protocol handshake.
    NoConfirmedDevice,
    /// A USB identity candidate did not complete the protocol handshake.
    Probe(ProbeError),
    /// The confirmed session could not request initial STATUS.
    Actor(HostFault),
}

struct Status {
    state: u8,
    selected: u8,
    slots: Vec<SlotSnapshot>,
    generation: u32,
    pairing_status: PairingStatus,
    pairing_remaining_ms: Option<u32>,
    challenge_id: Option<u32>,
    comparison_value: Option<u32>,
}

impl Status {
    fn parse(bytes: &[u8], minor: u16) -> Result<Self, ProtocolError> {
        let mut c = Cbor { bytes, at: 0 };
        c.expect(if minor >= 1 { 0xa6 } else { 0xa5 })?;
        c.expect(1)?;
        let state = c.uint()?;
        c.expect(2)?;
        let selected = c.uint()?;
        c.expect(3)?;
        let count = c.array_len()?;
        if count > 3 {
            return Err(ProtocolError::Payload);
        }
        let mut slots: Vec<SlotSnapshot> = Vec::new();
        for _ in 0..count {
            c.expect(0xa5)?;
            c.expect(1)?;
            let slot = c.uint()?;
            c.expect(2)?;
            c.expect(0x50)?;
            let bond_token: [u8; 16] = c.bytes(16)?.try_into().unwrap();
            c.expect(3)?;
            let is_ready = c.bool()?;
            c.expect(4)?;
            let subscribed = c.bool()?;
            c.expect(5)?;
            let _interval = c.uint()?;
            if slot == 0 || slot > 3 || slots.iter().any(|seen| u64::from(seen.slot) == slot) {
                return Err(ProtocolError::Payload);
            }
            slots.push(SlotSnapshot {
                slot: slot as u8,
                bond_token,
                ready: is_ready,
                subscribed,
            });
        }
        c.expect(4)?;
        let _errors = c.uint()?;
        c.expect(5)?;
        let generation = c.uint()?;
        let mut pairing_status = PairingStatus::Unsupported;
        let mut pairing_remaining_ms = None;
        let mut challenge_id = None;
        let mut comparison_value = None;
        if minor >= 1 {
            c.expect(6)?;
            let len = c.byte()?;
            if !(0xa1..=0xa4).contains(&len) {
                return Err(ProtocolError::Payload);
            }
            c.expect(1)?;
            pairing_status = match c.uint()? {
                0 => PairingStatus::Closed,
                1 => PairingStatus::Waiting,
                2 => PairingStatus::Challenge,
                3 => PairingStatus::Rejected,
                4 => PairingStatus::Capacity,
                5 => PairingStatus::Timeout,
                _ => return Err(ProtocolError::Payload),
            };
            match pairing_status {
                PairingStatus::Waiting | PairingStatus::Challenge => {
                    if (pairing_status == PairingStatus::Waiting && len != 0xa2)
                        || (pairing_status == PairingStatus::Challenge && len != 0xa4)
                    {
                        return Err(ProtocolError::Payload);
                    }
                    c.expect(2)?;
                    let remaining = c.uint()?;
                    if !(1..=60000).contains(&remaining) {
                        return Err(ProtocolError::Payload);
                    }
                    pairing_remaining_ms = Some(remaining as u32);
                    if pairing_status == PairingStatus::Challenge {
                        c.expect(3)?;
                        let id = c.uint()?;
                        c.expect(4)?;
                        let number = c.uint()?;
                        if id == 0 || id > u32::MAX as u64 || number > 999999 {
                            return Err(ProtocolError::Payload);
                        }
                        challenge_id = Some(id as u32);
                        comparison_value = Some(number as u32);
                    }
                }
                _ if len != 0xa1 => return Err(ProtocolError::Payload),
                _ => {}
            }
        }
        if c.at != bytes.len() || state > 6 || selected > 3 || generation > u32::MAX as u64 {
            return Err(ProtocolError::Payload);
        }
        if minor >= 1
            && (state == 5)
                != matches!(
                    pairing_status,
                    PairingStatus::Waiting | PairingStatus::Challenge
                )
        {
            return Err(ProtocolError::Payload);
        }
        Ok(Self {
            state: state as u8,
            selected: selected as u8,
            slots,
            generation: generation as u32,
            pairing_status,
            pairing_remaining_ms,
            challenge_id,
            comparison_value,
        })
    }
}

struct Cbor<'a> {
    bytes: &'a [u8],
    at: usize,
}
impl Cbor<'_> {
    fn byte(&mut self) -> Result<u8, ProtocolError> {
        let result = *self.bytes.get(self.at).ok_or(ProtocolError::Payload)?;
        self.at += 1;
        Ok(result)
    }
    fn expect(&mut self, expected: u8) -> Result<(), ProtocolError> {
        if self.byte()? == expected {
            Ok(())
        } else {
            Err(ProtocolError::Payload)
        }
    }
    fn uint(&mut self) -> Result<u64, ProtocolError> {
        let initial = self.byte()?;
        if initial >> 5 != 0 {
            return Err(ProtocolError::Payload);
        }
        match initial & 31 {
            v @ 0..=23 => Ok(u64::from(v)),
            24 => Ok(u64::from(self.byte()?)),
            25 => Ok(u64::from(u16::from_be_bytes(
                self.bytes(2)?.try_into().unwrap(),
            ))),
            26 => Ok(u64::from(u32::from_be_bytes(
                self.bytes(4)?.try_into().unwrap(),
            ))),
            27 => Ok(u64::from_be_bytes(self.bytes(8)?.try_into().unwrap())),
            _ => Err(ProtocolError::Payload),
        }
    }
    fn array_len(&mut self) -> Result<usize, ProtocolError> {
        let initial = self.byte()?;
        if initial >> 5 == 4 && initial & 31 <= 3 {
            Ok(usize::from(initial & 31))
        } else {
            Err(ProtocolError::Payload)
        }
    }
    fn bytes(&mut self, count: usize) -> Result<&[u8], ProtocolError> {
        let end = self.at.checked_add(count).ok_or(ProtocolError::Payload)?;
        let value = self.bytes.get(self.at..end).ok_or(ProtocolError::Payload)?;
        self.at = end;
        Ok(value)
    }
    fn bool(&mut self) -> Result<bool, ProtocolError> {
        match self.byte()? {
            0xf4 => Ok(false),
            0xf5 => Ok(true),
            _ => Err(ProtocolError::Payload),
        }
    }
}

fn encode_cbor_u32(value: u32, output: &mut Vec<u8>) {
    match value {
        0..=23 => output.push(value as u8),
        24..=255 => output.extend_from_slice(&[0x18, value as u8]),
        256..=65535 => {
            output.push(0x19);
            output.extend_from_slice(&(value as u16).to_be_bytes());
        }
        _ => {
            output.push(0x1a);
            output.extend_from_slice(&value.to_be_bytes());
        }
    }
}
