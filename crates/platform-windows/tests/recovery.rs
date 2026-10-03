// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Verifies independent fake-clock worker leases, system-transition disarming, and physical escape.

use esp32_kvm_input_core::{Action, HotkeyConfig, HotkeyMatcher, Key};
use esp32_kvm_platform_windows::{CaptureFault, CaptureGate};

#[test]
fn stalled_routing_actor_disarms_without_waiting_for_actor_poll() {
    let (gate, _) = CaptureGate::new(8);
    gate.actor_heartbeat_at(100);
    gate.capture_heartbeat_at(100);
    assert!(gate.arm(7));
    gate.capture_heartbeat_at(500);
    gate.check_watchdog_at(599);
    assert_eq!(gate.generation(), 7);
    gate.check_watchdog_at(600);
    assert_eq!(gate.generation(), 0);
    assert_eq!(gate.fault(), Some(CaptureFault::RoutingWorkerStalled));
    assert!(!gate.arm(8));
}

#[test]
fn stalled_capture_pump_and_system_transition_are_terminal_until_reconciliation() {
    let (gate, _) = CaptureGate::new(8);
    gate.actor_heartbeat_at(100);
    gate.capture_heartbeat_at(100);
    assert!(gate.arm(3));
    gate.actor_heartbeat_at(350);
    gate.check_watchdog_at(349);
    assert_eq!(gate.generation(), 3);
    gate.check_watchdog_at(350);
    assert_eq!(gate.fault(), Some(CaptureFault::CaptureWorkerStalled));
    assert_eq!(gate.generation(), 0);
    assert!(gate.clear_fault());
    gate.actor_heartbeat_at(400);
    gate.capture_heartbeat_at(400);
    assert!(gate.arm(4));
    gate.system_transition();
    assert_eq!(gate.generation(), 0);
    assert_eq!(gate.fault(), Some(CaptureFault::SystemTransition));
}

#[test]
fn stale_capture_pump_cannot_be_armed_before_watchdog_wakes() {
    let (gate, _) = CaptureGate::new(8);
    gate.capture_heartbeat_at(10);
    assert!(gate.capture_pump_recent_at(259));
    assert!(!gate.capture_pump_recent_at(260));
    gate.capture_heartbeat_at(260);
    assert!(gate.capture_pump_recent_at(260));
}

#[test]
fn both_control_hold_disarms_even_when_no_actor_consumes_the_hotkey() {
    let (gate, _) = CaptureGate::new(8);
    assert!(gate.arm(1));
    let mut hotkeys = HotkeyMatcher::new(HotkeyConfig::defaults());
    let left = Key {
        scan: 0x1d,
        extended: false,
    };
    let right = Key {
        scan: 0x1d,
        extended: true,
    };
    hotkeys.on_key(left, true, false, 0);
    hotkeys.on_key(right, true, false, 1);
    assert_eq!(hotkeys.tick(1000), None);
    assert_eq!(hotkeys.tick(1001), Some(Action::Local));
    gate.disarm();
    assert_eq!(gate.generation(), 0);
}
