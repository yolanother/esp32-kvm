// Copyright (c) ESP32 KVM contributors. Use is governed by the repository LICENSE.
// Discovers USB serial candidates by hardware identity and confirms an ESP32 KVM
// device through a bounded, framed protocol handshake. It negotiates minor-one
// pairing support and preserves the verified firmware version for setup UI.

#![forbid(unsafe_code)]

use esp32_kvm_protocol::{Frame, FrameDecoder, MINOR, MessageKind, ProtocolError};
use std::io::{self, Read, Write};
use std::time::{Duration, Instant};

/// Espressif's USB Serial/JTAG identity observed on the board during M0.
pub const ESPRESSIF_VID: u16 = 0x303a;
/// ESP32-S3 USB Serial/JTAG product ID observed on the board during M0.
pub const USB_SERIAL_JTAG_PID: u16 = 0x1001;
const PROBE_TIMEOUT: Duration = Duration::from_secs(2);

/// USB identity and OS-assigned name of a serial interface.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PortIdentity {
    /// OS port path or name, which is not trusted as a device identifier.
    pub name: String,
    /// USB vendor and product IDs when the OS exposes them.
    pub usb_id: Option<(u16, u16)>,
}

impl PortIdentity {
    /// Constructs an interface with a known USB identity.
    pub fn usb(name: impl Into<String>, vid: u16, pid: u16) -> Self {
        Self {
            name: name.into(),
            usb_id: Some((vid, pid)),
        }
    }

    /// Constructs an interface with no USB identity evidence.
    pub fn non_usb(name: impl Into<String>) -> Self {
        Self {
            name: name.into(),
            usb_id: None,
        }
    }
}

/// Returns names of interfaces eligible for a protocol probe; no name alone qualifies.
pub fn candidate_ports(ports: &[PortIdentity]) -> Vec<&str> {
    ports
        .iter()
        .filter(|p| p.usb_id == Some((ESPRESSIF_VID, USB_SERIAL_JTAG_PID)))
        .map(|p| p.name.as_str())
        .collect()
}

/// Reason a candidate was not confirmed as the expected application firmware.
#[derive(Debug)]
pub enum ProbeError {
    /// OS refused to open a matching serial interface.
    Open(serialport::Error),
    /// The port disappeared or returned EOF during negotiation.
    Disconnected,
    /// No complete, valid response arrived before the deadline.
    Timeout,
    /// OS I/O failed for another reason.
    Io(io::Error),
    /// The COBS, CRC, or message payload was invalid.
    Protocol(ProtocolError),
    /// The firmware advertises a different board identifier.
    BoardMismatch,
    /// Protocol major or supported minor range does not include this host.
    VersionMismatch,
    /// The response did not advance the expected handshake step.
    UnexpectedResponse,
}

/// Confirmed device session and capability summary; no input routing is armed.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ConfirmedDevice {
    /// Board identifier supplied by validated CAPS and compared with expectation.
    pub board_id: String,
    /// Firmware-selected nonzero session identifier.
    pub session_id: u64,
    /// Maximum live guest count advertised by firmware; requires hardware validation.
    pub max_connections: u8,
    /// Maximum retained bonds advertised by firmware.
    pub max_bonds: u8,
    /// Firmware version from validated CAPS.
    pub firmware_version: String,
    /// Explicitly selected compatible minor version for STATUS interpretation.
    pub negotiated_minor: u16,
}

/// Outcome for one USB identity candidate, including mismatched firmware.
#[derive(Debug)]
pub struct PortProbe {
    /// OS-assigned port name at the time of this scan.
    pub port_name: String,
    /// Confirmed application firmware, or the exact rejection reason.
    pub outcome: Result<ConfirmedDevice, ProbeError>,
}

/// Enumerates host serial interfaces with their USB IDs, excluding unknown bus types.
pub fn available_usb_ports() -> Result<Vec<PortIdentity>, serialport::Error> {
    Ok(serialport::available_ports()?
        .into_iter()
        .map(|p| match p.port_type {
            serialport::SerialPortType::UsbPort(usb) => {
                PortIdentity::usb(p.port_name, usb.vid, usb.pid)
            }
            _ => PortIdentity::non_usb(p.port_name),
        })
        .collect())
}

/// Opens each eligible USB interface and confirms a protocol session.
///
/// A serial port is only a candidate until CAPS board ID and protocol range,
/// followed by SESSION_OPEN ACK, have been checked. Call again after unplug or
/// re-enumeration; this function holds no stale COM-name cache.
pub fn discover_system(
    expected_board_id: &str,
    host_version: &str,
) -> Result<Vec<PortProbe>, serialport::Error> {
    let ports = available_usb_ports()?;
    Ok(probe_candidates_with(
        &ports,
        expected_board_id,
        host_version,
        |name| {
            serialport::new(name, 115_200)
                .timeout(Duration::from_millis(100))
                .open()
                .map_err(ProbeError::Open)
        },
    ))
}

/// Probes eligible ports with a caller-supplied opener for re-enumeration tests.
pub fn probe_candidates_with<S, F>(
    ports: &[PortIdentity],
    expected_board_id: &str,
    host_version: &str,
    mut open: F,
) -> Vec<PortProbe>
where
    S: Read + Write,
    F: FnMut(&str) -> Result<S, ProbeError>,
{
    candidate_ports(ports)
        .into_iter()
        .map(|name| {
            let outcome = open(name)
                .and_then(|mut stream| probe_stream(&mut stream, expected_board_id, host_version));
            PortProbe {
                port_name: name.to_owned(),
                outcome,
            }
        })
        .collect()
}

/// Negotiates HELLO, CAPS, and SESSION_OPEN on a binary byte stream.
///
/// This does not transmit keyboard or mouse state, and does not ARM routing.
pub fn probe_stream<S: Read + Write + ?Sized>(
    stream: &mut S,
    expected_board_id: &str,
    host_version: &str,
) -> Result<ConfirmedDevice, ProbeError> {
    if expected_board_id.is_empty()
        || expected_board_id.len() > 32
        || host_version.is_empty()
        || host_version.len() > 32
        || host_version.contains('\0')
    {
        return Err(ProbeError::UnexpectedResponse);
    }
    let hello = Frame::new(MessageKind::Hello, 0, 0, 0, vec![0, 0, 0, 0, 0, 0]);
    let deadline = Instant::now() + PROBE_TIMEOUT;
    let mut caps_frame = None;
    for attempt in 0..2 {
        send(stream, &hello)?;
        match receive(stream, deadline) {
            Ok(frame) => {
                caps_frame = Some(frame);
                break;
            }
            Err(ProbeError::Protocol(_)) if attempt == 0 => continue,
            Err(error) => return Err(error),
        }
    }
    let caps_frame = caps_frame.ok_or(ProbeError::Timeout)?;
    if caps_frame.kind != MessageKind::Caps {
        return Err(ProbeError::UnexpectedResponse);
    }
    let mut caps = parse_caps(&caps_frame)?;
    if caps.board_id != expected_board_id {
        return Err(ProbeError::BoardMismatch);
    }
    if caps.min_minor > 0 || caps.max_minor < caps.min_minor {
        return Err(ProbeError::VersionMismatch);
    }
    let negotiated_minor = u16::try_from(caps.max_minor.min(u64::from(MINOR)))
        .map_err(|_| ProbeError::VersionMismatch)?;
    if negotiated_minor > 0 {
        send(
            stream,
            &Frame::new(
                MessageKind::Hello,
                0,
                0,
                0,
                vec![negotiated_minor as u8, 0, 0, 0, 0, 0],
            ),
        )?;
        let upgraded = receive(stream, deadline)?;
        if upgraded.kind != MessageKind::Caps {
            return Err(ProbeError::UnexpectedResponse);
        }
        let next = parse_caps(&upgraded)?;
        if next.board_id != caps.board_id
            || next.session_id != caps.session_id
            || next.firmware_version != caps.firmware_version
            || next.min_minor > u64::from(negotiated_minor)
            || next.max_minor < u64::from(negotiated_minor)
        {
            return Err(ProbeError::VersionMismatch);
        }
        caps = next;
    }

    let mut payload = Vec::with_capacity(host_version.len() + 6);
    payload.extend_from_slice(&[0xa2, 1]);
    encode_text(&mut payload, host_version);
    payload.extend_from_slice(&[2, 0]);
    send(
        stream,
        &Frame::new(MessageKind::SessionOpen, caps.session_id, 1, 0, payload),
    )?;
    let ack = receive(stream, deadline)?;
    if ack.kind != MessageKind::Ack
        || ack.session_id != caps.session_id
        || ack.payload[0] != MessageKind::SessionOpen as u8
        || u32::from_le_bytes(ack.payload[1..5].try_into().unwrap()) != 1
    {
        return Err(ProbeError::UnexpectedResponse);
    }
    Ok(ConfirmedDevice {
        board_id: caps.board_id,
        session_id: caps.session_id,
        max_connections: caps.max_connections,
        max_bonds: caps.max_bonds,
        firmware_version: caps.firmware_version,
        negotiated_minor,
    })
}

fn send<S: Write + ?Sized>(stream: &mut S, frame: &Frame) -> Result<(), ProbeError> {
    let bytes = frame.encode().map_err(ProbeError::Protocol)?;
    stream.write_all(&bytes).map_err(map_io)?;
    stream.flush().map_err(map_io)
}

fn receive<S: Read + ?Sized>(stream: &mut S, deadline: Instant) -> Result<Frame, ProbeError> {
    let mut decoder = FrameDecoder::new();
    let mut byte = [0_u8; 1];
    while Instant::now() < deadline {
        match stream.read(&mut byte) {
            Ok(0) => return Err(ProbeError::Disconnected),
            Ok(_) => {
                if let Some(result) = decoder.push(byte[0]) {
                    return result.map_err(|error| match error {
                        ProtocolError::Version => ProbeError::VersionMismatch,
                        other => ProbeError::Protocol(other),
                    });
                }
            }
            Err(e)
                if e.kind() == io::ErrorKind::TimedOut || e.kind() == io::ErrorKind::WouldBlock =>
            {
                continue;
            }
            Err(e) => return Err(map_io(e)),
        }
    }
    Err(ProbeError::Timeout)
}

fn map_io(e: io::Error) -> ProbeError {
    if matches!(
        e.kind(),
        io::ErrorKind::BrokenPipe | io::ErrorKind::ConnectionAborted | io::ErrorKind::NotConnected
    ) {
        ProbeError::Disconnected
    } else {
        ProbeError::Io(e)
    }
}

struct Caps {
    firmware_version: String,
    board_id: String,
    session_id: u64,
    min_minor: u64,
    max_minor: u64,
    max_connections: u8,
    max_bonds: u8,
}

fn parse_caps(frame: &Frame) -> Result<Caps, ProbeError> {
    let mut c = Cbor {
        bytes: &frame.payload,
        at: 0,
    };
    if c.byte()? != 0xa8 {
        return Err(ProbeError::UnexpectedResponse);
    }
    c.key(1)?;
    let firmware_version = c.text()?.to_owned();
    c.key(2)?;
    let board_id = c.text()?.to_owned();
    c.key(3)?;
    let min_minor = c.uint()?;
    c.key(4)?;
    let max_minor = c.uint()?;
    c.key(5)?;
    let max_connections = c.uint()? as u8;
    c.key(6)?;
    let max_bonds = c.uint()? as u8;
    c.key(7)?;
    let _features = c.uint()?;
    c.key(8)?;
    let session_id = c.uint()?;
    if session_id != frame.session_id || c.at != c.bytes.len() {
        return Err(ProbeError::UnexpectedResponse);
    }
    Ok(Caps {
        firmware_version,
        board_id,
        session_id,
        min_minor,
        max_minor,
        max_connections,
        max_bonds,
    })
}

struct Cbor<'a> {
    bytes: &'a [u8],
    at: usize,
}

impl<'a> Cbor<'a> {
    fn byte(&mut self) -> Result<u8, ProbeError> {
        let v = *self
            .bytes
            .get(self.at)
            .ok_or(ProbeError::UnexpectedResponse)?;
        self.at += 1;
        Ok(v)
    }
    fn take(&mut self, n: usize) -> Result<&'a [u8], ProbeError> {
        let end = self
            .at
            .checked_add(n)
            .ok_or(ProbeError::UnexpectedResponse)?;
        let v = self
            .bytes
            .get(self.at..end)
            .ok_or(ProbeError::UnexpectedResponse)?;
        self.at = end;
        Ok(v)
    }
    fn item(&mut self, major: u8) -> Result<u64, ProbeError> {
        let initial = self.byte()?;
        if initial >> 5 != major {
            return Err(ProbeError::UnexpectedResponse);
        }
        Ok(match initial & 31 {
            v @ 0..=23 => u64::from(v),
            24 => u64::from(self.byte()?),
            25 => u64::from(u16::from_be_bytes(self.take(2)?.try_into().unwrap())),
            26 => u64::from(u32::from_be_bytes(self.take(4)?.try_into().unwrap())),
            27 => u64::from_be_bytes(self.take(8)?.try_into().unwrap()),
            _ => return Err(ProbeError::UnexpectedResponse),
        })
    }
    fn uint(&mut self) -> Result<u64, ProbeError> {
        self.item(0)
    }
    fn key(&mut self, expected: u8) -> Result<(), ProbeError> {
        if self.uint()? == u64::from(expected) {
            Ok(())
        } else {
            Err(ProbeError::UnexpectedResponse)
        }
    }
    fn text(&mut self) -> Result<&'a str, ProbeError> {
        let len = usize::try_from(self.item(3)?).map_err(|_| ProbeError::UnexpectedResponse)?;
        std::str::from_utf8(self.take(len)?).map_err(|_| ProbeError::UnexpectedResponse)
    }
}

fn encode_text(out: &mut Vec<u8>, text: &str) {
    if text.len() <= 23 {
        out.push(0x60 + text.len() as u8);
    } else {
        out.extend_from_slice(&[0x78, text.len() as u8]);
    }
    out.extend_from_slice(text.as_bytes());
}
