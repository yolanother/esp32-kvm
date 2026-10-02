// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Defines bounded Windows physical-input capture and handoff to a native routing actor.
// Keyboard/buttons/wheels and physical shortcuts use hooks; relative motion
// uses Raw Input, outside the webview.
use esp32_kvm_input_core::Action;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::mpsc::{Receiver, SyncSender, TrySendError, sync_channel};

#[cfg(windows)]
mod windows;
#[cfg(windows)]
pub use windows::{CaptureService, CaptureStartError};

/// Physical mouse button identity.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MouseButton {
    /// Primary button.
    Left,
    /// Secondary button.
    Right,
    /// Middle button.
    Middle,
    /// First auxiliary button.
    X1,
    /// Second auxiliary button.
    X2,
}
/// Mouse wheel axis.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MouseAxis {
    /// Vertical scroll wheel.
    Vertical,
    /// Horizontal scroll wheel.
    Horizontal,
}
/// A low-level mouse hook event; movement is used only for suppression.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MouseInput {
    /// Legacy cursor movement, used only for suppression.
    Move,
    /// Button identity and press state.
    Button(MouseButton, bool),
    /// Scroll axis and signed wheel units.
    Wheel(MouseAxis, i16),
}
/// Physical input to be interpreted by the native routing actor.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PhysicalEvent {
    /// Physical shortcut action, recognized before guest key mapping.
    Hotkey(Action),
    /// Physical scan code and metadata, including repeated key-down events.
    Key {
        virtual_key: u32,
        scan_code: u32,
        extended: bool,
        down: bool,
        repeat: bool,
    },
    /// Relative pointer movement.
    Motion(i32, i32),
    /// Button transition.
    Button(MouseButton, bool),
    /// Wheel delta in Windows wheel units.
    Wheel(MouseAxis, i16),
}
/// An event tagged with the route generation active when it was observed.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CaptureEvent {
    /// Generation sampled when the event was observed.
    pub generation: u32,
    /// The captured physical input.
    pub event: PhysicalEvent,
}
/// Hook action for local Windows delivery and native actor handoff.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CaptureDecision {
    /// Whether the hook must call the next hook for local delivery.
    pub pass_to_os: bool,
    /// Event to offer to the routing actor, if any.
    pub event: Option<PhysicalEvent>,
}

/// Classify a keyboard hook notification without allocation or I/O.
pub fn classify_keyboard(
    armed: bool,
    injected: bool,
    virtual_key: u32,
    scan_code: u32,
    extended: bool,
    down: bool,
    repeat: bool,
) -> CaptureDecision {
    // Windows emits 0x021D as a synthesized left Ctrl around AltGr. It must
    // neither reach the local app nor become an independent guest Ctrl press.
    if armed && (virtual_key == 0x11 || virtual_key == 0xa2) && scan_code == 0x021d {
        return CaptureDecision {
            pass_to_os: false,
            event: None,
        };
    }
    if !armed || injected {
        return CaptureDecision {
            pass_to_os: true,
            event: None,
        };
    }
    CaptureDecision {
        pass_to_os: false,
        event: Some(PhysicalEvent::Key {
            virtual_key,
            scan_code,
            extended,
            down,
            repeat,
        }),
    }
}
/// Classify a mouse hook notification; hook movement never becomes a forwarded delta.
pub fn classify_mouse(armed: bool, injected: bool, input: MouseInput) -> CaptureDecision {
    if !armed || injected {
        return CaptureDecision {
            pass_to_os: true,
            event: None,
        };
    }
    let event = match input {
        MouseInput::Move => None,
        MouseInput::Button(button, down) => Some(PhysicalEvent::Button(button, down)),
        MouseInput::Wheel(axis, delta) => Some(PhysicalEvent::Wheel(axis, delta)),
    };
    CaptureDecision {
        pass_to_os: false,
        event,
    }
}
/// The meaning of a Raw Input mouse packet.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RawMotionOutcome {
    /// No active route or no movement.
    Ignore,
    /// Relative X and Y deltas.
    Motion(i32, i32),
    /// Absolute input that requires local failover.
    UnsupportedAbsolute,
}
/// Accept only nonzero relative Raw Input deltas while armed.
pub fn classify_raw_motion(armed: bool, absolute: bool, dx: i32, dy: i32) -> RawMotionOutcome {
    if !armed || (dx == 0 && dy == 0 && !absolute) {
        RawMotionOutcome::Ignore
    } else if absolute {
        RawMotionOutcome::UnsupportedAbsolute
    } else {
        RawMotionOutcome::Motion(dx, dy)
    }
}
/// A capture fault requiring local failover and guest-state release by the routing actor.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u32)]
pub enum CaptureFault {
    /// The bounded event queue filled.
    QueueOverflow = 1,
    /// The routing actor's receiver closed.
    ReceiverClosed = 2,
    /// A mouse reported absolute movement.
    UnsupportedAbsoluteInput = 3,
    /// Windows returned invalid Raw Input data.
    RawInputFailure = 4,
    /// The capture message pump ended unexpectedly.
    MessagePumpFailure = 5,
    /// Editable shortcut configuration was unavailable to the hook.
    HotkeyConfigBusy = 6,
}

/// Atomic route gate and bounded sender shared by native callbacks.
pub struct CaptureGate {
    state: AtomicU64,
    sender: SyncSender<CaptureEvent>,
}
impl CaptureGate {
    /// Create a disarmed gate and receiver with positive queue capacity.
    pub fn new(capacity: usize) -> (Arc<Self>, Receiver<CaptureEvent>) {
        assert!(capacity > 0, "capture queue capacity must be positive");
        let (sender, receiver) = sync_channel(capacity);
        (
            Arc::new(Self {
                state: AtomicU64::new(0),
                sender,
            }),
            receiver,
        )
    }
    /// Arm a nonzero generation only from a fault-free disarmed state.
    pub fn arm(&self, generation: u32) -> bool {
        generation != 0
            && self
                .state
                .compare_exchange(
                    0,
                    u64::from(generation),
                    Ordering::AcqRel,
                    Ordering::Acquire,
                )
                .is_ok()
    }
    /// Stop suppression and forwarding immediately.
    pub fn disarm(&self) {
        self.state.fetch_and(!0xffff_ffff, Ordering::AcqRel);
    }
    /// Get the selected generation, or zero when disarmed.
    pub fn generation(&self) -> u32 {
        self.state.load(Ordering::Acquire) as u32
    }
    /// Read the first capture fault, if any.
    pub fn fault(&self) -> Option<CaptureFault> {
        match (self.state.load(Ordering::Acquire) >> 32) as u32 {
            1 => Some(CaptureFault::QueueOverflow),
            2 => Some(CaptureFault::ReceiverClosed),
            3 => Some(CaptureFault::UnsupportedAbsoluteInput),
            4 => Some(CaptureFault::RawInputFailure),
            5 => Some(CaptureFault::MessagePumpFailure),
            6 => Some(CaptureFault::HotkeyConfigBusy),
            _ => None,
        }
    }
    /// Clear a diagnosed fault while disarmed; stale events retain their old generation.
    pub fn clear_fault(&self) -> bool {
        let state = self.state.load(Ordering::Acquire);
        state as u32 == 0
            && self
                .state
                .compare_exchange(state, 0, Ordering::AcqRel, Ordering::Acquire)
                .is_ok()
    }
    /// Try a bounded send; a full or closed queue atomically disarms capture.
    pub fn offer(&self, event: PhysicalEvent) -> bool {
        let generation = self.generation();
        if generation == 0 {
            return false;
        }
        match self.sender.try_send(CaptureEvent { generation, event }) {
            Ok(()) => true,
            Err(TrySendError::Full(_)) => {
                self.mark_fault(CaptureFault::QueueOverflow);
                false
            }
            Err(TrySendError::Disconnected(_)) => {
                self.mark_fault(CaptureFault::ReceiverClosed);
                false
            }
        }
    }
    /// Send a recognized shortcut even when guest capture is locally disarmed.
    pub fn offer_hotkey(&self, action: Action) -> bool {
        let generation = self.generation();
        match self.sender.try_send(CaptureEvent {
            generation,
            event: PhysicalEvent::Hotkey(action),
        }) {
            Ok(()) => true,
            Err(TrySendError::Full(_)) => {
                self.mark_fault(CaptureFault::QueueOverflow);
                false
            }
            Err(TrySendError::Disconnected(_)) => {
                self.mark_fault(CaptureFault::ReceiverClosed);
                false
            }
        }
    }
    /// Record the first fault and force local pass-through in one atomic transition.
    pub fn mark_fault(&self, fault: CaptureFault) {
        let mut current = self.state.load(Ordering::Acquire);
        loop {
            let code = if current >> 32 == 0 {
                fault as u32
            } else {
                (current >> 32) as u32
            };
            let next = u64::from(code) << 32;
            match self.state.compare_exchange_weak(
                current,
                next,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(_) => return,
                Err(observed) => current = observed,
            }
        }
    }
}
