// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Drives the USB update handoff with a fake serial peer, app flasher, and verified reconnect.
// Verifies exact release/prepare ACKs and local fail-closed behavior without device I/O.

use esp32_kvm_host_actor::{
    AppFlashRequest, AppFlasher, FlashError, HostActor, HostUpdateError, KeyMapper, MappedKey,
    ReconnectError, Reconnector,
};
use esp32_kvm_platform_windows::CaptureGate;
use esp32_kvm_protocol::{Frame, FrameDecoder, MessageKind};
use esp32_kvm_update_core::DeviceIdentity;
use esp32_kvm_usb_transport::ConfirmedDevice;
use std::collections::VecDeque;
use std::io::{self, Read, Write};
use std::sync::{Arc, Mutex};

const HASH_ABC: &str = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

#[derive(Clone, Copy)]
enum Reply {
    Exact,
    WrongRelease,
    WrongSession,
    NackPrepare,
    Silence,
}

struct Wire {
    incoming: VecDeque<u8>,
    sent: Vec<Frame>,
    reply: Reply,
}

#[derive(Clone)]
struct FakePort(Arc<Mutex<Wire>>);

impl FakePort {
    fn new(reply: Reply) -> Self {
        Self(Arc::new(Mutex::new(Wire {
            incoming: VecDeque::new(),
            sent: Vec::new(),
            reply,
        })))
    }

    fn feed(&self, frame: Frame) {
        self.0
            .lock()
            .unwrap()
            .incoming
            .extend(frame.encode().unwrap());
    }

    fn sent(&self) -> Vec<Frame> {
        self.0.lock().unwrap().sent.clone()
    }
}

impl Read for FakePort {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let mut wire = self.0.lock().unwrap();
        let mut count = 0;
        while count < buf.len() {
            let Some(byte) = wire.incoming.pop_front() else {
                break;
            };
            buf[count] = byte;
            count += 1;
        }
        if count == 0 {
            Err(io::ErrorKind::WouldBlock.into())
        } else {
            Ok(count)
        }
    }
}

impl Write for FakePort {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        let mut decoder = FrameDecoder::new();
        let mut wire = self.0.lock().unwrap();
        for frame in bytes
            .iter()
            .filter_map(|byte| decoder.push(*byte))
            .flatten()
        {
            if matches!(
                frame.kind,
                MessageKind::ReleaseAll | MessageKind::UpdatePrepare
            ) {
                let generation = if frame.kind == MessageKind::ReleaseAll {
                    frame.route_generation + 1
                } else {
                    frame.route_generation
                };
                let mut answer = ack(&frame, generation);
                match (wire.reply, frame.kind) {
                    (Reply::WrongRelease, MessageKind::ReleaseAll) => {
                        answer.payload[0] = MessageKind::Arm as u8;
                    }
                    (Reply::WrongSession, MessageKind::ReleaseAll) => {
                        answer.session_id = 99;
                    }
                    (Reply::NackPrepare, MessageKind::UpdatePrepare) => {
                        answer.kind = MessageKind::Nack;
                        answer.payload[5] = 6;
                    }
                    (Reply::Silence, _) => {
                        wire.sent.push(frame);
                        continue;
                    }
                    _ => {}
                }
                wire.incoming.extend(answer.encode().unwrap());
            }
            wire.sent.push(frame);
        }
        Ok(bytes.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

fn ack(original: &Frame, generation: u32) -> Frame {
    let mut payload = vec![original.kind as u8];
    payload.extend_from_slice(&original.seq.to_le_bytes());
    payload.push(0);
    payload.extend_from_slice(&generation.to_le_bytes());
    Frame::new(
        MessageKind::Ack,
        original.session_id,
        original.seq,
        generation,
        payload,
    )
}

struct Mapper;
impl KeyMapper for Mapper {
    fn map_key(&self, _: u32, _: u32, _: bool) -> Option<MappedKey> {
        None
    }
}

fn device(version: &str, session_id: u64) -> ConfirmedDevice {
    ConfirmedDevice {
        board_id: "esp32-kvm-s3".into(),
        session_id,
        max_connections: 1,
        max_bonds: 8,
        firmware_version: version.into(),
        negotiated_minor: 2,
    }
}

fn local_status() -> Frame {
    Frame::new(
        MessageKind::Status,
        5,
        1,
        4,
        vec![0xa6, 1, 0, 2, 0, 3, 0x80, 4, 0, 5, 4, 6, 0xa1, 1, 0],
    )
}

fn manifest() -> Vec<u8> {
    format!(r#"{{"schema":1,"board_id":"esp32-kvm-s3","protocol_major":1,"protocol_minor_min":0,"protocol_minor_max":2,"firmware_version":"0.2.0","partition":"app","image_size":3,"image_sha256":"{HASH_ABC}"}}"#).into_bytes()
}

fn setup_actor(reply: Reply) -> (HostActor<FakePort>, FakePort, Arc<CaptureGate>) {
    let port = FakePort::new(reply);
    let (gate, _) = CaptureGate::new(32);
    let mut actor = HostActor::from_confirmed(
        port.clone(),
        Box::new(gate.clone()),
        device("0.1.0", 5),
        vec![1],
        Box::new(Mapper),
        0,
    )
    .unwrap();
    port.feed(local_status());
    actor.poll(1);
    (actor, port, gate)
}

#[derive(Default)]
struct FakeFlasher {
    calls: usize,
    fail: bool,
}
impl AppFlasher for FakeFlasher {
    fn flash_app(&mut self, request: AppFlashRequest<'_>) -> Result<(), FlashError> {
        self.calls += 1;
        assert_eq!(request.partition(), "app");
        assert_eq!(request.offset(), 0x20000);
        assert_eq!(request.bytes(), b"abc");
        assert!(request.bytes().len() as u64 <= request.capacity());
        if self.fail {
            Err(FlashError::Failed)
        } else {
            Ok(())
        }
    }
}

struct FakeReconnect {
    session_id: u64,
    local: bool,
    armed: bool,
    version: &'static str,
}
impl Reconnector for FakeReconnect {
    fn reconnect(&mut self, _: &str) -> Result<(ConfirmedDevice, DeviceIdentity), ReconnectError> {
        Ok((
            device(self.version, self.session_id),
            DeviceIdentity {
                board_id: "esp32-kvm-s3".into(),
                protocol_major: 1,
                protocol_minor: 2,
                app_partition_bytes: 0x650000,
                firmware_version: self.version.into(),
                local: self.local,
                armed: self.armed,
            },
        ))
    }
}

#[test]
fn exact_controls_flash_app_and_require_new_disarmed_session() {
    let (actor, port, gate) = setup_actor(Reply::Exact);
    let handoff = actor.prepare_update(&manifest(), b"abc".to_vec()).unwrap();
    assert_eq!(gate.generation(), 0);
    let controls: Vec<_> = port
        .sent()
        .into_iter()
        .filter(|f| matches!(f.kind, MessageKind::ReleaseAll | MessageKind::UpdatePrepare))
        .collect();
    assert_eq!(
        controls.iter().map(|f| f.kind).collect::<Vec<_>>(),
        vec![MessageKind::ReleaseAll, MessageKind::UpdatePrepare]
    );
    assert!(
        !port
            .sent()
            .iter()
            .any(|frame| frame.kind == MessageKind::Arm)
    );
    let mut flasher = FakeFlasher::default();
    let mut reconnect = FakeReconnect {
        session_id: 6,
        local: true,
        armed: false,
        version: "0.2.0",
    };
    assert_eq!(
        handoff.flash_and_reconnect(&mut flasher, &mut reconnect),
        Ok(())
    );
    assert_eq!(flasher.calls, 1);
}

#[test]
fn malformed_manifest_or_wrong_ack_never_reaches_flash() {
    let (actor, port, gate) = setup_actor(Reply::Exact);
    assert!(gate.arm(4));
    assert_eq!(gate.generation(), 4);
    assert!(actor.prepare_update(&manifest(), b"bad".to_vec()).is_err());
    assert_eq!(gate.generation(), 0);
    assert!(
        !port
            .sent()
            .iter()
            .any(|f| f.kind == MessageKind::ReleaseAll)
    );
    let (actor, port, _) = setup_actor(Reply::WrongRelease);
    assert!(matches!(
        actor.prepare_update(&manifest(), b"abc".to_vec()),
        Err(HostUpdateError::Control(_))
    ));
    assert!(
        !port
            .sent()
            .iter()
            .any(|f| f.kind == MessageKind::UpdatePrepare)
    );
    let (actor, _, _) = setup_actor(Reply::NackPrepare);
    assert!(matches!(
        actor.prepare_update(&manifest(), b"abc".to_vec()),
        Err(HostUpdateError::Control(_))
    ));
}

#[test]
fn control_timeout_flash_failure_and_old_or_armed_reconnect_fail_closed() {
    let (actor, _, gate) = setup_actor(Reply::Silence);
    assert!(actor.prepare_update(&manifest(), b"abc".to_vec()).is_err());
    assert_eq!(gate.generation(), 0);
    let (actor, _, _) = setup_actor(Reply::Exact);
    let handoff = actor.prepare_update(&manifest(), b"abc".to_vec()).unwrap();
    let mut flasher = FakeFlasher {
        calls: 0,
        fail: true,
    };
    let mut reconnect = FakeReconnect {
        session_id: 6,
        local: true,
        armed: false,
        version: "0.2.0",
    };
    assert!(
        handoff
            .flash_and_reconnect(&mut flasher, &mut reconnect)
            .is_err()
    );
    let (actor, _, _) = setup_actor(Reply::Exact);
    let handoff = actor.prepare_update(&manifest(), b"abc".to_vec()).unwrap();
    let mut flasher = FakeFlasher::default();
    reconnect.session_id = 5;
    assert_eq!(
        handoff.flash_and_reconnect(&mut flasher, &mut reconnect),
        Err(HostUpdateError::Reconnect)
    );
    let (actor, _, _) = setup_actor(Reply::Exact);
    let handoff = actor.prepare_update(&manifest(), b"abc".to_vec()).unwrap();
    reconnect.session_id = 6;
    reconnect.armed = true;
    assert_eq!(
        handoff.flash_and_reconnect(&mut flasher, &mut reconnect),
        Err(HostUpdateError::Reconnect)
    );
}

#[test]
fn wrong_session_reply_and_wrong_reconnect_version_cannot_complete() {
    let (actor, port, gate) = setup_actor(Reply::WrongSession);
    assert!(matches!(
        actor.prepare_update(&manifest(), b"abc".to_vec()),
        Err(HostUpdateError::Control(_))
    ));
    assert_eq!(gate.generation(), 0);
    assert!(
        !port
            .sent()
            .iter()
            .any(|frame| frame.kind == MessageKind::UpdatePrepare)
    );

    let (actor, _, _) = setup_actor(Reply::Exact);
    let handoff = actor.prepare_update(&manifest(), b"abc".to_vec()).unwrap();
    let mut flasher = FakeFlasher::default();
    let mut reconnect = FakeReconnect {
        session_id: 6,
        local: true,
        armed: false,
        version: "0.1.0",
    };
    assert_eq!(
        handoff.flash_and_reconnect(&mut flasher, &mut reconnect),
        Err(HostUpdateError::Reconnect)
    );
}
