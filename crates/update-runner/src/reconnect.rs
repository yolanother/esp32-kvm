// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Rediscovers a uniquely identified USB Serial/JTAG interface after app flashing,
// negotiates a fresh firmware session, and requires authoritative local/disarmed STATUS.

use crate::flasher::{PortSource, SystemPorts};
use esp32_kvm_host_actor::{
    HostActor, HostState, KeyMapper, MappedKey, ReconnectError, Reconnector,
};
use esp32_kvm_platform_windows::CaptureGate;
use esp32_kvm_protocol::MAJOR;
use esp32_kvm_update_core::DeviceIdentity;
use esp32_kvm_usb_transport::{ConfirmedDevice, candidate_ports, probe_stream};
use std::thread;
use std::time::{Duration, Instant};

const REENUMERATION_TIMEOUT: Duration = Duration::from_secs(10);
const STATUS_TIMEOUT: Duration = Duration::from_millis(500);

/// Opens an application-mode candidate and proves CAPS, SESSION_OPEN, and local STATUS.
pub trait DeviceProbe {
    /// Returns a fresh verified session and its authoritative STATUS identity.
    fn probe(
        &mut self,
        port: &str,
        expected_board_id: &str,
    ) -> Result<(ConfirmedDevice, DeviceIdentity), ReconnectError>;
}

/// Production probe using the existing USB handshake and single serial actor.
pub struct SystemDeviceProbe {
    host_version: String,
}

impl SystemDeviceProbe {
    /// Sets the desktop host version transmitted during SESSION_OPEN.
    pub fn new(host_version: impl Into<String>) -> Self {
        Self {
            host_version: host_version.into(),
        }
    }
}

struct NoopMapper;

impl KeyMapper for NoopMapper {
    fn map_key(&self, _: u32, _: u32, _: bool) -> Option<MappedKey> {
        None
    }
}

impl DeviceProbe for SystemDeviceProbe {
    fn probe(
        &mut self,
        port: &str,
        expected_board_id: &str,
    ) -> Result<(ConfirmedDevice, DeviceIdentity), ReconnectError> {
        let mut stream = serialport::new(port, 115_200)
            .timeout(Duration::from_millis(20))
            .open()
            .map_err(|_| ReconnectError::Unavailable)?;
        let confirmed = probe_stream(&mut *stream, expected_board_id, &self.host_version)
            .map_err(|_| ReconnectError::Unavailable)?;
        let (capture, _receiver) = CaptureGate::new(32);
        let mut actor = HostActor::from_confirmed(
            stream,
            Box::new(capture),
            confirmed.clone(),
            Vec::new(),
            Box::new(NoopMapper),
            0,
        )
        .map_err(|_| ReconnectError::Unavailable)?;
        let started = Instant::now();
        while started.elapsed() < STATUS_TIMEOUT {
            actor.poll(started.elapsed().as_millis() as u64);
            match actor.state() {
                HostState::Local => {
                    return Ok((
                        confirmed.clone(),
                        DeviceIdentity {
                            board_id: confirmed.board_id.clone(),
                            protocol_major: MAJOR,
                            protocol_minor: confirmed.negotiated_minor,
                            app_partition_bytes: 0x650000,
                            firmware_version: confirmed.firmware_version.clone(),
                            local: true,
                            armed: false,
                        },
                    ));
                }
                HostState::Failed | HostState::Guest(_) | HostState::Pairing => {
                    return Err(ReconnectError::Unavailable);
                }
                HostState::AwaitStatus | HostState::Switching => {
                    thread::sleep(Duration::from_millis(5))
                }
            }
        }
        Err(ReconnectError::Unavailable)
    }
}

/// Re-enumerates USB candidates after flashing, refusing ambiguity or nonlocal sessions.
pub struct BoundedReconnector<P: PortSource, D: DeviceProbe> {
    ports: P,
    probe: D,
    timeout: Duration,
}

impl<P: PortSource, D: DeviceProbe> BoundedReconnector<P, D> {
    /// Creates a reconnect adapter with a ten-second re-enumeration deadline.
    pub fn new(ports: P, probe: D) -> Self {
        Self {
            ports,
            probe,
            timeout: REENUMERATION_TIMEOUT,
        }
    }
}

impl BoundedReconnector<SystemPorts, SystemDeviceProbe> {
    /// Constructs the production USB port enumerator and protocol probe.
    pub fn system(host_version: impl Into<String>) -> Self {
        Self::new(SystemPorts, SystemDeviceProbe::new(host_version))
    }
}

impl<P: PortSource, D: DeviceProbe> Reconnector for BoundedReconnector<P, D> {
    fn reconnect(
        &mut self,
        expected_board_id: &str,
    ) -> Result<(ConfirmedDevice, DeviceIdentity), ReconnectError> {
        let started = Instant::now();
        loop {
            let interfaces = self
                .ports
                .ports()
                .map_err(|_| ReconnectError::Unavailable)?;
            let candidates = candidate_ports(&interfaces);
            if candidates.len() > 1 {
                return Err(ReconnectError::Unavailable);
            }
            if let [port] = candidates.as_slice()
                && let Ok((confirmed, status)) = self.probe.probe(port, expected_board_id)
                && confirmed.session_id != 0
                && confirmed.board_id == expected_board_id
                && status.board_id == confirmed.board_id
                && status.firmware_version == confirmed.firmware_version
                && status.protocol_major == MAJOR
                && status.protocol_minor == confirmed.negotiated_minor
                && status.local
                && !status.armed
            {
                return Ok((confirmed, status));
            }
            if started.elapsed() >= self.timeout {
                return Err(ReconnectError::Unavailable);
            }
            thread::sleep(Duration::from_millis(20));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::flasher::PortError;
    use esp32_kvm_usb_transport::PortIdentity;
    use std::sync::{Arc, Mutex};

    struct Ports(Vec<PortIdentity>);
    impl PortSource for Ports {
        fn ports(&mut self) -> Result<Vec<PortIdentity>, PortError> {
            Ok(self.0.clone())
        }
    }

    struct Probe {
        calls: Arc<Mutex<usize>>,
        local: bool,
        session: u64,
    }
    impl DeviceProbe for Probe {
        fn probe(
            &mut self,
            _: &str,
            _: &str,
        ) -> Result<(ConfirmedDevice, DeviceIdentity), ReconnectError> {
            *self.calls.lock().unwrap() += 1;
            let confirmed = ConfirmedDevice {
                board_id: "esp32-kvm-s3".into(),
                session_id: self.session,
                max_connections: 1,
                max_bonds: 8,
                firmware_version: "0.2.0".into(),
                negotiated_minor: 2,
            };
            let status = DeviceIdentity {
                board_id: confirmed.board_id.clone(),
                protocol_major: MAJOR,
                protocol_minor: 2,
                app_partition_bytes: 0x650000,
                firmware_version: confirmed.firmware_version.clone(),
                local: self.local,
                armed: !self.local,
            };
            Ok((confirmed, status))
        }
    }

    fn ports(count: usize) -> Ports {
        Ports(
            (0..count)
                .map(|n| PortIdentity::usb(format!("COM{}", n + 7), 0x303a, 0x1001))
                .collect(),
        )
    }

    #[test]
    fn accepts_only_unique_verified_local_disarmed_reconnect() {
        let calls = Arc::new(Mutex::new(0));
        let mut reconnector = BoundedReconnector::new(
            ports(1),
            Probe {
                calls: calls.clone(),
                local: true,
                session: 6,
            },
        );
        assert!(reconnector.reconnect("esp32-kvm-s3").is_ok());
        assert_eq!(*calls.lock().unwrap(), 1);
    }

    #[test]
    fn ambiguous_port_and_remote_or_zero_session_fail_closed() {
        let calls = Arc::new(Mutex::new(0));
        let mut ambiguous = BoundedReconnector::new(
            ports(2),
            Probe {
                calls: calls.clone(),
                local: true,
                session: 6,
            },
        );
        assert!(ambiguous.reconnect("esp32-kvm-s3").is_err());
        assert_eq!(*calls.lock().unwrap(), 0);
        for (local, session) in [(false, 6), (true, 0)] {
            let mut reconnect = BoundedReconnector::new(
                ports(1),
                Probe {
                    calls: calls.clone(),
                    local,
                    session,
                },
            );
            reconnect.timeout = Duration::ZERO;
            assert!(reconnect.reconnect("esp32-kvm-s3").is_err());
        }
    }
}
