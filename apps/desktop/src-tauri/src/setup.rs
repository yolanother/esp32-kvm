// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Provides the Tauri setup boundary and crash-tolerant local guest labels.
// The serial-owning host actor supplies verified bonds and pairing controls;
// absent challenge events and all-up proof keep confirmation and testing closed.
use serde::{Deserialize, Serialize};
use std::fs::{self, OpenOptions};
use std::io::Write;
use std::path::PathBuf;
use std::sync::Mutex;
use tauri::State;

const PROFILE_SCHEMA: u32 = 1;

/// USB state supplied by the serial owner, or a candidate-only scan.
#[derive(Clone, Serialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum DeviceState {
    /// No eligible ESP32 USB interface was seen.
    Missing,
    /// An eligible VID/PID is present but firmware remains unverified.
    Candidate { port: String },
    /// The native session owner is checking the device.
    Handshaking,
    /// The firmware cannot be used by this host.
    Incompatible { reason: String },
    /// Board and protocol were confirmed by the single serial owner.
    Verified {
        /// Confirmed board ID.
        #[serde(rename = "boardId")]
        board_id: String,
        /// Version when CAPS exposes it; absent versions are never invented.
        #[serde(rename = "firmwareVersion")]
        firmware_version: Option<String>,
        /// Reported retained bond capacity.
        #[serde(rename = "maxBonds")]
        max_bonds: u8,
        /// Reported simultaneous connection capacity.
        #[serde(rename = "maxConnections")]
        max_connections: u8,
    },
    /// The serial or profile service could not provide status.
    Unavailable { reason: String },
}

/// Current firmware pairing state; codes never enter profile storage.
#[derive(Clone, Serialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum PairingState {
    /// No pairing window is open.
    Closed,
    /// Firmware opened a window but may not provide a deadline yet.
    Waiting {
        #[serde(rename = "deadlineMs")]
        deadline_ms: Option<u64>,
    },
}

/// A user-editable label tied to a 16-byte opaque firmware identity.
#[derive(Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct GuestProfile {
    /// Lowercase hexadecimal token, never BLE keys or addresses.
    pub bond_token: String,
    /// Friendly name scoped to this host.
    pub name: String,
    /// Guest OS choice used for future mapping suggestions.
    pub os: String,
    /// Explicit mapping profile choice.
    pub profile: String,
}

/// Combined native status and locally stored labels for the webview.
#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SetupSnapshot {
    /// Native USB and firmware status.
    pub device: DeviceState,
    /// Firmware pairing progress, if the wire contract exposes it.
    pub pairing: PairingState,
    /// Opaque identities currently reported by firmware.
    pub bond_tokens: Vec<String>,
    /// Identities with encrypted, subscribed HID readiness.
    pub ready_tokens: Vec<String>,
    /// Host-local names retained across unplug and restart.
    pub profiles: Vec<GuestProfile>,
    /// Whether native pairing commands can be sent to a verified session.
    pub pairing_available: bool,
}

/// Actor facts before host-local labels are joined for Tauri serialization.
#[derive(Clone)]
pub struct BackendSnapshot {
    /// Verified or candidate device state.
    pub device: DeviceState,
    /// Pairing window state supported by the current firmware contract.
    pub pairing: PairingState,
    /// Identities reported in firmware STATUS.
    pub bond_tokens: Vec<String>,
    /// Encrypted and subscribed live identities.
    pub ready_tokens: Vec<String>,
    /// Whether an actor-local pairing request can be accepted.
    pub pairing_available: bool,
}

impl BackendSnapshot {
    fn into_setup_snapshot(self, profiles: Vec<GuestProfile>) -> SetupSnapshot {
        SetupSnapshot {
            device: self.device,
            pairing: self.pairing,
            bond_tokens: self.bond_tokens,
            ready_tokens: self.ready_tokens,
            profiles,
            pairing_available: self.pairing_available,
        }
    }
}

/// Device facts and controls provided by the single serial-owning host actor.
pub trait SetupBackend: Send + Sync {
    /// Returns a current snapshot without opening a second serial stream.
    fn snapshot(&self) -> Result<BackendSnapshot, String>;
    /// Requests a 60-second pairing window.
    fn begin(&self) -> Result<(), String> {
        Err("Verified pairing transport is unavailable.".into())
    }
    /// Cancels pairing.
    fn cancel(&self) -> Result<(), String> {
        Err("Verified pairing transport is unavailable.".into())
    }
    /// Answers a firmware challenge ID.
    fn confirm(&self, _challenge_id: u32, _approved: bool) -> Result<(), String> {
        Err("Firmware challenge events are unavailable.".into())
    }
    /// Runs an explicit test and sends all-up before returning success.
    fn test_controls(&self, _bond_token: &str) -> Result<(), String> {
        Err("Native HID test transport is unavailable.".into())
    }
}

#[derive(Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct ProfileFile {
    schema_version: u32,
    revision: u64,
    profiles: Vec<GuestProfile>,
}

struct ProfileStore {
    directory: PathBuf,
    revision: u64,
    active_slot: usize,
    profiles: Vec<GuestProfile>,
}

impl ProfileStore {
    fn slot(&self, index: usize) -> PathBuf {
        self.directory.join(format!("guest-profiles-{index}.json"))
    }

    fn load(directory: PathBuf) -> Result<Self, String> {
        let mut store = Self {
            directory,
            revision: 0,
            active_slot: 1,
            profiles: Vec::new(),
        };
        let mut saw_file = false;
        let mut saw_valid = false;
        for slot in 0..2 {
            let bytes = match fs::read(store.slot(slot)) {
                Ok(bytes) => bytes,
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => continue,
                Err(error) => return Err(error.to_string()),
            };
            saw_file = true;
            let Ok(candidate) = serde_json::from_slice::<ProfileFile>(&bytes) else {
                continue;
            };
            if candidate.schema_version != PROFILE_SCHEMA
                || candidate
                    .profiles
                    .iter()
                    .any(|profile| !valid_profile(profile))
            {
                continue;
            }
            if !saw_valid || candidate.revision > store.revision {
                store.revision = candidate.revision;
                store.active_slot = slot;
                store.profiles = candidate.profiles;
            }
            saw_valid = true;
        }
        if saw_file && !saw_valid {
            return Err(
                "Saved guest profiles are incompatible or damaged; no file was erased.".into(),
            );
        }
        Ok(store)
    }

    fn save(&mut self, profile: GuestProfile) -> Result<(), String> {
        if !valid_profile(&profile) {
            return Err("Invalid guest name, token, OS or key profile.".into());
        }
        let mut profiles = self.profiles.clone();
        if let Some(existing) = profiles
            .iter_mut()
            .find(|entry| entry.bond_token == profile.bond_token)
        {
            *existing = profile;
        } else {
            profiles.push(profile);
        }
        let next = ProfileFile {
            schema_version: PROFILE_SCHEMA,
            revision: self.revision + 1,
            profiles: profiles.clone(),
        };
        let bytes = serde_json::to_vec(&next).map_err(|error| error.to_string())?;
        fs::create_dir_all(&self.directory).map_err(|error| error.to_string())?;
        let slot = 1 - self.active_slot;
        let path = self.slot(slot);
        let mut file = OpenOptions::new()
            .create(true)
            .write(true)
            .truncate(true)
            .open(path)
            .map_err(|error| error.to_string())?;
        file.write_all(&bytes).map_err(|error| error.to_string())?;
        file.sync_all().map_err(|error| error.to_string())?;
        self.profiles = profiles;
        self.revision = next.revision;
        self.active_slot = slot;
        Ok(())
    }
}

fn valid_profile(profile: &GuestProfile) -> bool {
    profile.bond_token.len() == 32
        && profile
            .bond_token
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
        && profile.bond_token.bytes().any(|byte| byte != b'0')
        && !profile.name.trim().is_empty()
        && profile.name.trim().chars().count() <= 64
        && matches!(profile.os.as_str(), "windows" | "macos" | "linux" | "other")
        && matches!(profile.profile.as_str(), "unchanged" | "windows-to-mac")
}

/// Shared Tauri setup service; its backend can be replaced by the host actor.
pub struct SetupService {
    backend: Box<dyn SetupBackend>,
    profiles: Mutex<Result<ProfileStore, String>>,
}

impl SetupService {
    /// Loads local labels and uses the supplied sole serial-session owner.
    pub fn new(directory: PathBuf, backend: Box<dyn SetupBackend>) -> Self {
        Self {
            backend,
            profiles: Mutex::new(ProfileStore::load(directory)),
        }
    }

    fn snapshot(&self) -> Result<SetupSnapshot, String> {
        let backend = self.backend.snapshot()?;
        let guard = self.profiles.lock().map_err(|error| error.to_string())?;
        let profiles = guard.as_ref().map_err(Clone::clone)?.profiles.clone();
        Ok(backend.into_setup_snapshot(profiles))
    }

    fn save_profile(&self, profile: GuestProfile) -> Result<(), String> {
        if !self
            .backend
            .snapshot()?
            .bond_tokens
            .contains(&profile.bond_token)
        {
            return Err("Firmware has not reported this bond identity.".into());
        }
        let mut guard = self.profiles.lock().map_err(|error| error.to_string())?;
        guard.as_mut().map_err(|error| error.clone())?.save(profile)
    }
}

/// Returns native setup status and saved local labels.
#[tauri::command]
pub fn setup_snapshot(service: State<'_, SetupService>) -> Result<SetupSnapshot, String> {
    service.snapshot()
}
/// Requests an explicit firmware pairing window; never flashes or arms input.
#[tauri::command]
pub fn setup_begin(service: State<'_, SetupService>) -> Result<(), String> {
    service.backend.begin()
}
/// Cancels the firmware pairing window.
#[tauri::command]
pub fn setup_cancel(service: State<'_, SetupService>) -> Result<(), String> {
    service.backend.cancel()
}
/// Answers a specific numeric challenge, rejecting zero and stale IDs in backend.
#[tauri::command]
pub fn setup_confirm(
    service: State<'_, SetupService>,
    challenge_id: u32,
    approved: bool,
) -> Result<(), String> {
    if challenge_id == 0 {
        return Err("A valid challenge ID is required.".into());
    }
    service.backend.confirm(challenge_id, approved)
}
/// Saves a name only for an identity present in live firmware STATUS.
#[tauri::command]
pub fn setup_save_profile(
    service: State<'_, SetupService>,
    profile: GuestProfile,
) -> Result<(), String> {
    service.save_profile(profile)
}
/// Performs an explicit native HID test that must end with all-up.
#[tauri::command]
pub fn setup_test_controls(
    service: State<'_, SetupService>,
    bond_token: String,
) -> Result<(), String> {
    if !service
        .backend
        .snapshot()?
        .ready_tokens
        .contains(&bond_token)
    {
        return Err("Guest is offline or HID is not ready.".into());
    }
    service.backend.test_controls(&bond_token)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn backend_snapshot_keeps_verified_facts_and_local_pairing_distinct() {
        let value = BackendSnapshot {
            device: DeviceState::Verified {
                board_id: "esp32-kvm-s3".into(),
                firmware_version: None,
                max_bonds: 8,
                max_connections: 1,
            },
            pairing: PairingState::Closed,
            bond_tokens: vec!["00112233445566778899aabbccddeeff".into()],
            ready_tokens: Vec::new(),
            pairing_available: true,
        };
        let json = serde_json::to_value(value.into_setup_snapshot(Vec::new())).unwrap();
        assert_eq!(json["device"]["kind"], "verified");
        assert_eq!(json["device"]["boardId"], "esp32-kvm-s3");
        assert_eq!(json["pairing"]["kind"], "closed");
        assert_eq!(json["readyTokens"], serde_json::json!([]));
    }

    struct OneBondBackend;
    impl SetupBackend for OneBondBackend {
        fn snapshot(&self) -> Result<BackendSnapshot, String> {
            Ok(BackendSnapshot {
                device: DeviceState::Verified {
                    board_id: "esp32-kvm-s3".into(),
                    firmware_version: None,
                    max_bonds: 8,
                    max_connections: 1,
                },
                pairing: PairingState::Closed,
                bond_tokens: vec!["00112233445566778899aabbccddeeff".into()],
                ready_tokens: Vec::new(),
                pairing_available: false,
            })
        }
    }

    fn profile(token: &str) -> GuestProfile {
        GuestProfile {
            bond_token: token.into(),
            name: "Work Mac".into(),
            os: "macos".into(),
            profile: "unchanged".into(),
        }
    }

    #[test]
    fn profile_validation_rejects_unbound_shapes() {
        assert!(valid_profile(&profile("00112233445566778899aabbccddeeff")));
        assert!(!valid_profile(&profile("0011")));
        assert!(!valid_profile(&profile("00000000000000000000000000000000")));
        assert!(!valid_profile(&profile("00112233445566778899AABBCCDDEEFF")));
        let mut invalid = profile("00112233445566778899aabbccddeeff");
        invalid.name = " ".into();
        assert!(!valid_profile(&invalid));
    }

    #[test]
    fn two_slot_storage_keeps_a_last_good_revision() {
        let directory =
            std::env::temp_dir().join(format!("esp32-kvm-setup-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&directory);
        let mut store = ProfileStore::load(directory.clone()).unwrap();
        let token = "00112233445566778899aabbccddeeff";
        store.save(profile(token)).unwrap();
        let prior_slot = store.active_slot;
        let mut renamed = profile(token);
        renamed.name = "Renamed".into();
        store.save(renamed).unwrap();
        assert_eq!(
            ProfileStore::load(directory.clone()).unwrap().profiles[0].name,
            "Renamed"
        );
        fs::write(store.slot(store.active_slot), b"interrupted").unwrap();
        let recovered = ProfileStore::load(directory.clone()).unwrap();
        assert_eq!(recovered.active_slot, prior_slot);
        assert_eq!(recovered.profiles[0].name, "Work Mac");
        fs::remove_dir_all(directory).unwrap();
    }

    #[test]
    fn profile_save_requires_a_firmware_reported_bond_and_stays_offline() {
        let directory =
            std::env::temp_dir().join(format!("esp32-kvm-bond-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&directory);
        let service = SetupService::new(directory.clone(), Box::new(OneBondBackend));
        assert!(
            service
                .save_profile(profile("ffeeddccbbaa99887766554433221100"))
                .is_err()
        );
        service
            .save_profile(profile("00112233445566778899aabbccddeeff"))
            .unwrap();
        let snapshot = service.snapshot().unwrap();
        assert_eq!(snapshot.profiles.len(), 1);
        assert!(snapshot.ready_tokens.is_empty());
        fs::remove_dir_all(directory).unwrap();
    }
}
