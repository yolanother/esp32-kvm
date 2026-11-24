use esp32_kvm_host_actor::{HostActor, HostState, SetOneKeyMapper};
use esp32_kvm_input_core::Action;
use esp32_kvm_platform_windows::{CaptureEvent, CaptureGate, PhysicalEvent};
use esp32_kvm_protocol::FrameDecoder;
use esp32_kvm_usb_transport::probe_stream;
use std::io::{self, Read, Write};
use std::thread;
use std::time::{Duration, Instant};

struct TracedPort {
    serial: Box<dyn serialport::SerialPort>,
    decoder: FrameDecoder,
    written_decoder: FrameDecoder,
    trace: bool,
}

impl Read for TracedPort {
    fn read(&mut self, bytes: &mut [u8]) -> io::Result<usize> {
        let count = self.serial.read(bytes)?;
        if self.trace {
            for byte in bytes[..count].iter().copied() {
                if let Some(frame) = self.decoder.push(byte) {
                    eprintln!("received {frame:?}");
                }
            }
        }
        Ok(count)
    }
}

impl Write for TracedPort {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        let count = self.serial.write(bytes)?;
        if self.trace {
            for byte in bytes[..count].iter().copied() {
                if let Some(frame) = self.written_decoder.push(byte) {
                    eprintln!("sent {frame:?}");
                }
            }
        }
        Ok(count)
    }
    fn flush(&mut self) -> io::Result<()> {
        self.serial.flush()
    }
}

fn main() -> Result<(), String> {
    let (capture, receiver) = CaptureGate::new(16);
    let started = Instant::now();
    let serial = serialport::new("COM4", 115_200)
        .timeout(Duration::from_millis(20))
        .open()
        .map_err(|error| format!("open failed: {error:?}"))?;
    let mut port = TracedPort {
        serial,
        decoder: FrameDecoder::new(),
        written_decoder: FrameDecoder::new(),
        trace: std::env::args().any(|argument| argument == "--wire-trace"),
    };
    let device = probe_stream(&mut port, "esp32-kvm-s3", "send-a")
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

    let deadline = Instant::now() + Duration::from_secs(4);
    while Instant::now() < deadline && actor.state() != HostState::Local {
        actor.drive(&receiver, started.elapsed().as_millis() as u64);
        actor.observe_all_up();
        thread::sleep(Duration::from_millis(5));
    }
    let before = actor.setup_snapshot();
    println!(
        "snapshot state={:?} fault={:?} pairing={:?} slots={:?}",
        before.state, before.fault, before.pairing_status, before.slots
    );
    let slot = before
        .slots
        .iter()
        .find(|slot| slot.ready && slot.subscribed)
        .map(|slot| slot.slot)
        .ok_or("Mac is not ready and subscribed")?;
    let before_count = before
        .slots
        .iter()
        .find(|seen| seen.slot == slot)
        .map(|seen| seen.notifications_delivered)
        .unwrap_or(0);
    println!(
        "before slot={slot} delivered={before_count} state={:?}",
        actor.state()
    );

    actor.request(Action::Direct(slot), started.elapsed().as_millis() as u64);
    let deadline = Instant::now() + Duration::from_secs(4);
    while Instant::now() < deadline && !matches!(actor.state(), HostState::Guest(_)) {
        actor.drive(&receiver, started.elapsed().as_millis() as u64);
        actor.observe_all_up();
        thread::sleep(Duration::from_millis(5));
    }
    if !matches!(actor.state(), HostState::Guest(_)) {
        return Err(format!(
            "route failed: state={:?} fault={:?}",
            actor.state(),
            actor.fault()
        ));
    }
    let generation = capture.generation();
    for down in [true, false] {
        actor.on_capture(
            CaptureEvent {
                generation,
                event: PhysicalEvent::Key {
                    virtual_key: 0x41,
                    scan_code: 0x1e,
                    extended: false,
                    down,
                    repeat: false,
                },
            },
            started.elapsed().as_millis() as u64,
        );
        // Keep the diagnostic press visible across several BLE intervals.
        if down {
            let release_at = Instant::now() + Duration::from_millis(100);
            while Instant::now() < release_at {
                actor.drive(&receiver, started.elapsed().as_millis() as u64);
                thread::sleep(Duration::from_millis(5));
            }
        }
    }

    let deadline = Instant::now() + Duration::from_secs(3);
    while Instant::now() < deadline {
        actor.drive(&receiver, started.elapsed().as_millis() as u64);
        thread::sleep(Duration::from_millis(5));
    }
    let after = actor.setup_snapshot();
    let after_count = after
        .slots
        .iter()
        .find(|seen| seen.slot == slot)
        .map(|seen| seen.notifications_delivered)
        .unwrap_or(0);
    println!(
        "after delivered={after_count} delta={} state={:?} fault={:?} trace={:?}",
        after_count.wrapping_sub(before_count),
        actor.state(),
        actor.fault(),
        actor.input_trace()
    );
    let successful = matches!(actor.state(), HostState::Guest(_))
        && actor.fault().is_none()
        && actor.input_trace().last_key_seq.is_some()
        && actor.input_trace().ble_enqueued_seq == actor.input_trace().last_key_seq;
    actor.shutdown();
    if !successful {
        return Err("keyboard test did not reach BLE on an active guest route".into());
    }
    Ok(())
}
