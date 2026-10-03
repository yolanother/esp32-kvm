// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Restricts external esptool calls to read-only partition/boot-selection checks followed by
// one bounded factory-app image write. It validates observed layout before any flash command.

use esp32_kvm_host_actor::{AppFlashRequest, AppFlasher, FlashError};
use esp32_kvm_usb_transport::{PortIdentity, available_usb_ports, candidate_ports};
use std::ffi::OsString;
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicU64, Ordering};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

const EXPECTED_TABLE: &[u8; 0xc00] = include_bytes!("../expected-partitions.bin");
const TABLE_OFFSET: u32 = 0x8000;
const OTA_DATA_OFFSET: u32 = 0xd000;
const OTA_DATA_SIZE: usize = 0x2000;
const FACTORY_APP_OFFSET: u32 = 0x20000;
const FACTORY_APP_CAPACITY: u64 = 0x650000;
const READ_TIMEOUT: Duration = Duration::from_secs(15);
const WRITE_TIMEOUT: Duration = Duration::from_secs(120);
static NEXT_STAGE: AtomicU64 = AtomicU64::new(0);

/// A bounded esptool process did not complete successfully.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ToolError {
    /// The executable could not start.
    Spawn,
    /// It exited unsuccessfully.
    Failed,
    /// It exceeded its deadline and was terminated.
    Timeout,
}

/// Invokes a process without a shell; tests replace it with a recording fake.
pub trait ToolRunner {
    /// Runs one argument-vector command and kills it on timeout.
    fn run(
        &mut self,
        program: &Path,
        args: &[OsString],
        timeout: Duration,
    ) -> Result<(), ToolError>;
}

/// Production child-process runner with bounded waits and no inherited stdin.
pub struct SystemToolRunner;

impl ToolRunner for SystemToolRunner {
    fn run(
        &mut self,
        program: &Path,
        args: &[OsString],
        timeout: Duration,
    ) -> Result<(), ToolError> {
        let mut command = Command::new(program);
        command
            .args(args)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null());
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            command.creation_flags(0x0800_0000);
        }
        let mut child = command.spawn().map_err(|_| ToolError::Spawn)?;
        let started = Instant::now();
        loop {
            match child.try_wait() {
                Ok(Some(status)) => {
                    return if status.success() {
                        Ok(())
                    } else {
                        Err(ToolError::Failed)
                    };
                }
                Ok(None) if started.elapsed() < timeout => thread::sleep(Duration::from_millis(10)),
                Ok(None) => {
                    let _ = child.kill();
                    let _ = child.wait();
                    return Err(ToolError::Timeout);
                }
                Err(_) => {
                    let _ = child.kill();
                    let _ = child.wait();
                    return Err(ToolError::Failed);
                }
            }
        }
    }
}

/// USB enumeration failed before a unique matching interface could be selected.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PortError {
    /// The operating system could not enumerate serial interfaces.
    Unavailable,
}

/// Supplies OS serial interfaces for unique Espressif USB identity selection.
pub trait PortSource {
    /// Lists current port names and USB hardware identifiers.
    fn ports(&mut self) -> Result<Vec<PortIdentity>, PortError>;
}

/// Production USB serial interface enumeration.
pub struct SystemPorts;

impl PortSource for SystemPorts {
    fn ports(&mut self) -> Result<Vec<PortIdentity>, PortError> {
        available_usb_ports().map_err(|_| PortError::Unavailable)
    }
}

/// Invokes pinned esptool only after checking the exact device partition table and erased OTA data.
pub struct EsptoolFlasher<R: ToolRunner, P: PortSource> {
    executable: PathBuf,
    staging_root: PathBuf,
    runner: R,
    ports: P,
}

impl<R: ToolRunner, P: PortSource> EsptoolFlasher<R, P> {
    /// Supplies a trusted esptool executable, caller-owned staging root, and testable adapters.
    pub fn new(executable: PathBuf, staging_root: PathBuf, runner: R, ports: P) -> Self {
        Self {
            executable,
            staging_root,
            runner,
            ports,
        }
    }

    fn flash_bytes(&mut self, bytes: &[u8], offset: u32, capacity: u64) -> Result<(), FlashError> {
        if offset != FACTORY_APP_OFFSET
            || capacity != FACTORY_APP_CAPACITY
            || bytes.len() as u64 > capacity
            || !valid_app_identity(bytes)
            || !self.executable.is_absolute()
            || !self.executable.is_file()
        {
            return Err(FlashError::Failed);
        }
        let interfaces = self.ports.ports().map_err(|_| FlashError::Failed)?;
        let candidates = candidate_ports(&interfaces);
        let [port] = candidates.as_slice() else {
            return Err(FlashError::Failed);
        };
        let stage = StageDir::create(&self.staging_root).map_err(|_| FlashError::Failed)?;
        let table_path = stage.path("partitions.bin");
        let ota_path = stage.path("otadata.bin");
        let image_path = stage.path("app.bin");
        self.run_esptool(
            port,
            "read_flash",
            TABLE_OFFSET,
            Some(EXPECTED_TABLE.len()),
            &table_path,
            READ_TIMEOUT,
        )?;
        if read_exact_file(&table_path, EXPECTED_TABLE.len())
            .ok()
            .as_deref()
            != Some(EXPECTED_TABLE)
        {
            return Err(FlashError::Failed);
        }
        self.run_esptool(
            port,
            "read_flash",
            OTA_DATA_OFFSET,
            Some(OTA_DATA_SIZE),
            &ota_path,
            READ_TIMEOUT,
        )?;
        let ota = read_exact_file(&ota_path, OTA_DATA_SIZE).map_err(|_| FlashError::Failed)?;
        if ota.iter().any(|byte| *byte != 0xff) {
            return Err(FlashError::Failed);
        }
        let mut image = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&image_path)
            .map_err(|_| FlashError::Failed)?;
        image.write_all(bytes).map_err(|_| FlashError::Failed)?;
        image.sync_all().map_err(|_| FlashError::Failed)?;
        drop(image);
        if read_exact_file(&image_path, bytes.len()).ok().as_deref() != Some(bytes) {
            return Err(FlashError::Failed);
        }
        self.run_esptool(
            port,
            "write_flash",
            offset,
            None,
            &image_path,
            WRITE_TIMEOUT,
        )
    }

    fn run_esptool(
        &mut self,
        port: &str,
        operation: &str,
        offset: u32,
        length: Option<usize>,
        path: &Path,
        timeout: Duration,
    ) -> Result<(), FlashError> {
        let mut args = vec![
            "--chip".into(),
            "esp32s3".into(),
            "--port".into(),
            port.into(),
            "--before".into(),
            "default_reset".into(),
            "--after".into(),
            "hard_reset".into(),
            operation.into(),
        ];
        if operation == "write_flash" {
            args.push("--verify".into());
        }
        args.push(format!("0x{offset:x}").into());
        if let Some(length) = length {
            args.push(format!("0x{length:x}").into());
        }
        args.push(path.as_os_str().to_os_string());
        self.runner
            .run(&self.executable, &args, timeout)
            .map_err(|_| FlashError::Failed)
    }
}

impl<R: ToolRunner, P: PortSource> AppFlasher for EsptoolFlasher<R, P> {
    fn flash_app(&mut self, request: AppFlashRequest<'_>) -> Result<(), FlashError> {
        if request.partition() != "app" {
            return Err(FlashError::Failed);
        }
        self.flash_bytes(request.bytes(), request.offset(), request.capacity())
    }
}

fn read_exact_file(path: &Path, expected_len: usize) -> std::io::Result<Vec<u8>> {
    let mut file = File::open(path)?;
    if file.metadata()?.len() != expected_len as u64 {
        return Err(std::io::ErrorKind::InvalidData.into());
    }
    let mut bytes = vec![0; expected_len];
    file.read_exact(&mut bytes)?;
    Ok(bytes)
}

fn valid_app_identity(bytes: &[u8]) -> bool {
    if bytes.len() < 32 + 256 || bytes[0] != 0xe9 || !(1..=16).contains(&bytes[1]) {
        return false;
    }
    let chip_id = u16::from_le_bytes([bytes[12], bytes[13]]);
    let segment_len = u32::from_le_bytes(bytes[28..32].try_into().unwrap()) as usize;
    let magic = u32::from_le_bytes(bytes[32..36].try_into().unwrap());
    if chip_id != 9 || segment_len < 256 || segment_len > bytes.len() - 32 || magic != 0xabcd5432 {
        return false;
    }
    let field = |range: std::ops::Range<usize>| {
        let data = &bytes[range];
        let end = data.iter().position(|byte| *byte == 0)?;
        let value = std::str::from_utf8(&data[..end]).ok()?;
        (!value.is_empty() && value.bytes().all(|byte| byte.is_ascii_graphic())).then_some(value)
    };
    field(80..112) == Some("esp32_kvm") && field(48..80).is_some()
}

struct StageDir(PathBuf);

impl StageDir {
    fn create(root: &Path) -> std::io::Result<Self> {
        for _ in 0..8 {
            let nanos = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap_or_default()
                .as_nanos();
            let id = NEXT_STAGE.fetch_add(1, Ordering::Relaxed);
            let path = root.join(format!(
                "esp32-kvm-update-{}-{nanos}-{id}",
                std::process::id()
            ));
            match fs::create_dir(&path) {
                Ok(()) => return Ok(Self(path)),
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => return Err(error),
            }
        }
        Err(std::io::ErrorKind::AlreadyExists.into())
    }

    fn path(&self, name: &str) -> PathBuf {
        self.0.join(name)
    }
}

impl Drop for StageDir {
    fn drop(&mut self) {
        for name in ["partitions.bin", "otadata.bin", "app.bin"] {
            let _ = fs::remove_file(self.path(name));
        }
        let _ = fs::remove_dir(&self.0);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::OsString;
    use std::sync::{Arc, Mutex};

    #[derive(Clone)]
    struct FakePorts;
    impl PortSource for FakePorts {
        fn ports(&mut self) -> Result<Vec<PortIdentity>, PortError> {
            Ok(vec![PortIdentity::usb("COM9", 0x303a, 0x1001)])
        }
    }

    #[derive(Clone)]
    struct FakeRunner {
        calls: Arc<Mutex<Vec<Vec<OsString>>>>,
        table_ok: bool,
        oversize_table: bool,
        ota_erased: bool,
        fail_read: bool,
    }

    impl FakeRunner {
        fn new() -> Self {
            Self {
                calls: Arc::new(Mutex::new(Vec::new())),
                table_ok: true,
                oversize_table: false,
                ota_erased: true,
                fail_read: false,
            }
        }
    }

    impl ToolRunner for FakeRunner {
        fn run(&mut self, _: &Path, args: &[OsString], _: Duration) -> Result<(), ToolError> {
            self.calls.lock().unwrap().push(args.to_vec());
            let words: Vec<_> = args.iter().map(|arg| arg.to_string_lossy()).collect();
            if words.contains(&std::borrow::Cow::Borrowed("read_flash")) {
                if self.fail_read {
                    return Err(ToolError::Timeout);
                }
                let output = Path::new(args.last().unwrap());
                let bytes = if words.contains(&std::borrow::Cow::Borrowed("0x8000")) {
                    if self.oversize_table {
                        vec![0xff; EXPECTED_TABLE.len() + 1]
                    } else if self.table_ok {
                        EXPECTED_TABLE.to_vec()
                    } else {
                        vec![0; EXPECTED_TABLE.len()]
                    }
                } else if self.ota_erased {
                    vec![0xff; OTA_DATA_SIZE]
                } else {
                    vec![0; OTA_DATA_SIZE]
                };
                std::fs::write(output, bytes).unwrap();
            }
            Ok(())
        }
    }

    fn harness(runner: FakeRunner) -> EsptoolFlasher<FakeRunner, FakePorts> {
        EsptoolFlasher::new(
            std::env::current_exe().unwrap(),
            std::env::temp_dir(),
            runner,
            FakePorts,
        )
    }

    fn app_image() -> Vec<u8> {
        let mut bytes = vec![0; 32 + 256];
        bytes[0] = 0xe9;
        bytes[1] = 1;
        bytes[12..14].copy_from_slice(&9_u16.to_le_bytes());
        bytes[28..32].copy_from_slice(&256_u32.to_le_bytes());
        bytes[32..36].copy_from_slice(&0xabcd5432_u32.to_le_bytes());
        bytes[48..53].copy_from_slice(b"0.2.0");
        bytes[80..89].copy_from_slice(b"esp32_kvm");
        bytes
    }

    #[test]
    fn exact_observed_layout_and_blank_ota_allow_only_factory_app_write() {
        let runner = FakeRunner::new();
        let calls = runner.calls.clone();
        let mut flasher = harness(runner);
        flasher
            .flash_bytes(&app_image(), FACTORY_APP_OFFSET, FACTORY_APP_CAPACITY)
            .unwrap();
        let calls = calls.lock().unwrap();
        assert_eq!(calls.len(), 3);
        let commands: Vec<_> = calls
            .iter()
            .map(|call| {
                call.iter()
                    .map(|arg| arg.to_string_lossy().to_string())
                    .collect::<Vec<_>>()
            })
            .collect();
        assert_eq!(
            commands[0][commands[0].iter().position(|a| a == "read_flash").unwrap() + 1],
            "0x8000"
        );
        assert_eq!(
            commands[1][commands[1].iter().position(|a| a == "read_flash").unwrap() + 1],
            "0xd000"
        );
        assert_eq!(
            commands[2][commands[2].iter().position(|a| a == "write_flash").unwrap() + 1],
            "--verify"
        );
        assert!(commands[2].contains(&"0x20000".to_string()));
        assert!(
            !commands
                .iter()
                .flatten()
                .any(|arg| arg.contains("erase_flash"))
        );
    }

    #[test]
    fn partition_mismatch_or_active_ota_blocks_write() {
        for (table_ok, ota_erased) in [(false, true), (true, false)] {
            let mut runner = FakeRunner::new();
            runner.table_ok = table_ok;
            runner.ota_erased = ota_erased;
            let calls = runner.calls.clone();
            assert_eq!(
                harness(runner).flash_bytes(&app_image(), FACTORY_APP_OFFSET, FACTORY_APP_CAPACITY),
                Err(FlashError::Failed)
            );
            assert!(
                !calls
                    .lock()
                    .unwrap()
                    .iter()
                    .flatten()
                    .any(|arg| arg == "write_flash")
            );
        }
    }

    #[test]
    fn oversized_partition_read_is_rejected_before_write() {
        let mut runner = FakeRunner::new();
        runner.oversize_table = true;
        let calls = runner.calls.clone();
        assert_eq!(
            harness(runner).flash_bytes(&app_image(), FACTORY_APP_OFFSET, FACTORY_APP_CAPACITY),
            Err(FlashError::Failed)
        );
        assert_eq!(calls.lock().unwrap().len(), 1);
    }

    #[test]
    fn wrong_address_invalid_image_and_tool_timeout_block_write() {
        let runner = FakeRunner::new();
        let calls = runner.calls.clone();
        let mut flasher = harness(runner);
        assert_eq!(
            flasher.flash_bytes(&app_image(), 0x9000, FACTORY_APP_CAPACITY),
            Err(FlashError::Failed)
        );
        assert_eq!(
            flasher.flash_bytes(b"abc", FACTORY_APP_OFFSET, FACTORY_APP_CAPACITY),
            Err(FlashError::Failed)
        );
        let mut wrong_chip = app_image();
        wrong_chip[12..14].copy_from_slice(&1_u16.to_le_bytes());
        assert_eq!(
            flasher.flash_bytes(&wrong_chip, FACTORY_APP_OFFSET, FACTORY_APP_CAPACITY),
            Err(FlashError::Failed)
        );
        let mut wrong_project = app_image();
        wrong_project[80] = b'x';
        assert_eq!(
            flasher.flash_bytes(&wrong_project, FACTORY_APP_OFFSET, FACTORY_APP_CAPACITY),
            Err(FlashError::Failed)
        );
        assert!(calls.lock().unwrap().is_empty());
        let mut runner = FakeRunner::new();
        runner.fail_read = true;
        let calls = runner.calls.clone();
        assert_eq!(
            harness(runner).flash_bytes(&app_image(), FACTORY_APP_OFFSET, FACTORY_APP_CAPACITY),
            Err(FlashError::Failed)
        );
        assert_eq!(calls.lock().unwrap().len(), 1);
    }
}
