use esp32_kvm_protocol::{Frame, MessageKind};
use esp32_kvm_usb_transport::{available_usb_ports, probe_stream};
use std::{
    io::{Read, Write},
    time::{Duration, Instant},
};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("ports={:?}", available_usb_ports()?);
    let mut serial = serialport::new("COM4", 115_200)
        .timeout(Duration::from_millis(20))
        .open()?;
    let bytes = Frame::new(MessageKind::Hello, 0, 0, 0, vec![0; 6])
        .encode()
        .unwrap();
    println!("hello={bytes:02x?}");
    serial.write_all(&[0])?;
    serial.write_all(&bytes)?;
    serial.flush()?;
    let start = Instant::now();
    let mut buffer = [0u8; 256];
    while start.elapsed() < Duration::from_secs(1) {
        if let Ok(length) = serial.read(&mut buffer) {
            println!("at={:?} bytes={:02x?}", start.elapsed(), &buffer[..length]);
        }
    }
    let start = Instant::now();
    println!(
        "probe={:?}",
        probe_stream(&mut *serial, "esp32-kvm-s3", "probe-serial")
    );
    println!("probe duration={:?}", start.elapsed());
    Ok(())
}
