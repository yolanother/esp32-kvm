// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Models physical-pixel Windows monitor geometry and derives only exposed edge segments.
// Validates directed host-to-guest portals against a generation of that topology.

#![forbid(unsafe_code)]

/// Half-open physical-pixel rectangle in the Windows virtual desktop.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Rect {
    /// Left physical pixel, possibly negative.
    pub x: i32,
    /// Top physical pixel, possibly negative.
    pub y: i32,
    /// Positive physical width.
    pub width: u32,
    /// Positive physical height.
    pub height: u32,
}

impl Rect {
    fn valid(self) -> bool {
        self.width > 0
            && self.height > 0
            && i64::from(self.x) + i64::from(self.width) <= i64::from(i32::MAX)
            && i64::from(self.y) + i64::from(self.height) <= i64::from(i32::MAX)
    }
    fn right(self) -> i32 {
        (i64::from(self.x) + i64::from(self.width)) as i32
    }
    fn bottom(self) -> i32 {
        (i64::from(self.y) + i64::from(self.height)) as i32
    }
    fn overlaps(self, other: Self) -> bool {
        self.x < other.right()
            && other.x < self.right()
            && self.y < other.bottom()
            && other.y < self.bottom()
    }
}

/// Rotation of panel-native coordinates into its physical desktop rectangle.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Rotation {
    /// No rotation.
    Deg0,
    /// 90 degrees clockwise.
    Deg90,
    /// 180 degrees.
    Deg180,
    /// 270 degrees clockwise.
    Deg270,
}

/// A point in monitor-native, unrotated device-independent pixels.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct LogicalPoint {
    /// Horizontal logical coordinate relative to the monitor origin.
    pub x: f64,
    /// Vertical logical coordinate relative to the monitor origin.
    pub y: f64,
}

/// A pixel in global Windows virtual-desktop coordinates.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PhysicalPoint {
    /// Global horizontal physical coordinate.
    pub x: i32,
    /// Global vertical physical coordinate.
    pub y: i32,
}

/// One host monitor reported by the OS, using a stable adapter/display ID.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Monitor {
    /// Stable monitor identity from the host OS, never an array index.
    pub id: String,
    /// Final physical desktop bounds after rotation.
    pub rect: Rect,
    /// Horizontal DPI of panel-native logical coordinates.
    pub dpi_x: u32,
    /// Vertical DPI of panel-native logical coordinates.
    pub dpi_y: u32,
    /// Native-to-desktop orientation.
    pub rotation: Rotation,
    /// Whether this is the current Windows primary monitor.
    pub primary: bool,
}

impl Monitor {
    /// Converts a panel-native logical point to a global physical pixel.
    /// Fractional scaled pixels use floor; out-of-bounds or non-finite values are rejected.
    pub fn logical_to_physical(&self, point: LogicalPoint) -> Option<PhysicalPoint> {
        if !point.x.is_finite()
            || !point.y.is_finite()
            || point.x < 0.0
            || point.y < 0.0
            || self.dpi_x == 0
            || self.dpi_y == 0
            || !self.rect.valid()
        {
            return None;
        }
        let sx = (point.x * f64::from(self.dpi_x) / 96.0).floor();
        let sy = (point.y * f64::from(self.dpi_y) / 96.0).floor();
        let (native_w, native_h) = match self.rotation {
            Rotation::Deg0 | Rotation::Deg180 => (self.rect.width, self.rect.height),
            Rotation::Deg90 | Rotation::Deg270 => (self.rect.height, self.rect.width),
        };
        if sx >= f64::from(native_w) || sy >= f64::from(native_h) {
            return None;
        }
        let (sx, sy) = (sx as i64, sy as i64);
        let (w, h) = (i64::from(self.rect.width), i64::from(self.rect.height));
        let (x, y) = match self.rotation {
            Rotation::Deg0 => (sx, sy),
            Rotation::Deg90 => (w - 1 - sy, sx),
            Rotation::Deg180 => (w - 1 - sx, h - 1 - sy),
            Rotation::Deg270 => (sy, h - 1 - sx),
        };
        Some(PhysicalPoint {
            x: (i64::from(self.rect.x) + x) as i32,
            y: (i64::from(self.rect.y) + y) as i32,
        })
    }

    /// Converts a physical pixel on this monitor to panel-native logical position.
    pub fn physical_to_logical(&self, point: PhysicalPoint) -> Option<LogicalPoint> {
        if self.dpi_x == 0
            || self.dpi_y == 0
            || !self.rect.valid()
            || point.x < self.rect.x
            || point.x >= self.rect.right()
            || point.y < self.rect.y
            || point.y >= self.rect.bottom()
        {
            return None;
        }
        let (x, y) = (
            i64::from(point.x) - i64::from(self.rect.x),
            i64::from(point.y) - i64::from(self.rect.y),
        );
        let (w, h) = (i64::from(self.rect.width), i64::from(self.rect.height));
        let (sx, sy) = match self.rotation {
            Rotation::Deg0 => (x, y),
            Rotation::Deg90 => (y, w - 1 - x),
            Rotation::Deg180 => (w - 1 - x, h - 1 - y),
            Rotation::Deg270 => (h - 1 - y, x),
        };
        Some(LogicalPoint {
            x: sx as f64 * 96.0 / f64::from(self.dpi_x),
            y: sy as f64 * 96.0 / f64::from(self.dpi_y),
        })
    }
}

/// Side of a physical host monitor rectangle.
#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
pub enum Edge {
    Left,
    Right,
    Top,
    Bottom,
}

/// Half-open coordinate interval on one monitor edge.
#[derive(Clone, Debug, Eq, Ord, PartialEq, PartialOrd)]
pub struct EdgeSegment {
    /// Stable source monitor ID.
    pub monitor_id: String,
    /// Source edge.
    pub edge: Edge,
    /// Inclusive physical coordinate along the edge.
    pub start: i32,
    /// Exclusive physical coordinate along the edge.
    pub end: i32,
}

impl EdgeSegment {
    /// Constructs an edge interval with a stable monitor identity.
    pub fn new(monitor_id: impl Into<String>, edge: Edge, start: i32, end: i32) -> Self {
        Self {
            monitor_id: monitor_id.into(),
            edge,
            start,
            end,
        }
    }
}

/// Invalid monitor data; the live topology remains intact on replacement failure.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TopologyError {
    /// A monitor ID is blank or reused.
    DuplicateOrEmptyId,
    /// Bounds are empty or exceed representable physical desktop coordinates.
    InvalidRect,
    /// DPI must be positive on both axes.
    InvalidDpi,
    /// Exactly one monitor must be primary.
    InvalidPrimary,
    /// Distinct monitor rectangles overlap in physical pixels.
    OverlappingMonitors,
    /// The topology generation cannot advance further.
    GenerationExhausted,
}

/// Validated physical monitor arrangement with a monotonic change generation.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Topology {
    monitors: Vec<Monitor>,
    generation: u64,
}

impl Topology {
    /// Validates and canonicalizes the initial monitor snapshot.
    pub fn new(monitors: Vec<Monitor>) -> Result<Self, TopologyError> {
        Ok(Self {
            monitors: validate_monitors(monitors)?,
            generation: 1,
        })
    }

    /// Returns sorted monitor records; array position is not an identity.
    pub fn monitors(&self) -> &[Monitor] {
        &self.monitors
    }

    /// Returns the generation used to invalidate portal geometry after OS changes.
    pub fn generation(&self) -> u64 {
        self.generation
    }

    /// Atomically replaces the OS snapshot; returns whether any identity, geometry,
    /// DPI, orientation, or primary-display attribute changed.
    pub fn replace(&mut self, monitors: Vec<Monitor>) -> Result<bool, TopologyError> {
        let next = validate_monitors(monitors)?;
        if next == self.monitors {
            return Ok(false);
        }
        let generation = self
            .generation
            .checked_add(1)
            .ok_or(TopologyError::GenerationExhausted)?;
        self.monitors = next;
        self.generation = generation;
        Ok(true)
    }

    /// Returns maximal exposed segments, subtracting only physically shared seams.
    pub fn exposed_edges(&self) -> Vec<EdgeSegment> {
        let mut output = Vec::new();
        for monitor in &self.monitors {
            for edge in [Edge::Left, Edge::Right, Edge::Top, Edge::Bottom] {
                let (start, end) = match edge {
                    Edge::Left | Edge::Right => (monitor.rect.y, monitor.rect.bottom()),
                    Edge::Top | Edge::Bottom => (monitor.rect.x, monitor.rect.right()),
                };
                let mut remaining = vec![(start, end)];
                for other in &self.monitors {
                    if other.id == monitor.id {
                        continue;
                    }
                    let covering = match edge {
                        Edge::Left if other.rect.right() == monitor.rect.x => {
                            Some((other.rect.y, other.rect.bottom()))
                        }
                        Edge::Right if other.rect.x == monitor.rect.right() => {
                            Some((other.rect.y, other.rect.bottom()))
                        }
                        Edge::Top if other.rect.bottom() == monitor.rect.y => {
                            Some((other.rect.x, other.rect.right()))
                        }
                        Edge::Bottom if other.rect.y == monitor.rect.bottom() => {
                            Some((other.rect.x, other.rect.right()))
                        }
                        _ => None,
                    };
                    if let Some((cover_start, cover_end)) = covering {
                        remaining = remaining
                            .into_iter()
                            .flat_map(|(a, b)| {
                                let mut pieces = Vec::with_capacity(2);
                                if a < cover_start {
                                    pieces.push((a, b.min(cover_start)));
                                }
                                if cover_end < b {
                                    pieces.push((a.max(cover_end), b));
                                }
                                if cover_end <= a || cover_start >= b {
                                    pieces = vec![(a, b)];
                                }
                                pieces.into_iter().filter(|(a, b)| a < b)
                            })
                            .collect();
                    }
                }
                output.extend(
                    remaining
                        .into_iter()
                        .map(|(a, b)| EdgeSegment::new(&monitor.id, edge, a, b)),
                );
            }
        }
        output.sort();
        output
    }
}

fn validate_monitors(mut monitors: Vec<Monitor>) -> Result<Vec<Monitor>, TopologyError> {
    monitors.sort_by(|a, b| a.id.cmp(&b.id));
    if monitors.iter().any(|m| m.id.trim().is_empty())
        || monitors.windows(2).any(|pair| pair[0].id == pair[1].id)
    {
        return Err(TopologyError::DuplicateOrEmptyId);
    }
    if monitors.iter().any(|m| !m.rect.valid()) {
        return Err(TopologyError::InvalidRect);
    }
    if monitors.iter().any(|m| m.dpi_x == 0 || m.dpi_y == 0) {
        return Err(TopologyError::InvalidDpi);
    }
    if monitors.iter().filter(|m| m.primary).count() != 1 {
        return Err(TopologyError::InvalidPrimary);
    }
    for (index, monitor) in monitors.iter().enumerate() {
        if monitors[..index]
            .iter()
            .any(|prior| prior.rect.overlaps(monitor.rect))
        {
            return Err(TopologyError::OverlappingMonitors);
        }
    }
    Ok(monitors)
}

/// Directed host edge to persistent guest destination.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Portal {
    /// Stable portal identity.
    pub id: String,
    /// Physical source interval.
    pub source: EdgeSegment,
    /// Persistent guest identity, never a live slot index.
    pub destination_guest_id: String,
}

impl Portal {
    /// Constructs one directed portal on a physical source interval.
    pub fn new(
        id: impl Into<String>,
        monitor_id: impl Into<String>,
        edge: Edge,
        start: i32,
        end: i32,
        destination_guest_id: impl Into<String>,
    ) -> Self {
        Self {
            id: id.into(),
            source: EdgeSegment::new(monitor_id, edge, start, end),
            destination_guest_id: destination_guest_id.into(),
        }
    }
}

/// Invalid or stale portal binding.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PortalError {
    /// A portal ID or destination identity is blank or a portal ID repeats.
    InvalidIdentity,
    /// A physical portal interval is empty or reversed.
    InvalidInterval,
    /// The portal is not completely on one exposed source segment.
    NotExposed,
    /// Two directed portals claim overlapping source pixels.
    Overlap,
}

/// Validated directed portals bound to one topology generation.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PortalGraph {
    portals: Vec<Portal>,
    topology_generation: u64,
    validated_monitors: Vec<Monitor>,
}

impl PortalGraph {
    /// Validates directed portals before they can activate guest selection.
    pub fn new(topology: &Topology, portals: Vec<Portal>) -> Result<Self, PortalError> {
        validate_portals(topology, &portals)?;
        Ok(Self {
            portals,
            topology_generation: topology.generation,
            validated_monitors: topology.monitors.clone(),
        })
    }

    /// Returns the configured directed portals.
    pub fn portals(&self) -> &[Portal] {
        &self.portals
    }

    /// Whether the graph was checked against the current monitor snapshot.
    pub fn is_current(&self, topology: &Topology) -> bool {
        self.topology_generation == topology.generation
            && self.validated_monitors == topology.monitors
    }

    /// Finds the unique portal covering one physical coordinate on an exposed edge.
    /// Stale graphs never return a destination.
    pub fn at_edge(
        &self,
        topology: &Topology,
        monitor_id: &str,
        edge: Edge,
        coordinate: i32,
    ) -> Option<&Portal> {
        if !self.is_current(topology) {
            return None;
        }
        self.portals.iter().find(|portal| {
            portal.source.monitor_id == monitor_id
                && portal.source.edge == edge
                && portal.source.start <= coordinate
                && coordinate < portal.source.end
        })
    }

    /// Revalidates after any hotplug, DPI, rotation, or geometry change.
    /// A failed validation leaves the graph stale and prevents activation.
    pub fn revalidate(&mut self, topology: &Topology) -> Result<(), PortalError> {
        validate_portals(topology, &self.portals)?;
        self.topology_generation = topology.generation;
        self.validated_monitors = topology.monitors.clone();
        Ok(())
    }
}

fn validate_portals(topology: &Topology, portals: &[Portal]) -> Result<(), PortalError> {
    let exposed = topology.exposed_edges();
    for (index, portal) in portals.iter().enumerate() {
        if portal.id.trim().is_empty()
            || portal.destination_guest_id.trim().is_empty()
            || portals[..index].iter().any(|prior| prior.id == portal.id)
        {
            return Err(PortalError::InvalidIdentity);
        }
        if portal.source.start >= portal.source.end {
            return Err(PortalError::InvalidInterval);
        }
        if !exposed.iter().any(|segment| {
            segment.monitor_id == portal.source.monitor_id
                && segment.edge == portal.source.edge
                && segment.start <= portal.source.start
                && portal.source.end <= segment.end
        }) {
            return Err(PortalError::NotExposed);
        }
        if portals[..index].iter().any(|prior| {
            prior.source.monitor_id == portal.source.monitor_id
                && prior.source.edge == portal.source.edge
                && prior.source.start < portal.source.end
                && portal.source.start < prior.source.end
        }) {
            return Err(PortalError::Overlap);
        }
    }
    Ok(())
}
