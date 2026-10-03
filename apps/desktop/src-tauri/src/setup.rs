// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Provides the Tauri setup boundary and versioned, crash-tolerant local guest profiles.
// The serial-owning host actor supplies live status, authoritative retained
// inventory, pairing controls, and physical key rule storage. Removal preserves
// profiles until verified, while mapping drafts are validated before install.
use esp32_kvm_input_core::{
    Destination, MappingPreset, MappingProfile, MappingRule, Side, SourceKey, preset_profile,
};
use serde::{Deserialize, Serialize};
use std::collections::BTreeSet;
use std::fs::{self, OpenOptions};
use std::io::Write;
use std::path::PathBuf;
use std::sync::Mutex;
use tauri::State;

const PROFILE_SCHEMA: u32 = 2;

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
    /// A fresh numeric comparison reported by firmware.
    Challenge {
        #[serde(rename = "challengeId")]
        challenge_id: u32,
        number: u32,
        #[serde(rename = "deadlineMs")]
        deadline_ms: Option<u64>,
    },
    /// Firmware bond storage is at capacity.
    Full,
    /// The pairing window expired.
    Expired,
    /// This firmware does not support the required pairing status contract.
    Unsupported { reason: String },
    /// Firmware rejected pairing or sent incomplete challenge data.
    Failed { reason: String },
}

/// Actor-confirmed route; USB or BLE readiness alone cannot select a guest.
#[derive(Clone, Serialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum RouteState {
    /// Capture is disarmed and Windows owns input.
    Local,
    /// Firmware STATUS is still pending after a verified handshake.
    AwaitingStatus,
    /// A control request awaits the exact firmware ACK.
    Switching,
    /// Pairing keeps input on the local host.
    Pairing,
    /// An acknowledged guest route with a firmware-reported bond identity.
    Guest {
        /// Firmware slot index.
        slot: u8,
        /// Lowercase hex of the opaque sixteen-byte bond identity.
        #[serde(rename = "bondToken")]
        bond_token: String,
    },
    /// The serial or capture session failed and local input was restored.
    Failed {
        /// Machine-readable fault category, without typed input.
        reason: String,
    },
}

/// Idempotent result of a confirmed guest forget request.
#[derive(Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum ForgetOutcome {
    /// Firmware no longer reports the bond and the local profile was removed.
    Forgot,
    /// No local profile remained from an earlier completed request.
    AlreadyAbsent,
}

/// A user-editable label tied to a 16-byte opaque firmware identity.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
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
    /// Optional direct-select shortcut label; registration is a separate gate.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub direct_shortcut: Option<String>,
    /// Optional identifier of a host-local mapping profile.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub mapping_profile_id: Option<String>,
    /// Optional identifier of a host-local monitor-layout link.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub layout_link_id: Option<String>,
    /// Base preset retained when a custom copy is reset.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub custom_base_preset: Option<String>,
    /// Host-local custom sided modifier replacements; only changed bindings are saved.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub modifier_bindings: Vec<ModifierBinding>,
    /// Bounded per-guest physical key and exact-chord overrides.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub key_rules: Vec<StoredKeyRule>,
}

/// One physical modifier replacement in a guest's custom preset.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct ModifierBinding {
    /// Physical HID modifier usage from E0 through E7.
    pub source_usage: u8,
    /// Guest HID modifier usage from E0 through E7.
    pub target_usage: u8,
}

/// Physical side stored alongside a USB HID usage for an exact trigger.
#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum StoredSide {
    Unspecified,
    Left,
    Right,
}

/// One physical HID keyboard usage and its modifier side.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct StoredKey {
    /// USB HID keyboard usage, not a translated text character.
    pub usage: u8,
    /// Physical side for modifier usages; ordinary keys are unspecified.
    pub side: StoredSide,
}

/// One bounded user override, applied once above the chosen preset.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct StoredKeyRule {
    /// Exact physical trigger, in any order.
    pub source: Vec<StoredKey>,
    /// Emitted guest usages and modifiers.
    pub target: Vec<StoredKey>,
    /// Higher priority wins among otherwise equal triggers.
    pub priority: i16,
    /// Disabled rules remain saved but cannot emit input.
    pub enabled: bool,
}

fn native_side(side: StoredSide) -> Side {
    match side {
        StoredSide::Unspecified => Side::Unspecified,
        StoredSide::Left => Side::Left,
        StoredSide::Right => Side::Right,
    }
}

/// Converts a persisted guest preset and physical key overrides to native rules.
pub fn mapping_profile(guest: &GuestProfile) -> Result<MappingProfile, String> {
    if !valid_profile(guest) {
        return Err("Invalid guest mapping profile.".into());
    }
    let mut result = match guest.profile.as_str() {
        "unchanged" => preset_profile(MappingPreset::Unchanged),
        "cmd-to-ctrl" => preset_profile(MappingPreset::CmdToCtrl),
        "windows-to-mac" => preset_profile(MappingPreset::WindowsToMac),
        "custom" => {
            let rules = guest
                .modifier_bindings
                .iter()
                .map(|binding| MappingRule {
                    source: vec![SourceKey {
                        usage: binding.source_usage,
                        side: if binding.source_usage <= 0xe3 {
                            Side::Left
                        } else {
                            Side::Right
                        },
                    }],
                    target: vec![Destination::Modifier(1 << (binding.target_usage - 0xe0))],
                    priority: 0,
                    enabled: true,
                })
                .collect();
            MappingProfile {
                preset: rules,
                rules: Vec::new(),
            }
        }
        _ => return Err("Unknown mapping preset.".into()),
    };
    result.rules = guest
        .key_rules
        .iter()
        .map(|rule| MappingRule {
            source: rule
                .source
                .iter()
                .map(|key| SourceKey {
                    usage: key.usage,
                    side: native_side(key.side),
                })
                .collect(),
            target: rule
                .target
                .iter()
                .map(|key| {
                    if key.usage >= 0xe0 {
                        Destination::Modifier(1 << (key.usage - 0xe0))
                    } else {
                        Destination::Usage(key.usage)
                    }
                })
                .collect(),
            priority: rule.priority,
            enabled: rule.enabled,
        })
        .collect();
    Ok(result)
}

pub(crate) fn token_bytes(token: &str) -> Result<[u8; 16], String> {
    if token.len() != 32 {
        return Err("Invalid bond identity token.".into());
    }
    let mut bytes = [0; 16];
    for (index, byte) in bytes.iter_mut().enumerate() {
        *byte = u8::from_str_radix(&token[index * 2..index * 2 + 2], 16)
            .map_err(|_| "Invalid bond identity token.")?;
    }
    Ok(bytes)
}

/// Combined native status and locally stored labels for the webview.
#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SetupSnapshot {
    /// Native USB and firmware status.
    pub device: DeviceState,
    /// Confirmed local, transitional, guest, or failed route.
    pub route: RouteState,
    /// Firmware pairing progress, if the wire contract exposes it.
    pub pairing: PairingState,
    /// Opaque identities currently reported by firmware.
    pub bond_tokens: Vec<String>,
    /// Fresh retained firmware bonds, or unknown while unread or unavailable.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub retained_bond_tokens: Option<Vec<String>>,
    /// Bond identities with a live BLE peer, even before HID subscription.
    pub connected_tokens: Vec<String>,
    /// Identities with encrypted, subscribed HID readiness.
    pub ready_tokens: Vec<String>,
    /// Host-local names retained across unplug and restart.
    pub profiles: Vec<GuestProfile>,
    /// Saved mappings that await installation in the native actor.
    pub mapping_pending_tokens: Vec<String>,
    /// Whether native pairing commands can be sent to a verified session.
    pub pairing_available: bool,
}

/// Actor facts before host-local labels are joined for Tauri serialization.
#[derive(Clone)]
pub struct BackendSnapshot {
    /// Verified or candidate device state.
    pub device: DeviceState,
    /// Routing truth from the host actor or disarmed local gate.
    pub route: RouteState,
    /// Pairing window state supported by the current firmware contract.
    pub pairing: PairingState,
    /// Identities reported in firmware STATUS.
    pub bond_tokens: Vec<String>,
    /// Fresh retained firmware bonds, distinct from live STATUS slots.
    pub retained_bond_tokens: Option<Vec<String>>,
    /// Connected peer identities from firmware STATUS.
    pub connected_tokens: Vec<String>,
    /// Encrypted and subscribed live identities.
    pub ready_tokens: Vec<String>,
    /// Whether an actor-local pairing request can be accepted.
    pub pairing_available: bool,
}

impl BackendSnapshot {
    fn into_setup_snapshot(self, profiles: Vec<GuestProfile>) -> SetupSnapshot {
        SetupSnapshot {
            device: self.device,
            route: self.route,
            pairing: self.pairing,
            bond_tokens: self.bond_tokens,
            retained_bond_tokens: self.retained_bond_tokens,
            connected_tokens: self.connected_tokens,
            ready_tokens: self.ready_tokens,
            profiles,
            mapping_pending_tokens: Vec::new(),
            pairing_available: self.pairing_available,
        }
    }
}

/// Device facts and controls provided by the single serial-owning host actor.
pub trait SetupBackend: Send + Sync {
    /// Returns a current snapshot without opening a second serial stream.
    fn snapshot(&self) -> Result<BackendSnapshot, String>;
    /// Caches a validated per-bond mapping in the single serial-owning worker.
    fn install_mapping(
        &self,
        _bond_token: [u8; 16],
        _profile: MappingProfile,
    ) -> Result<(), String> {
        Ok(())
    }
    /// Refreshes authoritative retained inventory on the verified actor stream.
    fn refresh_inventory(&self) -> Result<BackendSnapshot, String> {
        Err("Firmware retained-bond inventory is unavailable.".into())
    }
    /// Forgets one firmware bond and returns only after ACK and fresh inventory.
    fn forget_bond(&self, _bond_token: &str) -> Result<(), String> {
        Err("Firmware bond forgetting is not wired to the host actor.".into())
    }
    /// Requests an acknowledged return to the local host when an actor exists.
    fn return_local(&self) -> Result<(), String> {
        Err("Native local-return transport is unavailable.".into())
    }
    /// Selects a currently ready guest by its opaque bond identity.
    fn select_guest(&self, _bond_token: [u8; 16]) -> Result<(), String> {
        Err("Native guest-selection transport is unavailable.".into())
    }
    /// Disarms capture and closes the verified stream before app exit.
    fn release_for_exit(&self) -> Result<(), String> {
        Err("Native release transport is unavailable.".into())
    }
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
        let mut selected_schema = 0;
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
            if !matches!(candidate.schema_version, 1 | PROFILE_SCHEMA)
                || !valid_profiles(&candidate.profiles)
            {
                continue;
            }
            if !saw_valid
                || candidate.revision > store.revision
                || (candidate.revision == store.revision
                    && candidate.schema_version > selected_schema)
            {
                store.revision = candidate.revision;
                store.active_slot = slot;
                store.profiles = candidate.profiles;
                selected_schema = candidate.schema_version;
            }
            saw_valid = true;
        }
        if saw_file && !saw_valid {
            return Err(
                "Saved guest profiles are incompatible or damaged; no file was erased.".into(),
            );
        }
        if saw_valid && selected_schema == 1 {
            store.backup_v1()?;
            store.write_profiles(store.profiles.clone())?;
        }
        Ok(store)
    }

    fn backup_v1(&self) -> Result<(), String> {
        let path = self.directory.join("guest-profiles-v1-backup.json");
        if path.exists() {
            let bytes = fs::read(path).map_err(|error| error.to_string())?;
            let saved: ProfileFile =
                serde_json::from_slice(&bytes).map_err(|error| error.to_string())?;
            if saved.schema_version != 1 || !valid_profiles(&saved.profiles) {
                return Err("Existing v1 profile backup is damaged; migration stopped.".into());
            }
            return Ok(());
        }
        let bytes = fs::read(self.slot(self.active_slot)).map_err(|error| error.to_string())?;
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(path)
            .map_err(|error| error.to_string())?;
        file.write_all(&bytes).map_err(|error| error.to_string())?;
        file.sync_all().map_err(|error| error.to_string())
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
        if !valid_profiles(&profiles) {
            return Err("Duplicate bond identity or direct shortcut.".into());
        }
        self.write_profiles(profiles)
    }

    fn write_profiles(&mut self, profiles: Vec<GuestProfile>) -> Result<(), String> {
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

    fn remove(&mut self, token: &str) -> Result<(), String> {
        let profiles: Vec<_> = self
            .profiles
            .iter()
            .filter(|profile| profile.bond_token != token)
            .cloned()
            .collect();
        if profiles.len() == self.profiles.len() {
            return Ok(());
        }
        self.write_profiles(profiles)
    }
}

fn valid_profiles(profiles: &[GuestProfile]) -> bool {
    profiles.iter().enumerate().all(|(index, profile)| {
        valid_profile(profile)
            && profiles[..index].iter().all(|other| {
                other.bond_token != profile.bond_token
                    && (profile.direct_shortcut.is_none()
                        || other.direct_shortcut != profile.direct_shortcut)
            })
    })
}

fn valid_stored_key(key: &StoredKey) -> bool {
    (0x04..=0xe7).contains(&key.usage)
        && match key.usage {
            0xe0..=0xe3 => key.side == StoredSide::Left,
            0xe4..=0xe7 => key.side == StoredSide::Right,
            _ => key.side == StoredSide::Unspecified,
        }
}

fn valid_key_rules(rules: &[StoredKeyRule]) -> bool {
    rules.len() <= 32
        && rules.iter().enumerate().all(|(index, rule)| {
            (-100..=100).contains(&rule.priority)
                && (1..=4).contains(&rule.source.len())
                && (1..=4).contains(&rule.target.len())
                && rule.source.iter().all(valid_stored_key)
                && rule.target.iter().all(valid_stored_key)
                && rule
                    .source
                    .iter()
                    .enumerate()
                    .all(|(i, key)| !rule.source[..i].contains(key))
                && rule
                    .target
                    .iter()
                    .enumerate()
                    .all(|(i, key)| !rule.target[..i].contains(key))
                && !(rule.source.iter().any(|key| key.usage == 0xe0)
                    && rule.source.iter().any(|key| key.usage == 0xe4))
                && (!rule.enabled
                    || rules[..index].iter().all(|earlier| {
                        !earlier.enabled
                            || earlier.priority != rule.priority
                            || earlier.source.len() != rule.source.len()
                            || !earlier.source.iter().all(|key| rule.source.contains(key))
                    }))
        })
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
        && matches!(
            profile.profile.as_str(),
            "unchanged" | "cmd-to-ctrl" | "windows-to-mac" | "custom"
        )
        && profile
            .custom_base_preset
            .as_deref()
            .is_none_or(|base| matches!(base, "unchanged" | "cmd-to-ctrl" | "windows-to-mac"))
        && (profile.profile == "custom"
            || (profile.custom_base_preset.is_none() && profile.modifier_bindings.is_empty()))
        && profile.modifier_bindings.len() <= 8
        && valid_key_rules(&profile.key_rules)
        && profile
            .modifier_bindings
            .iter()
            .enumerate()
            .all(|(index, binding)| {
                (0xe0..=0xe7).contains(&binding.source_usage)
                    && (0xe0..=0xe7).contains(&binding.target_usage)
                    && binding.source_usage != binding.target_usage
                    && profile.modifier_bindings[..index]
                        .iter()
                        .all(|other| other.source_usage != binding.source_usage)
            })
        && profile.direct_shortcut.as_deref().is_none_or(|value| {
            !value.is_empty()
                && value.len() <= 64
                && value
                    .bytes()
                    .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'+' | b'-'))
        })
        && [
            profile.mapping_profile_id.as_deref(),
            profile.layout_link_id.as_deref(),
        ]
        .into_iter()
        .all(|value| {
            value.is_none_or(|id| {
                !id.is_empty()
                    && id.len() <= 64
                    && id.bytes().all(|byte| {
                        byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'.')
                    })
            })
        })
}

/// Shared Tauri setup service; its backend can be replaced by the host actor.
pub struct SetupService {
    backend: Box<dyn SetupBackend>,
    profiles: Mutex<Result<ProfileStore, String>>,
    mapping_pending: Mutex<BTreeSet<String>>,
}

impl SetupService {
    /// Loads local labels and uses the supplied sole serial-session owner.
    pub fn new(directory: PathBuf, backend: Box<dyn SetupBackend>) -> Self {
        let profiles = ProfileStore::load(directory);
        let pending = profiles
            .as_ref()
            .map(|store| {
                store
                    .profiles
                    .iter()
                    .map(|guest| guest.bond_token.clone())
                    .collect()
            })
            .unwrap_or_default();
        Self {
            backend,
            profiles: Mutex::new(profiles),
            mapping_pending: Mutex::new(pending),
        }
    }

    fn reconcile_one(&self, pending: &mut BTreeSet<String>) {
        let Some(token) = pending.iter().next().cloned() else {
            return;
        };
        let profile = self.profiles.lock().ok().and_then(|guard| {
            guard.as_ref().ok().and_then(|store| {
                store
                    .profiles
                    .iter()
                    .find(|guest| guest.bond_token == token)
                    .cloned()
            })
        });
        let Some(profile) = profile else {
            pending.remove(&token);
            return;
        };
        if let (Ok(bytes), Ok(mapping)) = (token_bytes(&token), mapping_profile(&profile))
            && self.backend.install_mapping(bytes, mapping).is_ok()
        {
            pending.remove(&token);
        }
    }

    /// Requests local host control through the sole serial owner.
    pub(crate) fn return_local(&self) -> Result<(), String> {
        self.backend.return_local()
    }

    /// Selects a saved, ready guest only while the verified route is local.
    pub(crate) fn select_guest(&self, token: &str) -> Result<(), String> {
        let bond_token = token_bytes(token)?;
        let snapshot = self.snapshot()?;
        if !matches!(snapshot.device, DeviceState::Verified { .. })
            || !matches!(snapshot.route, RouteState::Local)
        {
            return Err("Guest selection requires verified local control.".into());
        }
        if !snapshot
            .profiles
            .iter()
            .any(|profile| profile.bond_token == token)
        {
            return Err("Save a guest profile before selecting it.".into());
        }
        if snapshot
            .mapping_pending_tokens
            .iter()
            .any(|pending| pending == token)
        {
            return Err("Guest mapping is still being installed.".into());
        }
        if !snapshot.ready_tokens.iter().any(|ready| ready == token) {
            return Err("Guest is offline or HID is not ready.".into());
        }
        self.backend.select_guest(bond_token)
    }

    /// Disarms and stops the actor for an explicit tray quit.
    pub(crate) fn release_for_exit(&self) -> Result<(), String> {
        self.backend.release_for_exit()
    }

    pub(crate) fn snapshot(&self) -> Result<SetupSnapshot, String> {
        let mut pending = self
            .mapping_pending
            .lock()
            .map_err(|error| error.to_string())?;
        self.reconcile_one(&mut pending);
        let mut backend = self.backend.snapshot()?;
        if matches!(backend.device, DeviceState::Verified { .. })
            && matches!(backend.route, RouteState::Local)
            && backend.retained_bond_tokens.is_none()
            && let Ok(fresh) = self.backend.refresh_inventory()
        {
            backend = fresh;
        }
        let guard = self.profiles.lock().map_err(|error| error.to_string())?;
        let profiles = guard.as_ref().map_err(Clone::clone)?.profiles.clone();
        let mut snapshot = backend.into_setup_snapshot(profiles);
        snapshot.mapping_pending_tokens = pending.iter().cloned().collect();
        Ok(snapshot)
    }

    fn save_profile(&self, profile: GuestProfile) -> Result<(), String> {
        mapping_profile(&profile)?;
        token_bytes(&profile.bond_token)?;
        let known = self
            .profiles
            .lock()
            .map_err(|error| error.to_string())?
            .as_ref()
            .map_err(Clone::clone)?
            .profiles
            .iter()
            .any(|entry| entry.bond_token == profile.bond_token);
        if !known
            && !self
                .backend
                .snapshot()?
                .bond_tokens
                .contains(&profile.bond_token)
        {
            return Err("Firmware has not reported this bond identity.".into());
        }
        let mut pending = self
            .mapping_pending
            .lock()
            .map_err(|error| error.to_string())?;
        let mut guard = self.profiles.lock().map_err(|error| error.to_string())?;
        let token = profile.bond_token.clone();
        guard
            .as_mut()
            .map_err(|error| error.clone())?
            .save(profile)?;
        pending.insert(token);
        drop(guard);
        self.reconcile_one(&mut pending);
        Ok(())
    }

    fn forget_profile(&self, token: &str) -> Result<ForgetOutcome, String> {
        token_bytes(token)?;
        let exists = self
            .profiles
            .lock()
            .map_err(|error| error.to_string())?
            .as_ref()
            .map_err(Clone::clone)?
            .profiles
            .iter()
            .any(|entry| entry.bond_token == token);
        let before = self.backend.snapshot()?;
        if !matches!(before.device, DeviceState::Verified { .. })
            || !matches!(before.route, RouteState::Local)
        {
            return Err("Return locally with a verified device before forgetting this guest; the profile was kept.".into());
        }
        let fresh = self.backend.refresh_inventory()?;
        if !matches!(fresh.device, DeviceState::Verified { .. })
            || !matches!(fresh.route, RouteState::Local)
        {
            return Err(
                "A fresh local firmware session is required; the local profile was kept.".into(),
            );
        }
        let Some(retained) = fresh.retained_bond_tokens else {
            return Err(
                "Retained-bond inventory is unavailable; the local profile was kept.".into(),
            );
        };
        if !retained.iter().any(|bond| bond == token) {
            return if exists {
                Err("Firmware inventory already excludes this bond; the local profile was kept for reconciliation.".into())
            } else {
                Ok(ForgetOutcome::AlreadyAbsent)
            };
        }
        self.backend.forget_bond(token)?;
        let after = self.backend.snapshot()?;
        if !matches!(after.device, DeviceState::Verified { .. })
            || !matches!(after.route, RouteState::Local)
            || after
                .retained_bond_tokens
                .as_ref()
                .is_none_or(|bonds| bonds.iter().any(|bond| bond == token))
        {
            return Err(
                "Firmware has not confirmed bond removal; the local profile was kept.".into(),
            );
        }
        if exists {
            let mut guard = self.profiles.lock().map_err(|error| error.to_string())?;
            guard
                .as_mut()
                .map_err(|error| error.clone())?
                .remove(token)?;
            drop(guard);
            self.mapping_pending
                .lock()
                .map_err(|error| error.to_string())?
                .remove(token);
        }
        Ok(ForgetOutcome::Forgot)
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

/// Forgets a guest only after explicit confirmation and firmware removal.
#[tauri::command]
pub fn setup_forget_guest(
    service: State<'_, SetupService>,
    bond_token: String,
    confirmed: bool,
) -> Result<ForgetOutcome, String> {
    if !confirmed {
        return Err("Explicit confirmation is required; no profile was removed.".into());
    }
    service.forget_profile(&bond_token)
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

/// Requests local input through the verified actor; the UI waits for STATUS.
#[tauri::command]
pub fn dashboard_return_local(service: State<'_, SetupService>) -> Result<(), String> {
    service.return_local()
}

/// Requests a saved ready guest by token; the serial actor chooses its live slot.
#[tauri::command]
pub fn dashboard_select_guest(
    service: State<'_, SetupService>,
    bond_token: String,
) -> Result<(), String> {
    service.select_guest(&bond_token)
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
            route: RouteState::Guest {
                slot: 1,
                bond_token: "00112233445566778899aabbccddeeff".into(),
            },
            bond_tokens: vec!["00112233445566778899aabbccddeeff".into()],
            retained_bond_tokens: None,
            connected_tokens: vec!["00112233445566778899aabbccddeeff".into()],
            ready_tokens: Vec::new(),
            pairing_available: true,
        };
        let json = serde_json::to_value(value.into_setup_snapshot(Vec::new())).unwrap();
        assert_eq!(json["device"]["kind"], "verified");
        assert_eq!(json["device"]["boardId"], "esp32-kvm-s3");
        assert_eq!(json["pairing"]["kind"], "closed");
        assert_eq!(json["route"]["kind"], "guest");
        assert_eq!(
            json["route"]["bondToken"],
            "00112233445566778899aabbccddeeff"
        );
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
                route: RouteState::Local,
                pairing: PairingState::Closed,
                bond_tokens: vec!["00112233445566778899aabbccddeeff".into()],
                retained_bond_tokens: None,
                connected_tokens: vec!["00112233445566778899aabbccddeeff".into()],
                ready_tokens: Vec::new(),
                pairing_available: false,
            })
        }
    }

    struct ReadyBondBackend;
    impl SetupBackend for ReadyBondBackend {
        fn snapshot(&self) -> Result<BackendSnapshot, String> {
            let mut snapshot = OneBondBackend.snapshot()?;
            snapshot.ready_tokens = snapshot.bond_tokens.clone();
            Ok(snapshot)
        }

        fn select_guest(&self, token: [u8; 16]) -> Result<(), String> {
            assert_eq!(token, token_bytes("00112233445566778899aabbccddeeff")?);
            Ok(())
        }
    }

    fn profile(token: &str) -> GuestProfile {
        GuestProfile {
            bond_token: token.into(),
            name: "Work Mac".into(),
            os: "macos".into(),
            profile: "unchanged".into(),
            direct_shortcut: None,
            mapping_profile_id: None,
            layout_link_id: None,
            custom_base_preset: None,
            modifier_bindings: Vec::new(),
            key_rules: Vec::new(),
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
    fn selection_requires_saved_ready_guest_and_forwards_opaque_token() {
        let directory =
            std::env::temp_dir().join(format!("esp32-kvm-selection-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&directory);
        let token = "00112233445566778899aabbccddeeff";
        let ready = SetupService::new(directory.clone(), Box::new(ReadyBondBackend));
        assert!(ready.select_guest(token).is_err());
        ready.save_profile(profile(token)).unwrap();
        ready.select_guest(token).unwrap();
        let offline = SetupService::new(directory.clone(), Box::new(OneBondBackend));
        assert!(offline.select_guest(token).is_err());
        fs::remove_dir_all(directory).unwrap();
    }

    #[test]
    fn physical_chord_rule_persists_and_rejects_duplicate_or_emergency_trigger() {
        let directory =
            std::env::temp_dir().join(format!("esp32-kvm-key-rule-{}", std::process::id()));
        let _ = fs::remove_dir_all(&directory);
        let mut guest = profile("00112233445566778899aabbccddeeff");
        let ctrl = StoredKey {
            usage: 0xe0,
            side: StoredSide::Left,
        };
        let c = StoredKey {
            usage: 0x06,
            side: StoredSide::Unspecified,
        };
        let rule = StoredKeyRule {
            source: vec![ctrl.clone(), c.clone()],
            target: vec![
                StoredKey {
                    usage: 0xe3,
                    side: StoredSide::Left,
                },
                c.clone(),
            ],
            priority: 2,
            enabled: true,
        };
        guest.key_rules.push(rule.clone());
        let mut store = ProfileStore::load(directory.clone()).unwrap();
        store.save(guest.clone()).unwrap();
        assert_eq!(
            ProfileStore::load(directory.clone()).unwrap().profiles[0].key_rules,
            vec![rule.clone()]
        );
        let native = mapping_profile(&guest).unwrap();
        assert_eq!(native.rules[0].priority, 2);
        assert_eq!(native.rules[0].target[0], Destination::Modifier(0x08));
        let mut engine = esp32_kvm_input_core::MappingEngine::new(native).unwrap();
        engine.press(SourceKey {
            usage: 0xe0,
            side: Side::Left,
        });
        let report = engine.press(SourceKey {
            usage: 0x06,
            side: Side::Unspecified,
        });
        assert_eq!(report.modifiers, 0x08);
        assert_eq!(report.keys[0], 0x06);
        let report = engine.release(SourceKey {
            usage: 0x06,
            side: Side::Unspecified,
        });
        assert_eq!(report.modifiers, 0x01);
        guest.key_rules.push(rule.clone());
        assert!(!valid_profile(&guest));
        guest.key_rules = vec![StoredKeyRule {
            source: vec![
                ctrl,
                StoredKey {
                    usage: 0xe4,
                    side: StoredSide::Right,
                },
            ],
            ..rule
        }];
        assert!(!valid_profile(&guest));
        fs::remove_dir_all(directory).unwrap();
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

    #[test]
    fn v1_labels_migrate_to_v2_with_a_retained_backup() {
        let directory =
            std::env::temp_dir().join(format!("esp32-kvm-migrate-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&directory);
        fs::create_dir_all(&directory).unwrap();
        let original = serde_json::to_vec(&serde_json::json!({
            "schemaVersion": 1, "revision": 7,
            "profiles": [{"bondToken":"00112233445566778899aabbccddeeff", "name":"Work Mac", "os":"macos", "profile":"unchanged"}]
        })).unwrap();
        fs::write(directory.join("guest-profiles-0.json"), &original).unwrap();
        let store = ProfileStore::load(directory.clone()).unwrap();
        assert_eq!(store.revision, 8);
        assert_eq!(store.profiles[0].direct_shortcut, None);
        assert_eq!(
            fs::read(directory.join("guest-profiles-v1-backup.json")).unwrap(),
            original
        );
        let upgraded: ProfileFile =
            serde_json::from_slice(&fs::read(store.slot(store.active_slot)).unwrap()).unwrap();
        assert_eq!(upgraded.schema_version, 2);
        fs::remove_dir_all(directory).unwrap();
    }

    #[test]
    fn profile_links_survive_restart_without_storing_bond_secrets() {
        let directory =
            std::env::temp_dir().join(format!("esp32-kvm-links-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&directory);
        let mut store = ProfileStore::load(directory.clone()).unwrap();
        let mut linked = profile("00112233445566778899aabbccddeeff");
        linked.direct_shortcut = Some("Ctrl+Alt+1".into());
        linked.mapping_profile_id = Some("mac-default".into());
        linked.layout_link_id = Some("desk-right".into());
        store.save(linked.clone()).unwrap();
        let mut conflicting = profile("ffeeddccbbaa99887766554433221100");
        conflicting.direct_shortcut = linked.direct_shortcut.clone();
        assert!(store.save(conflicting).is_err());
        let loaded = ProfileStore::load(directory.clone()).unwrap();
        assert_eq!(loaded.profiles, vec![linked]);
        let bytes = fs::read(loaded.slot(loaded.active_slot)).unwrap();
        assert!(!String::from_utf8(bytes).unwrap().contains("bleKey"));
        fs::remove_dir_all(directory).unwrap();
    }

    struct OfflineBackend;
    impl SetupBackend for OfflineBackend {
        fn snapshot(&self) -> Result<BackendSnapshot, String> {
            Ok(BackendSnapshot {
                device: DeviceState::Missing,
                route: RouteState::Local,
                pairing: PairingState::Closed,
                bond_tokens: Vec::new(),
                retained_bond_tokens: None,
                connected_tokens: Vec::new(),
                ready_tokens: Vec::new(),
                pairing_available: false,
            })
        }
    }

    #[test]
    fn existing_host_label_can_be_edited_while_guest_is_offline() {
        let directory = std::env::temp_dir().join(format!(
            "esp32-kvm-offline-edit-test-{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&directory);
        let token = "00112233445566778899aabbccddeeff";
        SetupService::new(directory.clone(), Box::new(OneBondBackend))
            .save_profile(profile(token))
            .unwrap();
        let service = SetupService::new(directory.clone(), Box::new(OfflineBackend));
        let mut renamed = profile(token);
        renamed.name = "Travel Mac".into();
        service.save_profile(renamed).unwrap();
        assert_eq!(
            ProfileStore::load(directory.clone()).unwrap().profiles[0].name,
            "Travel Mac"
        );
        assert!(
            service
                .save_profile(profile("ffeeddccbbaa99887766554433221100"))
                .is_err()
        );
        fs::remove_dir_all(directory).unwrap();
    }

    #[test]
    fn forget_preserves_local_profile_until_firmware_confirms_removal() {
        let directory =
            std::env::temp_dir().join(format!("esp32-kvm-forget-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&directory);
        let token = "00112233445566778899aabbccddeeff";
        let service = SetupService::new(directory.clone(), Box::new(OneBondBackend));
        service.save_profile(profile(token)).unwrap();
        assert!(service.forget_profile(token).is_err());
        assert_eq!(service.snapshot().unwrap().profiles.len(), 1);
        let offline = SetupService::new(directory.clone(), Box::new(OfflineBackend));
        assert!(offline.forget_profile(token).is_err());
        assert_eq!(offline.snapshot().unwrap().profiles.len(), 1);
        fs::remove_dir_all(directory).unwrap();
    }

    struct ConfirmedForgetBackend {
        present: std::sync::atomic::AtomicBool,
        acknowledge: bool,
    }
    impl SetupBackend for ConfirmedForgetBackend {
        fn snapshot(&self) -> Result<BackendSnapshot, String> {
            let bonds = if self.present.load(std::sync::atomic::Ordering::SeqCst) {
                vec!["00112233445566778899aabbccddeeff".into()]
            } else {
                Vec::new()
            };
            Ok(BackendSnapshot {
                device: DeviceState::Verified {
                    board_id: "esp32-kvm-s3".into(),
                    firmware_version: None,
                    max_bonds: 8,
                    max_connections: 1,
                },
                route: RouteState::Local,
                pairing: PairingState::Closed,
                bond_tokens: bonds,
                retained_bond_tokens: Some(
                    if self.present.load(std::sync::atomic::Ordering::SeqCst) {
                        vec!["00112233445566778899aabbccddeeff".into()]
                    } else {
                        Vec::new()
                    },
                ),
                connected_tokens: Vec::new(),
                ready_tokens: Vec::new(),
                pairing_available: false,
            })
        }
        fn forget_bond(&self, _bond_token: &str) -> Result<(), String> {
            if self.acknowledge {
                self.present
                    .store(false, std::sync::atomic::Ordering::SeqCst);
            }
            Ok(())
        }
        fn refresh_inventory(&self) -> Result<BackendSnapshot, String> {
            self.snapshot()
        }
    }

    #[test]
    fn forget_is_retry_safe_after_firmware_status_removes_bond() {
        let directory =
            std::env::temp_dir().join(format!("esp32-kvm-forget-ack-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&directory);
        let token = "00112233445566778899aabbccddeeff";
        let service = SetupService::new(
            directory.clone(),
            Box::new(ConfirmedForgetBackend {
                present: std::sync::atomic::AtomicBool::new(true),
                acknowledge: false,
            }),
        );
        service.save_profile(profile(token)).unwrap();
        assert!(service.forget_profile(token).is_err());
        assert_eq!(service.snapshot().unwrap().profiles.len(), 1);
        let service = SetupService::new(
            directory.clone(),
            Box::new(ConfirmedForgetBackend {
                present: std::sync::atomic::AtomicBool::new(true),
                acknowledge: true,
            }),
        );
        assert_eq!(
            service.forget_profile(token).unwrap(),
            ForgetOutcome::Forgot
        );
        assert_eq!(
            service.forget_profile(token).unwrap(),
            ForgetOutcome::AlreadyAbsent
        );
        assert!(
            ProfileStore::load(directory.clone())
                .unwrap()
                .profiles
                .is_empty()
        );
        fs::remove_dir_all(directory).unwrap();
    }

    #[test]
    fn offline_bond_missing_from_live_slots_never_deletes_local_profile() {
        let directory = std::env::temp_dir().join(format!(
            "esp32-kvm-offline-bond-test-{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&directory);
        let token = "00112233445566778899aabbccddeeff";
        SetupService::new(directory.clone(), Box::new(OneBondBackend))
            .save_profile(profile(token))
            .unwrap();
        // Live STATUS contains no slot for this guest, but firmware may still retain its bond.
        let service = SetupService::new(
            directory.clone(),
            Box::new(ConfirmedForgetBackend {
                present: std::sync::atomic::AtomicBool::new(false),
                acknowledge: true,
            }),
        );
        assert!(service.forget_profile(token).is_err());
        assert_eq!(service.snapshot().unwrap().profiles.len(), 1);
        fs::remove_dir_all(directory).unwrap();
    }

    struct OfflineRetainedBackend {
        present: std::sync::atomic::AtomicBool,
    }
    impl SetupBackend for OfflineRetainedBackend {
        fn snapshot(&self) -> Result<BackendSnapshot, String> {
            Ok(BackendSnapshot {
                device: DeviceState::Verified {
                    board_id: "esp32-kvm-s3".into(),
                    firmware_version: None,
                    max_bonds: 8,
                    max_connections: 1,
                },
                route: RouteState::Local,
                pairing: PairingState::Closed,
                bond_tokens: Vec::new(),
                retained_bond_tokens: Some(
                    if self.present.load(std::sync::atomic::Ordering::SeqCst) {
                        vec!["00112233445566778899aabbccddeeff".into()]
                    } else {
                        Vec::new()
                    },
                ),
                connected_tokens: Vec::new(),
                ready_tokens: Vec::new(),
                pairing_available: false,
            })
        }
        fn refresh_inventory(&self) -> Result<BackendSnapshot, String> {
            self.snapshot()
        }
        fn forget_bond(&self, _token: &str) -> Result<(), String> {
            self.present
                .store(false, std::sync::atomic::Ordering::SeqCst);
            Ok(())
        }
    }

    #[test]
    fn setup_snapshot_refreshes_unread_inventory_for_offline_forget_control() {
        struct UnreadInventory;
        impl SetupBackend for UnreadInventory {
            fn snapshot(&self) -> Result<BackendSnapshot, String> {
                let mut value = OneBondBackend.snapshot()?;
                value.bond_tokens.clear();
                value.retained_bond_tokens = None;
                Ok(value)
            }
            fn refresh_inventory(&self) -> Result<BackendSnapshot, String> {
                let mut value = self.snapshot()?;
                value.retained_bond_tokens = Some(vec!["00112233445566778899aabbccddeeff".into()]);
                Ok(value)
            }
        }
        let directory =
            std::env::temp_dir().join(format!("esp32-kvm-unread-inventory-{}", std::process::id()));
        let service = SetupService::new(directory.clone(), Box::new(UnreadInventory));
        let snapshot = service.snapshot().unwrap();
        assert!(snapshot.bond_tokens.is_empty());
        assert_eq!(
            snapshot.retained_bond_tokens,
            Some(vec!["00112233445566778899aabbccddeeff".into()])
        );
        let _ = fs::remove_dir_all(directory);
    }

    #[test]
    fn offline_retained_bond_requires_inventory_proof_before_local_removal() {
        let directory =
            std::env::temp_dir().join(format!("esp32-kvm-retained-forget-{}", std::process::id()));
        let _ = fs::remove_dir_all(&directory);
        let token = "00112233445566778899aabbccddeeff";
        SetupService::new(directory.clone(), Box::new(OneBondBackend))
            .save_profile(profile(token))
            .unwrap();
        let service = SetupService::new(
            directory.clone(),
            Box::new(OfflineRetainedBackend {
                present: std::sync::atomic::AtomicBool::new(true),
            }),
        );
        assert_eq!(
            service.forget_profile(token).unwrap(),
            ForgetOutcome::Forgot
        );
        assert!(
            ProfileStore::load(directory.clone())
                .unwrap()
                .profiles
                .is_empty()
        );
        fs::remove_dir_all(directory).unwrap();
    }

    #[test]
    fn retained_bond_without_local_profile_is_still_removed_from_firmware() {
        let directory = std::env::temp_dir().join(format!(
            "esp32-kvm-firmware-only-forget-{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&directory);
        let token = "00112233445566778899aabbccddeeff";
        let service = SetupService::new(
            directory.clone(),
            Box::new(OfflineRetainedBackend {
                present: std::sync::atomic::AtomicBool::new(true),
            }),
        );
        assert_eq!(
            service.forget_profile(token).unwrap(),
            ForgetOutcome::Forgot
        );
        assert_eq!(
            service.forget_profile(token).unwrap(),
            ForgetOutcome::AlreadyAbsent
        );
        let _ = fs::remove_dir_all(directory);
    }

    #[test]
    fn custom_modifier_clone_round_trips_and_rejects_duplicate_sources() {
        let directory =
            std::env::temp_dir().join(format!("esp32-kvm-custom-map-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&directory);
        let mut custom = profile("00112233445566778899aabbccddeeff");
        custom.profile = "custom".into();
        custom.custom_base_preset = Some("windows-to-mac".into());
        custom.modifier_bindings = vec![ModifierBinding {
            source_usage: 0xe0,
            target_usage: 0xe3,
        }];
        let mut store = ProfileStore::load(directory.clone()).unwrap();
        store.save(custom.clone()).unwrap();
        assert_eq!(
            ProfileStore::load(directory.clone()).unwrap().profiles,
            vec![custom.clone()]
        );
        let native = mapping_profile(&custom).unwrap();
        assert_eq!(native.preset.len(), 1);
        custom
            .modifier_bindings
            .push(custom.modifier_bindings[0].clone());
        assert!(store.save(custom).is_err());
        fs::remove_dir_all(directory).unwrap();
    }

    #[test]
    fn preset_choice_is_explicit_and_does_not_reverse_cmd_to_ctrl() {
        let mut guest = profile("00112233445566778899aabbccddeeff");
        guest.profile = "cmd-to-ctrl".into();
        let profile = mapping_profile(&guest).unwrap();
        assert_eq!(profile.preset[0].source[0].usage, 0xe3);
        assert_eq!(
            profile.preset[0].target[0],
            esp32_kvm_input_core::Destination::Modifier(1)
        );
        guest.profile = "windows-to-mac".into();
        let profile = mapping_profile(&guest).unwrap();
        assert_eq!(profile.preset[0].source[0].usage, 0xe0);
        assert_eq!(
            profile.preset[0].target[0],
            esp32_kvm_input_core::Destination::Modifier(8)
        );
    }

    type RecordedMappings = std::sync::Arc<Mutex<Vec<([u8; 16], MappingProfile)>>>;
    struct MappingRecorder(RecordedMappings);

    impl SetupBackend for MappingRecorder {
        fn snapshot(&self) -> Result<BackendSnapshot, String> {
            OneBondBackend.snapshot()
        }

        fn install_mapping(&self, token: [u8; 16], profile: MappingProfile) -> Result<(), String> {
            self.0.lock().unwrap().push((token, profile));
            Ok(())
        }
    }

    #[test]
    fn saved_guest_mapping_is_reinstalled_by_token_after_restart() {
        let directory =
            std::env::temp_dir().join(format!("esp32-kvm-map-restore-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&directory);
        let installed = std::sync::Arc::new(Mutex::new(Vec::new()));
        let mut guest = profile("00112233445566778899aabbccddeeff");
        guest.profile = "cmd-to-ctrl".into();
        SetupService::new(
            directory.clone(),
            Box::new(MappingRecorder(installed.clone())),
        )
        .save_profile(guest)
        .unwrap();
        assert_eq!(installed.lock().unwrap().len(), 1);
        installed.lock().unwrap().clear();
        let restored = SetupService::new(
            directory.clone(),
            Box::new(MappingRecorder(installed.clone())),
        );
        restored.snapshot().unwrap();
        let records = installed.lock().unwrap();
        assert_eq!(records.len(), 1);
        assert_eq!(
            records[0].0,
            token_bytes("00112233445566778899aabbccddeeff").unwrap()
        );
        assert_eq!(records[0].1.preset[0].source[0].usage, 0xe3);
        drop(records);
        fs::remove_dir_all(directory).unwrap();
    }

    struct FlakyMappingBackend(std::sync::Arc<std::sync::atomic::AtomicBool>);

    impl SetupBackend for FlakyMappingBackend {
        fn snapshot(&self) -> Result<BackendSnapshot, String> {
            OneBondBackend.snapshot()
        }

        fn install_mapping(
            &self,
            _token: [u8; 16],
            _profile: MappingProfile,
        ) -> Result<(), String> {
            if self.0.load(std::sync::atomic::Ordering::SeqCst) {
                Ok(())
            } else {
                Err("worker temporarily unavailable".into())
            }
        }
    }

    #[test]
    fn transient_mapping_failure_keeps_saved_profiles_visible_and_retries() {
        let directory =
            std::env::temp_dir().join(format!("esp32-kvm-map-pending-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&directory);
        let available = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
        let service = SetupService::new(
            directory.clone(),
            Box::new(FlakyMappingBackend(available.clone())),
        );
        let token = "00112233445566778899aabbccddeeff";
        service.save_profile(profile(token)).unwrap();
        let pending = service.snapshot().unwrap();
        assert_eq!(pending.profiles.len(), 1);
        assert_eq!(pending.mapping_pending_tokens, vec![token]);
        drop(service);
        let restored = SetupService::new(
            directory.clone(),
            Box::new(FlakyMappingBackend(available.clone())),
        );
        assert_eq!(restored.snapshot().unwrap().profiles.len(), 1);
        available.store(true, std::sync::atomic::Ordering::SeqCst);
        assert!(
            restored
                .snapshot()
                .unwrap()
                .mapping_pending_tokens
                .is_empty()
        );
        fs::remove_dir_all(directory).unwrap();
    }
}
