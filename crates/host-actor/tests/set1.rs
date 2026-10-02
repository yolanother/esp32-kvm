// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Checks conservative Windows set-one scan-code translation before guest rules.

use esp32_kvm_host_actor::{KeyMapper, MappedKey, SetOneKeyMapper};

#[test]
fn common_physical_keys_translate_to_hid_usages() {
    let mapper = SetOneKeyMapper;
    assert_eq!(mapper.map_key(0x41, 0x1e, false), Some(MappedKey::Usage(4)));
    assert_eq!(mapper.map_key(0x41, 0x30, false), Some(MappedKey::Usage(5)));
    assert_eq!(
        mapper.map_key(0x70, 0x3b, false),
        Some(MappedKey::Usage(0x3a))
    );
    assert_eq!(
        mapper.map_key(0x26, 0x4b, true),
        Some(MappedKey::Usage(0x50))
    );
    assert_eq!(
        mapper.map_key(0x25, 0x4b, false),
        Some(MappedKey::Usage(0x5c))
    );
}

#[test]
fn left_and_right_modifiers_and_extended_keypad_are_distinct() {
    let mapper = SetOneKeyMapper;
    assert_eq!(
        mapper.map_key(0x11, 0x1d, false),
        Some(MappedKey::Modifier(1))
    );
    assert_eq!(
        mapper.map_key(0x11, 0x1d, true),
        Some(MappedKey::Modifier(0x10))
    );
    assert_eq!(
        mapper.map_key(0x12, 0x38, true),
        Some(MappedKey::Modifier(0x40))
    );
    assert_eq!(
        mapper.map_key(0x5b, 0x5b, true),
        Some(MappedKey::Modifier(8))
    );
    assert_eq!(
        mapper.map_key(0x6f, 0x35, true),
        Some(MappedKey::Usage(0x54))
    );
    assert_eq!(
        mapper.map_key(0x0d, 0x1c, true),
        Some(MappedKey::Usage(0x58))
    );
}

#[test]
fn unknown_or_synthetic_scan_codes_are_never_guessed_from_virtual_key() {
    let mapper = SetOneKeyMapper;
    assert_eq!(mapper.map_key(0x41, 0, false), None);
    assert_eq!(mapper.map_key(0x41, 0x90, false), None);
    assert_eq!(mapper.map_key(0x41, 0x021d, false), None);
    assert_eq!(mapper.map_key(0x13, 0x45, false), None);
}
