// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Verifies that physical input remains tracked across disarmed and hotkey-consumed capture.

use esp32_kvm_platform_windows::{CaptureGate, MouseButton};

#[test]
fn held_hotkey_and_mouse_button_block_guest_arm_until_physical_release() {
    let (gate, _) = CaptureGate::new(8);
    gate.record_physical_key(0x1d, false, true);
    gate.record_physical_key(0x58, false, true); // consumed F12 trigger
    gate.record_physical_button(MouseButton::Left, true);
    assert!(!gate.physical_all_up());
    assert!(!gate.arm(1));
    gate.record_physical_key(0x1d, false, false);
    gate.record_physical_key(0x58, false, false);
    assert!(!gate.arm(1));
    gate.record_physical_button(MouseButton::Left, false);
    assert!(gate.physical_all_up());
    assert!(gate.arm(1));
}

#[test]
fn repeated_down_and_generation_disarm_do_not_lose_physical_state() {
    let (gate, _) = CaptureGate::new(8);
    gate.record_physical_key(0x1d, true, true);
    gate.record_physical_key(0x1d, true, true);
    gate.disarm();
    assert!(!gate.physical_all_up());
    gate.record_physical_key(0x1d, true, false);
    assert!(gate.physical_all_up());
}
