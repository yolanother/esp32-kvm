// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Evaluates physical host-edge dwell and crossing guards against validated portals.
// Emits intent and a safe local-return point without moving a cursor or guessing guest position.

#![forbid(unsafe_code)]

use esp32_kvm_topology_core::{Edge, Monitor, PhysicalPoint, Portal, PortalGraph, Rect, Topology};
use std::collections::BTreeSet;

/// Timing and physical-pixel geometry for host-to-guest edge activation.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CrossingConfig {
    /// Continuous eligible dwell, from 0 to 1000 milliseconds.
    pub dwell_ms: u16,
    /// Pixels excluded at each end of a portal segment.
    pub corner_exclusion_px: u32,
    /// Minimum delay after a crossing before another may activate.
    pub cooldown_ms: u64,
    /// Thickness of the physical activation strip inside a monitor edge.
    pub strip_px: u32,
    /// Pixels to move inward on local return, capped to source bounds.
    pub return_inset_px: u32,
    /// Opt-in to crossing while the host reports a fullscreen app.
    pub allow_fullscreen: bool,
}

impl Default for CrossingConfig {
    fn default() -> Self {
        Self {
            dwell_ms: 200,
            corner_exclusion_px: 12,
            cooldown_ms: 500,
            strip_px: 1,
            return_inset_px: 16,
            allow_fullscreen: false,
        }
    }
}

/// Invalid timing or geometry configuration.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ConfigError {
    /// Dwell exceeds the supported 1000 ms range.
    InvalidDwell,
    /// Strip or return inset must contain at least one physical pixel.
    InvalidGeometry,
}

/// Current host capture and UI guards, supplied by authoritative native state.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CrossingGuards {
    /// The host currently receives local input.
    pub local_control: bool,
    /// User paused automatic crossing.
    pub paused: bool,
    /// At least one physical mouse button is held.
    pub buttons_down: bool,
    /// An app is fullscreen and crossing was not explicitly enabled.
    pub fullscreen_active: bool,
    /// Physical shortcut recorder is active.
    pub recording_hotkey: bool,
    /// Routing actor has an unfinished transition.
    pub switching: bool,
}

impl Default for CrossingGuards {
    fn default() -> Self {
        Self {
            local_control: true,
            paused: false,
            buttons_down: false,
            fullscreen_active: false,
            recording_hotkey: false,
            switching: false,
        }
    }
}

/// One OS cursor sample in global physical pixels with relative movement.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CursorSample {
    /// Stable OS monitor ID containing the cursor.
    pub monitor_id: String,
    /// Clamped physical cursor position in that monitor.
    pub point: PhysicalPoint,
    /// Relative physical horizontal movement since the previous sample.
    pub dx: i32,
    /// Relative physical vertical movement since the previous sample.
    pub dy: i32,
    /// Monotonic sample time in milliseconds.
    pub now_ms: u64,
}

/// Standard BLE entry behavior; guest cursor position is never inferred.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum GuestEntry {
    /// Leave the guest OS cursor wherever its own system last placed it.
    RetainCursor,
}

/// One eligible intent for the serialized host routing actor.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CrossingRequest {
    /// Persistent guest ID resolved by the caller to a currently ready slot.
    pub destination_guest_id: String,
    /// Validated source portal ID.
    pub portal_id: String,
    /// Local physical cursor position saved before routing away.
    pub saved_host_cursor: PhysicalPoint,
    /// No absolute remote guest position is available in Standard BLE mode.
    pub entry: GuestEntry,
}

#[derive(Clone, Debug)]
struct Candidate {
    portal_id: String,
    since_ms: u64,
}
#[derive(Clone, Debug)]
struct Latch {
    monitor_id: String,
    edge: Edge,
    triggered_ms: u64,
    must_leave_strip: bool,
}
#[derive(Clone, Debug)]
struct SavedCursor {
    monitor_id: String,
    edge: Edge,
    point: PhysicalPoint,
}

/// Stateful, deterministic host-edge crossing policy driven by explicit samples.
pub struct CrossingPolicy {
    config: CrossingConfig,
    candidate: Option<Candidate>,
    latch: Option<Latch>,
    saved_cursor: Option<SavedCursor>,
    last_now_ms: Option<u64>,
    observed_topology: Option<Topology>,
}

impl CrossingPolicy {
    /// Cancels an unfinished dwell after a missing sample or lost native guard.
    /// A saved local return point survives while the guest route is active.
    pub fn cancel_pending(&mut self) {
        self.candidate = None;
    }

    /// Validates parameters before processing cursor samples.
    pub fn new(config: CrossingConfig) -> Result<Self, ConfigError> {
        if config.dwell_ms > 1000 {
            return Err(ConfigError::InvalidDwell);
        }
        if config.strip_px == 0 || config.strip_px > 32 || config.return_inset_px == 0 {
            return Err(ConfigError::InvalidGeometry);
        }
        Ok(Self {
            config,
            candidate: None,
            latch: None,
            saved_cursor: None,
            last_now_ms: None,
            observed_topology: None,
        })
    }

    /// Evaluates one sample; an emitted request is only intent, not a completed switch.
    /// The caller supplies readiness by persistent guest ID and must route through HostActor.
    pub fn observe(
        &mut self,
        topology: &Topology,
        graph: &PortalGraph,
        sample: CursorSample,
        guards: CrossingGuards,
        ready_guests: &BTreeSet<String>,
    ) -> Option<CrossingRequest> {
        if self.observed_topology.as_ref() != Some(topology) {
            self.candidate = None;
            self.observed_topology = Some(topology.clone());
        }
        if self.last_now_ms.is_some_and(|last| sample.now_ms < last) {
            self.candidate = None;
            return None;
        }
        self.last_now_ms = Some(sample.now_ms);
        self.update_latch(topology, &sample);
        if !graph.is_current(topology)
            || !guards.local_control
            || guards.paused
            || guards.buttons_down
            || guards.recording_hotkey
            || guards.switching
            || (guards.fullscreen_active && !self.config.allow_fullscreen)
        {
            self.candidate = None;
            return None;
        }
        if self.latch.as_ref().is_some_and(|latch| {
            latch.must_leave_strip
                || sample.now_ms.saturating_sub(latch.triggered_ms) < self.config.cooldown_ms
        }) {
            self.candidate = None;
            return None;
        }
        let Some(monitor) = topology
            .monitors()
            .iter()
            .find(|monitor| monitor.id == sample.monitor_id)
        else {
            self.candidate = None;
            return None;
        };
        let Some(portal) = self.portal_at_sample(topology, graph, monitor, &sample) else {
            self.candidate = None;
            return None;
        };
        if !ready_guests.contains(&portal.destination_guest_id) {
            self.candidate = None;
            return None;
        }
        let same = self
            .candidate
            .as_ref()
            .is_some_and(|candidate| candidate.portal_id == portal.id);
        if inward(portal.source.edge, sample.dx, sample.dy) {
            self.candidate = None;
            return None;
        }
        if !same {
            self.candidate = None;
            if !outward(portal.source.edge, sample.dx, sample.dy) {
                return None;
            }
            self.candidate = Some(Candidate {
                portal_id: portal.id.clone(),
                since_ms: sample.now_ms,
            });
        }
        if sample
            .now_ms
            .saturating_sub(self.candidate.as_ref()?.since_ms)
            < u64::from(self.config.dwell_ms)
        {
            return None;
        }
        self.candidate = None;
        self.latch = Some(Latch {
            monitor_id: monitor.id.clone(),
            edge: portal.source.edge,
            triggered_ms: sample.now_ms,
            must_leave_strip: true,
        });
        self.saved_cursor = Some(SavedCursor {
            monitor_id: monitor.id.clone(),
            edge: portal.source.edge,
            point: sample.point,
        });
        Some(CrossingRequest {
            destination_guest_id: portal.destination_guest_id.clone(),
            portal_id: portal.id.clone(),
            saved_host_cursor: sample.point,
            entry: GuestEntry::RetainCursor,
        })
    }

    /// Consumes the saved local-return point after routing returns to the host.
    /// If the source monitor vanished, falls back to the current primary center.
    pub fn take_local_return(&mut self, topology: &Topology) -> Option<PhysicalPoint> {
        let saved = self.saved_cursor.take()?;
        if let Some(monitor) = topology
            .monitors()
            .iter()
            .find(|monitor| monitor.id == saved.monitor_id)
        {
            return Some(inset_point(
                monitor.rect,
                saved.edge,
                saved.point,
                self.config.return_inset_px,
            ));
        }
        topology
            .monitors()
            .iter()
            .find(|monitor| monitor.primary)
            .map(|monitor| PhysicalPoint {
                x: (i64::from(monitor.rect.x) + i64::from(monitor.rect.width) / 2) as i32,
                y: (i64::from(monitor.rect.y) + i64::from(monitor.rect.height) / 2) as i32,
            })
    }

    fn update_latch(&mut self, topology: &Topology, sample: &CursorSample) {
        let Some(latch) = self.latch.as_mut() else {
            return;
        };
        if latch.must_leave_strip {
            let still_inside = topology
                .monitors()
                .iter()
                .find(|monitor| monitor.id == latch.monitor_id)
                .is_some_and(|monitor| {
                    sample.monitor_id == latch.monitor_id
                        && in_strip(monitor.rect, latch.edge, sample.point, self.config.strip_px)
                });
            if !still_inside {
                latch.must_leave_strip = false;
            }
        }
    }

    fn portal_at_sample<'a>(
        &self,
        topology: &Topology,
        graph: &'a PortalGraph,
        monitor: &Monitor,
        sample: &CursorSample,
    ) -> Option<&'a Portal> {
        let mut eligible = None;
        for edge in [Edge::Left, Edge::Right, Edge::Top, Edge::Bottom] {
            if !in_strip(monitor.rect, edge, sample.point, self.config.strip_px) {
                continue;
            }
            let coordinate = match edge {
                Edge::Left | Edge::Right => sample.point.y,
                Edge::Top | Edge::Bottom => sample.point.x,
            };
            let Some(portal) = graph.at_edge(topology, &monitor.id, edge, coordinate) else {
                continue;
            };
            let margin = i64::from(self.config.corner_exclusion_px);
            let position = i64::from(coordinate);
            if position >= i64::from(portal.source.start) + margin
                && position < i64::from(portal.source.end) - margin
            {
                if eligible.is_some() {
                    return None;
                }
                eligible = Some(portal);
            }
        }
        eligible
    }
}

fn outward(edge: Edge, dx: i32, dy: i32) -> bool {
    match edge {
        Edge::Left => dx < 0,
        Edge::Right => dx > 0,
        Edge::Top => dy < 0,
        Edge::Bottom => dy > 0,
    }
}

fn inward(edge: Edge, dx: i32, dy: i32) -> bool {
    match edge {
        Edge::Left => dx > 0,
        Edge::Right => dx < 0,
        Edge::Top => dy > 0,
        Edge::Bottom => dy < 0,
    }
}

fn in_strip(rect: Rect, edge: Edge, point: PhysicalPoint, strip_px: u32) -> bool {
    let (x, y) = (i64::from(point.x), i64::from(point.y));
    let (left, top) = (i64::from(rect.x), i64::from(rect.y));
    let (right, bottom) = (left + i64::from(rect.width), top + i64::from(rect.height));
    if x < left || x >= right || y < top || y >= bottom {
        return false;
    }
    let strip = i64::from(strip_px);
    match edge {
        Edge::Left => x < left + strip,
        Edge::Right => x >= right - strip,
        Edge::Top => y < top + strip,
        Edge::Bottom => y >= bottom - strip,
    }
}

fn inset_point(rect: Rect, edge: Edge, saved: PhysicalPoint, inset_px: u32) -> PhysicalPoint {
    let (left, top) = (i64::from(rect.x), i64::from(rect.y));
    let (right, bottom) = (
        left + i64::from(rect.width) - 1,
        top + i64::from(rect.height) - 1,
    );
    let inset = i64::from(inset_px);
    let (x, y) = match edge {
        Edge::Left => (
            (left + inset).min(right),
            i64::from(saved.y).clamp(top, bottom),
        ),
        Edge::Right => (
            (right - inset).max(left),
            i64::from(saved.y).clamp(top, bottom),
        ),
        Edge::Top => (
            i64::from(saved.x).clamp(left, right),
            (top + inset).min(bottom),
        ),
        Edge::Bottom => (
            i64::from(saved.x).clamp(left, right),
            (bottom - inset).max(top),
        ),
    };
    PhysicalPoint {
        x: x as i32,
        y: y as i32,
    }
}
