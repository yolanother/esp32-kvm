// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Validates and persists the Windows host's physical cycle-to-next shortcut.
// It maps chosen keys to set-one scan codes, preserves reserved routing chords,
// and keeps a last-good setting across interrupted writes and app restarts.

use esp32_kvm_input_core::{HotkeyConfig, Key, Modifiers};
use serde::{Deserialize, Serialize};
use std::fs::{self, OpenOptions};
use std::io::Write;
use std::path::PathBuf;

const SCHEMA_VERSION: u8 = 1;

/// The saved physical trigger and exact modifier classes for Action::Next.
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SwitchBinding {
    /// KeyboardEvent.code-style name of a supported physical key.
    pub trigger: String,
    /// Ctrl=1, Alt=2, Shift=4, Win=8; at least one modifier is required.
    pub modifiers: u8,
}

impl Default for SwitchBinding {
    fn default() -> Self {
        Self {
            trigger: "F12".into(),
            modifiers: 3,
        }
    }
}

impl SwitchBinding {
    /// Builds the full native shortcut set or explains why this choice is invalid.
    pub fn validate(&self) -> Result<HotkeyConfig, String> {
        if self.modifiers == 0 || self.modifiers & !15 != 0 {
            return Err("Choose at least one supported modifier.".into());
        }
        let Some(scan) = physical_scan(&self.trigger) else {
            return Err("Choose a supported physical key.".into());
        };
        let mut modifiers = Modifiers::NONE;
        for (bit, class) in [
            (1, Modifiers::CTRL),
            (2, Modifiers::ALT),
            (4, Modifiers::SHIFT),
            (8, Modifiers::GUI),
        ] {
            if self.modifiers & bit != 0 {
                modifiers = modifiers.union(class);
            }
        }
        HotkeyConfig::defaults()
            .with_next(
                Key {
                    scan,
                    extended: false,
                },
                modifiers,
            )
            .map_err(|_| "That combination is reserved for another routing action.".into())
    }
}

fn physical_scan(code: &str) -> Option<u16> {
    match code {
        "F1" => Some(0x3b),
        "F2" => Some(0x3c),
        "F3" => Some(0x3d),
        "F4" => Some(0x3e),
        "F5" => Some(0x3f),
        "F6" => Some(0x40),
        "F7" => Some(0x41),
        "F8" => Some(0x42),
        "F9" => Some(0x43),
        "F10" => Some(0x44),
        "F11" => Some(0x57),
        "F12" => Some(0x58),
        "KeyA" => Some(0x1e),
        "KeyB" => Some(0x30),
        "KeyC" => Some(0x2e),
        "KeyD" => Some(0x20),
        "KeyE" => Some(0x12),
        "KeyF" => Some(0x21),
        "KeyG" => Some(0x22),
        "KeyH" => Some(0x23),
        "KeyI" => Some(0x17),
        "KeyJ" => Some(0x24),
        "KeyK" => Some(0x25),
        "KeyL" => Some(0x26),
        "KeyM" => Some(0x32),
        "KeyN" => Some(0x31),
        "KeyO" => Some(0x18),
        "KeyP" => Some(0x19),
        "KeyQ" => Some(0x10),
        "KeyR" => Some(0x13),
        "KeyS" => Some(0x1f),
        "KeyT" => Some(0x14),
        "KeyU" => Some(0x16),
        "KeyV" => Some(0x2f),
        "KeyW" => Some(0x11),
        "KeyX" => Some(0x2d),
        "KeyY" => Some(0x15),
        "KeyZ" => Some(0x2c),
        "Digit1" => Some(0x02),
        "Digit2" => Some(0x03),
        "Digit3" => Some(0x04),
        "Digit4" => Some(0x05),
        "Digit5" => Some(0x06),
        "Digit6" => Some(0x07),
        "Digit7" => Some(0x08),
        "Digit8" => Some(0x09),
        "Digit9" => Some(0x0a),
        "Digit0" => Some(0x0b),
        _ => None,
    }
}

#[derive(Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct SavedBinding {
    schema_version: u8,
    revision: u64,
    binding: SwitchBinding,
}

/// Two-slot host-local setting; a damaged newer write never destroys the last good one.
pub struct SwitchShortcutStore {
    directory: PathBuf,
    revision: u64,
    active_slot: usize,
    current: SwitchBinding,
}

impl SwitchShortcutStore {
    fn slot(&self, index: usize) -> PathBuf {
        self.directory.join(format!("cycle-shortcut-{index}.json"))
    }

    /// Loads the newest valid setting or the default if none has been saved.
    pub fn load(directory: PathBuf) -> Result<Self, String> {
        let mut store = Self {
            directory,
            revision: 0,
            active_slot: 1,
            current: SwitchBinding::default(),
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
            let Ok(saved) = serde_json::from_slice::<SavedBinding>(&bytes) else {
                continue;
            };
            if saved.schema_version != SCHEMA_VERSION || saved.binding.validate().is_err() {
                continue;
            }
            if !saw_valid || saved.revision > store.revision {
                store.revision = saved.revision;
                store.active_slot = slot;
                store.current = saved.binding;
            }
            saw_valid = true;
        }
        if saw_file && !saw_valid {
            return Err(
                "Saved cycle shortcut settings are damaged; capture is using defaults.".into(),
            );
        }
        Ok(store)
    }

    /// Returns the binding currently backed by the last good slot.
    pub fn current(&self) -> &SwitchBinding {
        &self.current
    }

    /// Writes a validated choice to the other slot before making it current.
    pub fn save(&mut self, binding: SwitchBinding) -> Result<(), String> {
        binding.validate()?;
        let next = SavedBinding {
            schema_version: SCHEMA_VERSION,
            revision: self.revision + 1,
            binding: binding.clone(),
        };
        fs::create_dir_all(&self.directory).map_err(|error| error.to_string())?;
        let slot = 1 - self.active_slot;
        let mut file = OpenOptions::new()
            .create(true)
            .write(true)
            .truncate(true)
            .open(self.slot(slot))
            .map_err(|error| error.to_string())?;
        file.write_all(&serde_json::to_vec(&next).map_err(|error| error.to_string())?)
            .map_err(|error| error.to_string())?;
        file.sync_all().map_err(|error| error.to_string())?;
        self.current = binding;
        self.revision = next.revision;
        self.active_slot = slot;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::{SwitchBinding, SwitchShortcutStore};

    #[test]
    fn cycle_binding_rejects_unsafe_and_reserved_chords() {
        assert!(
            SwitchBinding {
                trigger: "F8".into(),
                modifiers: 3
            }
            .validate()
            .is_ok()
        );
        assert!(
            SwitchBinding {
                trigger: "F8".into(),
                modifiers: 0
            }
            .validate()
            .is_err()
        );
        assert!(
            SwitchBinding {
                trigger: "F10".into(),
                modifiers: 3
            }
            .validate()
            .is_err()
        );
        assert!(
            SwitchBinding {
                trigger: "Digit1".into(),
                modifiers: 3
            }
            .validate()
            .is_err()
        );
        assert!(
            SwitchBinding {
                trigger: "ControlLeft".into(),
                modifiers: 3
            }
            .validate()
            .is_err()
        );
    }

    #[test]
    fn cycle_binding_survives_a_host_restart() {
        let directory = std::env::temp_dir().join(format!(
            "esp32-kvm-switch-shortcut-test-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let mut store = SwitchShortcutStore::load(directory.clone()).unwrap();
        assert_eq!(store.current(), &SwitchBinding::default());
        let chosen = SwitchBinding {
            trigger: "F8".into(),
            modifiers: 3,
        };
        store.save(chosen.clone()).unwrap();
        assert_eq!(
            SwitchShortcutStore::load(directory.clone())
                .unwrap()
                .current(),
            &chosen
        );
        std::fs::remove_dir_all(directory).unwrap();
    }
}
