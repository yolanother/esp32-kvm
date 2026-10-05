// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Owns the desktop's one verified host actor, native capture worker, and
// prepared host-edge routing, editable physical shortcuts, and opt-in idle
// guest pointer bumps. It maps STATUS into fail-closed Tauri setup facts;
// no UI thread opens serial or controls input timing.

use crate::layout::{EdgeGate, LayoutRuntime};
use crate::setup::{
    BackendSnapshot, DeviceState, PairingState, RouteState, SetupBackend, token_bytes,
};
use crate::switch_shortcut::{SwitchBinding, SwitchShortcutStore};
use esp32_kvm_host_actor::{
    CaptureControl, ConnectError, ForgetError, HostActor, HostState, InventoryError, PairingError,
    PairingStatus, SetOneKeyMapper, SetupSnapshot as ActorSnapshot, connect_system,
};
use esp32_kvm_input_core::Action;
use esp32_kvm_input_core::MappingProfile;
use esp32_kvm_platform_windows::{
    CaptureEvent, CaptureGate, CaptureService, PhysicalEvent, discover_monitors,
    foreground_fullscreen, physical_cursor_position,
};
use esp32_kvm_usb_transport::{ProbeError, available_usb_ports, candidate_ports};
use std::collections::BTreeMap;
use std::path::PathBuf;
use std::sync::{
    Arc, Mutex,
    atomic::{AtomicBool, Ordering},
    mpsc::{self, Receiver, SyncSender},
};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

const EXPECTED_BOARD: &str = "esp32-kvm-s3";
const HOST_VERSION: &str = "0.1.0-m1";
const COMMAND_TIMEOUT: Duration = Duration::from_millis(500);
const INVENTORY_TIMEOUT: Duration = Duration::from_millis(900);
const FORGET_TIMEOUT: Duration = Duration::from_millis(2500);
const KEEP_AWAKE_INTERVAL: Duration = Duration::from_secs(30);

/// The native setup adapter: one worker owns and drives the confirmed serial stream.
pub struct ActorBackend {
    snapshot: Arc<Mutex<BackendSnapshot>>,
    commands: SyncSender<Command>,
    capture: Arc<dyn CaptureControl + Send + Sync>,
    keep_awake: Arc<AtomicBool>,
    shortcut_store: Arc<Mutex<Result<SwitchShortcutStore, String>>>,
}

enum CommandKind {
    Begin,
    Cancel,
    Confirm {
        challenge_id: u32,
        approved: bool,
    },
    Local,
    Select {
        token: [u8; 16],
    },
    RefreshInventory,
    Forget {
        token: [u8; 16],
    },
    SetMapping {
        bond_token: [u8; 16],
        profile: MappingProfile,
    },
    SetSwitchShortcut {
        binding: SwitchBinding,
    },
    Quit,
}

struct Command {
    kind: CommandKind,
    deadline: Instant,
    reply: SyncSender<Result<(), String>>,
}

/// Keeps setup available if hooks fail while refusing every route arm.
struct DisabledCapture {
    gate: Arc<CaptureGate>,
}
impl CaptureControl for DisabledCapture {
    fn arm(&self, _generation: u32) -> bool {
        false
    }
    fn disarm(&self) {
        self.gate.disarm();
    }
    fn generation(&self) -> u32 {
        0
    }
    fn fault(&self) -> Option<esp32_kvm_platform_windows::CaptureFault> {
        None
    }
    fn physical_all_up(&self) -> bool {
        false
    }
}

impl ActorBackend {
    /// Starts a disarmed worker; its first verified session is opened on that thread.
    pub fn start(layout: Arc<Mutex<LayoutRuntime>>, directory: PathBuf) -> Self {
        let snapshot = Arc::new(Mutex::new(empty_snapshot(DeviceState::Missing)));
        let (commands, receiver) = mpsc::sync_channel(8);
        let worker_snapshot = Arc::clone(&snapshot);
        let (capture, capture_events, capture_running): (
            Arc<dyn CaptureControl + Send + Sync>,
            Receiver<CaptureEvent>,
            bool,
        ) = match CaptureService::start(64) {
            Ok((service, receiver)) => (Arc::new(service), receiver, true),
            Err(error) => {
                layout
                    .lock()
                    .unwrap_or_else(|poisoned| poisoned.into_inner())
                    .capture_failed(format!("Native capture worker could not start: {error}"));
                let (gate, receiver) = CaptureGate::new(64);
                (Arc::new(DisabledCapture { gate }), receiver, false)
            }
        };
        let worker_capture = Arc::clone(&capture);
        let mut stored_shortcut = SwitchShortcutStore::load(directory);
        if capture_running
            && let Ok(store) = stored_shortcut.as_ref()
            && let Ok(config) = store.current().validate()
            && !capture.set_hotkeys(config)
        {
            stored_shortcut =
                Err("Native capture could not install the saved cycle shortcut.".into());
        }
        let shortcut_store = Arc::new(Mutex::new(stored_shortcut));
        let worker_shortcut_store = Arc::clone(&shortcut_store);
        let keep_awake = Arc::new(AtomicBool::new(false));
        let worker_keep_awake = Arc::clone(&keep_awake);
        thread::Builder::new()
            .name("esp32-kvm-setup-actor".into())
            .spawn(move || {
                worker(
                    receiver,
                    worker_snapshot,
                    worker_capture,
                    capture_events,
                    layout,
                    capture_running,
                    worker_keep_awake,
                    worker_shortcut_store,
                )
            })
            .expect("failed to start setup actor thread");
        Self {
            snapshot,
            commands,
            capture,
            keep_awake,
            shortcut_store,
        }
    }

    fn request(&self, kind: CommandKind) -> Result<(), String> {
        let timeout = match &kind {
            CommandKind::RefreshInventory => INVENTORY_TIMEOUT,
            CommandKind::Forget { .. } => FORGET_TIMEOUT,
            CommandKind::SetSwitchShortcut { .. } => Duration::from_secs(2),
            _ => COMMAND_TIMEOUT,
        };
        let (reply, result) = mpsc::sync_channel(1);
        self.commands
            .try_send(Command {
                kind,
                deadline: Instant::now() + timeout,
                reply,
            })
            .map_err(|_| "Setup actor is busy or disconnected.".to_owned())?;
        result
            .recv_timeout(timeout)
            .map_err(|_| "Setup actor did not answer before the deadline.".to_owned())?
    }
}

impl SetupBackend for ActorBackend {
    fn cycle_shortcut(&self) -> Result<SwitchBinding, String> {
        self.shortcut_store
            .lock()
            .map_err(|error| error.to_string())?
            .as_ref()
            .map(|store| store.current().clone())
            .map_err(Clone::clone)
    }

    fn set_cycle_shortcut(&self, binding: SwitchBinding) -> Result<(), String> {
        self.request(CommandKind::SetSwitchShortcut { binding })
    }

    fn keep_awake_enabled(&self) -> bool {
        self.keep_awake.load(Ordering::Acquire)
    }

    fn set_keep_awake(&self, enabled: bool) {
        self.keep_awake.store(enabled, Ordering::Release);
    }
    fn snapshot(&self) -> Result<BackendSnapshot, String> {
        self.snapshot
            .lock()
            .map(|value| value.clone())
            .map_err(|error| error.to_string())
    }

    fn install_mapping(&self, bond_token: [u8; 16], profile: MappingProfile) -> Result<(), String> {
        let result = self.request(CommandKind::SetMapping {
            bond_token,
            profile,
        });
        if result.is_err() {
            self.capture.disarm();
        }
        result
    }

    fn refresh_inventory(&self) -> Result<BackendSnapshot, String> {
        self.request(CommandKind::RefreshInventory)?;
        self.snapshot()
    }

    fn forget_bond(&self, bond_token: &str) -> Result<(), String> {
        self.request(CommandKind::Forget {
            token: token_bytes(bond_token)?,
        })
    }

    fn begin(&self) -> Result<(), String> {
        self.request(CommandKind::Begin)
    }

    fn cancel(&self) -> Result<(), String> {
        self.request(CommandKind::Cancel)
    }

    fn confirm(&self, challenge_id: u32, approved: bool) -> Result<(), String> {
        self.request(CommandKind::Confirm {
            challenge_id,
            approved,
        })
    }

    fn return_local(&self) -> Result<(), String> {
        self.request(CommandKind::Local)
    }

    fn select_guest(&self, bond_token: [u8; 16]) -> Result<(), String> {
        self.request(CommandKind::Select { token: bond_token })
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
        retained_bond_tokens: None,
        connected_tokens: Vec::new(),
        ready_tokens: Vec::new(),
        pairing_available: false,
    }
}

fn publish(shared: &Mutex<BackendSnapshot>, next: BackendSnapshot) {
    if let Ok(mut value) = shared.lock() {
        *value = next;
    }
}

fn map_snapshot(value: ActorSnapshot, monotonic_ms: u64, wall_ms: u64) -> BackendSnapshot {
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
    let retained_bond_tokens = if active {
        value
            .retained_bonds
            .as_ref()
            .map(|tokens| tokens.iter().map(token_hex).collect())
    } else {
        None
    };
    let connected_tokens = value
        .slots
        .iter()
        .filter(|slot| active && slot.ready)
        .map(|slot| token_hex(&slot.bond_token))
        .collect();
    let ready_tokens = value
        .slots
        .iter()
        .filter(|slot| active && slot.ready && slot.subscribed)
        .map(|slot| token_hex(&slot.bond_token))
        .collect();
    let deadline_ms = value
        .pairing_deadline_ms
        .map(|deadline| wall_ms.saturating_add(deadline.saturating_sub(monotonic_ms).min(60_000)));
    let pairing = if !active {
        PairingState::Closed
    } else {
        match value.pairing_status {
            PairingStatus::Unsupported => PairingState::Unsupported {
                reason: "Firmware does not support verified pairing challenge events.".into(),
            },
            PairingStatus::Closed => PairingState::Closed,
            PairingStatus::Waiting => PairingState::Waiting { deadline_ms },
            PairingStatus::Challenge => match (value.challenge_id, value.comparison_value) {
                (Some(challenge_id), Some(number)) => PairingState::Challenge {
                    challenge_id,
                    number,
                    deadline_ms,
                },
                _ => PairingState::Failed {
                    reason: "Firmware challenge data is incomplete.".into(),
                },
            },
            PairingStatus::Rejected => PairingState::Failed {
                reason: "Pairing was rejected.".into(),
            },
            PairingStatus::Capacity => PairingState::Full,
            PairingStatus::Timeout => PairingState::Expired,
        }
    };
    BackendSnapshot {
        device,
        route,
        pairing,
        bond_tokens,
        retained_bond_tokens,
        connected_tokens,
        ready_tokens,
        pairing_available: value.state == HostState::Local
            && value.pairing_status != PairingStatus::Unsupported,
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
        PairingError::Unsupported => "Firmware does not support verified pairing challenge events.",
    }
    .to_owned()
}

fn inventory_error(error: InventoryError) -> String {
    match error {
        InventoryError::Unsupported => "Firmware does not support retained-bond inventory.".into(),
        InventoryError::Busy => "Device is finishing another control request.".into(),
        InventoryError::Host(fault) => format!("Retained-bond inventory failed: {fault:?}"),
    }
}

fn forget_error(error: ForgetError) -> String {
    match error {
        ForgetError::Busy => "Device is finishing another control request.".into(),
        ForgetError::NotLocal => "Return to local control before removing a bond.".into(),
        ForgetError::UnknownBond => "Invalid bond identity token.".into(),
        ForgetError::Unsupported => "Firmware does not support retained-bond inventory.".into(),
        ForgetError::AlreadyAbsent => "Firmware inventory already excludes this bond; local profile was kept for reconciliation.".into(),
        ForgetError::StillBonded => "Firmware still retains this bond; local profile was kept.".into(),
        ForgetError::Host(fault) => format!("Bond removal was not confirmed: {fault:?}"),
    }
}

/// Resolves one HID-ready identity from the latest authoritative slot table.
fn ready_slot(snapshot: &ActorSnapshot, token: &[u8; 16]) -> Option<u8> {
    snapshot
        .slots
        .iter()
        .find(|slot| &slot.bond_token == token && slot.ready && slot.subscribed)
        .map(|slot| slot.slot)
}

/// A continuous idle window belongs to one exact authenticated guest route.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct BumpTarget {
    slot: u8,
    bond_token: [u8; 16],
    generation: u32,
}

/// Resolves an eligible route only while capture is healthy and physical input is released.
fn bump_target(
    snapshot: &ActorSnapshot,
    capture_running: bool,
    generation: u32,
    capture_healthy: bool,
    physical_all_up: bool,
) -> Option<BumpTarget> {
    if !capture_running || generation == 0 || !capture_healthy || !physical_all_up {
        return None;
    }
    let HostState::Guest(slot) = snapshot.state else {
        return None;
    };
    snapshot
        .slots
        .iter()
        .find(|seen| seen.slot == slot && seen.ready && seen.subscribed)
        .map(|seen| BumpTarget {
            slot,
            bond_token: seen.bond_token,
            generation,
        })
}

/// Requires one uninterrupted eligible interval for the same guest and generation.
fn bump_due(
    since: &mut Option<(BumpTarget, Instant)>,
    target: Option<BumpTarget>,
    now: Instant,
) -> bool {
    let Some(target) = target else {
        *since = None;
        return false;
    };
    match since {
        Some((previous, started)) if *previous == target => {
            if now.duration_since(*started) >= KEEP_AWAKE_INTERVAL {
                *since = None;
                true
            } else {
                false
            }
        }
        _ => {
            *since = Some((target, now));
            false
        }
    }
}

/// Emits a cancelling X pair after thirty seconds on a confirmed guest route.
fn keep_guest_awake(
    actor: &mut HostActor<Box<dyn serialport::SerialPort>>,
    capture: &dyn CaptureControl,
    capture_running: bool,
    enabled: bool,
    since: &mut Option<(BumpTarget, Instant)>,
    now_ms: u64,
) {
    let target = enabled
        .then(|| {
            bump_target(
                &actor.setup_snapshot(),
                capture_running,
                capture.generation(),
                capture.fault().is_none(),
                capture.physical_all_up(),
            )
        })
        .flatten();
    let now = Instant::now();
    if !bump_due(since, target, now) {
        return;
    }
    let target = target.expect("an eligible target is required when a bump is due");
    actor.on_capture(
        CaptureEvent {
            generation: target.generation,
            event: PhysicalEvent::Motion(1, 0),
        },
        now_ms,
    );
    if bump_target(
        &actor.setup_snapshot(),
        capture_running,
        capture.generation(),
        capture.fault().is_none(),
        capture.physical_all_up(),
    ) == Some(target)
    {
        actor.on_capture(
            CaptureEvent {
                generation: target.generation,
                event: PhysicalEvent::Motion(-1, 0),
            },
            now_ms,
        );
    }
}

fn worker(
    receiver: Receiver<Command>,
    shared: Arc<Mutex<BackendSnapshot>>,
    capture: Arc<dyn CaptureControl + Send + Sync>,
    capture_events: Receiver<CaptureEvent>,
    layout: Arc<Mutex<LayoutRuntime>>,
    capture_running: bool,
    keep_awake: Arc<AtomicBool>,
    shortcut_store: Arc<Mutex<Result<SwitchShortcutStore, String>>>,
) {
    let started = Instant::now();
    let mut actor: Option<HostActor<Box<dyn serialport::SerialPort>>> = None;
    let mut mappings = BTreeMap::<[u8; 16], MappingProfile>::new();
    let mut next_scan = Instant::now();
    let mut next_topology_scan = Instant::now();
    let mut keep_awake_since = None;
    loop {
        match receiver.recv_timeout(Duration::from_millis(20)) {
            Ok(command) => {
                let publish_after = matches!(
                    &command.kind,
                    CommandKind::RefreshInventory | CommandKind::Forget { .. }
                );
                if matches!(command.kind, CommandKind::Quit) {
                    capture.disarm();
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
                } else if let CommandKind::SetMapping {
                    bond_token,
                    profile,
                } = command.kind
                {
                    let applied = if let Some(current) = actor.as_mut() {
                        current
                            .set_guest_profile(bond_token, profile.clone())
                            .map_err(|error| format!("Invalid native mapping: {error:?}"))
                    } else {
                        Ok(())
                    };
                    applied.map(|()| {
                        mappings.insert(bond_token, profile);
                    })
                } else if let CommandKind::SetSwitchShortcut { binding } = command.kind {
                    let config = binding.validate();
                    if actor
                        .as_ref()
                        .is_some_and(|current| current.state() != HostState::Local)
                        || capture.generation() != 0
                    {
                        Err("Return to local control before changing the cycle shortcut.".into())
                    } else if !capture_running || capture.fault().is_some() {
                        Err("Native input capture is unavailable.".into())
                    } else if !capture.physical_all_up() {
                        Err("Release every physical key and mouse button, then retry.".into())
                    } else {
                        config.and_then(|config| {
                            let mut guard =
                                shortcut_store.lock().map_err(|error| error.to_string())?;
                            let store = guard.as_mut().map_err(|error| error.clone())?;
                            let previous = store.current().validate()?;
                            if !capture.set_hotkeys(config) {
                                return Err("Native capture rejected the cycle shortcut.".into());
                            }
                            if let Err(error) = store.save(binding) {
                                let _ = capture.set_hotkeys(previous);
                                return Err(error);
                            }
                            Ok(())
                        })
                    }
                } else if let Some(current) = actor.as_mut() {
                    let now_ms = started.elapsed().as_millis() as u64;
                    match command.kind {
                        CommandKind::Begin => current.pair_begin(60, now_ms).map_err(pair_error),
                        CommandKind::Cancel => current.pair_cancel(now_ms).map_err(pair_error),
                        CommandKind::Confirm {
                            challenge_id,
                            approved,
                        } => current
                            .pair_reply(challenge_id, approved, now_ms)
                            .map_err(pair_error),
                        CommandKind::Local => {
                            current.request(Action::Local, now_ms);
                            Ok(())
                        }
                        CommandKind::Select { token } => {
                            let snapshot = current.setup_snapshot();
                            if snapshot.state != HostState::Local {
                                Err("Return to local control before selecting a guest.".into())
                            } else if !capture_running || capture.fault().is_some() {
                                Err("Native input capture is unavailable.".into())
                            } else if !capture.physical_all_up() {
                                Err("Release all physical keys and buttons, then retry.".into())
                            } else if let Some(slot) = ready_slot(&snapshot, &token) {
                                current.request(Action::Direct(slot), now_ms);
                                Ok(())
                            } else {
                                Err("Guest is offline or HID is not ready.".into())
                            }
                        }
                        CommandKind::RefreshInventory => current
                            .refresh_bond_inventory(now_ms)
                            .map(|_| ())
                            .map_err(inventory_error),
                        CommandKind::Forget { token } => {
                            current.forget_bond(token, now_ms).map_err(forget_error)
                        }
                        CommandKind::SetMapping { .. } => unreachable!(),
                        CommandKind::SetSwitchShortcut { .. } => unreachable!(),
                        CommandKind::Quit => unreachable!(),
                    }
                } else if matches!(command.kind, CommandKind::Local) {
                    Ok(())
                } else {
                    Err("No verified device session is available.".into())
                };
                if publish_after && let Some(current) = actor.as_ref() {
                    let now_ms = started.elapsed().as_millis() as u64;
                    let wall_ms = SystemTime::now()
                        .duration_since(UNIX_EPOCH)
                        .unwrap_or_default()
                        .as_millis() as u64;
                    publish(
                        &shared,
                        map_snapshot(current.setup_snapshot(), now_ms, wall_ms),
                    );
                }
                let _ = command.reply.send(outcome);
            }
            Err(mpsc::RecvTimeoutError::Disconnected) => break,
            Err(mpsc::RecvTimeoutError::Timeout) => {}
        }

        if let Some(current) = actor.as_mut() {
            current.drive(&capture_events, started.elapsed().as_millis() as u64);
            edge_tick(
                current,
                &layout,
                capture.as_ref(),
                capture_running,
                &mut next_topology_scan,
                started.elapsed().as_millis() as u64,
            );
            keep_guest_awake(
                current,
                capture.as_ref(),
                capture_running,
                keep_awake.load(Ordering::Acquire),
                &mut keep_awake_since,
                started.elapsed().as_millis() as u64,
            );
            let now_ms = started.elapsed().as_millis() as u64;
            let wall_ms = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap_or_default()
                .as_millis() as u64;
            let next = map_snapshot(current.setup_snapshot(), now_ms, wall_ms);
            let failed = matches!(next.device, DeviceState::Unavailable { .. });
            publish(&shared, next);
            if failed {
                capture.disarm();
                layout
                    .lock()
                    .unwrap_or_else(|error| error.into_inner())
                    .set_gate(EdgeGate::default());
                actor = None;
                keep_awake_since = None;
                next_scan = Instant::now() + Duration::from_secs(1);
            }
        } else if Instant::now() >= next_scan {
            keep_awake_since = None;
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
                            Box::new(Arc::clone(&capture)),
                            Vec::new(),
                            Box::new(SetOneKeyMapper),
                            started.elapsed().as_millis() as u64,
                        ) {
                            Ok(mut connected) => {
                                if mappings.iter().all(|(token, profile)| {
                                    connected.set_guest_profile(*token, profile.clone()).is_ok()
                                }) {
                                    actor = Some(connected);
                                } else {
                                    publish(
                                        &shared,
                                        empty_snapshot(DeviceState::Unavailable {
                                            reason: "Saved mapping could not be restored.".into(),
                                        }),
                                    );
                                }
                            }
                            Err(error) => publish(&shared, empty_snapshot(connect_error(error))),
                        }
                    }
                },
            }
        }
    }
}

/// Samples the physical host edge only on the one serial-owning actor thread.
fn edge_tick(
    actor: &mut HostActor<Box<dyn serialport::SerialPort>>,
    layout: &Mutex<LayoutRuntime>,
    capture: &dyn CaptureControl,
    capture_running: bool,
    next_topology_scan: &mut Instant,
    now_ms: u64,
) {
    let mut request_local = false;
    let mut selection = None;
    {
        let mut layout = layout.lock().unwrap_or_else(|error| error.into_inner());
        if Instant::now() >= *next_topology_scan {
            *next_topology_scan = Instant::now() + Duration::from_millis(100);
            let before = layout.status().generation;
            match discover_monitors() {
                Ok(monitors) => {
                    if layout.refresh(monitors).is_err() || layout.status().generation != before {
                        request_local =
                            !matches!(actor.state(), HostState::Local | HostState::AwaitStatus);
                    }
                }
                Err(error) => {
                    layout.invalidate(error);
                    request_local =
                        !matches!(actor.state(), HostState::Local | HostState::AwaitStatus);
                }
            }
        }
        let snapshot = actor.setup_snapshot();
        if let HostState::Guest(slot) = snapshot.state
            && !snapshot
                .slots
                .iter()
                .any(|seen| seen.slot == slot && seen.ready && seen.subscribed)
        {
            request_local = true;
        }
        let gate = edge_gate(
            &snapshot,
            capture_running && capture.fault().is_none(),
            capture.physical_all_up(),
            foreground_fullscreen().unwrap_or(true),
        );
        layout.set_gate(gate.clone());
        if !request_local && gate.local {
            match physical_cursor_position() {
                Ok((x, y)) => selection = layout.observe_cursor(x, y, now_ms, &gate),
                Err(error) => {
                    layout.cursor_failed(error);
                }
            }
        }
    }
    if request_local {
        capture.disarm();
        actor.request(Action::Local, now_ms);
    } else if let Some(action) = selection {
        actor.request(action, now_ms);
    }
}

/// Projects verified actor slot readiness and physical capture facts into edge guards.
fn edge_gate(
    snapshot: &ActorSnapshot,
    capture_running: bool,
    physical_all_up: bool,
    fullscreen_active: bool,
) -> EdgeGate {
    EdgeGate {
        capture_running,
        local: snapshot.state == HostState::Local,
        physical_all_up,
        fullscreen_active,
        ready_slots: snapshot
            .slots
            .iter()
            .filter(|slot| slot.ready && slot.subscribed)
            .map(|slot| (token_hex(&slot.bond_token), slot.slot))
            .collect(),
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
    use esp32_kvm_host_actor::{
        HostState, PairingStatus, SetupSnapshot as ActorSnapshot, SlotSnapshot,
    };

    fn actor(state: HostState) -> ActorSnapshot {
        ActorSnapshot {
            board_id: "esp32-kvm-s3".into(),
            max_connections: 1,
            max_bonds: 8,
            firmware_version: None,
            pairing_status: if state == HostState::Pairing {
                PairingStatus::Waiting
            } else {
                PairingStatus::Closed
            },
            state,
            slots: vec![SlotSnapshot {
                slot: 1,
                bond_token: [0xab; 16],
                ready: true,
                subscribed: true,
            }],
            retained_bonds: Some(vec![[0xcd; 16]]),
            retained_bonds_observed_ms: Some(0),
            pairing_deadline_ms: None,
            challenge_id: None,
            comparison_value: None,
            fault: None,
        }
    }

    #[test]
    fn edge_gate_uses_only_ready_subscribed_verified_slots() {
        let mut snapshot = actor(HostState::Local);
        let gate = edge_gate(&snapshot, true, true, false);
        assert_eq!(gate.ready_slots, vec![("ab".repeat(16), 1)]);
        snapshot.slots[0].subscribed = false;
        assert!(
            edge_gate(&snapshot, true, true, false)
                .ready_slots
                .is_empty()
        );
        snapshot.slots[0].subscribed = true;
        snapshot.slots[0].ready = false;
        assert!(
            edge_gate(&snapshot, true, true, false)
                .ready_slots
                .is_empty()
        );
        assert!(!edge_gate(&actor(HostState::Guest(1)), true, true, false).local);
        assert!(!edge_gate(&actor(HostState::Local), false, true, false).capture_running);
    }

    #[test]
    fn guest_selection_resolves_only_a_live_subscribed_token() {
        let mut snapshot = actor(HostState::Local);
        assert_eq!(ready_slot(&snapshot, &[0xab; 16]), Some(1));
        assert_eq!(ready_slot(&snapshot, &[0xcd; 16]), None);
        snapshot.slots[0].subscribed = false;
        assert_eq!(ready_slot(&snapshot, &[0xab; 16]), None);
    }

    #[test]
    fn idle_bump_requires_a_healthy_captured_guest_with_no_physical_input() {
        let mut snapshot = actor(HostState::Guest(1));
        let target = bump_target(&snapshot, true, 9, true, true).unwrap();
        assert_eq!(target.slot, 1);
        assert_eq!(target.bond_token, [0xab; 16]);
        assert_eq!(target.generation, 9);
        assert_eq!(bump_target(&snapshot, true, 0, true, true), None);
        assert_eq!(bump_target(&snapshot, true, 9, true, false), None);
        assert_eq!(bump_target(&snapshot, true, 9, false, true), None);
        assert_eq!(bump_target(&snapshot, false, 9, true, true), None);
        snapshot.slots[0].subscribed = false;
        assert_eq!(bump_target(&snapshot, true, 9, true, true), None);
        snapshot.slots[0].subscribed = true;
        snapshot.state = HostState::Local;
        assert_eq!(bump_target(&snapshot, true, 9, true, true), None);
        snapshot.state = HostState::Guest(1);
        assert_ne!(bump_target(&snapshot, true, 10, true, true), Some(target));
        snapshot.slots[0].bond_token = [0xcd; 16];
        assert_ne!(bump_target(&snapshot, true, 9, true, true), Some(target));
    }

    #[test]
    fn idle_bump_timer_restarts_after_release_or_guest_reconnection() {
        let target = BumpTarget {
            slot: 1,
            bond_token: [0xab; 16],
            generation: 9,
        };
        let started = Instant::now();
        let mut since = None;
        assert!(!bump_due(&mut since, Some(target), started));
        assert!(!bump_due(
            &mut since,
            None,
            started + Duration::from_secs(20)
        ));
        assert!(!bump_due(
            &mut since,
            Some(target),
            started + Duration::from_secs(21)
        ));
        assert!(!bump_due(
            &mut since,
            Some(target),
            started + Duration::from_secs(49)
        ));
        let reconnected = BumpTarget {
            generation: 10,
            ..target
        };
        assert!(!bump_due(
            &mut since,
            Some(reconnected),
            started + Duration::from_secs(50)
        ));
        assert!(!bump_due(
            &mut since,
            Some(reconnected),
            started + Duration::from_secs(79)
        ));
        assert!(bump_due(
            &mut since,
            Some(reconnected),
            started + Duration::from_secs(80)
        ));
        assert_eq!(since, None);
    }

    #[test]
    fn verified_status_exposes_opaque_bonds_but_only_local_allows_begin() {
        let local = map_snapshot(actor(HostState::Local), 0, 1_000);
        assert!(local.pairing_available);
        assert_eq!(local.bond_tokens, vec!["ab".repeat(16)]);
        assert_eq!(local.retained_bond_tokens, Some(vec!["cd".repeat(16)]));
        assert_eq!(local.ready_tokens, local.bond_tokens);
        assert_eq!(local.connected_tokens, local.bond_tokens);
        assert!(matches!(local.route, crate::setup::RouteState::Local));
        let awaiting = map_snapshot(actor(HostState::AwaitStatus), 0, 1_000);
        assert!(!awaiting.pairing_available);
        assert!(matches!(
            awaiting.pairing,
            crate::setup::PairingState::Closed
        ));
        let pairing = map_snapshot(actor(HostState::Pairing), 0, 1_000);
        assert!(!pairing.pairing_available);
        assert!(matches!(
            pairing.pairing,
            crate::setup::PairingState::Waiting { .. }
        ));
        let guest = map_snapshot(actor(HostState::Guest(1)), 0, 1_000);
        assert!(matches!(
            guest.route,
            crate::setup::RouteState::Guest { slot: 1, .. }
        ));
    }

    #[test]
    fn guest_route_does_not_enable_setup_controls() {
        assert!(!map_snapshot(actor(HostState::Guest(1)), 0, 1_000).pairing_available);
        let failed = map_snapshot(actor(HostState::Failed), 0, 1_000);
        assert!(!failed.pairing_available);
        assert!(failed.ready_tokens.is_empty());
        assert!(failed.connected_tokens.is_empty());
        assert!(matches!(
            failed.route,
            crate::setup::RouteState::Failed { .. }
        ));
    }

    #[test]
    fn challenge_uses_wall_clock_deadline_and_unsupported_stays_closed() {
        let mut challenge = actor(HostState::Pairing);
        challenge.pairing_status = PairingStatus::Challenge;
        challenge.pairing_deadline_ms = Some(10_000);
        challenge.challenge_id = Some(7);
        challenge.comparison_value = Some(123_456);
        let mapped = map_snapshot(challenge, 1_000, 100_000);
        assert!(matches!(
            mapped.pairing,
            PairingState::Challenge {
                challenge_id: 7,
                number: 123_456,
                deadline_ms: Some(109_000)
            }
        ));

        let mut old = actor(HostState::Local);
        old.pairing_status = PairingStatus::Unsupported;
        let mapped = map_snapshot(old, 0, 100_000);
        assert!(!mapped.pairing_available);
        assert!(matches!(mapped.pairing, PairingState::Unsupported { .. }));

        let mut failed = actor(HostState::Failed);
        failed.pairing_status = PairingStatus::Challenge;
        failed.challenge_id = Some(7);
        failed.comparison_value = Some(123_456);
        assert!(matches!(
            map_snapshot(failed, 0, 100_000).pairing,
            PairingState::Closed
        ));
    }

    #[test]
    fn connected_peer_without_subscription_is_not_hid_ready() {
        let mut value = actor(HostState::Local);
        value.slots[0].subscribed = false;
        let snapshot = map_snapshot(value, 0, 0);
        assert_eq!(snapshot.connected_tokens, vec!["ab".repeat(16)]);
        assert!(snapshot.ready_tokens.is_empty());
    }
}
