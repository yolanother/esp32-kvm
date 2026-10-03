// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Models the release-first USB update transaction with exact control ACKs,
// app-only flash authorization, interruption recovery, and disarmed reconnect.

use crate::{DeviceIdentity, VerifiedImage};
use esp32_kvm_protocol::{Frame, MessageKind};
use sha2::{Digest, Sha256};

/// One observable phase of the update operation.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum UpdateState {
    /// RELEASE_ALL must receive an exact ACK; local capture is already disarmed.
    AwaitRelease,
    /// UPDATE_PREPARE must receive an exact ACK.
    AwaitPrepare,
    /// An app-only flasher may be started after checking bytes again.
    ReadyToFlash,
    /// An external app-only flasher is working.
    Flashing,
    /// Flash completed; wait for a fresh verified session and local STATUS.
    AwaitReconnect,
    /// Expected firmware reconnected, locally disarmed.
    Complete,
    /// Manual recovery or a new verified update attempt is required.
    RecoveryRequired,
}

/// The only actions an integration layer may perform after a valid transition.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum UpdateAction {
    /// Disarm local capture before sending this RELEASE_ALL frame.
    DisarmAndRelease(Frame),
    /// Send UPDATE_PREPARE after the exact release ACK.
    SendPrepare(Frame),
    /// Start only an app-partition flash with independently verified bytes.
    FlashApp {
        partition: &'static str,
        image_len: usize,
    },
    /// Re-enumerate and open a new verified USB session, remaining local.
    WaitForReconnect,
}

/// A transition could not safely authorize flashing or reconnect.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum UpdateError {
    /// Session, sequence, or generation cannot advance safely.
    InvalidStart,
    /// Event does not belong to the current update phase.
    InvalidState,
    /// Reply belongs to an older command and was ignored.
    StaleResponse,
    /// Firmware rejected the current control command.
    ControlRejected,
    /// Current reply has the wrong session, kind, sequence, or generation.
    ControlMismatch,
    /// App bytes changed after manifest preflight.
    ImageChanged,
    /// Flash stopped or update control timed out.
    Interrupted,
    /// Fresh device identity, version, or local STATUS differs from the plan.
    ReconnectMismatch,
}

/// Pure update coordinator; it never opens USB, erases NVS, or emits ARM.
pub struct UpdateMachine {
    plan: VerifiedImage,
    state: UpdateState,
    session_id: u64,
    release_seq: u32,
    generation: u32,
}

impl UpdateMachine {
    /// Starts from a verified manifest and device, requiring local capture to
    /// disarm before the returned RELEASE_ALL frame is sent.
    pub fn begin(
        plan: VerifiedImage,
        session_id: u64,
        generation: u32,
        sequence: u32,
    ) -> Result<(Self, UpdateAction), UpdateError> {
        if session_id == 0 || generation == u32::MAX || sequence == u32::MAX {
            return Err(UpdateError::InvalidStart);
        }
        let frame = Frame::new(
            MessageKind::ReleaseAll,
            session_id,
            sequence,
            generation,
            vec![],
        );
        Ok((
            Self {
                plan,
                state: UpdateState::AwaitRelease,
                session_id,
                release_seq: sequence,
                generation,
            },
            UpdateAction::DisarmAndRelease(frame),
        ))
    }

    /// Returns the current fail-closed update phase.
    pub fn state(&self) -> UpdateState {
        self.state
    }

    /// Processes one ACK/NACK for the current release or prepare command.
    /// Stale sequence replies are ignored; malformed current replies require recovery.
    pub fn on_response(&mut self, response: &Frame) -> Result<UpdateAction, UpdateError> {
        let (kind, seq, expected_generation) = match self.state {
            UpdateState::AwaitRelease => (
                MessageKind::ReleaseAll,
                self.release_seq,
                self.generation + 1,
            ),
            UpdateState::AwaitPrepare => (
                MessageKind::UpdatePrepare,
                self.release_seq + 1,
                self.generation + 1,
            ),
            _ => return Err(UpdateError::InvalidState),
        };
        if response.seq < seq {
            return Err(UpdateError::StaleResponse);
        }
        if response.seq > seq {
            self.state = UpdateState::RecoveryRequired;
            return Err(UpdateError::ControlMismatch);
        }
        let payload = &response.payload;
        let original_seq = payload
            .get(1..5)
            .and_then(|bytes| bytes.try_into().ok())
            .map(u32::from_le_bytes);
        let resulting_generation = payload
            .get(6..10)
            .and_then(|bytes| bytes.try_into().ok())
            .map(u32::from_le_bytes);
        if response.session_id != self.session_id
            || payload.len() != 10
            || payload[0] != kind as u8
            || original_seq != Some(seq)
            || resulting_generation != Some(expected_generation)
            || response.route_generation != expected_generation
            || !matches!(response.kind, MessageKind::Ack | MessageKind::Nack)
        {
            self.state = UpdateState::RecoveryRequired;
            return Err(UpdateError::ControlMismatch);
        }
        if response.kind == MessageKind::Nack || payload[5] != 0 {
            self.state = UpdateState::RecoveryRequired;
            return Err(UpdateError::ControlRejected);
        }
        if self.state == UpdateState::AwaitRelease {
            self.state = UpdateState::AwaitPrepare;
            let frame = Frame::new(
                MessageKind::UpdatePrepare,
                self.session_id,
                self.release_seq + 1,
                expected_generation,
                vec![],
            );
            Ok(UpdateAction::SendPrepare(frame))
        } else {
            self.state = UpdateState::ReadyToFlash;
            Ok(UpdateAction::FlashApp {
                partition: "app",
                image_len: self.plan.image_len,
            })
        }
    }

    /// Begins the external app-only flash only if the original bytes are still intact.
    pub fn flash_started(&mut self, image: &[u8]) -> Result<(), UpdateError> {
        if self.state != UpdateState::ReadyToFlash {
            return Err(UpdateError::InvalidState);
        }
        if image.len() != self.plan.image_len
            || Sha256::digest(image).as_slice() != self.plan.image_sha256
        {
            self.state = UpdateState::RecoveryRequired;
            return Err(UpdateError::ImageChanged);
        }
        self.state = UpdateState::Flashing;
        Ok(())
    }

    /// Records the external flasher's result; interruption requires manual recovery.
    pub fn flash_finished(&mut self, succeeded: bool) -> Result<UpdateAction, UpdateError> {
        if self.state != UpdateState::Flashing {
            return Err(UpdateError::InvalidState);
        }
        if !succeeded {
            self.state = UpdateState::RecoveryRequired;
            return Err(UpdateError::Interrupted);
        }
        self.state = UpdateState::AwaitReconnect;
        Ok(UpdateAction::WaitForReconnect)
    }

    /// Accepts only the expected firmware on a new verified, locally disarmed session.
    pub fn reconnected(&mut self, device: &DeviceIdentity) -> Result<(), UpdateError> {
        if self.state != UpdateState::AwaitReconnect {
            return Err(UpdateError::InvalidState);
        }
        if device.board_id != self.plan.board_id
            || device.protocol_major != self.plan.protocol_major
            || device.protocol_minor < self.plan.protocol_minor_min
            || device.protocol_minor > self.plan.protocol_minor_max
            || device.firmware_version != self.plan.firmware_version
            || !device.local
            || device.armed
        {
            self.state = UpdateState::RecoveryRequired;
            return Err(UpdateError::ReconnectMismatch);
        }
        self.state = UpdateState::Complete;
        Ok(())
    }

    /// Terminally disarms the transaction after a missing control response.
    pub fn control_timeout(&mut self) {
        self.state = UpdateState::RecoveryRequired;
    }
}
