use esp32_kvm_host_actor::{HostActor, HostState, SetOneKeyMapper};
use esp32_kvm_platform_windows::CaptureGate;
use esp32_kvm_usb_transport::probe_stream;
use std::thread;
use std::time::{Duration, Instant};

/// Hides (`0`) or shows (`1`) the first guest on the device screen and prints the firmware's reply.
/// Usage: guest_enabled <0|1>
fn main() -> Result<(), String> {
    let enabled = match std::env::args().nth(1).as_deref() {
        Some("0") => false,
        Some("1") => true,
        _ => return Err("usage: guest_enabled <0|1>".into()),
    };
    let (capture, receiver) = CaptureGate::new(64);
    let started = Instant::now();
    let now = || started.elapsed().as_millis() as u64;
    let mut port = serialport::new("COM4", 115_200)
        .timeout(Duration::from_millis(20))
        .open()
        .map_err(|error| format!("open failed: {error:?}"))?;
    let device = probe_stream(&mut port, "esp32-kvm-s3", "guest-enabled")
        .map_err(|error| format!("connect failed: {error:?}"))?;
    println!("negotiated minor {}", device.negotiated_minor);
    let mut actor = HostActor::from_confirmed(
        port,
        Box::new(capture),
        device,
        Vec::new(),
        Box::new(SetOneKeyMapper),
        0,
    )
    .map_err(|error| format!("connect failed: {error:?}"))?;
    let deadline = Instant::now() + Duration::from_secs(4);
    while Instant::now() < deadline && actor.state() != HostState::Local {
        actor.drive(&receiver, now());
        actor.observe_all_up();
        thread::sleep(Duration::from_millis(5));
    }
    // Opening the port resets the board; wait for a guest to reconnect.
    let deadline = Instant::now() + Duration::from_secs(40);
    while Instant::now() < deadline && actor.setup_snapshot().slots.is_empty() {
        actor.drive(&receiver, now());
        actor.observe_all_up();
        thread::sleep(Duration::from_millis(20));
    }
    let snapshot = actor.setup_snapshot();
    let token = snapshot
        .slots
        .first()
        .map(|slot| slot.bond_token)
        .ok_or("no guest slot reported")?;
    println!("slots {:?}", snapshot.slots.len());
    println!(
        "announce enabled={enabled} -> {:?}",
        actor.announce_guest_enabled(token, enabled, now())
    );
    Ok(())
}
