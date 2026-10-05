// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Tests the persisted, validated Windows host cycle shortcut before its native
// setting and capture-worker installation are implemented.

#[cfg(test)]
mod tests {
    use super::{SwitchBinding, SwitchShortcutStore};

    #[test]
    fn cycle_binding_rejects_unsafe_and_reserved_chords() {
        assert!(SwitchBinding { trigger: "F8".into(), modifiers: 3 }.validate().is_ok());
        assert!(SwitchBinding { trigger: "F8".into(), modifiers: 0 }.validate().is_err());
        assert!(SwitchBinding { trigger: "F10".into(), modifiers: 3 }.validate().is_err());
        assert!(SwitchBinding { trigger: "Digit1".into(), modifiers: 3 }.validate().is_err());
        assert!(SwitchBinding { trigger: "ControlLeft".into(), modifiers: 3 }.validate().is_err());
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
        let chosen = SwitchBinding { trigger: "F8".into(), modifiers: 3 };
        store.save(chosen.clone()).unwrap();
        assert_eq!(SwitchShortcutStore::load(directory.clone()).unwrap().current(), &chosen);
        std::fs::remove_dir_all(directory).unwrap();
    }
}
