// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Exercises deterministic host-edge dwell, suppression guards, cooldown, and safe cursor return.

use esp32_kvm_edge_policy::{
    CrossingConfig, CrossingGuards, CrossingPolicy, CursorSample, GuestEntry,
};
use esp32_kvm_topology_core::{
    Edge, Monitor, PhysicalPoint, Portal, PortalGraph, Rect, Rotation, Topology,
};
use std::collections::BTreeSet;

fn monitor(id: &str, x: i32, y: i32, width: u32, height: u32, primary: bool) -> Monitor {
    Monitor {
        id: id.into(),
        rect: Rect {
            x,
            y,
            width,
            height,
        },
        dpi_x: 96,
        dpi_y: 96,
        rotation: Rotation::Deg0,
        primary,
    }
}
fn ready() -> BTreeSet<String> {
    ["guest".to_string()].into_iter().collect()
}
fn sample(monitor_id: &str, x: i32, y: i32, dx: i32, dy: i32, now_ms: u64) -> CursorSample {
    CursorSample {
        monitor_id: monitor_id.into(),
        point: PhysicalPoint { x, y },
        dx,
        dy,
        now_ms,
    }
}
fn simple(edge: Edge) -> (Topology, PortalGraph) {
    let topology = Topology::new(vec![monitor("main", -100, -100, 100, 100, true)]).unwrap();
    let (start, end) = match edge {
        Edge::Left | Edge::Right => (-100, 0),
        Edge::Top | Edge::Bottom => (-100, 0),
    };
    let graph = PortalGraph::new(
        &topology,
        vec![Portal::new("portal", "main", edge, start, end, "guest")],
    )
    .unwrap();
    (topology, graph)
}

#[test]
fn each_exposed_direction_requires_outward_motion_then_default_dwell() {
    for (edge, point, motion) in [
        (Edge::Left, (-100, -50), (-1, 0)),
        (Edge::Right, (-1, -50), (1, 0)),
        (Edge::Top, (-50, -100), (0, -1)),
        (Edge::Bottom, (-50, -1), (0, 1)),
    ] {
        let (topology, graph) = simple(edge);
        let mut policy = CrossingPolicy::new(CrossingConfig::default()).unwrap();
        let guests = ready();
        assert!(
            policy
                .observe(
                    &topology,
                    &graph,
                    sample("main", point.0, point.1, motion.0, motion.1, 100),
                    CrossingGuards::default(),
                    &guests
                )
                .is_none()
        );
        assert!(
            policy
                .observe(
                    &topology,
                    &graph,
                    sample("main", point.0, point.1, 0, 0, 299),
                    CrossingGuards::default(),
                    &guests
                )
                .is_none()
        );
        let request = policy
            .observe(
                &topology,
                &graph,
                sample("main", point.0, point.1, 0, 0, 300),
                CrossingGuards::default(),
                &guests,
            )
            .unwrap();
        assert_eq!(request.destination_guest_id, "guest");
        assert_eq!(request.entry, GuestEntry::RetainCursor);
        assert_eq!(
            request.saved_host_cursor,
            PhysicalPoint {
                x: point.0,
                y: point.1
            }
        );
    }
}

#[test]
fn corners_wrong_motion_and_internal_seams_never_start_dwell() {
    let (topology, graph) = simple(Edge::Right);
    let mut policy = CrossingPolicy::new(CrossingConfig::default()).unwrap();
    let guests = ready();
    assert!(
        policy
            .observe(
                &topology,
                &graph,
                sample("main", -1, -99, 1, 0, 0),
                CrossingGuards::default(),
                &guests
            )
            .is_none()
    );
    assert!(
        policy
            .observe(
                &topology,
                &graph,
                sample("main", -1, -99, 0, 0, 1000),
                CrossingGuards::default(),
                &guests
            )
            .is_none()
    );
    assert!(
        policy
            .observe(
                &topology,
                &graph,
                sample("main", -1, -50, -1, 0, 1100),
                CrossingGuards::default(),
                &guests
            )
            .is_none()
    );
    assert!(
        policy
            .observe(
                &topology,
                &graph,
                sample("main", -1, -50, 0, 0, 1400),
                CrossingGuards::default(),
                &guests
            )
            .is_none()
    );
    let seam_topology = Topology::new(vec![
        monitor("main", 0, 0, 100, 100, true),
        monitor("right", 100, 20, 100, 60, false),
    ])
    .unwrap();
    let seam_graph = PortalGraph::new(
        &seam_topology,
        vec![Portal::new("outer", "main", Edge::Right, 0, 20, "guest")],
    )
    .unwrap();
    assert!(
        policy
            .observe(
                &seam_topology,
                &seam_graph,
                sample("main", 99, 50, 1, 0, 1500),
                CrossingGuards::default(),
                &guests
            )
            .is_none()
    );
}

#[test]
fn drag_pause_fullscreen_recording_switch_and_offline_guards_reset_dwell() {
    let (topology, graph) = simple(Edge::Right);
    let guests = ready();
    for guard in [
        CrossingGuards {
            buttons_down: true,
            ..Default::default()
        },
        CrossingGuards {
            paused: true,
            ..Default::default()
        },
        CrossingGuards {
            fullscreen_active: true,
            ..Default::default()
        },
        CrossingGuards {
            recording_hotkey: true,
            ..Default::default()
        },
        CrossingGuards {
            switching: true,
            ..Default::default()
        },
        CrossingGuards {
            local_control: false,
            ..Default::default()
        },
    ] {
        let mut policy = CrossingPolicy::new(CrossingConfig::default()).unwrap();
        policy.observe(
            &topology,
            &graph,
            sample("main", -1, -50, 1, 0, 0),
            CrossingGuards::default(),
            &guests,
        );
        assert!(
            policy
                .observe(
                    &topology,
                    &graph,
                    sample("main", -1, -50, 0, 0, 250),
                    guard,
                    &guests
                )
                .is_none()
        );
        assert!(
            policy
                .observe(
                    &topology,
                    &graph,
                    sample("main", -1, -50, 0, 0, 500),
                    CrossingGuards::default(),
                    &guests
                )
                .is_none()
        );
    }
    let mut policy = CrossingPolicy::new(CrossingConfig::default()).unwrap();
    assert!(
        policy
            .observe(
                &topology,
                &graph,
                sample("main", -1, -50, 1, 0, 0),
                CrossingGuards::default(),
                &BTreeSet::new()
            )
            .is_none()
    );
    assert!(
        policy
            .observe(
                &topology,
                &graph,
                sample("main", -1, -50, 0, 0, 300),
                CrossingGuards::default(),
                &guests
            )
            .is_none()
    );
}

#[test]
fn cooldown_requires_both_time_and_leaving_activation_strip() {
    let (topology, graph) = simple(Edge::Right);
    let guests = ready();
    let mut policy = CrossingPolicy::new(CrossingConfig::default()).unwrap();
    policy.observe(
        &topology,
        &graph,
        sample("main", -1, -50, 1, 0, 0),
        CrossingGuards::default(),
        &guests,
    );
    assert!(
        policy
            .observe(
                &topology,
                &graph,
                sample("main", -1, -50, 0, 0, 200),
                CrossingGuards::default(),
                &guests
            )
            .is_some()
    );
    assert!(
        policy
            .observe(
                &topology,
                &graph,
                sample("main", -1, -50, 1, 0, 1000),
                CrossingGuards::default(),
                &guests
            )
            .is_none()
    );
    policy.observe(
        &topology,
        &graph,
        sample("main", -2, -50, -1, 0, 1001),
        CrossingGuards::default(),
        &guests,
    );
    assert!(
        policy
            .observe(
                &topology,
                &graph,
                sample("main", -1, -50, 1, 0, 1002),
                CrossingGuards::default(),
                &guests
            )
            .is_none()
    );
    assert!(
        policy
            .observe(
                &topology,
                &graph,
                sample("main", -1, -50, 0, 0, 1202),
                CrossingGuards::default(),
                &guests
            )
            .is_some()
    );
}

#[test]
fn local_return_uses_safe_inset_and_unplug_falls_back_to_current_primary() {
    let (topology, graph) = simple(Edge::Right);
    let guests = ready();
    let mut policy = CrossingPolicy::new(CrossingConfig::default()).unwrap();
    policy.observe(
        &topology,
        &graph,
        sample("main", -1, -50, 1, 0, 0),
        CrossingGuards::default(),
        &guests,
    );
    policy
        .observe(
            &topology,
            &graph,
            sample("main", -1, -50, 0, 0, 200),
            CrossingGuards::default(),
            &guests,
        )
        .unwrap();
    assert_eq!(
        policy.take_local_return(&topology),
        Some(PhysicalPoint { x: -17, y: -50 })
    );
    assert_eq!(policy.take_local_return(&topology), None);
    policy.observe(
        &topology,
        &graph,
        sample("main", -2, -50, -1, 0, 700),
        CrossingGuards::default(),
        &guests,
    );
    policy.observe(
        &topology,
        &graph,
        sample("main", -1, -50, 1, 0, 701),
        CrossingGuards::default(),
        &guests,
    );
    policy
        .observe(
            &topology,
            &graph,
            sample("main", -1, -50, 0, 0, 901),
            CrossingGuards::default(),
            &guests,
        )
        .unwrap();
    let unplugged = Topology::new(vec![monitor("new-primary", 200, 300, 100, 100, true)]).unwrap();
    assert_eq!(
        policy.take_local_return(&unplugged),
        Some(PhysicalPoint { x: 250, y: 350 })
    );
}

#[test]
fn leaving_strip_or_moving_inward_cancels_dwell_and_requires_new_outward_motion() {
    let (topology, graph) = simple(Edge::Right);
    let guests = ready();
    let mut policy = CrossingPolicy::new(CrossingConfig::default()).unwrap();
    policy.observe(
        &topology,
        &graph,
        sample("main", -1, -50, 1, 0, 0),
        CrossingGuards::default(),
        &guests,
    );
    assert!(
        policy
            .observe(
                &topology,
                &graph,
                sample("main", -2, -50, -1, 0, 50),
                CrossingGuards::default(),
                &guests
            )
            .is_none()
    );
    assert!(
        policy
            .observe(
                &topology,
                &graph,
                sample("main", -1, -50, 0, 0, 300),
                CrossingGuards::default(),
                &guests
            )
            .is_none()
    );
    policy.observe(
        &topology,
        &graph,
        sample("main", -1, -50, 1, 0, 301),
        CrossingGuards::default(),
        &guests,
    );
    assert!(
        policy
            .observe(
                &topology,
                &graph,
                sample("main", -1, -50, -1, 0, 400),
                CrossingGuards::default(),
                &guests
            )
            .is_none()
    );
    assert!(
        policy
            .observe(
                &topology,
                &graph,
                sample("main", -1, -50, 0, 0, 600),
                CrossingGuards::default(),
                &guests
            )
            .is_none()
    );
}

#[test]
fn zero_dwell_is_immediate_and_invalid_settings_are_rejected() {
    let (topology, graph) = simple(Edge::Right);
    let guests = ready();
    let mut config = CrossingConfig {
        dwell_ms: 0,
        ..Default::default()
    };
    let mut policy = CrossingPolicy::new(config).unwrap();
    assert!(
        policy
            .observe(
                &topology,
                &graph,
                sample("main", -1, -50, 1, 0, 0),
                CrossingGuards::default(),
                &guests
            )
            .is_some()
    );
    config.dwell_ms = 1001;
    assert!(CrossingPolicy::new(config).is_err());
    config.dwell_ms = 200;
    config.strip_px = 0;
    assert!(CrossingPolicy::new(config).is_err());
    config.strip_px = 1000;
    assert!(CrossingPolicy::new(config).is_err());
}

#[test]
fn topology_change_restarts_dwell_even_when_portal_id_survives_revalidation() {
    let (mut topology, mut graph) = simple(Edge::Right);
    let guests = ready();
    let mut policy = CrossingPolicy::new(CrossingConfig::default()).unwrap();
    policy.observe(
        &topology,
        &graph,
        sample("main", -1, -50, 1, 0, 0),
        CrossingGuards::default(),
        &guests,
    );
    let mut changed = topology.monitors()[0].clone();
    changed.dpi_x = 120;
    topology.replace(vec![changed]).unwrap();
    graph.revalidate(&topology).unwrap();
    assert!(
        policy
            .observe(
                &topology,
                &graph,
                sample("main", -1, -50, 0, 0, 300),
                CrossingGuards::default(),
                &guests
            )
            .is_none()
    );
    policy.observe(
        &topology,
        &graph,
        sample("main", -1, -50, 1, 0, 301),
        CrossingGuards::default(),
        &guests,
    );
    assert!(
        policy
            .observe(
                &topology,
                &graph,
                sample("main", -1, -50, 0, 0, 501),
                CrossingGuards::default(),
                &guests
            )
            .is_some()
    );
}

#[test]
fn a_corner_with_two_eligible_portals_is_ambiguous_even_with_zero_corner_margin() {
    let topology = Topology::new(vec![monitor("main", 0, 0, 100, 100, true)]).unwrap();
    let graph = PortalGraph::new(
        &topology,
        vec![
            Portal::new("right", "main", Edge::Right, 0, 100, "guest"),
            Portal::new("top", "main", Edge::Top, 0, 100, "guest"),
        ],
    )
    .unwrap();
    let mut policy = CrossingPolicy::new(CrossingConfig {
        dwell_ms: 0,
        corner_exclusion_px: 0,
        ..Default::default()
    })
    .unwrap();
    assert!(
        policy
            .observe(
                &topology,
                &graph,
                sample("main", 99, 0, 1, -1, 0),
                CrossingGuards::default(),
                &ready()
            )
            .is_none()
    );
}

#[test]
fn fullscreen_opt_in_and_clock_regression_have_explicit_effects() {
    let (topology, graph) = simple(Edge::Right);
    let guests = ready();
    let mut policy = CrossingPolicy::new(CrossingConfig {
        allow_fullscreen: true,
        ..Default::default()
    })
    .unwrap();
    let fullscreen = CrossingGuards {
        fullscreen_active: true,
        ..Default::default()
    };
    policy.observe(
        &topology,
        &graph,
        sample("main", -1, -50, 1, 0, 100),
        fullscreen,
        &guests,
    );
    assert!(
        policy
            .observe(
                &topology,
                &graph,
                sample("main", -1, -50, 0, 0, 99),
                fullscreen,
                &guests
            )
            .is_none()
    );
    assert!(
        policy
            .observe(
                &topology,
                &graph,
                sample("main", -1, -50, 0, 0, 400),
                fullscreen,
                &guests
            )
            .is_none()
    );
    policy.observe(
        &topology,
        &graph,
        sample("main", -1, -50, 1, 0, 401),
        fullscreen,
        &guests,
    );
    assert!(
        policy
            .observe(
                &topology,
                &graph,
                sample("main", -1, -50, 0, 0, 601),
                fullscreen,
                &guests
            )
            .is_some()
    );
}
