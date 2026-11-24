use esp32_kvm_host_actor::{HostActor, HostState, SetOneKeyMapper};
use esp32_kvm_platform_windows::CaptureGate;
use esp32_kvm_usb_transport::probe_stream;
use std::io::{self, Read, Write};
use std::thread;
use std::time::{Duration, Instant};


struct Logged(Box<dyn serialport::SerialPort>);
impl Read for Logged {
    fn read(&mut self, bytes: &mut [u8]) -> io::Result<usize> {
        let result = self.0.read(bytes);
        match &result {
            Ok(0) => eprintln!("read -> Ok(0)"),
            Err(error) if !matches!(error.kind(), io::ErrorKind::TimedOut | io::ErrorKind::WouldBlock) => {
                eprintln!("read -> Err({:?}: {error})", error.kind())
            }
            _ => {}
        }
        result
    }
}
impl Write for Logged {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        let result = self.0.write(bytes);
        if let Err(error) = &result {
            eprintln!("write -> Err({:?}: {error})", error.kind());
        }
        result
    }
    fn flush(&mut self) -> io::Result<()> {
        let result = self.0.flush();
        if let Err(error) = &result {
            eprintln!("flush -> Err({:?}: {error})", error.kind());
        }
        result
    }
}

/// Prints guest slot readiness once per change for N seconds (default 60).
fn main() -> Result<(), String> {
    let seconds: u64 = std::env::args().nth(1).and_then(|v| v.parse().ok()).unwrap_or(60);
    let (capture, receiver) = CaptureGate::new(16);
    let started = Instant::now();
    let now = || started.elapsed().as_millis() as u64;
    let port = serialport::new("COM4", 115_200)
        .timeout(Duration::from_millis(20))
        .open()
        .map_err(|error| format!("open failed: {error:?}"))?;
    let mut port = Logged(port);
    let device = probe_stream(&mut port, "esp32-kvm-s3", "watch")
        .map_err(|error| format!("connect failed: {error:?}"))?;
    let mut actor = HostActor::from_confirmed(
        port,
        Box::new(capture),
        device,
        Vec::new(),
        Box::new(SetOneKeyMapper),
        0,
    )
    .map_err(|error| format!("connect failed: {error:?}"))?;
    let mut last = String::new();
    while started.elapsed() < Duration::from_secs(seconds) {
        actor.drive(&receiver, now());
        actor.observe_all_up();
        let snapshot = actor.setup_snapshot();
        let line = format!(
            "state={:?} slots={:?}",
            snapshot.state,
            snapshot
                .slots
                .iter()
                .map(|s| (s.slot, s.ready, s.subscribed))
                .collect::<Vec<_>>()
        );
        if line != last {
            println!("{:>6}ms {line}", now());
            last = line;
        }
        if actor.state() == HostState::Failed {
            println!("{:>6}ms FAILED fault={:?}", now(), actor.fault());
            break;
        }
        thread::sleep(Duration::from_millis(20));
    }
    actor.shutdown();
    Ok(())
}
