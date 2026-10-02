// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Specifies physical shortcut priority, offline cycling, and serialized
// fail-local routing transactions without a live USB device.

use esp32_kvm_input_core::{
    Action, Command, HotkeyConfig, HotkeyMatcher, Key, Modifiers, RequestActor, Shortcut,
    ShortcutError, State,
};

fn key(scan: u16, extended: bool) -> Key {
    Key { scan, extended }
}

#[test]
fn default_shortcuts_consume_trigger_down_and_up_before_mapping() {
    let mut matcher = HotkeyMatcher::new(HotkeyConfig::defaults());
    assert_eq!(
        matcher.on_key(key(0x1d, false), true, false, 0).action,
        None
    );
    assert_eq!(
        matcher.on_key(key(0x38, false), true, false, 1).action,
        None
    );
    let trigger = key(0x58, false); // physical F12
    let down = matcher.on_key(trigger, true, false, 2);
    assert_eq!(down.action, Some(Action::Next));
    assert!(down.consume);
    assert_eq!(matcher.on_key(trigger, true, true, 3).action, None);
    assert!(matcher.on_key(trigger, false, false, 4).consume);
    assert_eq!(
        matcher.on_key(key(0x57, false), true, false, 5).action,
        Some(Action::Previous)
    );
    assert_eq!(
        matcher.on_key(key(0x44, false), true, false, 6).action,
        Some(Action::Local)
    );
}

#[test]
fn direct_shortcuts_are_physical_and_editable_with_conflicts_rejected() {
    let mut matcher = HotkeyMatcher::new(HotkeyConfig::defaults());
    matcher.on_key(key(0x1d, true), true, false, 0);
    matcher.on_key(key(0x38, true), true, false, 0);
    for (scan, slot) in [(0x02, 1), (0x03, 2), (0x04, 3)] {
        assert_eq!(
            matcher.on_key(key(scan, false), true, false, 0).action,
            Some(Action::Direct(slot))
        );
        matcher.on_key(key(scan, false), false, false, 0);
    }
    let custom = Shortcut {
        trigger: key(0x21, false),
        modifiers: Modifiers::CTRL,
        action: Action::Next,
    };
    let config = HotkeyConfig::try_new(vec![custom]).unwrap();
    matcher.set_config(config);
    matcher.on_key(key(0x38, true), false, false, 0);
    assert_eq!(
        matcher.on_key(key(0x21, false), true, false, 0).action,
        Some(Action::Next)
    );
    assert_eq!(
        HotkeyConfig::try_new(vec![custom, custom]),
        Err(ShortcutError::Conflict)
    );
    let ctrl_trigger = Shortcut {
        trigger: key(0x1d, false),
        ..custom
    };
    assert_eq!(
        HotkeyConfig::try_new(vec![ctrl_trigger]),
        Err(ShortcutError::ModifierTrigger)
    );
}

#[test]
fn both_control_keys_hold_for_one_second_to_preempt() {
    let mut matcher = HotkeyMatcher::new(HotkeyConfig::defaults());
    matcher.on_key(key(0x1d, false), true, false, 20);
    matcher.on_key(key(0x1d, true), true, false, 30);
    assert_eq!(matcher.tick(1029), None);
    assert_eq!(matcher.tick(1030), Some(Action::Local));
    assert_eq!(matcher.tick(2030), None);
    matcher.on_key(key(0x1d, true), false, false, 2040);
    matcher.on_key(key(0x1d, true), true, false, 2050);
    assert_eq!(matcher.tick(3050), Some(Action::Local));
}

#[test]
fn cycle_skips_offline_and_includes_local() {
    let mut actor = RequestActor::new(vec![1, 2, 3]);
    assert_eq!(actor.set_ready(1, true), None);
    assert_eq!(actor.set_ready(3, true), None);
    assert_eq!(
        actor.request(Action::Next, 0),
        Some(Command::ReleaseAll { generation: 1 })
    );
    assert_eq!(
        actor.release_ack(1),
        Some(Command::Select {
            slot: 1,
            generation: 1
        })
    );
    assert_eq!(
        actor.switch_ack(1),
        Some(Command::Arm {
            slot: 1,
            generation: 1
        })
    );
    assert_eq!(actor.arm_ack(1, 1), None);
    assert_eq!(actor.state(), State::Guest(1));
    assert_eq!(
        actor.request(Action::Next, 2),
        Some(Command::ReleaseAll { generation: 2 })
    );
    actor.release_ack(2);
    assert_eq!(
        actor.switch_ack(2),
        Some(Command::Arm {
            slot: 3,
            generation: 2
        })
    );
    assert_eq!(actor.arm_ack(2, 3), None);
    assert_eq!(
        actor.request(Action::Next, 4),
        Some(Command::ReleaseAll { generation: 3 })
    );
    assert_eq!(actor.state(), State::Local);
    assert_eq!(
        actor.request(Action::Previous, 5),
        Some(Command::ReleaseAll { generation: 4 })
    );
    assert_eq!(
        actor.release_ack(4),
        Some(Command::Select {
            slot: 3,
            generation: 4
        })
    );
}

#[test]
fn local_preempts_pending_switch_and_stale_ack_cannot_rearm() {
    let mut actor = RequestActor::new(vec![1, 2]);
    assert_eq!(actor.set_ready(1, true), None);
    assert_eq!(actor.set_ready(2, true), None);
    assert_eq!(
        actor.request(Action::Direct(1), 0),
        Some(Command::ReleaseAll { generation: 1 })
    );
    assert_eq!(actor.request(Action::Direct(2), 1), None);
    assert_eq!(
        actor.request(Action::Local, 2),
        Some(Command::ReleaseAll { generation: 2 })
    );
    assert_eq!(actor.state(), State::Local);
    assert_eq!(actor.switch_ack(1), None);
    assert_eq!(actor.arm_ack(1, 3), None);
    assert_eq!(
        actor.request(Action::Direct(2), 4),
        Some(Command::ReleaseAll { generation: 3 })
    );
    actor.release_ack(3);
    actor.switch_ack(3);
    assert_eq!(actor.arm_ack(3, 5), None);
    assert_eq!(actor.state(), State::Guest(2));
}

#[test]
fn queued_selection_starts_after_ack_and_offline_direct_fails_local() {
    let mut actor = RequestActor::new(vec![1, 2]);
    assert_eq!(actor.set_ready(1, true), None);
    assert_eq!(
        actor.request(Action::Direct(1), 0),
        Some(Command::ReleaseAll { generation: 1 })
    );
    assert_eq!(actor.request(Action::Direct(2), 1), None);
    actor.release_ack(1);
    actor.switch_ack(1);
    assert_eq!(
        actor.arm_ack(1, 2),
        Some(Command::ReleaseAll { generation: 2 })
    );
    assert_eq!(actor.state(), State::Local);
    assert!(!actor.can_forward_input());
}

#[test]
fn guest_disconnect_during_release_or_active_route_fails_local() {
    let mut actor = RequestActor::new(vec![1]);
    assert_eq!(actor.set_ready(1, true), None);
    actor.request(Action::Direct(1), 0);
    assert_eq!(
        actor.set_ready(1, false),
        Some(Command::ReleaseAll { generation: 2 })
    );
    assert_eq!(actor.state(), State::Local);
    assert_eq!(actor.release_ack(1), None);
    assert_eq!(actor.set_ready(1, true), None);
    actor.request(Action::Direct(1), 10);
    actor.release_ack(3);
    actor.switch_ack(3);
    actor.arm_ack(3, 11);
    assert_eq!(
        actor.set_ready(1, false),
        Some(Command::ReleaseAll { generation: 4 })
    );
    assert_eq!(actor.state(), State::Local);
}
