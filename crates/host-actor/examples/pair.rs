use esp32_kvm_host_actor::{HostActor, HostState, PairingStatus, SetOneKeyMapper};
use esp32_kvm_platform_windows::CaptureGate;
use esp32_kvm_usb_transport::probe_stream;
use std::path::PathBuf;
use std::thread;
use std::time::{Duration, Instant};

/// Opens the 60 s pairing window, prints the numeric comparison, and answers it from a flag
/// file: `<TEMP>/kvm-pair-reply` containing `yes` approves, anything else rejects.
fn main() -> Result<(), String> {
    let (capture, receiver) = CaptureGate::new(16);
    let started = Instant::now();
    let now = || started.elapsed().as_millis() as u64;
    let reply_file = PathBuf::from(std::env::var("TEMP").unwrap_or_else(|_| ".".into()))
        .join("kvm-pair-reply");
    let _ = std::fs::remove_file(&reply_file);
    let port = serialport::new("COM4", 115_200)
        .timeout(Duration::from_millis(20))
        .open()
        .map_err(|error| format!("open failed: {error:?}"))?;
    let mut port = port;
    let device = probe_stream(&mut port, "esp32-kvm-s3", "pair")
        .map_err(|error| format!("connect failed: {error:?}"))?;
    let mut actor = HostActor::from_confirmed(
        port,
        Box::new(capture.clone()),
        device,
        Vec::new(),
        Box::new(SetOneKeyMapper),
        0,
    )
    .map_err(|error| format!("connect failed: {error:?}"))?;

    let settle = Instant::now() + Duration::from_secs(4);
    while Instant::now() < settle && actor.state() != HostState::Local {
        actor.drive(&receiver, now());
        actor.observe_all_up();
        thread::sleep(Duration::from_millis(5));
    }
    actor
        .pair_begin(60, now())
        .map_err(|error| format!("pair_begin failed: {error:?}"))?;
    println!("PAIR_BEGIN sent");

    let deadline = Instant::now() + Duration::from_secs(70);
    let mut last = None;
    let mut replied = false;
    while Instant::now() < deadline {
        actor.drive(&receiver, now());
        let snapshot = actor.setup_snapshot();
        if last != Some(snapshot.pairing_status) {
            println!("PAIRING_STATUS {:?}", snapshot.pairing_status);
            last = Some(snapshot.pairing_status);
        }
        if snapshot.pairing_status == PairingStatus::Challenge && !replied {
            let (Some(id), Some(value)) = (snapshot.challenge_id, snapshot.comparison_value)
            else {
                continue;
            };
            println!("CHALLENGE id={id} comparison={value:06}");
            let wait = Instant::now() + Duration::from_secs(40);
            while Instant::now() < wait && !reply_file.exists() {
                actor.drive(&receiver, now());
                thread::sleep(Duration::from_millis(5));
            }
            let approved = std::fs::read_to_string(&reply_file)
                .map(|text| text.trim() == "yes")
                .unwrap_or(false);
            println!("REPLY approved={approved}");
            actor
                .pair_reply(id, approved, now())
                .map_err(|error| format!("pair_reply failed: {error:?}"))?;
            replied = true;
        }
        if replied
            && matches!(
                snapshot.pairing_status,
                PairingStatus::Closed | PairingStatus::Rejected | PairingStatus::Timeout
            )
        {
            break;
        }
        thread::sleep(Duration::from_millis(5));
    }
    let end = Instant::now() + Duration::from_secs(3);
    while Instant::now() < end {
        actor.drive(&receiver, now());
        thread::sleep(Duration::from_millis(5));
    }
    let snapshot = actor.setup_snapshot();
    println!(
        "FINAL status={:?} state={:?} fault={:?} slots={:?}",
        snapshot.pairing_status, snapshot.state, snapshot.fault, snapshot.slots
    );
    actor.shutdown();
    Ok(())
}
