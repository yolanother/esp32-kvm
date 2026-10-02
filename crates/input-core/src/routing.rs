// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Implements fail-local input routing. It orders release, selection, and arming
// acknowledgments while rejecting stale results, enforcing a switch deadline,
// and suppressing held input across targets.

/// Maximum time allowed for an ordinary routing transaction.
pub const SWITCH_TIMEOUT_MS: u64 = 500;

/// Commands that the host transport must send in order.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Command {
    /// Clear input on the old target and disarm forwarding.
    ReleaseAll { generation: u32 },
    /// Select a ready target after release has been acknowledged.
    Select { slot: u8, generation: u32 },
    /// Arm the selected target after selection has been acknowledged.
    Arm { slot: u8, generation: u32 },
}

/// User-visible routing outcome. A pending transaction is never an active guest.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum State {
    /// Host retains input locally.
    Local,
    /// A guest has acknowledged arming.
    Guest(u8),
    /// A transaction is waiting for ordered transport acknowledgments.
    Switching,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Phase {
    Local,
    Guest(u8),
    Release { slot: u8, deadline: u64 },
    Select { slot: u8, deadline: u64 },
    Arm { slot: u8, deadline: u64 },
}

/// Serializes a single guest switch and fails to local control on uncertainty.
#[derive(Debug)]
pub struct Router {
    phase: Phase,
    generation: u32,
    held_suppressed: bool,
}

impl Default for Router {
    fn default() -> Self {
        Self::new()
    }
}

impl Router {
    /// Creates a disarmed local router with no active guest.
    pub fn new() -> Self {
        Self {
            phase: Phase::Local,
            generation: 0,
            held_suppressed: true,
        }
    }

    /// Returns the current externally visible routing state.
    pub fn state(&self) -> State {
        match self.phase {
            Phase::Local => State::Local,
            Phase::Guest(slot) => State::Guest(slot),
            _ => State::Switching,
        }
    }

    /// Returns the generation that acknowledgments must echo.
    pub fn generation(&self) -> u32 {
        self.generation
    }

    /// Starts a guest switch; zero and overlapping requests are rejected.
    pub fn begin_switch(&mut self, slot: u8, now_ms: u64) -> Option<Command> {
        if slot == 0 || self.state() == State::Switching {
            return None;
        }
        self.generation = self.generation.wrapping_add(1);
        self.held_suppressed = true;
        self.phase = Phase::Release {
            slot,
            deadline: now_ms.saturating_add(SWITCH_TIMEOUT_MS),
        };
        Some(Command::ReleaseAll {
            generation: self.generation,
        })
    }

    /// Advances to selection only for the current release acknowledgment.
    pub fn release_ack(&mut self, generation: u32) -> Option<Command> {
        if generation != self.generation {
            return None;
        }
        if let Phase::Release { slot, deadline } = self.phase {
            self.phase = Phase::Select { slot, deadline };
            return Some(Command::Select { slot, generation });
        }
        None
    }

    /// Advances to arming only for the current selection acknowledgment.
    pub fn switch_ack(&mut self, generation: u32) -> Option<Command> {
        if generation != self.generation {
            return None;
        }
        if let Phase::Select { slot, deadline } = self.phase {
            self.phase = Phase::Arm { slot, deadline };
            return Some(Command::Arm { slot, generation });
        }
        None
    }

    /// Marks a guest active only after its current arm acknowledgment.
    pub fn arm_ack(&mut self, generation: u32) -> Option<State> {
        if generation != self.generation {
            return None;
        }
        if let Phase::Arm { slot, .. } = self.phase {
            self.phase = Phase::Guest(slot);
            return Some(State::Guest(slot));
        }
        None
    }

    /// Disarms a timed-out switch at or after its deadline.
    pub fn tick(&mut self, now_ms: u64) -> Option<Command> {
        let deadline = match self.phase {
            Phase::Release { deadline, .. }
            | Phase::Select { deadline, .. }
            | Phase::Arm { deadline, .. } => deadline,
            _ => return None,
        };
        (now_ms >= deadline).then(|| self.fail_local())
    }

    /// Fails local when the current target reports that it is not ready.
    pub fn target_not_ready(&mut self, generation: u32) -> Option<Command> {
        if generation == self.generation && self.state() == State::Switching {
            return Some(self.fail_local());
        }
        None
    }

    /// Immediately disarms after a link loss or firmware reset.
    pub fn link_lost(&mut self) -> Command {
        self.fail_local()
    }

    /// Sends an all-up baseline after reconnect, before any new switch.
    pub fn reconnected(&mut self) -> Command {
        self.phase = Phase::Local;
        self.held_suppressed = true;
        Command::ReleaseAll {
            generation: self.generation,
        }
    }

    /// Disarms rather than dropping a key or button transition on overflow.
    pub fn queue_overflow(&mut self) -> Command {
        self.fail_local()
    }

    /// Preempts any transaction and returns local control unconditionally.
    pub fn return_local(&mut self) -> Command {
        self.fail_local()
    }

    /// Reports whether fresh input may be forwarded to an armed guest.
    pub fn can_forward_input(&self) -> bool {
        matches!(self.phase, Phase::Guest(_)) && !self.held_suppressed
    }

    /// Ends held-input suppression after physical keys and buttons are all up.
    pub fn observe_all_released(&mut self) {
        if matches!(self.phase, Phase::Guest(_)) {
            self.held_suppressed = false;
        }
    }

    fn fail_local(&mut self) -> Command {
        self.generation = self.generation.wrapping_add(1);
        self.phase = Phase::Local;
        self.held_suppressed = true;
        Command::ReleaseAll {
            generation: self.generation,
        }
    }
}
