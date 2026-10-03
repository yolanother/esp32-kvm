// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Consumes the sole verified host USB actor to disarm capture and obtain exact release and
// update-prepare acknowledgments before handing verified app bytes to an external flasher.
// A fresh verified, local, disarmed session is required after flashing; this module never flashes.

use super::{HostActor, HostFault, HostState};
use esp32_kvm_protocol::{FrameDecoder, MAJOR, MessageKind};
use esp32_kvm_update_core::{
    DeviceIdentity, ManifestError, UpdateAction, UpdateError, UpdateMachine, VerificationError,
    parse_manifest, verify_image,
};
use esp32_kvm_usb_transport::ConfirmedDevice;
use std::io::{self, Read, Write};
use std::thread;
use std::time::{Duration, Instant};

/// Factory app partition offset from the checked-in `firmware/partitions.csv`.
pub const FACTORY_APP_OFFSET: u32 = 0x20000;
/// Factory app partition capacity from the checked-in `firmware/partitions.csv`.
pub const FACTORY_APP_CAPACITY: u64 = 0x650000;
const CONTROL_TIMEOUT: Duration = Duration::from_millis(500);

/// A release artifact or USB/flash transition could not safely finish.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum HostUpdateError {
    /// Actor was not in a stable local or guest state with no pending command.
    Busy,
    /// Manifest schema or app-only policy failed.
    Manifest(ManifestError),
    /// Image, board, protocol, capacity, or hash failed preflight.
    Image(VerificationError),
    /// Exact release or prepare ACK failed, or image changed before flash.
    Control(UpdateError),
    /// The verified actor's serial session failed.
    Transport(HostFault),
    /// The app-only flasher reported failure or interruption.
    Flash,
    /// Re-enumeration or local/disarmed identity verification failed.
    Reconnect,
}

/// A bounded app-only flash request; no NVS, bootloader, or full-chip target is expressible.
pub struct AppFlashRequest<'a> {
    bytes: &'a [u8],
}

impl<'a> AppFlashRequest<'a> {
    /// Returns the only permitted partition class.
    pub fn partition(&self) -> &'static str {
        "app"
    }

    /// Returns the checked-in factory app offset, subject to native partition-table verification.
    pub fn offset(&self) -> u32 {
        FACTORY_APP_OFFSET
    }

    /// Returns the checked-in factory app capacity.
    pub fn capacity(&self) -> u64 {
        FACTORY_APP_CAPACITY
    }

    /// Returns bytes rehashed by the update machine immediately before the flasher call.
    pub fn bytes(&self) -> &'a [u8] {
        self.bytes
    }
}

/// The external app-partition write failed or was interrupted.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum FlashError {
    /// No completed app-only write can be proven.
    Failed,
}

/// External flash adapter implemented only after board partition and recovery validation.
pub trait AppFlasher {
    /// Writes exactly the supplied bytes to the checked-in app partition; never erases NVS.
    fn flash_app(&mut self, request: AppFlashRequest<'_>) -> Result<(), FlashError>;
}

/// A fresh verified USB session could not be established after flashing.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ReconnectError {
    /// No matching board completed CAPS, SESSION_OPEN, and a fresh local STATUS.
    Unavailable,
}

/// Re-probes USB after flashing and returns facts from fresh CAPS, SESSION_OPEN, and STATUS.
pub trait Reconnector {
    /// Returns a new confirmed session and matching local/disarmed STATUS identity.
    fn reconnect(
        &mut self,
        expected_board_id: &str,
    ) -> Result<(ConfirmedDevice, DeviceIdentity), ReconnectError>;
}

/// A prepared, released update with the original verified bytes; the serial actor was consumed.
pub struct FlashHandoff {
    machine: UpdateMachine,
    image: Vec<u8>,
    board_id: String,
    old_session_id: u64,
}

impl FlashHandoff {
    /// Rechecks image bytes, invokes only the app flasher, then verifies fresh local reconnect.
    pub fn flash_and_reconnect(
        mut self,
        flasher: &mut impl AppFlasher,
        reconnect: &mut impl Reconnector,
    ) -> Result<(), HostUpdateError> {
        self.machine
            .flash_started(&self.image)
            .map_err(HostUpdateError::Control)?;
        if flasher
            .flash_app(AppFlashRequest { bytes: &self.image })
            .is_err()
        {
            let _ = self.machine.flash_finished(false);
            return Err(HostUpdateError::Flash);
        }
        self.machine
            .flash_finished(true)
            .map_err(HostUpdateError::Control)?;
        let (confirmed, status) = reconnect
            .reconnect(&self.board_id)
            .map_err(|_| HostUpdateError::Reconnect)?;
        if confirmed.session_id == 0
            || confirmed.session_id == self.old_session_id
            || confirmed.board_id != status.board_id
            || confirmed.firmware_version != status.firmware_version
            || confirmed.negotiated_minor != status.protocol_minor
        {
            return Err(HostUpdateError::Reconnect);
        }
        self.machine
            .reconnected(&status)
            .map_err(|_| HostUpdateError::Reconnect)
    }
}

impl<S: Read + Write> HostActor<S> {
    /// Consumes this actor, disarms capture, and obtains exact release/prepare ACKs.
    ///
    /// Call on the native actor worker after stopping capture-event delivery. The
    /// manifest image must fit the known factory app partition. The actor is dropped
    /// before the returned handoff can invoke a flasher, releasing its serial stream.
    pub fn prepare_update(
        mut self,
        manifest_bytes: &[u8],
        image: Vec<u8>,
    ) -> Result<FlashHandoff, HostUpdateError> {
        if !matches!(self.state(), HostState::Local | HostState::Guest(_))
            || self.pending.is_some()
            || self.inventory_request_seq.is_some()
        {
            return Err(HostUpdateError::Busy);
        }
        if self.device.board_id != "esp32-kvm-s3" {
            return Err(HostUpdateError::Image(VerificationError::Board));
        }
        let manifest = parse_manifest(manifest_bytes).map_err(HostUpdateError::Manifest)?;
        let identity = DeviceIdentity {
            board_id: self.device.board_id.clone(),
            protocol_major: MAJOR,
            protocol_minor: self.device.negotiated_minor,
            app_partition_bytes: FACTORY_APP_CAPACITY,
            firmware_version: self.device.firmware_version.clone(),
            local: self.state() == HostState::Local,
            armed: matches!(self.state(), HostState::Guest(_)),
        };
        let plan = verify_image(&manifest, &image, &identity).map_err(HostUpdateError::Image)?;
        let old_session_id = self.device.session_id;
        let (mut machine, release) = UpdateMachine::begin(
            plan,
            old_session_id,
            self.confirmed_generation,
            self.next_seq,
        )
        .map_err(HostUpdateError::Control)?;
        self.capture.disarm();
        self.clear_input();
        self.all_up = false;
        self.baseline_sent = false;
        let UpdateAction::DisarmAndRelease(release) = release else {
            unreachable!("begin returns release")
        };
        self.write_frame(&release)
            .map_err(HostUpdateError::Transport)?;
        let prepare = self.await_update_reply(&mut machine)?;
        let UpdateAction::SendPrepare(prepare) = prepare else {
            return Err(HostUpdateError::Control(UpdateError::InvalidState));
        };
        self.write_frame(&prepare)
            .map_err(HostUpdateError::Transport)?;
        let flash = self.await_update_reply(&mut machine)?;
        if !matches!(flash, UpdateAction::FlashApp { partition: "app", image_len } if image_len == image.len())
        {
            return Err(HostUpdateError::Control(UpdateError::InvalidState));
        }
        Ok(FlashHandoff {
            machine,
            image,
            board_id: self.device.board_id.clone(),
            old_session_id,
        })
    }

    fn await_update_reply(
        &mut self,
        machine: &mut UpdateMachine,
    ) -> Result<UpdateAction, HostUpdateError> {
        let deadline = Instant::now() + CONTROL_TIMEOUT;
        let mut decoder = FrameDecoder::new();
        let mut bytes = [0_u8; 512];
        while Instant::now() < deadline {
            match self.stream.read(&mut bytes) {
                Ok(0) => return Err(HostUpdateError::Transport(HostFault::Transport)),
                Ok(count) => {
                    for byte in bytes[..count].iter().copied() {
                        let Some(result) = decoder.push(byte) else {
                            continue;
                        };
                        let frame =
                            result.map_err(|_| HostUpdateError::Transport(HostFault::Protocol))?;
                        if frame.session_id != self.device.session_id {
                            return Err(HostUpdateError::Control(UpdateError::ControlMismatch));
                        }
                        match frame.kind {
                            MessageKind::Ack | MessageKind::Nack => {
                                match machine.on_response(&frame) {
                                    Ok(action) => return Ok(action),
                                    Err(UpdateError::StaleResponse) => continue,
                                    Err(error) => return Err(HostUpdateError::Control(error)),
                                }
                            }
                            MessageKind::Status | MessageKind::InputProgress => continue,
                            _ => return Err(HostUpdateError::Transport(HostFault::Protocol)),
                        }
                    }
                }
                Err(error)
                    if matches!(
                        error.kind(),
                        io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut
                    ) =>
                {
                    thread::sleep(Duration::from_millis(1));
                }
                Err(_) => return Err(HostUpdateError::Transport(HostFault::Transport)),
            }
        }
        machine.control_timeout();
        Err(HostUpdateError::Control(UpdateError::Interrupted))
    }
}
