// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Validates manual host-monitor layout drafts and dry-run portals against the
// native topology core. It never enables crossing or infers guest geometry.

use esp32_kvm_topology_core::{Edge, Monitor, Portal, PortalGraph, Rect, Rotation, Topology};
use serde::{Deserialize, Serialize};

/// User-entered host rectangle for dry-run arrangement, not OS enumeration.
#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct HostDraft {
    /// Stable draft identifier.
    pub id: String,
    /// Left physical-pixel coordinate in the draft virtual desktop.
    pub x: i32,
    /// Top physical-pixel coordinate in the draft virtual desktop.
    pub y: i32,
    /// Physical width in draft pixels.
    pub width: u32,
    /// Physical height in draft pixels.
    pub height: u32,
    /// Assumed horizontal DPI for the dry run.
    pub dpi_x: u32,
    /// Assumed vertical DPI for the dry run.
    pub dpi_y: u32,
    /// Draft rotation: deg0, deg90, deg180 or deg270.
    pub rotation: String,
    /// Exactly one draft host display is primary.
    pub primary: bool,
}

/// One directed host-to-saved-guest dry-run portal.
#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PortalDraft {
    /// Stable draft portal identifier.
    pub id: String,
    /// Source draft host ID.
    pub monitor_id: String,
    /// Physical edge name.
    pub edge: String,
    /// Inclusive physical coordinate along that edge.
    pub start: i32,
    /// Exclusive physical coordinate along that edge.
    pub end: i32,
    /// Opaque saved-guest identity, never a firmware slot.
    pub destination_token: String,
    /// Desired outward-edge dwell from zero through 1000 ms.
    pub dwell_ms: u16,
    /// Only outward host-to-guest direction exists in Standard BLE mode.
    pub direction: String,
}

/// One edge interval exposed by the validated draft topology.
#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PreviewSegment {
    /// Source draft monitor ID.
    pub monitor_id: String,
    /// Left, right, top or bottom.
    pub edge: &'static str,
    /// Inclusive physical coordinate.
    pub start: i32,
    /// Exclusive physical coordinate.
    pub end: i32,
}

/// Pure dry-run result. Activation remains unavailable until OS topology is verified.
#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct LayoutPreview {
    /// Maximal exposed physical edge intervals.
    pub segments: Vec<PreviewSegment>,
    /// Number of validated directed portals.
    pub portal_count: usize,
    /// Always false while capture and OS monitor discovery are disconnected.
    pub activation_available: bool,
}

fn rotation(value: &str) -> Result<Rotation, String> {
    match value {
        "deg0" => Ok(Rotation::Deg0),
        "deg90" => Ok(Rotation::Deg90),
        "deg180" => Ok(Rotation::Deg180),
        "deg270" => Ok(Rotation::Deg270),
        _ => Err("Choose a supported monitor rotation.".into()),
    }
}

fn edge(value: &str) -> Result<Edge, String> {
    match value {
        "left" => Ok(Edge::Left),
        "right" => Ok(Edge::Right),
        "top" => Ok(Edge::Top),
        "bottom" => Ok(Edge::Bottom),
        _ => Err("Choose a host edge.".into()),
    }
}

fn edge_name(value: Edge) -> &'static str {
    match value {
        Edge::Left => "left",
        Edge::Right => "right",
        Edge::Top => "top",
        Edge::Bottom => "bottom",
    }
}

/// Checks manual geometry and portal constraints using the production topology core.
pub fn validate_draft(
    hosts: Vec<HostDraft>,
    portals: Vec<PortalDraft>,
) -> Result<LayoutPreview, String> {
    let monitors = hosts
        .into_iter()
        .map(|draft| {
            Ok(Monitor {
                id: draft.id,
                rect: Rect {
                    x: draft.x,
                    y: draft.y,
                    width: draft.width,
                    height: draft.height,
                },
                dpi_x: draft.dpi_x,
                dpi_y: draft.dpi_y,
                rotation: rotation(&draft.rotation)?,
                primary: draft.primary,
            })
        })
        .collect::<Result<Vec<_>, String>>()?;
    let topology = Topology::new(monitors)
        .map_err(|error| format!("Monitor geometry is invalid: {error:?}"))?;
    let portals = portals
        .into_iter()
        .map(|draft| {
            if draft.dwell_ms > 1000 {
                return Err("Dwell must be between 0 and 1000 ms.".into());
            }
            if draft.direction != "outward" {
                return Err("Standard BLE supports only outward host-to-guest portals.".into());
            }
            if draft.destination_token.len() != 32
                || !draft
                    .destination_token
                    .bytes()
                    .all(|byte| byte.is_ascii_hexdigit())
            {
                return Err("Select a saved guest identity for each portal.".into());
            }
            Ok(Portal::new(
                draft.id,
                draft.monitor_id,
                edge(&draft.edge)?,
                draft.start,
                draft.end,
                draft.destination_token,
            ))
        })
        .collect::<Result<Vec<_>, String>>()?;
    let graph = PortalGraph::new(&topology, portals)
        .map_err(|error| format!("Portal must use a unique exposed edge segment: {error:?}"))?;
    Ok(LayoutPreview {
        segments: topology
            .exposed_edges()
            .into_iter()
            .map(|segment| PreviewSegment {
                monitor_id: segment.monitor_id,
                edge: edge_name(segment.edge),
                start: segment.start,
                end: segment.end,
            })
            .collect(),
        portal_count: graph.portals().len(),
        activation_available: false,
    })
}

/// Runs the native dry-run validator without saving or activating any portal.
#[tauri::command]
pub fn layout_validate_draft(
    hosts: Vec<HostDraft>,
    portals: Vec<PortalDraft>,
) -> Result<LayoutPreview, String> {
    validate_draft(hosts, portals)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn host(id: &str, x: i32) -> HostDraft {
        HostDraft {
            id: id.into(),
            x,
            y: 0,
            width: 1920,
            height: 1080,
            dpi_x: 96,
            dpi_y: 96,
            rotation: "deg0".into(),
            primary: x == 0,
        }
    }

    fn portal(edge: &str, start: i32, end: i32) -> PortalDraft {
        PortalDraft {
            id: "portal-1".into(),
            monitor_id: "host-1".into(),
            edge: edge.into(),
            start,
            end,
            destination_token: "ab".repeat(16),
            dwell_ms: 200,
            direction: "outward".into(),
        }
    }

    #[test]
    fn dry_run_reports_exposed_physical_segments_without_arming() {
        let preview =
            validate_draft(vec![host("host-1", 0)], vec![portal("right", 12, 1068)]).unwrap();
        assert!(
            preview
                .segments
                .iter()
                .any(|segment| segment.monitor_id == "host-1"
                    && segment.edge == "right"
                    && segment.start == 0
                    && segment.end == 1080)
        );
        assert_eq!(preview.portal_count, 1);
        assert!(!preview.activation_available);
    }

    #[test]
    fn shared_seam_and_invalid_dwell_fail_the_dry_run() {
        let mut second = host("host-2", 1920);
        second.primary = false;
        assert!(
            validate_draft(
                vec![host("host-1", 0), second],
                vec![portal("right", 12, 1068)]
            )
            .is_err()
        );
        let mut bad = portal("right", 12, 1068);
        bad.dwell_ms = 1001;
        assert!(validate_draft(vec![host("host-1", 0)], vec![bad]).is_err());
    }

    #[test]
    fn overlapping_portals_and_reverse_direction_are_rejected() {
        let mut overlapping = portal("right", 100, 400);
        overlapping.id = "portal-2".into();
        assert!(
            validate_draft(
                vec![host("host-1", 0)],
                vec![portal("right", 12, 300), overlapping]
            )
            .is_err()
        );
        let mut reverse = portal("right", 12, 1068);
        reverse.direction = "guest_to_host".into();
        assert!(validate_draft(vec![host("host-1", 0)], vec![reverse]).is_err());
    }
}
