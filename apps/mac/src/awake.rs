// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Schedules optional paired one-pixel guest mouse motion for the macOS host client.
// The scheduler has no USB access and requires a ready subscribed route and all-up input.

use esp32_kvm_host_actor::HostState;
use esp32_kvm_platform_windows::{CaptureEvent, PhysicalEvent};

const INTERVAL_MS: u64 = 30_000;

/// Opt-in deadline for a reversible idle pointer bump.
pub struct AwakeBump {
    enabled: bool,
    next_due_ms: Option<u64>,
    route: Option<(u8, u32)>,
}

impl AwakeBump {
    /// Creates a disabled scheduler with no pending deadline.
    pub fn new() -> Self {
        Self {
            enabled: false,
            next_due_ms: None,
            route: None,
        }
    }

    /// Turns idle motion on or off, resetting its deadline on every change.
    pub fn set_enabled(&mut self, enabled: bool) {
        if self.enabled != enabled {
            self.enabled = enabled;
            self.next_due_ms = None;
            self.route = None;
        }
    }

    /// Reports whether optional idle motion is enabled.
    pub fn enabled(&self) -> bool {
        self.enabled
    }

    /// Returns true for one due tick on an active, unchanged, all-up guest route.
    pub fn due(
        &mut self,
        now_ms: u64,
        state: HostState,
        generation: u32,
        route_ready: bool,
        physical_all_up: bool,
    ) -> bool {
        let route = match state {
            HostState::Guest(slot)
                if self.enabled && generation != 0 && route_ready && physical_all_up =>
            {
                Some((slot, generation))
            }
            _ => None,
        };
        if route.is_none() || route != self.route {
            self.route = route;
            self.next_due_ms = route.map(|_| now_ms.saturating_add(INTERVAL_MS));
            return false;
        }
        if self.next_due_ms.is_some_and(|due| now_ms >= due) {
            self.next_due_ms = Some(now_ms.saturating_add(INTERVAL_MS));
            return true;
        }
        false
    }
}

/// Builds one zero-net-motion pair tagged with the current verified route generation.
pub fn paired_motion(generation: u32) -> [CaptureEvent; 2] {
    [
        CaptureEvent {
            generation,
            event: PhysicalEvent::Motion(1, 0),
        },
        CaptureEvent {
            generation,
            event: PhysicalEvent::Motion(-1, 0),
        },
    ]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn off_by_default_and_due_only_after_guest_idle_interval() {
        let mut bump = AwakeBump::new();
        assert!(!bump.due(0, HostState::Guest(1), 7, true, true));
        bump.set_enabled(true);
        assert!(!bump.due(0, HostState::Local, 0, false, true));
        assert!(!bump.due(0, HostState::Guest(1), 7, true, true));
        assert!(!bump.due(29_999, HostState::Guest(1), 7, true, true));
        assert!(bump.due(30_000, HostState::Guest(1), 7, true, true));
        assert!(!bump.due(30_001, HostState::Guest(1), 7, true, true));
    }

    #[test]
    fn held_input_and_route_change_cancel_pending_bump() {
        let mut bump = AwakeBump::new();
        bump.set_enabled(true);
        assert!(!bump.due(0, HostState::Guest(1), 7, true, true));
        assert!(!bump.due(30_000, HostState::Guest(1), 7, true, false));
        assert!(!bump.due(30_001, HostState::Guest(1), 7, true, true));
        assert!(!bump.due(40_000, HostState::Local, 0, false, true));
        assert!(!bump.due(50_000, HostState::Guest(2), 8, true, true));
        assert!(bump.due(80_000, HostState::Guest(2), 8, true, true));
        bump.set_enabled(false);
        assert!(!bump.due(110_000, HostState::Guest(2), 8, true, true));
    }

    #[test]
    fn same_slot_new_generation_and_fault_restart_idle_clock() {
        let mut bump = AwakeBump::new();
        bump.set_enabled(true);
        assert!(!bump.due(0, HostState::Guest(1), 7, true, true));
        assert!(!bump.due(30_000, HostState::Guest(1), 8, true, true));
        assert!(!bump.due(40_000, HostState::Failed, 0, false, true));
        assert!(!bump.due(41_000, HostState::Guest(1), 9, true, true));
        assert!(bump.due(71_000, HostState::Guest(1), 9, true, true));
    }

    #[test]
    fn subscription_loss_cancels_pending_bump() {
        let mut bump = AwakeBump::new();
        bump.set_enabled(true);
        assert!(!bump.due(0, HostState::Guest(1), 7, true, true));
        assert!(!bump.due(30_000, HostState::Guest(1), 7, false, true));
        assert!(!bump.due(30_001, HostState::Guest(1), 7, true, true));
        assert!(bump.due(60_001, HostState::Guest(1), 7, true, true));
    }

    #[test]
    fn paired_motion_has_zero_net_displacement_and_one_generation() {
        let events = paired_motion(14);
        assert_eq!(events[0].generation, 14);
        assert_eq!(events[1].generation, 14);
        assert_eq!(events[0].event, PhysicalEvent::Motion(1, 0));
        assert_eq!(events[1].event, PhysicalEvent::Motion(-1, 0));
    }
}
