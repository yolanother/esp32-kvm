// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Serializes hotkey, UI, and device selection requests against the routing
// transaction. Local return preempts queued work; cycles include only ready
// guests and local control, with no guest input during a transition.

use crate::{Action, Command, Router, State};

/// Serial request policy for one host-to-board routing link.
pub struct RequestActor {
    router: Router,
    order: Vec<u8>,
    ready: Vec<u8>,
    pending: Option<Action>,
    selecting: Option<u8>,
}

impl RequestActor {
    /// Creates a local actor with guest slots in their configured cycle order.
    pub fn new(order: Vec<u8>) -> Self {
        Self::with_generation(order, 0)
    }

    /// Creates a local actor after firmware STATUS confirms its current generation.
    pub fn with_generation(order: Vec<u8>, generation: u32) -> Self {
        let mut unique = Vec::new();
        for slot in order {
            if slot != 0 && !unique.contains(&slot) {
                unique.push(slot);
            }
        }
        Self {
            router: Router::with_generation(generation),
            order: unique,
            ready: Vec::new(),
            pending: None,
            selecting: None,
        }
    }

    /// Updates firmware readiness; returns a release command if the active or
    /// selected guest becomes unavailable.
    pub fn set_ready(&mut self, slot: u8, ready: bool) -> Option<Command> {
        self.ready.retain(|known| *known != slot);
        if ready && self.order.contains(&slot) {
            self.ready.push(slot);
        }
        if !ready {
            if self.router.state() == State::Guest(slot) {
                self.pending = None;
                return Some(self.router.return_local());
            }
            if self.router.state() == State::Switching && self.selecting == Some(slot) {
                self.pending = None;
                self.selecting = None;
                return self.router.target_not_ready(self.router.generation());
            }
        }
        None
    }

    /// Returns local, switching, or active guest state.
    pub fn state(&self) -> State {
        self.router.state()
    }

    /// Returns whether fresh, unsuppressed input may reach the active guest.
    pub fn can_forward_input(&self) -> bool {
        self.router.can_forward_input()
    }

    /// Ends held-input suppression after a physical all-up observation.
    pub fn observe_all_released(&mut self) {
        self.router.observe_all_released();
    }

    /// Submits one action; local return always preempts a pending transaction.
    pub fn request(&mut self, action: Action, now_ms: u64) -> Option<Command> {
        if action == Action::Local {
            self.pending = None;
            self.selecting = None;
            return Some(self.router.return_local());
        }
        if self.router.state() == State::Switching {
            self.pending = Some(action);
            return None;
        }
        let target = match action {
            Action::Next => self.cycle(true),
            Action::Previous => self.cycle(false),
            Action::Direct(slot) => {
                if !self.ready.contains(&slot) {
                    self.selecting = None;
                    return Some(self.router.return_local());
                }
                slot
            }
            Action::Local => unreachable!(),
        };
        if target == 0 {
            self.selecting = None;
            return Some(self.router.return_local());
        }
        if self.router.state() == State::Guest(target) {
            return None;
        }
        let command = self.router.begin_switch(target, now_ms);
        self.selecting = Some(target);
        command
    }

    /// Handles current release ACK, rejecting a target that became unavailable.
    pub fn release_ack(&mut self, generation: u32) -> Option<Command> {
        if self.target_offline(generation) {
            return self.fail_not_ready(generation);
        }
        self.router.release_ack(generation)
    }

    /// Handles current selection ACK, refusing to arm an offline guest.
    pub fn switch_ack(&mut self, generation: u32) -> Option<Command> {
        if self.target_offline(generation) {
            return self.fail_not_ready(generation);
        }
        self.router.switch_ack(generation)
    }

    /// Completes current arming and starts the latest queued selection, if any.
    pub fn arm_ack(&mut self, generation: u32, now_ms: u64) -> Option<Command> {
        if self.target_offline(generation) {
            return self.fail_not_ready(generation);
        }
        self.router.arm_ack(generation)?;
        self.selecting = None;
        self.pending
            .take()
            .and_then(|action| self.request(action, now_ms))
    }

    /// Fails local when a routing deadline elapses.
    pub fn tick(&mut self, now_ms: u64) -> Option<Command> {
        let command = self.router.tick(now_ms);
        if command.is_some() {
            self.pending = None;
            self.selecting = None;
        }
        command
    }

    /// Fails local immediately after transport loss or firmware reset.
    pub fn link_lost(&mut self) -> Command {
        self.pending = None;
        self.selecting = None;
        self.ready.clear();
        self.router.link_lost()
    }

    /// Fails local when a bounded input queue overflows.
    pub fn queue_overflow(&mut self) -> Command {
        self.pending = None;
        self.selecting = None;
        self.router.queue_overflow()
    }

    fn target_offline(&self, generation: u32) -> bool {
        self.router.generation() == generation
            && self.router.state() == State::Switching
            && self
                .selecting
                .is_some_and(|slot| !self.ready.contains(&slot))
    }

    fn fail_not_ready(&mut self, generation: u32) -> Option<Command> {
        self.pending = None;
        self.selecting = None;
        self.router.target_not_ready(generation)
    }

    fn cycle(&self, forward: bool) -> u8 {
        let available: Vec<u8> = std::iter::once(0)
            .chain(
                self.order
                    .iter()
                    .copied()
                    .filter(|slot| self.ready.contains(slot)),
            )
            .collect();
        let current = match self.router.state() {
            State::Guest(slot) => slot,
            _ => 0,
        };
        let index = available
            .iter()
            .position(|slot| *slot == current)
            .unwrap_or(0);
        let next = if forward {
            (index + 1) % available.len()
        } else {
            (index + available.len() - 1) % available.len()
        };
        available[next]
    }
}
