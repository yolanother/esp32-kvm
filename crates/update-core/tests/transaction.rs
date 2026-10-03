// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Verifies release-first update sequencing, exact control ACKs, interruption recovery,
// app-only flash authorization, and disarmed verified reconnect without auto-arm.

use esp32_kvm_protocol::{Frame, MessageKind};
use esp32_kvm_update_core::{
    DeviceIdentity, UpdateAction, UpdateError, UpdateMachine, UpdateState, parse_manifest,
    verify_image,
};

const HASH_ABC: &str = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

fn device() -> DeviceIdentity {
    DeviceIdentity {
        board_id: "esp32-kvm-s3".into(),
        protocol_major: 1,
        protocol_minor: 1,
        app_partition_bytes: 0x400000,
        firmware_version: "0.1.0".into(),
        local: false,
        armed: true,
    }
}

fn new_machine() -> (UpdateMachine, Frame) {
    let json = format!(
        r#"{{"schema":1,"board_id":"esp32-kvm-s3","protocol_major":1,"protocol_minor_min":0,"protocol_minor_max":1,"firmware_version":"0.2.0","partition":"app","image_size":3,"image_sha256":"{HASH_ABC}"}}"#
    );
    let plan = verify_image(&parse_manifest(json.as_bytes()).unwrap(), b"abc", &device()).unwrap();
    let (machine, action) = UpdateMachine::begin(plan, 7, 41, 10).unwrap();
    let UpdateAction::DisarmAndRelease(frame) = action else {
        panic!("release first")
    };
    assert_eq!(frame.kind, MessageKind::ReleaseAll);
    assert_eq!(frame.route_generation, 41);
    (machine, frame)
}

fn ack(frame: &Frame, generation: u32) -> Frame {
    let mut payload = vec![frame.kind as u8];
    payload.extend_from_slice(&frame.seq.to_le_bytes());
    payload.push(0);
    payload.extend_from_slice(&generation.to_le_bytes());
    Frame::new(
        MessageKind::Ack,
        frame.session_id,
        frame.seq,
        generation,
        payload,
    )
}

#[test]
fn exact_release_then_prepare_then_reverified_app_flash_then_local_reconnect() {
    let (mut machine, release) = new_machine();
    assert_eq!(machine.state(), UpdateState::AwaitRelease);
    let UpdateAction::SendPrepare(prepare) = machine.on_response(&ack(&release, 42)).unwrap()
    else {
        panic!("prepare")
    };
    assert_eq!(prepare.kind, MessageKind::UpdatePrepare);
    assert_eq!(prepare.route_generation, 42);
    assert_eq!(machine.state(), UpdateState::AwaitPrepare);
    let UpdateAction::FlashApp {
        partition,
        image_len,
    } = machine.on_response(&ack(&prepare, 42)).unwrap()
    else {
        panic!("flash")
    };
    assert_eq!(partition, "app");
    assert_eq!(image_len, 3);
    machine.flash_started(b"abc").unwrap();
    assert_eq!(machine.state(), UpdateState::Flashing);
    assert_eq!(
        machine.flash_finished(true).unwrap(),
        UpdateAction::WaitForReconnect
    );
    let mut new_device = device();
    new_device.firmware_version = "0.2.0".into();
    new_device.local = true;
    new_device.armed = false;
    machine.reconnected(&new_device).unwrap();
    assert_eq!(machine.state(), UpdateState::Complete);
}

#[test]
fn stale_reply_does_not_advance_and_wrong_current_ack_fails_closed() {
    let (mut machine, release) = new_machine();
    let mut stale = ack(&release, 42);
    stale.seq = 9;
    assert_eq!(machine.on_response(&stale), Err(UpdateError::StaleResponse));
    assert_eq!(machine.state(), UpdateState::AwaitRelease);
    let mut wrong = ack(&release, 42);
    wrong.payload[0] = MessageKind::Arm as u8;
    assert_eq!(
        machine.on_response(&wrong),
        Err(UpdateError::ControlMismatch)
    );
    assert_eq!(machine.state(), UpdateState::RecoveryRequired);

    let (mut machine, release) = new_machine();
    let mut future = ack(&release, 42);
    future.seq += 1;
    assert_eq!(
        machine.on_response(&future),
        Err(UpdateError::ControlMismatch)
    );
    assert_eq!(machine.state(), UpdateState::RecoveryRequired);
}

#[test]
fn nack_timeout_and_flash_interruption_never_authorize_rearm() {
    let (mut machine, release) = new_machine();
    let mut nack = ack(&release, 42);
    nack.kind = MessageKind::Nack;
    assert_eq!(
        machine.on_response(&nack),
        Err(UpdateError::ControlRejected)
    );
    assert_eq!(machine.state(), UpdateState::RecoveryRequired);

    let (mut machine, release) = new_machine();
    machine.control_timeout();
    assert_eq!(
        machine.on_response(&ack(&release, 42)),
        Err(UpdateError::InvalidState)
    );

    let (mut machine, release) = new_machine();
    let UpdateAction::SendPrepare(prepare) = machine.on_response(&ack(&release, 42)).unwrap()
    else {
        panic!()
    };
    machine.on_response(&ack(&prepare, 42)).unwrap();
    assert_eq!(
        machine.flash_started(b"abd"),
        Err(UpdateError::ImageChanged)
    );
    assert_eq!(machine.state(), UpdateState::RecoveryRequired);
}

#[test]
fn reconnect_requires_expected_firmware_and_disarmed_local_status() {
    let (mut machine, release) = new_machine();
    let UpdateAction::SendPrepare(prepare) = machine.on_response(&ack(&release, 42)).unwrap()
    else {
        panic!()
    };
    machine.on_response(&ack(&prepare, 42)).unwrap();
    machine.flash_started(b"abc").unwrap();
    machine.flash_finished(true).unwrap();
    let mut wrong = device();
    wrong.local = true;
    wrong.armed = false;
    assert_eq!(
        machine.reconnected(&wrong),
        Err(UpdateError::ReconnectMismatch)
    );
    assert_eq!(machine.state(), UpdateState::RecoveryRequired);
}
