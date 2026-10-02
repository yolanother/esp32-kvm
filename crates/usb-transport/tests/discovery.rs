// Copyright (c) ESP32 KVM contributors. Use is governed by the repository LICENSE.
// Exercises USB identity filtering and the framed HELLO/CAPS/SESSION_OPEN handshake
// against an in-memory byte stream without claiming physical board verification.

use esp32_kvm_protocol::{Frame, FrameDecoder, MessageKind};
use esp32_kvm_usb_transport::{
    PortIdentity, ProbeError, candidate_ports, probe_candidates_with, probe_stream,
};
use std::io::{self, Cursor, Read, Write};

struct ScriptedPort {
    input: Cursor<Vec<u8>>,
    written: Vec<u8>,
}

impl ScriptedPort {
    fn new(input: Vec<u8>) -> Self {
        Self {
            input: Cursor::new(input),
            written: Vec::new(),
        }
    }
}

impl Read for ScriptedPort {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        self.input.read(buf)
    }
}

impl Write for ScriptedPort {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        self.written.extend_from_slice(buf);
        Ok(buf.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

fn caps(board: &str, session: u64, min_minor: u8, max_minor: u8) -> Vec<u8> {
    let mut p = vec![0xa8, 1, 0x61, b'1', 2, 0x60 + board.len() as u8];
    p.extend_from_slice(board.as_bytes());
    p.extend_from_slice(&[3, min_minor, 4, max_minor, 5, 1, 6, 1, 7, 0, 8, 0x1b]);
    p.extend_from_slice(&session.to_be_bytes());
    Frame::new(MessageKind::Caps, session, 7, 0, p)
        .encode()
        .unwrap()
}

fn ack(session: u64) -> Vec<u8> {
    let mut p = vec![MessageKind::SessionOpen as u8];
    p.extend_from_slice(&1_u32.to_le_bytes());
    p.push(0);
    p.extend_from_slice(&0_u32.to_le_bytes());
    Frame::new(MessageKind::Ack, session, 8, 0, p)
        .encode()
        .unwrap()
}

fn decoded_frames(bytes: &[u8]) -> Vec<Frame> {
    let mut decoder = FrameDecoder::new();
    bytes
        .iter()
        .filter_map(|b| decoder.push(*b))
        .map(Result::unwrap)
        .collect()
}

#[test]
fn only_matching_usb_identity_is_a_candidate() {
    let ports = [
        PortIdentity::usb("COM1", 0x303a, 0x1001),
        PortIdentity::usb("COM2", 0x303a, 0x4001),
        PortIdentity::usb("COM3", 0x1234, 0x1001),
        PortIdentity::non_usb("COM4"),
    ];
    assert_eq!(candidate_ports(&ports), vec!["COM1"]);
}

#[test]
fn handshake_requires_board_id_protocol_range_and_ack() {
    let mut input = caps("esp32-kvm", 0x1_0000_0000, 0, 0);
    input.extend_from_slice(&ack(0x1_0000_0000));
    let mut stream = ScriptedPort::new(input);
    let result = probe_stream(&mut stream, "esp32-kvm", "host-test").unwrap();
    assert_eq!(result.session_id, 0x1_0000_0000);
    assert_eq!(result.board_id, "esp32-kvm");
    let sent = decoded_frames(&stream.written);
    assert_eq!(sent.len(), 2);
    assert_eq!(sent[0].kind, MessageKind::Hello);
    assert_eq!(sent[0].session_id, 0);
    assert_eq!(sent[1].kind, MessageKind::SessionOpen);
    assert_eq!(sent[1].session_id, result.session_id);
}

#[test]
fn wrong_board_and_incompatible_minor_are_rejected() {
    let mut wrong = ScriptedPort::new(caps("other", 0x1_0000_0000, 0, 0));
    assert!(matches!(
        probe_stream(&mut wrong, "esp32-kvm", "host-test"),
        Err(ProbeError::BoardMismatch)
    ));
    assert_eq!(decoded_frames(&wrong.written).len(), 1);

    let mut incompatible = ScriptedPort::new(caps("esp32-kvm", 0x1_0000_0000, 1, 1));
    assert!(matches!(
        probe_stream(&mut incompatible, "esp32-kvm", "host-test"),
        Err(ProbeError::VersionMismatch)
    ));

    let mut wrong_major = caps("esp32-kvm", 0x1_0000_0000, 0, 0);
    wrong_major[3] = 2;
    let mut wrong_major = ScriptedPort::new(wrong_major);
    assert!(matches!(
        probe_stream(&mut wrong_major, "esp32-kvm", "host-test"),
        Err(ProbeError::VersionMismatch)
    ));
}

#[test]
fn unplug_before_ack_fails_without_confirming_discovery() {
    let mut stream = ScriptedPort::new(caps("esp32-kvm", 0x1_0000_0000, 0, 0));
    assert!(matches!(
        probe_stream(&mut stream, "esp32-kvm", "host-test"),
        Err(ProbeError::Disconnected)
    ));
}

#[test]
fn discovery_reports_mismatch_and_ignores_non_usb_ports() {
    let ports = [
        PortIdentity::usb("COM7", 0x303a, 0x1001),
        PortIdentity::non_usb("COM8"),
    ];
    let outcomes = probe_candidates_with(&ports, "esp32-kvm", "host-test", |name| {
        assert_eq!(name, "COM7");
        Ok(ScriptedPort::new(caps("wrong-board", 0x1_0000_0000, 0, 0)))
    });
    assert_eq!(outcomes.len(), 1);
    assert_eq!(outcomes[0].port_name, "COM7");
    assert!(matches!(
        outcomes[0].outcome,
        Err(ProbeError::BoardMismatch)
    ));
}
