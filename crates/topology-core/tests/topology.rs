// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Tests exposed host monitor geometry, local DPI transforms, and portal invalidation.

use esp32_kvm_topology_core::{
    Edge, EdgeSegment, LogicalPoint, Monitor, PhysicalPoint, Portal, PortalError, PortalGraph,
    Rect, Rotation, Topology, TopologyError,
};

fn monitor(
    id: &str,
    bounds: (i32, i32, u32, u32),
    dpi: u32,
    rotation: Rotation,
    primary: bool,
) -> Monitor {
    let (x, y, width, height) = bounds;
    Monitor {
        id: id.into(),
        rect: Rect {
            x,
            y,
            width,
            height,
        },
        dpi_x: dpi,
        dpi_y: dpi,
        rotation,
        primary,
    }
}

#[test]
fn irregular_arrangement_exposes_only_outer_half_open_segments() {
    let topology = Topology::new(vec![
        monitor("main", (0, 0, 1920, 1080), 96, Rotation::Deg0, true),
        monitor("right", (1920, 200, 1000, 800), 144, Rotation::Deg0, false),
        monitor("above", (-800, -600, 800, 600), 120, Rotation::Deg0, false),
    ])
    .unwrap();
    let edges = topology.exposed_edges();
    assert!(edges.contains(&EdgeSegment::new("main", Edge::Right, 0, 200)));
    assert!(edges.contains(&EdgeSegment::new("main", Edge::Right, 1000, 1080)));
    assert!(!edges.contains(&EdgeSegment::new("main", Edge::Right, 200, 1000)));
    assert!(edges.contains(&EdgeSegment::new("above", Edge::Right, -600, 0)));
    assert!(!edges.contains(&EdgeSegment::new("main", Edge::Left, -600, 0)));
}

#[test]
fn mixed_dpi_and_rotation_transform_local_logical_points_to_physical_pixels() {
    let normal = monitor("normal", (-1920, 0, 1920, 1080), 120, Rotation::Deg0, true);
    assert_eq!(
        normal.logical_to_physical(LogicalPoint { x: 100.0, y: 80.0 }),
        Some(PhysicalPoint { x: -1795, y: 100 })
    );
    assert_eq!(
        normal.physical_to_logical(PhysicalPoint { x: -1795, y: 100 }),
        Some(LogicalPoint { x: 100.0, y: 80.0 })
    );
    let portrait = monitor(
        "portrait",
        (0, -1200, 800, 1200),
        96,
        Rotation::Deg90,
        false,
    );
    assert_eq!(
        portrait.logical_to_physical(LogicalPoint { x: 0.0, y: 0.0 }),
        Some(PhysicalPoint { x: 799, y: -1200 })
    );
    assert_eq!(
        portrait.logical_to_physical(LogicalPoint {
            x: 1199.0,
            y: 799.0
        }),
        Some(PhysicalPoint { x: 0, y: -1 })
    );
    assert_eq!(
        portrait.physical_to_logical(PhysicalPoint { x: 0, y: -1 }),
        Some(LogicalPoint {
            x: 1199.0,
            y: 799.0
        })
    );
    assert_eq!(
        normal.logical_to_physical(LogicalPoint { x: -1.0, y: 0.0 }),
        None
    );
}

#[test]
fn topology_replacement_is_atomic_and_invalidates_portals_on_hotplug_dpi_rotation() {
    let main = monitor("main", (0, 0, 100, 100), 96, Rotation::Deg0, true);
    let mut topology = Topology::new(vec![main.clone()]).unwrap();
    let mut graph = PortalGraph::new(
        &topology,
        vec![Portal::new("p", "main", Edge::Right, 0, 100, "guest-a")],
    )
    .unwrap();
    assert!(graph.is_current(&topology));
    assert!(!topology.replace(vec![main.clone()]).unwrap());
    let mut dpi = main.clone();
    dpi.dpi_x = 120;
    assert!(topology.replace(vec![dpi.clone()]).unwrap());
    assert!(!graph.is_current(&topology));
    graph.revalidate(&topology).unwrap();
    assert!(graph.is_current(&topology));
    let mut rotated = dpi;
    rotated.rotation = Rotation::Deg90;
    assert!(topology.replace(vec![rotated.clone()]).unwrap());
    assert!(!graph.is_current(&topology));
    let right = monitor("right", (100, 0, 100, 100), 96, Rotation::Deg0, false);
    assert!(topology.replace(vec![rotated, right]).unwrap());
    assert_eq!(graph.revalidate(&topology), Err(PortalError::NotExposed));
    assert!(!graph.is_current(&topology));
}

#[test]
fn portal_graph_rejects_ambiguous_overlap_and_internal_seams() {
    let topology = Topology::new(vec![
        monitor("a", (0, 0, 100, 100), 96, Rotation::Deg0, true),
        monitor("b", (100, 20, 100, 60), 96, Rotation::Deg0, false),
    ])
    .unwrap();
    let a = Portal::new("one", "a", Edge::Right, 0, 20, "guest-a");
    let b = Portal::new("two", "a", Edge::Right, 10, 20, "guest-b");
    assert_eq!(
        PortalGraph::new(&topology, vec![a.clone(), b]),
        Err(PortalError::Overlap)
    );
    let seam = Portal::new("seam", "a", Edge::Right, 20, 80, "guest-a");
    assert_eq!(
        PortalGraph::new(&topology, vec![seam]),
        Err(PortalError::NotExposed)
    );
    assert!(
        PortalGraph::new(
            &topology,
            vec![
                a,
                Portal::new("other", "a", Edge::Right, 80, 100, "guest-b")
            ]
        )
        .is_ok()
    );
}

#[test]
fn invalid_monitor_geometry_is_rejected_without_mutating_live_topology() {
    let good = monitor("a", (-100, 0, 100, 100), 96, Rotation::Deg0, true);
    let mut topology = Topology::new(vec![good.clone()]).unwrap();
    let revision = topology.generation();
    let overlap = monitor("b", (-50, 0, 100, 100), 96, Rotation::Deg0, false);
    assert_eq!(
        topology.replace(vec![good.clone(), overlap]),
        Err(TopologyError::OverlappingMonitors)
    );
    assert_eq!(topology.generation(), revision);
    assert_eq!(topology.monitors(), &[good]);
    assert_eq!(
        Topology::new(vec![monitor(
            "bad",
            (i32::MAX, 0, 100, 100),
            96,
            Rotation::Deg0,
            true
        )]),
        Err(TopologyError::InvalidRect)
    );
}

#[test]
fn same_generation_from_another_snapshot_cannot_reuse_portal_validation() {
    let first = Topology::new(vec![monitor(
        "main",
        (0, 0, 100, 100),
        96,
        Rotation::Deg0,
        true,
    )])
    .unwrap();
    let second = Topology::new(vec![monitor(
        "main",
        (0, 0, 100, 100),
        120,
        Rotation::Deg90,
        true,
    )])
    .unwrap();
    assert_eq!(first.generation(), second.generation());
    let graph = PortalGraph::new(
        &first,
        vec![Portal::new("p", "main", Edge::Right, 0, 100, "guest")],
    )
    .unwrap();
    assert!(!graph.is_current(&second));
}

#[test]
fn several_neighbors_remove_disjoint_seams_and_portal_hit_tests_are_half_open() {
    let topology = Topology::new(vec![
        monitor("main", (0, 0, 100, 100), 96, Rotation::Deg0, true),
        monitor("r1", (100, 10, 50, 20), 96, Rotation::Deg0, false),
        monitor("r2", (100, 60, 50, 30), 96, Rotation::Deg0, false),
    ])
    .unwrap();
    let edges = topology.exposed_edges();
    for (start, end) in [(0, 10), (30, 60), (90, 100)] {
        assert!(edges.contains(&EdgeSegment::new("main", Edge::Right, start, end)));
    }
    let graph = PortalGraph::new(
        &topology,
        vec![
            Portal::new("p1", "main", Edge::Right, 0, 10, "one"),
            Portal::new("p2", "main", Edge::Right, 30, 60, "two"),
        ],
    )
    .unwrap();
    assert_eq!(
        graph.at_edge(&topology, "main", Edge::Right, 9).unwrap().id,
        "p1"
    );
    assert!(graph.at_edge(&topology, "main", Edge::Right, 10).is_none());
    assert_eq!(
        graph
            .at_edge(&topology, "main", Edge::Right, 30)
            .unwrap()
            .id,
        "p2"
    );
    assert!(graph.at_edge(&topology, "main", Edge::Right, 60).is_none());
}

#[test]
fn other_rotations_and_axis_specific_dpi_use_physical_pixel_bounds() {
    let mut scaled = monitor("scaled", (0, 0, 200, 100), 96, Rotation::Deg0, true);
    scaled.dpi_x = 192;
    assert_eq!(
        scaled.logical_to_physical(LogicalPoint { x: 50.0, y: 50.0 }),
        Some(PhysicalPoint { x: 100, y: 50 })
    );
    assert_eq!(
        scaled.logical_to_physical(LogicalPoint { x: 100.0, y: 0.0 }),
        None
    );
    let half = monitor("half", (-400, -200, 400, 200), 96, Rotation::Deg180, true);
    assert_eq!(
        half.logical_to_physical(LogicalPoint { x: 0.0, y: 0.0 }),
        Some(PhysicalPoint { x: -1, y: -1 })
    );
    assert_eq!(
        half.physical_to_logical(PhysicalPoint { x: -1, y: -1 }),
        Some(LogicalPoint { x: 0.0, y: 0.0 })
    );
    let three_quarter = monitor("three", (0, 0, 800, 1200), 96, Rotation::Deg270, true);
    assert_eq!(
        three_quarter.logical_to_physical(LogicalPoint { x: 0.0, y: 0.0 }),
        Some(PhysicalPoint { x: 0, y: 1199 })
    );
    assert_eq!(
        three_quarter.physical_to_logical(PhysicalPoint { x: 0, y: 1199 }),
        Some(LogicalPoint { x: 0.0, y: 0.0 })
    );
}

#[test]
fn a_gap_and_corner_touch_leave_both_facing_edges_exposed() {
    let topology = Topology::new(vec![
        monitor("main", (0, 0, 100, 100), 96, Rotation::Deg0, true),
        monitor("gap", (101, 0, 100, 100), 96, Rotation::Deg0, false),
        monitor("corner", (100, 100, 100, 100), 96, Rotation::Deg0, false),
    ])
    .unwrap();
    let edges = topology.exposed_edges();
    assert!(edges.contains(&EdgeSegment::new("main", Edge::Right, 0, 100)));
    assert!(edges.contains(&EdgeSegment::new("gap", Edge::Left, 0, 100)));
    assert!(edges.contains(&EdgeSegment::new("main", Edge::Bottom, 0, 100)));
}
