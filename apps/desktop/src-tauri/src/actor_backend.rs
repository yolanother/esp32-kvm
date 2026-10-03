// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Owns the desktop setup worker's one verified host actor and maps its STATUS
// into fail-closed Tauri setup facts. No UI thread directly opens a serial port.

use crate::setup::{BackendSnapshot, DeviceState, PairingState, RouteState, SetupBackend};
use esp32_kvm_host_actor::{
    ConnectError, HostActor, HostState, KeyMapper, MappedKey, PairingError,
    SetupSnapshot as ActorSnapshot, connect_system,
};
use esp32_kvm_input_core::Action;
use esp32_kvm_platform_windows::CaptureGate;
use esp32_kvm_usb_transport::{ProbeError, available_usb_ports, candidate_ports};
use std::sync::{
    Arc, Mutex,
    mpsc::{self, Receiver, SyncSender},
};
use std::thread;
use std::time::{Duration, Instant};

const EXPECTED_BOARD: &str = "esp32-kvm-s3";
const HOST_VERSION: &str = "0.1.0-m1";
const COMMAND_TIMEOUT: Duration = Duration::from_millis(500);

/// The native setup adapter: one worker owns and drives the confirmed serial stream.
pub struct ActorBackend {
    snapshot: Arc<Mutex<BackendSnapshot>>,
    commands: SyncSender<Command>,
}

enum CommandKind {
    Begin,
    Cancel,
    Local,
    Quit,
}

struct Command {
    kind: CommandKind,
    deadline: Instant,
    reply: SyncSender<Result<(), String>>,
}

/// This mapper cannot produce guest output while setup has no physical ledger.
struct NoGuestMapper;

impl KeyMapper for NoGuestMapper {
    fn map_key(&self, _virtual_key: u32, _scan_code: u32, _extended: bool) -> Option<MappedKey> {
        None
    }
}

impl ActorBackend {
    /// Starts a disarmed worker; its first verified session is opened on that thread.
    pub fn start() -> Self {
        let snapshot = Arc::new(Mutex::new(empty_snapshot(DeviceState::Missing)));
        let (commands, receiver) = mpsc::sync_channel(8);
        let worker_snapshot = Arc::clone(&snapshot);
        thread::Builder::new()
            .name("esp32-kvm-setup-actor".into())
            .spawn(move || worker(receiver, worker_snapshot))
            .expect("failed to start setup actor thread");
        Self { snapshot, commands }
    }

    fn request(&self, kind: CommandKind) -> Result<(), String> {
        let (reply, result) = mpsc::sync_channel(1);
        self.commands
            .try_send(Command {
                kind,
                deadline: Instant::now() + COMMAND_TIMEOUT,
                reply,
            })
            .map_err(|_| "Setup actor is busy or disconnected.".to_owned())?;
        result
            .recv_timeout(COMMAND_TIMEOUT)
            .map_err(|_| "Setup actor did not answer before the deadline.".to_owned())?
    }
}

impl SetupBackend for ActorBackend {
    fn snapshot(&self) -> Result<BackendSnapshot, String> {
        self.snapshot
            .lock()
            .map(|value| value.clone())
            .map_err(|error| error.to_string())
    }

    fn begin(&self) -> Result<(), String> {
        self.request(CommandKind::Begin)
    }

    fn cancel(&self) -> Result<(), String> {
        self.request(CommandKind::Cancel)
    }

    fn return_local(&self) -> Result<(), String> {
        self.request(CommandKind::Local)
    }

    fn release_for_exit(&self) -> Result<(), String> {
        self.request(CommandKind::Quit)
    }
}

fn empty_snapshot(device: DeviceState) -> BackendSnapshot {
    BackendSnapshot {
        device,
        route: RouteState::Local,
        pairing: PairingState::Closed,
        bond_tokens: Vec::new(),
        ready_tokens: Vec::new(),
        pairing_available: false,
    }
}

fn publish(shared: &Mutex<BackendSnapshot>, next: BackendSnapshot) {
    if let Ok(mut value) = shared.lock() {
        *value = next;
    }
}

fn map_snapshot(value: ActorSnapshot) -> BackendSnapshot {
    let active = value.state != HostState::Failed;
    let route = match value.state {
        HostState::AwaitStatus => RouteState::AwaitingStatus,
        HostState::Local => RouteState::Local,
        HostState::Pairing => RouteState::Pairing,
        HostState::Switching => RouteState::Switching,
        HostState::Guest(slot) => value
            .slots
            .iter()
            .find(|entry| entry.slot == slot)
            .map(|entry| RouteState::Guest {
                slot,
                bond_token: token_hex(&entry.bond_token),
            })
            .unwrap_or(RouteState::AwaitingStatus),
        HostState::Failed => RouteState::Failed {
            reason: value
                .fault
                .map(|fault| format!("{fault:?}").to_lowercase())
                .unwrap_or_else(|| "unknown".into()),
        },
    };
    let device = if active {
        DeviceState::Verified {
            board_id: value.board_id,
            firmware_version: value.firmware_version,
            max_bonds: value.max_bonds,
            max_connections: value.max_connections,
        }
    } else {
        DeviceState::Unavailable {
            reason: format!("Verified device session failed: {:?}", value.fault),
        }
    };
    let bond_tokens: Vec<String> = value
        .slots
        .iter()
        .map(|slot| token_hex(&slot.bond_token))
        .collect();
    let ready_tokens = value
        .slots
        .iter()
        .filter(|slot| active && slot.ready && slot.subscribed)
        .map(|slot| token_hex(&slot.bond_token))
        .collect();
    BackendSnapshot {
        device,
        route,
        pairing: if value.state == HostState::Pairing {
            PairingState::Waiting {
                deadline_ms: value.pairing_deadline_ms,
            }
        } else {
            PairingState::Closed
        },
        bond_tokens,
        ready_tokens,
        pairing_available: value.state == HostState::Local,
    }
}

fn token_hex(token: &[u8; 16]) -> String {
    token.iter().map(|byte| format!("{byte:02x}")).collect()
}

fn pair_error(error: PairingError) -> String {
    match error {
        PairingError::Busy => "Device is finishing another control request.",
        PairingError::NotLocal => "Pairing requires a verified local device session.",
        PairingError::InvalidArgument => "Pairing request is invalid.",
        PairingError::Transport => "Verified device session ended.",
    }
    .to_owned()
}

fn worker(receiver: Receiver<Command>, shared: Arc<Mutex<BackendSnapshot>>) {
    let (gate, capture_events) = CaptureGate::new(64);
    let started = Instant::now();
    let mut actor: Option<HostActor<Box<dyn serialport::SerialPort>>> = None;
    let mut next_scan = Instant::now();
    loop {
        match receiver.recv_timeout(Duration::from_millis(20)) {
            Ok(command) => {
                if matches!(command.kind, CommandKind::Quit) {
                    gate.disarm();
                    if let Some(current) = actor.as_mut() {
                        current.request(Action::Local, started.elapsed().as_millis() as u64);
                        let deadline = Instant::now() + Duration::from_millis(400);
                        while Instant::now() < deadline
                            && !matches!(current.state(), HostState::Local | HostState::Failed)
                        {
                            current.drive(&capture_events, started.elapsed().as_millis() as u64);
                        }
                    }
                    drop(actor.take());
                    publish(&shared, empty_snapshot(DeviceState::Missing));
                    let _ = command.reply.send(Ok(()));
                    break;
                }
                let outcome = if Instant::now() >= command.deadline {
                    Err("Pairing request expired before the actor could process it.".into())
                } else if let Some(current) = actor.as_mut() {
                    let now_ms = started.elapsed().as_millis() as u64;
                    match command.kind {
                        CommandKind::Begin => current.pair_begin(60, now_ms),
                        CommandKind::Cancel => current.pair_cancel(now_ms),
                        CommandKind::Local => {
                            current.request(Action::Local, now_ms);
                            Ok(())
                        }
                        CommandKind::Quit => unreachable!(),
                    }
                    .map_err(pair_error)
                } else if matches!(command.kind, CommandKind::Local) {
                    Ok(())
                } else {
                    Err("No verified device session is available.".into())
                };
                let _ = command.reply.send(outcome);
            }
            Err(mpsc::RecvTimeoutError::Disconnected) => break,
            Err(mpsc::RecvTimeoutError::Timeout) => {}
        }

        if let Some(current) = actor.as_mut() {
            current.drive(&capture_events, started.elapsed().as_millis() as u64);
            let next = map_snapshot(current.setup_snapshot());
            let failed = matches!(next.device, DeviceState::Unavailable { .. });
            publish(&shared, next);
            if failed {
                actor = None;
                next_scan = Instant::now() + Duration::from_secs(1);
            }
        } else if Instant::now() >= next_scan {
            next_scan = Instant::now() + Duration::from_secs(1);
            match available_usb_ports() {
                Err(error) => publish(
                    &shared,
                    empty_snapshot(DeviceState::Unavailable {
                        reason: error.to_string(),
                    }),
                ),
                Ok(ports) => match candidate_ports(&ports).first() {
                    None => publish(&shared, empty_snapshot(DeviceState::Missing)),
                    Some(port) => {
                        publish(
                            &shared,
                            empty_snapshot(DeviceState::Candidate {
                                port: (*port).to_owned(),
                            }),
                        );
                        publish(&shared, empty_snapshot(DeviceState::Handshaking));
                        match connect_system(
                            EXPECTED_BOARD,
                            HOST_VERSION,
                            Box::new(Arc::clone(&gate)),
                            Vec::new(),
                            Box::new(NoGuestMapper),
                            started.elapsed().as_millis() as u64,
                        ) {
                            Ok(connected) => actor = Some(connected),
                            Err(error) => publish(&shared, empty_snapshot(connect_error(error))),
                        }
                    }
                },
            }
        }
    }
}

fn connect_error(error: ConnectError) -> DeviceState {
    let reason = match error {
        ConnectError::Probe(ProbeError::BoardMismatch) => "wrong_board",
        ConnectError::Probe(ProbeError::VersionMismatch) => "protocol",
        _ => "unresponsive",
    };
    DeviceState::Incompatible {
        reason: reason.into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use esp32_kvm_host_actor::{HostState, SetupSnapshot as ActorSnapshot, SlotSnapshot};

    fn actor(state: HostState) -> ActorSnapshot {
        ActorSnapshot {
            board_id: "esp32-kvm-s3".into(),
            max_connections: 1,
            max_bonds: 8,
            firmware_version: None,
            state,
            slots: vec![SlotSnapshot {
                slot: 1,
                bond_token: [0xab; 16],
                ready: true,
                subscribed: true,
            }],
            pairing_deadline_ms: None,
            challenge_id: None,
            comparison_value: None,
            fault: None,
        }
    }

    #[test]
    fn verified_status_exposes_opaque_bonds_but_only_local_allows_begin() {
        let local = map_snapshot(actor(HostState::Local));
        assert!(local.pairing_available);
        assert_eq!(local.bond_tokens, vec!["ab".repeat(16)]);
        assert_eq!(local.ready_tokens, local.bond_tokens);
        assert!(matches!(local.route, crate::setup::RouteState::Local));
        let awaiting = map_snapshot(actor(HostState::AwaitStatus));
        assert!(!awaiting.pairing_available);
        assert!(matches!(
            awaiting.pairing,
            crate::setup::PairingState::Closed
        ));
        let pairing = map_snapshot(actor(HostState::Pairing));
        assert!(!pairing.pairing_available);
        assert!(matches!(
            pairing.pairing,
            crate::setup::PairingState::Waiting { .. }
        ));
        let guest = map_snapshot(actor(HostState::Guest(1)));
        assert!(matches!(
            guest.route,
            crate::setup::RouteState::Guest { slot: 1, .. }
        ));
    }

    #[test]
    fn guest_route_does_not_enable_setup_controls() {
        assert!(!map_snapshot(actor(HostState::Guest(1))).pairing_available);
        let failed = map_snapshot(actor(HostState::Failed));
        assert!(!failed.pairing_available);
        assert!(failed.ready_tokens.is_empty());
        assert!(matches!(
            failed.route,
            crate::setup::RouteState::Failed { .. }
        ));
    }
}
