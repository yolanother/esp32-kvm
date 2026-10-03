// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Exercises physical guest mapping precedence, held-key release, and HID report safety.

use esp32_kvm_input_core::{
    Destination, MappingEngine, MappingPreset, MappingProfile, MappingRule, Side, SourceKey,
    preset_profile,
};

#[test]
fn explicit_modifier_presets_never_reverse_the_requested_direction() {
    let gui = key(0xe3, Side::Left);
    let ctrl = key(0xe0, Side::Left);
    let c = key(6, Side::Unspecified);
    let mut cmd_to_ctrl = MappingEngine::new(preset_profile(MappingPreset::CmdToCtrl)).unwrap();
    assert_eq!(cmd_to_ctrl.press(gui).modifiers, 0x01);
    assert_eq!(cmd_to_ctrl.press(c).keys[0], 6);
    cmd_to_ctrl.release(c);
    cmd_to_ctrl.release(gui);
    assert_eq!(cmd_to_ctrl.press(ctrl).modifiers, 0x01);

    let mut windows_to_mac =
        MappingEngine::new(preset_profile(MappingPreset::WindowsToMac)).unwrap();
    assert_eq!(windows_to_mac.press(ctrl).modifiers, 0x08);
    windows_to_mac.release(ctrl);
    assert_eq!(windows_to_mac.press(gui).modifiers, 0x08);
}

#[test]
fn windows_to_mac_preserves_altgr_chord_on_non_us_layouts() {
    let ctrl = key(0xe0, Side::Left);
    let altgr = key(0xe6, Side::Right);
    let q = key(20, Side::Unspecified);
    let mut engine = MappingEngine::new(preset_profile(MappingPreset::WindowsToMac)).unwrap();
    engine.press(ctrl);
    let report = engine.press(altgr);
    assert_eq!(report.modifiers, 0x41);
    assert_eq!(engine.press(q).modifiers, 0x41);
    engine.release(q);
    assert_eq!(engine.release(altgr).modifiers, 0x08);
    assert_eq!(engine.release(ctrl).modifiers, 0);
}

fn key(usage: u8, side: Side) -> SourceKey {
    SourceKey { usage, side }
}
fn rule(source: Vec<SourceKey>, target: Vec<Destination>, priority: i16) -> MappingRule {
    MappingRule {
        source,
        target,
        priority,
        enabled: true,
    }
}

#[test]
fn two_sources_to_one_modifier_keep_it_down_until_both_release() {
    let a = key(4, Side::Unspecified);
    let b = key(5, Side::Unspecified);
    let mut engine = MappingEngine::new(MappingProfile {
        preset: vec![],
        rules: vec![
            rule(vec![a], vec![Destination::Modifier(0x01)], 0),
            rule(vec![b], vec![Destination::Modifier(0x01)], 0),
        ],
    })
    .unwrap();
    assert_eq!(engine.press(a).modifiers, 1);
    assert_eq!(engine.press(b).modifiers, 1);
    assert_eq!(engine.release(a).modifiers, 1);
    assert_eq!(engine.release(b).modifiers, 0);
}

#[test]
fn exact_chord_overrides_singles_and_restores_survivor_on_release() {
    let ctrl = key(0xe0, Side::Left);
    let a = key(4, Side::Unspecified);
    let mut engine = MappingEngine::new(MappingProfile {
        preset: vec![],
        rules: vec![
            rule(vec![a], vec![Destination::Usage(5)], 1),
            rule(
                vec![ctrl, a],
                vec![Destination::Modifier(0x08), Destination::Usage(6)],
                0,
            ),
        ],
    })
    .unwrap();
    assert_eq!(engine.press(ctrl).modifiers, 1);
    let chord = engine.press(a);
    assert_eq!(chord.modifiers, 8);
    assert_eq!(chord.keys[0], 6);
    let survivor = engine.release(a);
    assert_eq!(survivor.modifiers, 1);
    assert_eq!(survivor.keys, [0; 6]);
}

#[test]
fn profile_edit_waits_for_all_held_keys_and_key_up_uses_old_output() {
    let a = key(4, Side::Unspecified);
    let old = MappingProfile {
        preset: vec![],
        rules: vec![rule(vec![a], vec![Destination::Usage(5)], 0)],
    };
    let new = MappingProfile {
        preset: vec![],
        rules: vec![rule(vec![a], vec![Destination::Usage(6)], 0)],
    };
    let mut engine = MappingEngine::new(old).unwrap();
    assert_eq!(engine.press(a).keys[0], 5);
    engine.set_profile(new).unwrap();
    assert_eq!(engine.report().keys[0], 5);
    assert_eq!(engine.release(a).keys, [0; 6]);
    assert_eq!(engine.press(a).keys[0], 6);
}

#[test]
fn rollover_and_repeat_follow_distinct_physical_presses() {
    let mut engine = MappingEngine::new(MappingProfile::default()).unwrap();
    for usage in 4..=10 {
        engine.press(key(usage, Side::Unspecified));
    }
    assert_eq!(engine.report().keys, [1; 6]);
    engine.press(key(4, Side::Unspecified));
    assert_eq!(engine.report().keys, [1; 6]);
    let report = engine.release(key(10, Side::Unspecified));
    assert_eq!(report.keys, [4, 5, 6, 7, 8, 9]);
}

#[test]
fn same_priority_duplicate_trigger_is_rejected_and_profile_wins_preset() {
    let a = key(4, Side::Unspecified);
    let first = rule(vec![a], vec![Destination::Usage(5)], 0);
    assert!(
        MappingEngine::new(MappingProfile {
            preset: vec![],
            rules: vec![first.clone(), first]
        })
        .is_err()
    );
    let mut engine = MappingEngine::new(MappingProfile {
        preset: vec![rule(vec![a], vec![Destination::Usage(5)], 100)],
        rules: vec![rule(vec![a], vec![Destination::Usage(6)], -100)],
    })
    .unwrap();
    assert_eq!(engine.press(a).keys[0], 6);
}

#[test]
fn releasing_modifier_first_restores_single_key_and_sides_are_distinct() {
    let left = key(0xe0, Side::Left);
    let right = key(0xe4, Side::Right);
    let a = key(4, Side::Unspecified);
    let mut engine = MappingEngine::new(MappingProfile {
        preset: vec![],
        rules: vec![
            rule(vec![left], vec![Destination::Modifier(0x08)], 0),
            rule(vec![left, a], vec![Destination::Usage(6)], 0),
        ],
    })
    .unwrap();
    assert_eq!(engine.press(left).modifiers, 8);
    assert_eq!(engine.press(a).keys[0], 6);
    let survivor = engine.release(left);
    assert_eq!(survivor.modifiers, 0);
    assert_eq!(survivor.keys[0], 4);
    engine.release(a);
    assert_eq!(engine.press(right).modifiers, 0x10);
}

#[test]
fn a_mapping_target_is_not_recursively_mapped() {
    let a = key(4, Side::Unspecified);
    let b = key(5, Side::Unspecified);
    let mut engine = MappingEngine::new(MappingProfile {
        preset: vec![],
        rules: vec![
            rule(vec![a], vec![Destination::Usage(5)], 0),
            rule(vec![b], vec![Destination::Usage(6)], 0),
        ],
    })
    .unwrap();
    assert_eq!(engine.press(a).keys[0], 5);
}
