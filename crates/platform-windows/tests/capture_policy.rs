// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Specifies the Windows capture adapter's local pass-through, injected-input isolation,
// one-path mouse delivery, routing-generation tagging, and bounded-queue fault behavior.
use esp32_kvm_platform_windows::{
    CaptureFault, CaptureGate, MouseAxis, MouseButton, MouseInput, PhysicalEvent, RawMotionOutcome,
    classify_keyboard, classify_mouse, classify_raw_motion,
};

#[test]
fn local_mode_passes_physical_keys_and_buttons_without_forwarding() {
    let key = classify_keyboard(false, false, 0x41, 0x1e, false, true, false);
    let button = classify_mouse(false, false, MouseInput::Button(MouseButton::Left, true));
    assert!(key.pass_to_os && key.event.is_none());
    assert!(button.pass_to_os && button.event.is_none());
}

#[test]
fn armed_mode_forwards_key_repeat_metadata_and_suppresses_locally() {
    let key = classify_keyboard(true, false, 0x41, 0x1e, false, true, true);
    assert!(!key.pass_to_os);
    assert_eq!(
        key.event,
        Some(PhysicalEvent::Key {
            virtual_key: 0x41,
            scan_code: 0x1e,
            extended: false,
            down: true,
            repeat: true,
        })
    );
}

#[test]
fn injected_events_pass_through_and_are_not_forwarded() {
    let key = classify_keyboard(true, true, 0x41, 0x1e, false, true, false);
    let mouse = classify_mouse(true, true, MouseInput::Wheel(MouseAxis::Vertical, 120));
    assert!(key.pass_to_os && key.event.is_none());
    assert!(mouse.pass_to_os && mouse.event.is_none());
}

#[test]
fn altgr_side_is_kept_distinct_from_an_injected_left_control() {
    let synthetic_control = classify_keyboard(true, true, 0xa2, 0x21d, false, true, false);
    let right_alt = classify_keyboard(true, false, 0xa5, 0x38, true, true, false);
    assert!(!synthetic_control.pass_to_os && synthetic_control.event.is_none());
    assert_eq!(
        right_alt.event,
        Some(PhysicalEvent::Key {
            virtual_key: 0xa5,
            scan_code: 0x38,
            extended: true,
            down: true,
            repeat: false,
        })
    );
}

#[test]
fn mouse_motion_uses_raw_path_and_hook_buttons_use_hook_path() {
    let hook_move = classify_mouse(true, false, MouseInput::Move);
    let hook_button = classify_mouse(true, false, MouseInput::Button(MouseButton::X2, true));
    let raw = classify_raw_motion(true, false, 9, -4);
    assert!(!hook_move.pass_to_os && hook_move.event.is_none());
    assert_eq!(
        hook_button.event,
        Some(PhysicalEvent::Button(MouseButton::X2, true))
    );
    assert_eq!(raw, RawMotionOutcome::Motion(9, -4));
    assert_eq!(
        classify_raw_motion(false, false, 9, -4),
        RawMotionOutcome::Ignore
    );
    assert_eq!(
        classify_raw_motion(true, true, 9, -4),
        RawMotionOutcome::UnsupportedAbsolute
    );
}

#[test]
fn bounded_queue_disarms_on_overflow_and_tags_prior_events() {
    let (gate, receiver) = CaptureGate::new(1);
    assert!(gate.arm(7));
    assert!(gate.offer(PhysicalEvent::Motion(1, 2)));
    assert!(!gate.offer(PhysicalEvent::Motion(3, 4)));
    assert_eq!(gate.generation(), 0);
    assert_eq!(gate.fault(), Some(CaptureFault::QueueOverflow));
    let queued = receiver.try_recv().unwrap();
    assert_eq!(queued.generation, 7);
    assert_eq!(queued.event, PhysicalEvent::Motion(1, 2));
    assert!(!gate.arm(8));
    gate.clear_fault();
    assert!(gate.arm(8));
}
