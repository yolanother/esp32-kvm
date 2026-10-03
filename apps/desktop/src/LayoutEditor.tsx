// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Edits local host and guest layout drafts, shows authoritative Windows monitor
// discovery, prepares validated portals, and requests guarded native crossing.
import { invoke } from "@tauri-apps/api/core";
import { useEffect, useRef, useState, type JSX, type PointerEvent, type RefObject } from "react";
import { addGuestPlaceholders, defaultDraft, exposedSegments, matchesDetectedHosts, moveDisplay, portalForSegment, restoreDraft, standardCapabilities, validDraft, withDetectedHosts,
  type DetectedHost, type EdgeSegment, type GuestDisplay, type HostDisplay, type LayoutDraft, type PortalDraft } from "./layout-model";
import type { GuestProfile } from "./setup-model";

const STORAGE_KEY = "esp32-kvm.manual-layout.v1";
const SCALE = 0.16;

interface NativePreview { segments: EdgeSegment[]; portalCount: number; activationAvailable: false }
interface NativeStatus { generation: number | null; hosts: DetectedHost[]; appliedPortals: number; activationAvailable: boolean; enabled: boolean; reason: string }

function loadDraft(): LayoutDraft {
  try { return restoreDraft(window.localStorage.getItem(STORAGE_KEY)); }
  catch { return defaultDraft(); }
}

function segmentKey(segment: EdgeSegment): string { return `${segment.monitorId}|${segment.edge}|${segment.start}|${segment.end}`; }

/** Accessible screen arrangement and explicit, native-gated crossing control. */
export default function LayoutEditor({ profiles, profilesReady, headingRef }: { profiles: GuestProfile[]; profilesReady: boolean; headingRef: RefObject<HTMLHeadingElement | null> }): JSX.Element {
  const [draft, setDraft] = useState<LayoutDraft>(loadDraft);
  const [preview, setPreview] = useState<NativePreview | null>(null);
  const [message, setMessage] = useState("");
  const [busy, setBusy] = useState(false);
  const [nativeStatus, setNativeStatus] = useState<NativeStatus | null>(null);
  const [discoveryError, setDiscoveryError] = useState("");
  const drag = useRef<{ id: string; pointerX: number; pointerY: number; startX: number; startY: number } | null>(null);
  const profileKey = profiles.map((profile) => `${profile.bondToken}:${profile.name}`).join("|");
  useEffect(() => { if (profilesReady) setDraft((current) => addGuestPlaceholders(current, profiles)); }, [profileKey, profilesReady]); // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => {
    let mounted = true;
    async function refresh(): Promise<void> {
      try {
        const status = await invoke<NativeStatus>("layout_discover");
        if (mounted) { setNativeStatus(status); setDiscoveryError(""); }
      } catch (error) {
        if (mounted) { setNativeStatus(null); setDiscoveryError(String(error)); }
      }
    }
    void refresh();
    const timer = window.setInterval(() => { void refresh(); }, 3000);
    return () => { mounted = false; window.clearInterval(timer); };
  }, []);

  function update(next: LayoutDraft): void { setDraft(next); setPreview(null); setMessage(""); }
  const allDisplays = [...draft.hosts, ...draft.guests];
  const minX = Math.min(...allDisplays.map((display) => display.x), 0);
  const minY = Math.min(...allDisplays.map((display) => display.y), 0);
  const maxX = Math.max(...allDisplays.map((display) => display.x + display.width), 1920);
  const maxY = Math.max(...allDisplays.map((display) => display.y + display.height), 1080);
  const segments = exposedSegments(draft.hosts);

  function startDrag(event: PointerEvent<HTMLDivElement>, display: HostDisplay | GuestDisplay, id: string): void {
    if (event.button !== 0) return;
    drag.current = { id, pointerX: event.clientX, pointerY: event.clientY, startX: display.x, startY: display.y };
    event.currentTarget.setPointerCapture(event.pointerId);
  }
  function dragDisplay(event: PointerEvent<HTMLDivElement>): void {
    if (!drag.current) return;
    const current = drag.current;
    update(moveDisplay(draft, current.id, current.startX + Math.round((event.clientX - current.pointerX) / SCALE), current.startY + Math.round((event.clientY - current.pointerY) / SCALE)));
  }

  function changeHost(id: string, patch: Partial<HostDisplay>): void {
    update({ ...draft, hosts: draft.hosts.map((host) => host.id === id ? { ...host, ...patch } : host) });
  }
  function changeGuest(token: string, patch: Partial<GuestDisplay>): void {
    update({ ...draft, guests: draft.guests.map((guest) => guest.bondToken === token ? { ...guest, ...patch } : guest) });
  }
  function addHost(): void {
    const next = Math.max(0, ...draft.hosts.map((host) => Number(host.id.replace("host-", "")) || 0)) + 1;
    const x = Math.max(...draft.hosts.map((host) => host.x + host.width)) + 80;
    update({ ...draft, hosts: [...draft.hosts, { ...defaultDraft().hosts[0], id: `host-${next}`, name: `Manual host display ${next}`, x, primary: false }] });
  }
  function removeHost(id: string): void {
    if (draft.hosts.length <= 1) return;
    const hosts = draft.hosts.filter((host) => host.id !== id);
    if (!hosts.some((host) => host.primary)) hosts[0] = { ...hosts[0], primary: true };
    update({ ...draft, hosts, portals: draft.portals.filter((portal) => portal.monitorId !== id) });
  }
  function changePortal(id: string, patch: Partial<PortalDraft>): void {
    update({ ...draft, portals: draft.portals.map((portal) => portal.id === id ? { ...portal, ...patch } : portal) });
  }
  function addPortal(): void {
    if (!segments[0] || !draft.guests[0]) return;
    const portal = portalForSegment(segments[0], draft.guests[0].bondToken);
    const id = `portal-${Date.now()}-${draft.portals.length}`;
    update({ ...draft, portals: [...draft.portals, { ...portal, id }] });
  }
  async function dryRun(): Promise<void> {
    if (!validDraft(draft) || draft.portals.length === 0) { setMessage("Correct the draft geometry and add a valid portal before dry run."); return; }
    setBusy(true); setPreview(null); setMessage("");
    try {
      const result = await invoke<NativePreview>("layout_validate_draft", { hosts: draft.hosts, portals: draft.portals });
      setPreview(result);
      setMessage(`${result.portalCount} portal${result.portalCount === 1 ? "" : "s"} validated for this draft. Apply against detected Windows monitors before enabling crossing.`);
    } catch (error) { setMessage(`Dry run failed: ${String(error)}`); }
    finally { setBusy(false); }
  }
  async function apply(): Promise<void> {
    if (!nativeStatus || !matchesDetectedHosts(draft, nativeStatus.hosts) || !validDraft(draft)) {
      setMessage("Refresh the detected Windows monitors and correct the portal draft before applying."); return;
    }
    setBusy(true); setMessage("");
    try {
      const status = await invoke<NativeStatus>("layout_apply", { hosts: draft.hosts, portals: draft.portals });
      setNativeStatus(status);
      setMessage(`${status.appliedPortals} portal${status.appliedPortals === 1 ? "" : "s"} prepared for Windows topology generation ${status.generation}. ${status.reason}`);
    } catch (error) { setMessage(`Apply failed: ${String(error)}`); }
    finally { setBusy(false); }
  }
  async function toggleCrossing(): Promise<void> {
    if (!nativeStatus) return;
    setBusy(true); setMessage("");
    try {
      const status = await invoke<NativeStatus>("layout_set_enabled", { enabled: !nativeStatus.enabled });
      setNativeStatus(status);
      setMessage(status.reason);
    } catch (error) { setMessage(`Crossing request failed: ${String(error)}`); }
    finally { setBusy(false); }
  }
  function save(): void {
    try { window.localStorage.setItem(STORAGE_KEY, JSON.stringify(draft)); setMessage("Layout draft saved on this computer. No crossing is enabled."); }
    catch (error) { setMessage(`Draft could not be saved: ${String(error)}`); }
  }

  return <section aria-labelledby="layout-title" className="layout-editor">
    <header className="page-intro"><div><p className="eyebrow">SCREEN ARRANGEMENT</p><h1 id="layout-title" ref={headingRef} tabIndex={-1}>Screen layout</h1><p>Review detected Windows monitors, apply host edge portals, and explicitly enable guarded crossing.</p></div></header>
    <div className="notice notice--warning" role="status"><strong>{nativeStatus ? `Windows monitors detected · generation ${nativeStatus.generation}` : "Windows monitor discovery unavailable"}</strong><span>{nativeStatus ? `${nativeStatus.hosts.length} physical host display${nativeStatus.hosts.length === 1 ? "" : "s"}. ${nativeStatus.appliedPortals} portals prepared. ${nativeStatus.reason}` : discoveryError || "Checking the current Windows display arrangement."}</span></div>
    <div className="notice" role="note"><strong>Layout draft</strong><span>{nativeStatus && matchesDetectedHosts(draft, nativeStatus.hosts) ? "Host geometry matches the current Windows snapshot." : "Host rectangles are local draft values. Copy detected Windows monitors before applying portals."} Guest rectangles are named placeholders, not remotely discovered screens.</span><button type="button" disabled={!nativeStatus || busy} onClick={() => { if (nativeStatus) update(withDetectedHosts(draft, nativeStatus.hosts)); }}>Use detected host monitors</button></div>
    <div className="layout-stage card" aria-label="Draggable manual display arrangement" style={{ minWidth: Math.max(640, (maxX - minX) * SCALE + 80), minHeight: Math.max(300, (maxY - minY) * SCALE + 80) }}>
      {draft.hosts.map((host) => <div key={host.id} className={`layout-tile layout-tile--host${host.primary ? " layout-tile--primary" : ""}`} style={{ left: (host.x - minX) * SCALE + 24, top: (host.y - minY) * SCALE + 24, width: Math.max(90, host.width * SCALE), height: Math.max(60, host.height * SCALE) }} tabIndex={0} role="button" aria-label={`${host.name}, draft host display, x ${host.x}, y ${host.y}. Arrow keys move by ten pixels.`} onPointerDown={(event) => startDrag(event, host, host.id)} onPointerMove={dragDisplay} onPointerUp={() => { drag.current = null; }} onKeyDown={(event) => { const delta = { ArrowLeft: [-10, 0], ArrowRight: [10, 0], ArrowUp: [0, -10], ArrowDown: [0, 10] }[event.key]; if (delta) { event.preventDefault(); update(moveDisplay(draft, host.id, host.x + delta[0], host.y + delta[1])); } }}><strong>{host.name}</strong><small>{host.width} × {host.height} · manual</small>{preview && draft.portals.filter((portal) => portal.monitorId === host.id).map((portal) => { const vertical = portal.edge === "left" || portal.edge === "right"; const origin = vertical ? host.y : host.x; const extent = vertical ? host.height : host.width; return <span key={portal.id} className={`layout-portal-mark layout-portal-mark--${portal.edge}`} style={vertical ? { top: `${(portal.start - origin) / extent * 100}%`, height: `${(portal.end - portal.start) / extent * 100}%` } : { left: `${(portal.start - origin) / extent * 100}%`, width: `${(portal.end - portal.start) / extent * 100}%` }} aria-label={`Validated ${portal.edge} edge portal`} />; })}</div>)}
      {draft.guests.map((guest) => <div key={guest.bondToken} className="layout-tile layout-tile--guest" style={{ left: (guest.x - minX) * SCALE + 24, top: (guest.y - minY) * SCALE + 24, width: Math.max(90, guest.width * SCALE), height: Math.max(60, guest.height * SCALE) }} tabIndex={0} role="button" aria-label={`${guest.name}, manual guest placeholder, x ${guest.x}, y ${guest.y}. Arrow keys move by ten pixels.`} onPointerDown={(event) => startDrag(event, guest, guest.bondToken)} onPointerMove={dragDisplay} onPointerUp={() => { drag.current = null; }} onKeyDown={(event) => { const delta = { ArrowLeft: [-10, 0], ArrowRight: [10, 0], ArrowUp: [0, -10], ArrowDown: [0, 10] }[event.key]; if (delta) { event.preventDefault(); update(moveDisplay(draft, guest.bondToken, guest.x + delta[0], guest.y + delta[1])); } }}><strong>{guest.name}</strong><small>Manual guest placeholder</small></div>)}
    </div>
    <div className="layout-columns">
      <div className="card"><div className="card-heading"><h2>Host rectangles</h2><button type="button" onClick={addHost}>Add manual display</button></div>{draft.hosts.map((host) => <fieldset key={host.id}><legend>{host.name}{host.primary ? " · primary" : ""}</legend><div className="layout-fields">{(["x", "y", "width", "height"] as const).map((field) => <label key={field}>{field.toUpperCase()} <input type="number" value={host[field]} onChange={(event) => changeHost(host.id, { [field]: Number(event.target.value) })} /></label>)}<label>DPI X<input type="number" min="1" value={host.dpiX} onChange={(event) => changeHost(host.id, { dpiX: Number(event.target.value) })} /></label><label>DPI Y<input type="number" min="1" value={host.dpiY} onChange={(event) => changeHost(host.id, { dpiY: Number(event.target.value) })} /></label><label>Rotation<select value={host.rotation} onChange={(event) => changeHost(host.id, { rotation: event.target.value as HostDisplay["rotation"] })}>{["deg0", "deg90", "deg180", "deg270"].map((value) => <option key={value} value={value}>{value}</option>)}</select></label></div><div className="card-tail"><button type="button" onClick={() => update({ ...draft, hosts: draft.hosts.map((item) => ({ ...item, primary: item.id === host.id })) })}>Set primary</button><button type="button" disabled={draft.hosts.length === 1} onClick={() => removeHost(host.id)}>Remove</button></div></fieldset>)}</div>
      <div className="card"><h2>Guest placeholders</h2>{draft.guests.length === 0 && <p>Save a guest profile to add a named placeholder.</p>}{draft.guests.map((guest) => <fieldset key={guest.bondToken}><legend>{guest.name} · manual</legend><div className="layout-fields">{(["x", "y", "width", "height"] as const).map((field) => <label key={field}>{field.toUpperCase()} <input type="number" value={guest[field]} onChange={(event) => changeGuest(guest.bondToken, { [field]: Number(event.target.value) })} /></label>)}</div></fieldset>)}</div>
    </div>
    <div className="card"><div className="card-heading"><h2>Directed host edge portals</h2><button type="button" onClick={addPortal} disabled={!segments.length || !draft.guests.length || !profilesReady}>Add portal</button></div>{draft.portals.length === 0 && <p>Select a saved guest and an exposed host edge to draft a portal.</p>}{draft.portals.map((portal) => { const source = segments.find((segment) => segment.monitorId === portal.monitorId && segment.edge === portal.edge && segment.start <= portal.start && portal.end <= segment.end); return <fieldset key={portal.id}><legend>Host → {draft.guests.find((guest) => guest.bondToken === portal.destinationToken)?.name ?? "Saved guest"}</legend><div className="layout-fields"><label>Exposed host segment<select value={source ? segmentKey(source) : ""} onChange={(event) => { const selected = segments.find((segment) => segmentKey(segment) === event.target.value); if (selected) changePortal(portal.id, { ...portalForSegment(selected, portal.destinationToken), id: portal.id }); }}>{!source && <option value="">Previously selected edge is no longer exposed</option>}{segments.map((segment) => <option key={segmentKey(segment)} value={segmentKey(segment)}>{segment.monitorId} {segment.edge} [{segment.start}, {segment.end})</option>)}</select></label><label>Guest destination<select value={portal.destinationToken} onChange={(event) => changePortal(portal.id, { destinationToken: event.target.value })}>{draft.guests.map((guest) => <option key={guest.bondToken} value={guest.bondToken}>{guest.name}</option>)}</select></label><label>Start pixel<input type="number" value={portal.start} onChange={(event) => changePortal(portal.id, { start: Number(event.target.value) })} /></label><label>End pixel, exclusive<input type="number" value={portal.end} onChange={(event) => changePortal(portal.id, { end: Number(event.target.value) })} /></label><label>Dwell, ms<input type="number" min="0" max="1000" value={portal.dwellMs} onChange={(event) => changePortal(portal.id, { dwellMs: Number(event.target.value) })} /></label><label>Direction<select value="outward" disabled><option value="outward">Host → guest only</option></select></label></div><button type="button" onClick={() => update({ ...draft, portals: draft.portals.filter((item) => item.id !== portal.id) })}>Remove portal</button></fieldset>; })}</div>
    <div className="layout-actions"><button type="button" onClick={save}>Save layout draft</button><button type="button" onClick={() => { void dryRun(); }} disabled={busy || draft.portals.length === 0}>Dry-run and highlight</button><button type="button" onClick={() => { void apply(); }} disabled={busy || !nativeStatus || !matchesDetectedHosts(draft, nativeStatus.hosts) || draft.portals.length === 0}>Apply validated portals</button><button type="button" onClick={() => { void toggleCrossing(); }} disabled={busy || !nativeStatus || (!nativeStatus.enabled && !nativeStatus.activationAvailable)} title={nativeStatus?.reason || discoveryError || "Windows monitor discovery is pending"}>{nativeStatus?.enabled ? "Disable crossing" : "Enable crossing"}</button></div>
    {message && <div role="status" className={`notice${message.includes("failed") || message.includes("Correct") ? " notice--warning" : ""}`}>{message}</div>}
    <div className="layout-columns"><div className="card"><h2>Standard BLE mode</h2><p>Crossing is one way from a Windows host edge to a ready guest. The guest keeps its own last cursor position; this draft cannot preview or move that cursor.</p><p>Return with the configured host shortcut when native shortcuts are active. The proposed default is Ctrl+Alt+F10.</p></div><div className="card"><h2>Optional guest helper</h2><p>Guest-edge return and exact cursor placement require a trusted helper on the guest. No helper is connected or verified here.</p><button type="button" disabled={!standardCapabilities().canReturnFromGuestEdge}>Guest-edge return unavailable</button><button type="button" disabled={!standardCapabilities().canPlaceGuestCursor}>Place guest cursor unavailable</button></div></div>
  </section>;
}
