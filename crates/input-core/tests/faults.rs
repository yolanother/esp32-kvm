// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Exercises input routing safety with a controllable clock and recorded transport
// commands, including exact deadline boundaries, queued request cancellation,
// delayed acknowledgments, resets, and queue overflow.

use esp32_kvm_input_core::{Action, Command, RequestActor, Router, State};

struct FakeClock(u64);

impl FakeClock {
    fn advance(&mut self, milliseconds: u64) {
        self.0 += milliseconds;
    }
}

#[derive(Default)]
struct FakeTransport(Vec<Command>);

impl FakeTransport {
    fn send(&mut self, command: Option<Command>) {
        if let Some(command) = command {
            self.0.push(command);
        }
    }
}

#[test]
fn delayed_ack_and_timeout_fail_local_without_arming() {
    let mut clock = FakeClock(10);
    let mut wire = FakeTransport::default();
    let mut router = Router::new();
    wire.send(router.begin_switch(1, clock.0));
    let generation = router.generation();
    assert_eq!(wire.0, vec![Command::ReleaseAll { generation }]);
    clock.advance(501);
    wire.send(router.tick(clock.0));
    assert_eq!(router.state(), State::Local);
    assert_eq!(
        wire.0.last(),
        Some(&Command::ReleaseAll {
            generation: router.generation()
        })
    );
    assert_eq!(router.release_ack(generation), None);
    assert_eq!(router.switch_ack(generation), None);
    assert_eq!(router.arm_ack(generation), None);
    assert_eq!(router.state(), State::Local);
}

#[test]
fn switch_requires_ordered_acks_and_all_up_before_input() {
    let mut wire = FakeTransport::default();
    let mut router = Router::new();
    wire.send(router.begin_switch(2, 0));
    let generation = router.generation();
    assert_eq!(router.switch_ack(generation), None);
    wire.send(router.release_ack(generation));
    assert_eq!(
        wire.0.last(),
        Some(&Command::Select {
            slot: 2,
            expected_generation: generation,
            new_generation: generation + 1,
        })
    );
    wire.send(router.switch_ack(generation + 1));
    assert_eq!(
        wire.0.last(),
        Some(&Command::Arm {
            slot: 2,
            generation: generation + 1,
        })
    );
    assert!(!router.can_forward_input());
    assert_eq!(router.arm_ack(generation + 1), Some(State::Guest(2)));
    assert_eq!(router.state(), State::Guest(2));
    assert!(!router.can_forward_input());
    router.observe_all_released();
    assert!(router.can_forward_input());
}

#[test]
fn link_loss_reset_and_overflow_disarm_without_replay() {
    let mut router = Router::new();
    router.begin_switch(1, 0);
    let old = router.generation();
    router.release_ack(old);
    router.switch_ack(old + 1);
    router.arm_ack(old + 1);
    router.observe_all_released();
    assert!(router.can_forward_input());
    let release = router.link_lost();
    assert_eq!(router.state(), State::Local);
    assert!(!router.can_forward_input());
    assert_eq!(
        release,
        Command::ReleaseAll {
            generation: router.generation()
        }
    );
    assert_eq!(router.arm_ack(old), None);
    assert_eq!(
        router.reconnected(),
        Command::ReleaseAll {
            generation: router.generation()
        }
    );
    router.begin_switch(1, 20);
    let next = router.generation();
    router.release_ack(next);
    router.switch_ack(next + 1);
    router.arm_ack(next + 1);
    assert!(!router.can_forward_input());
    assert_eq!(
        router.queue_overflow(),
        Command::ReleaseAll {
            generation: router.generation()
        }
    );
    assert_eq!(router.state(), State::Local);
}

#[test]
fn stale_generation_and_not_ready_target_do_not_arm() {
    let mut router = Router::new();
    assert_eq!(router.begin_switch(0, 0), None);
    router.begin_switch(3, 0);
    let generation = router.generation();
    assert_eq!(router.release_ack(generation.wrapping_sub(1)), None);
    assert_eq!(router.target_not_ready(generation.wrapping_sub(1)), None);
    assert_eq!(router.switch_ack(generation), None);
    router.release_ack(generation);
    assert_eq!(
        router.target_not_ready(generation + 1),
        Some(Command::ReleaseAll {
            generation: router.generation()
        })
    );
    assert_eq!(router.state(), State::Local);
    assert_eq!(router.arm_ack(generation), None);
}

#[test]
fn exact_deadline_disarms_in_release_select_and_arm_phases() {
    for phase in 0..3 {
        let mut router = Router::new();
        router.begin_switch(1, 100);
        if phase >= 1 {
            assert!(matches!(
                router.release_ack(1),
                Some(Command::Select { .. })
            ));
        }
        if phase >= 2 {
            assert_eq!(
                router.switch_ack(2),
                Some(Command::Arm {
                    slot: 1,
                    generation: 2
                })
            );
        }
        let pending_generation = router.generation();
        assert_eq!(router.tick(599), None);
        assert_eq!(router.state(), State::Switching);
        assert_eq!(
            router.tick(600),
            Some(Command::ReleaseAll {
                generation: pending_generation + 1,
            })
        );
        assert_eq!(router.state(), State::Local);
        assert_eq!(router.release_ack(pending_generation), None);
        assert_eq!(router.switch_ack(pending_generation), None);
        assert_eq!(router.arm_ack(pending_generation), None);
        assert!(!router.can_forward_input());
    }
}

#[test]
fn all_up_observed_before_arm_ack_does_not_unsuppress_new_guest() {
    let mut router = Router::new();
    router.begin_switch(1, 0);
    router.observe_all_released();
    router.release_ack(1);
    router.observe_all_released();
    router.switch_ack(2);
    router.observe_all_released();
    assert_eq!(router.arm_ack(2), Some(State::Guest(1)));
    assert!(!router.can_forward_input());
    router.observe_all_released();
    assert!(router.can_forward_input());
}

#[test]
fn pending_selection_is_dropped_after_timeout_without_replay() {
    let mut clock = FakeClock(10);
    let mut actor = RequestActor::new(vec![1, 2]);
    actor.set_ready(1, true);
    actor.set_ready(2, true);
    assert_eq!(
        actor.request(Action::Direct(1), clock.0),
        Some(Command::ReleaseAll { generation: 1 })
    );
    assert_eq!(actor.request(Action::Direct(2), clock.0 + 1), None);
    clock.advance(499);
    assert_eq!(actor.tick(clock.0), None);
    clock.advance(1);
    assert_eq!(
        actor.tick(clock.0),
        Some(Command::ReleaseAll { generation: 2 })
    );
    assert_eq!(actor.release_ack(1), None);
    assert_eq!(actor.switch_ack(2), None);
    assert_eq!(actor.arm_ack(2, clock.0), None);
    assert_eq!(actor.state(), State::Local);
    assert_eq!(
        actor.request(Action::Direct(1), clock.0 + 1),
        Some(Command::ReleaseAll { generation: 3 })
    );
    assert!(matches!(
        actor.release_ack(3),
        Some(Command::Select { slot: 1, .. })
    ));
    assert_eq!(
        actor.switch_ack(4),
        Some(Command::Arm {
            slot: 1,
            generation: 4
        })
    );
    assert_eq!(actor.arm_ack(4, clock.0 + 2), None);
    assert_eq!(actor.state(), State::Guest(1));
    assert!(!actor.can_forward_input());
}

#[test]
fn link_loss_clears_readiness_and_queued_selection() {
    let mut actor = RequestActor::new(vec![1, 2]);
    actor.set_ready(1, true);
    actor.set_ready(2, true);
    actor.request(Action::Direct(1), 0);
    actor.request(Action::Direct(2), 1);
    assert_eq!(actor.link_lost(), Command::ReleaseAll { generation: 2 });
    assert_eq!(actor.release_ack(1), None);
    assert_eq!(
        actor.request(Action::Direct(1), 2),
        Some(Command::ReleaseAll { generation: 3 })
    );
    assert_eq!(actor.state(), State::Local);
    actor.set_ready(1, true);
    assert_eq!(
        actor.request(Action::Direct(1), 3),
        Some(Command::ReleaseAll { generation: 4 })
    );
    assert!(matches!(
        actor.release_ack(4),
        Some(Command::Select { slot: 1, .. })
    ));
    assert_eq!(
        actor.switch_ack(5),
        Some(Command::Arm {
            slot: 1,
            generation: 5
        })
    );
    assert_eq!(actor.arm_ack(5, 4), None);
    assert_eq!(actor.state(), State::Guest(1));
    assert!(!actor.can_forward_input());
}
