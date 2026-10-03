// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Models local layout drafts and authoritative Windows host snapshots for the
// editor. It previews exposed edges; native topology validation owns application.

/** Physical host rectangle entered by the user, never claimed as OS detected. */
export interface HostDisplay { id: string; name: string; x: number; y: number; width: number; height: number; dpiX: number; dpiY: number; rotation: "deg0" | "deg90" | "deg180" | "deg270"; primary: boolean; source: "manual" | "windows" }
/** One Windows-enumerated physical monitor returned by the native layout service. */
export type DetectedHost = Omit<HostDisplay, "name" | "source">;
/** Manual visual placeholder tied to one saved opaque guest identity. */
export interface GuestDisplay { bondToken: string; name: string; x: number; y: number; width: number; height: number; source: "manual" }
/** Exposed half-open physical interval on one host edge. */
export interface EdgeSegment { monitorId: string; edge: "left" | "right" | "top" | "bottom"; start: number; end: number }
/** Directed host-to-guest crossing draft, never an enabled route. */
export interface PortalDraft extends EdgeSegment { id: string; destinationToken: string; dwellMs: number; direction: "outward" }
/** Versioned local draft; geometry remains unarmed until native discovery exists. */
export interface LayoutDraft { version: 1; hosts: HostDisplay[]; guests: GuestDisplay[]; portals: PortalDraft[] }

/** Starts with an explicitly manual host rectangle for editing. */
export function defaultDraft(): LayoutDraft {
  return { version: 1, hosts: [{ id: "host-1", name: "Manual host display 1", x: 0, y: 0, width: 1920, height: 1080, dpiX: 96, dpiY: 96, rotation: "deg0", primary: true, source: "manual" }], guests: [], portals: [] };
}

/** Checks every host attribute that is safety relevant against the current OS snapshot. */
export function matchesDetectedHosts(draft: LayoutDraft, detected: readonly DetectedHost[]): boolean {
  return draft.hosts.length === detected.length && draft.hosts.every((host) => detected.some((item) =>
    item.id === host.id && item.x === host.x && item.y === host.y && item.width === host.width &&
    item.height === host.height && item.dpiX === host.dpiX && item.dpiY === host.dpiY &&
    item.rotation === host.rotation && item.primary === host.primary));
}

/** Copies the authoritative Windows geometry into the local draft for review. */
export function withDetectedHosts(draft: LayoutDraft, detected: readonly DetectedHost[]): LayoutDraft {
  if (detected.length === 0) return draft;
  const unchanged = matchesDetectedHosts(draft, detected);
  return { ...draft, hosts: detected.map((item) => ({ ...item, name: item.id, source: "windows" })),
    portals: unchanged ? draft.portals : [] };
}

/** Restores only a valid versioned local draft; corrupt data starts fresh. */
export function restoreDraft(raw: string | null): LayoutDraft {
  if (!raw) return defaultDraft();
  try {
    const parsed: unknown = JSON.parse(raw);
    if (parsed && typeof parsed === "object" && "hosts" in parsed && "guests" in parsed && "portals" in parsed &&
      Array.isArray(parsed.hosts) && Array.isArray(parsed.guests) && Array.isArray(parsed.portals) && validDraft(parsed as LayoutDraft)) return parsed as LayoutDraft;
  } catch { /* A damaged local draft is discarded without affecting native routing. */ }
  return defaultDraft();
}

/** Keeps visual guests linked only to currently saved profiles. */
export function addGuestPlaceholders(draft: LayoutDraft, profiles: readonly { bondToken: string; name: string }[]): LayoutDraft {
  const guests = profiles.map((profile, index) => {
    const old = draft.guests.find((guest) => guest.bondToken === profile.bondToken);
    return { bondToken: profile.bondToken, name: profile.name, x: old?.x ?? 2100 + index * 340,
      y: old?.y ?? 100, width: old?.width ?? 300, height: old?.height ?? 170, source: "manual" as const };
  });
  return { ...draft, guests, portals: draft.portals.filter((portal) => guests.some((guest) => guest.bondToken === portal.destinationToken)) };
}

/** Moves one display with integer coordinates shared by drag and number fields. */
export function moveDisplay(draft: LayoutDraft, id: string, x: number, y: number): LayoutDraft {
  if (!Number.isSafeInteger(x) || !Number.isSafeInteger(y)) return draft;
  return { ...draft, hosts: draft.hosts.map((host) => host.id === id ? { ...host, x, y } : host),
    guests: draft.guests.map((guest) => guest.bondToken === id ? { ...guest, x, y } : guest) };
}

function subtract(intervals: [number, number][], cut: [number, number]): [number, number][] {
  return intervals.flatMap(([start, end]) => cut[1] <= start || cut[0] >= end ? [[start, end]] :
    [[start, Math.min(end, cut[0])], [Math.max(start, cut[1]), end]].filter(([a, b]) => a < b) as [number, number][]);
}

/** Visual preview of maximal host edges after physically shared seams are removed. */
export function exposedSegments(hosts: readonly HostDisplay[]): EdgeSegment[] {
  const output: EdgeSegment[] = [];
  for (const host of hosts) for (const edge of ["left", "right", "top", "bottom"] as const) {
    const vertical = edge === "left" || edge === "right";
    let intervals: [number, number][] = [[vertical ? host.y : host.x, vertical ? host.y + host.height : host.x + host.width]];
    for (const other of hosts) {
      if (other.id === host.id) continue;
      const shared = edge === "left" ? other.x + other.width === host.x : edge === "right" ? other.x === host.x + host.width : edge === "top" ? other.y + other.height === host.y : other.y === host.y + host.height;
      if (shared) intervals = subtract(intervals, [vertical ? other.y : other.x, vertical ? other.y + other.height : other.x + other.width]);
    }
    for (const [start, end] of intervals) if (start < end) output.push({ monitorId: host.id, edge, start, end });
  }
  return output;
}

/** Creates a directed default portal inset from segment corners for dry-run review. */
export function portalForSegment(segment: EdgeSegment | undefined, destinationToken: string): PortalDraft {
  if (!segment) throw new Error("Choose an exposed host edge segment.");
  const inset = segment.end - segment.start > 24 ? 12 : 0;
  return { ...segment, id: `portal-${segment.monitorId}-${segment.edge}-${segment.start}`, start: segment.start + inset,
    end: segment.end - inset, destinationToken, dwellMs: 200, direction: "outward" };
}

/** Checks draft shape before persistence; native topology core validates activation geometry. */
export function validDraft(draft: LayoutDraft): boolean {
  if (draft.version !== 1 || draft.hosts.length === 0 || draft.hosts.filter((host) => host.primary).length !== 1) return false;
  const validRect = (item: { x: number; y: number; width: number; height: number }) =>
    [item.x, item.y, item.width, item.height].every(Number.isSafeInteger) && item.width > 0 && item.height > 0 &&
    item.x >= -2147483648 && item.y >= -2147483648 && item.x + item.width <= 2147483647 && item.y + item.height <= 2147483647;
  if (draft.hosts.some((host) => !host.id || !validRect(host) || host.dpiX <= 0 || host.dpiY <= 0)) return false;
  if (new Set(draft.hosts.map((host) => host.id)).size !== draft.hosts.length) return false;
  if (draft.hosts.some((host, index) => draft.hosts.slice(0, index).some((other) => host.x < other.x + other.width && other.x < host.x + host.width && host.y < other.y + other.height && other.y < host.y + host.height))) return false;
  if (draft.guests.some((guest) => !validRect(guest) || !/^[0-9a-f]{32}$/.test(guest.bondToken))) return false;
  const exposed = exposedSegments(draft.hosts);
  return draft.portals.every((portal) => portal.direction === "outward" && Number.isInteger(portal.dwellMs) && portal.dwellMs >= 0 && portal.dwellMs <= 1000 && portal.start < portal.end &&
    draft.guests.some((guest) => guest.bondToken === portal.destinationToken) && exposed.some((segment) => segment.monitorId === portal.monitorId && segment.edge === portal.edge && segment.start <= portal.start && portal.end <= segment.end));
}

/** Helper-only capabilities; native crossing availability comes from LayoutStatus. */
export function standardCapabilities(): { canReturnFromGuestEdge: false; canPlaceGuestCursor: false } {
  return { canReturnFromGuestEdge: false, canPlaceGuestCursor: false };
}
