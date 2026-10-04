// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Renders five desktop destinations, actor-confirmed route status, a switch
// overlay, setup, and the opt-in guest idle bump control. Native status owns
// device truth; the webview never routes input.
import { useEffect, useRef, useState, type KeyboardEvent, type JSX } from "react";
import { destinations, moveSelection, type Destination } from "./navigation";
import { listen } from "@tauri-apps/api/event";
import { exampleGuests } from "./preview-fixtures";
import SetupWizard from "./SetupWizard";
import ProfileEditor from "./ProfileEditor";
import MappingPresets from "./MappingPresets";
import LayoutEditor from "./LayoutEditor";
import { mappingLabel } from "./mapping-presets";
import { keepAwakeEnabled, returnToHost, selectGuest, setKeepAwake, setupSnapshot, unavailableSnapshot } from "./setup-api";
import { type SetupSnapshot } from "./setup-model";
import { describeRoute, overlayForTransition, systemChoices, type SwitchAnnouncement } from "./dashboard-model";

/** A readable text badge whose label also carries its meaning. */
function StatusPill({ children, tone = "neutral" }: { children: string; tone?: "neutral" | "warning" | "accent" }): JSX.Element {
  return <span className={`status-pill status-pill--${tone}`}>{children}</span>;
}

/** Display a physical shortcut recognized by the native capture worker. */
function KeyChord({ keys }: { keys: readonly string[] }): JSX.Element {
  return <span className="key-chord" aria-label={keys.join(" plus ")}>{keys.map((key, index) => <kbd key={`${key}-${index}`}>{key}</kbd>)}</span>;
}

/** Explain why illustrative content cannot control hardware. */
function PreviewNotice(): JSX.Element {
  return <div className="notice notice--preview" role="note"><strong>Design preview</strong><span>Names and settings below are examples. They are not connected devices or saved settings.</span></div>;
}

/** Heading and explanatory copy shared by each destination. */
function PageIntro({ title, description, headingRef }: { title: string; description: string; headingRef: React.RefObject<HTMLHeadingElement | null> }): JSX.Element {
  return <header className="page-intro"><h1 ref={headingRef} tabIndex={-1}>{title}</h1><p>{description}</p></header>;
}

/** Systems destination with truthful local state and optional sample guest cards. */
function SystemsPage({ preview, headingRef, snapshot, onAddGuest, onReturn, onEdit, onSelect, keepAwake, keepAwakePending, onKeepAwake }: { preview: boolean; headingRef: React.RefObject<HTMLHeadingElement | null>; snapshot: SetupSnapshot; onAddGuest: () => void; onReturn: () => void; onEdit: (bondToken: string) => void; onSelect: (bondToken: string) => void; keepAwake: boolean; keepAwakePending: boolean; onKeepAwake: (enabled: boolean) => void }): JSX.Element {
  const route = describeRoute(snapshot);
  const choices = systemChoices(snapshot);
  return <>
    <PageIntro title="Systems" description="Choose where your keyboard and mouse go after setup." headingRef={headingRef} />
    <div className="toolbar"><StatusPill tone={route.tone}>{snapshot.route.kind === "guest" ? "Guest controlling" : snapshot.route.kind === "switching" ? "Switch pending" : "Local control"}</StatusPill><span className="muted">{snapshot.device.kind === "verified" ? `${snapshot.readyTokens.length} guest${snapshot.readyTokens.length === 1 ? "" : "s"} ready of ${snapshot.device.maxConnections} live slot${snapshot.device.maxConnections === 1 ? "" : "s"}` : "Device not verified"}</span><button type="button" onClick={onAddGuest}>Add guest</button></div>
    <section className="card route-card" aria-label="Active input destination"><span className="eyebrow">ACTIVE TARGET</span><h2>{route.title}</h2><p>{route.detail}</p><div className="setup-actions"><button type="button" onClick={onReturn} disabled={snapshot.route.kind === "local" || snapshot.route.kind === "failed"}>Return to this computer</button></div></section>
    <div className="card-grid">
      <article className={`card${snapshot.route.kind === "local" ? " card--local" : ""}`}><div className="card-heading"><h2>This computer</h2><StatusPill tone={snapshot.route.kind === "local" ? "accent" : "neutral"}>{snapshot.route.kind === "local" ? "Controlling" : "Local host"}</StatusPill></div><p>Windows host keyboard and mouse.</p><div className="card-tail"><span className="small muted">Return target</span><span className="small muted">Actor handles local return</span></div></article>
      {preview ? exampleGuests.map((guest) => <article className="card" key={guest.name}><div className="card-heading"><h2>{guest.name}</h2><StatusPill>Example only</StatusPill></div><p>{guest.os} · {guest.profile}</p><div className="card-tail"><KeyChord keys={guest.shortcut} /><span className="small muted">Selection unavailable</span></div></article>) : choices.length ? choices.map((choice) => { const guest = snapshot.profiles.find((item) => item.bondToken === choice.bondToken)!; return <article className={`card${choice.state === "Controlling" ? " card--local" : ""}`} key={choice.bondToken}><div className="card-heading"><h2>{choice.name}</h2><StatusPill tone={choice.state === "Offline" || choice.state === "Connected" ? "warning" : "accent"}>{choice.state}</StatusPill></div><p>{guest.os} · {mappingLabel(guest.profile)}</p><div className="card-tail"><button type="button" onClick={() => onEdit(choice.bondToken)}>Edit profile</button><button type="button" disabled={!choice.selectEnabled} onClick={() => onSelect(choice.bondToken)}>Select guest</button></div></article>; }) : <article className="card card--empty"><h2>No guest profiles yet</h2><p>Connect the device and pair a guest to add a saved profile.</p><StatusPill tone="warning">No saved guest</StatusPill></article>}
    </div>
    {preview && <PreviewNotice />}
    <div className="card"><label className="preview-toggle"><input type="checkbox" checked={keepAwake} disabled={keepAwakePending} onChange={(event) => onKeepAwake(event.target.checked)} />Keep guest awake with a mouse bump</label><p className="small muted">While a guest is controlling, send a cancelling pair of tiny pointer moves every 30 seconds when all physical keys and buttons are released. This setting resets when the app exits.</p></div>
    <div className="notice"><strong>Stay in control</strong><span>Select a ready guest after releasing every key and mouse button. The device must confirm the route before input moves.</span></div>
  </>;
}

/** Shortcuts destination describes the installed physical return controls. */
function ShortcutsPage({ headingRef }: { headingRef: React.RefObject<HTMLHeadingElement | null> }): JSX.Element {
  return <>
    <PageIntro title="Shortcuts" description="Use a physical keyboard shortcut to return input to this computer." headingRef={headingRef} />
    <div className="card table-card"><h2>Return to this computer</h2><div className="table-scroll"><table><thead><tr><th scope="col">Action</th><th scope="col">Physical key combination</th></tr></thead><tbody><tr><td>Return to Windows</td><td><KeyChord keys={["Ctrl", "Alt", "F10"]} /></td></tr></tbody></table></div></div>
    <div className="card emergency-card"><h2>Emergency return</h2><p>Hold both physical Ctrl keys for one second. The capture worker releases Windows input immediately, then asks the device to return to local control.</p></div>
  </>;
}

/** Device destination reports verified session and actor route facts. */
function DevicePage({ headingRef, snapshot, onSetup }: { headingRef: React.RefObject<HTMLHeadingElement | null>; snapshot: SetupSnapshot; onSetup: () => void }): JSX.Element {
  return <>
    <PageIntro title="Device" description="Connection health, firmware and diagnostics will appear here." headingRef={headingRef} />
    <div className="card card-heading"><div><h2>ESP32-S3 input bridge</h2><p>{snapshot.device.kind === "verified" ? `Firmware reports ${snapshot.device.boardId}.` : "Attached variant and revision are unverified."}</p></div><StatusPill tone={snapshot.device.kind === "verified" ? "accent" : "warning"}>{snapshot.device.kind === "verified" ? "Verified" : "Not verified"}</StatusPill></div>
    <div className="card-grid device-grid"><section className="card"><h2>Connection</h2><dl className="detail-list"><div><dt>USB session</dt><dd>{snapshot.device.kind === "verified" ? "Verified" : snapshot.device.kind === "candidate" ? "Candidate only" : "Unavailable"}</dd></div><div><dt>BLE guests ready</dt><dd>{snapshot.device.kind === "verified" ? snapshot.readyTokens.length : "Unavailable"}</dd></div><div><dt>Input destination</dt><dd>{describeRoute(snapshot).title}</dd></div><div><dt>Guest latency</dt><dd>Not measured</dd></div></dl></section><section className="card"><h2>Firmware</h2><p>Board identity and capabilities require a device handshake.</p><dl className="detail-list"><div><dt>Installed version</dt><dd>{snapshot.device.kind === "verified" ? snapshot.device.firmwareVersion ?? "Unavailable" : "Unknown"}</dd></div><div><dt>Protocol</dt><dd>{snapshot.device.kind === "verified" ? "Verified" : "Unverified"}</dd></div></dl><button type="button" onClick={onSetup}>Open setup</button></section></div>
    <div className="notice"><strong>Diagnostics</strong><span>Connection events and aggregate counters will be available later. Typed keys, pairing codes and bond secrets must never appear in exports.</span></div>
  </>;
}

/** Provide persistent navigation and truthful status around the five destinations. */
export default function App(): JSX.Element {
  const [page, setPage] = useState<Destination>("systems");
  const [preview, setPreview] = useState(false);
  const [setupOpen, setSetupOpen] = useState(false);
  const [editingToken, setEditingToken] = useState<string | null>(null);
  const [snapshot, setSnapshot] = useState<SetupSnapshot>(unavailableSnapshot());
  const [snapshotLoaded, setSnapshotLoaded] = useState(false);
  const [keepAwake, setKeepAwakeState] = useState(false);
  const [keepAwakePending, setKeepAwakePending] = useState(false);
  const [announcement, setAnnouncement] = useState<SwitchAnnouncement | null>(null);
  const lastSnapshot = useRef<SetupSnapshot | null>(null);
  const headingRef = useRef<HTMLHeadingElement>(null);
  const focusHeading = useRef(false);
  const navRefs = useRef<Record<Destination, HTMLButtonElement | null>>({ systems: null, layout: null, mappings: null, shortcuts: null, device: null });

  useEffect(() => {
    if (focusHeading.current) headingRef.current?.focus();
    focusHeading.current = false;
  }, [page]);

  useEffect(() => {
    let active = true;
    void keepAwakeEnabled().then((enabled) => { if (active) setKeepAwakeState(enabled); }).catch(() => {});
    return () => { active = false; };
  }, []);

  useEffect(() => {
    let active = true;
    async function refresh(): Promise<void> {
      const next = await setupSnapshot();
      if (!active) return;
      const change = overlayForTransition(lastSnapshot.current, next);
      if (change) setAnnouncement(change);
      lastSnapshot.current = next;
      setSnapshot(next);
      setSnapshotLoaded(true);
    }
    void refresh();
    const timer = window.setInterval(() => { void refresh(); }, 2000);
    return () => { active = false; window.clearInterval(timer); };
  }, []);

  useEffect(() => {
    let active = true;
    let unlisten: (() => void) | null = null;
    void listen("tray-open-settings", () => {
      setSetupOpen(false);
      setEditingToken(null);
      focusHeading.current = true;
      setPage("device");
    }).then((stop) => { if (active) unlisten = stop; else stop(); }).catch(() => {});
    return () => { active = false; unlisten?.(); };
  }, []);

  useEffect(() => {
    if (!announcement || announcement.persistent) return;
    const timer = window.setTimeout(() => setAnnouncement(null), 1500);
    return () => window.clearTimeout(timer);
  }, [announcement]);

  function selectPage(destination: Destination): void {
    setSetupOpen(false);
    setEditingToken(null);
    focusHeading.current = true;
    if (page === destination) headingRef.current?.focus();
    else setPage(destination);
  }

  function handleNavKey(event: KeyboardEvent<HTMLButtonElement>, current: Destination): void {
    const next = moveSelection(current, event.key);
    if (next === current && !["Home", "End", "ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight"].includes(event.key)) return;
    event.preventDefault();
    focusHeading.current = false;
    setPage(next);
    requestAnimationFrame(() => navRefs.current[next]?.focus());
  }

  const selectedLabel = destinations.find(({ id }) => id === page)?.label ?? "Systems";
  const route = describeRoute(snapshot);
  const editingGuest = snapshot.profiles.find((profile) => profile.bondToken === editingToken);
  function closeEditor(): void {
    setEditingToken(null);
    requestAnimationFrame(() => headingRef.current?.focus());
  }
  function requestLocal(): void {
    void returnToHost().catch((error) => setAnnouncement({ title: "Control request failed", message: `Local return request failed: ${String(error)}. Check the device connection.`, persistent: true }));
  }
  function requestGuest(bondToken: string): void {
    void selectGuest(bondToken).catch((error) => setAnnouncement({ title: "Control request failed", message: `Guest selection failed: ${String(error)}`, persistent: true }));
  }
  function changeKeepAwake(enabled: boolean): void {
    setKeepAwakePending(true);
    void setKeepAwake(enabled).then(() => setKeepAwakeState(enabled)).catch((error) => {
      setAnnouncement({ title: "Mouse bump unavailable", message: String(error), persistent: true });
    }).finally(() => setKeepAwakePending(false));
  }
  return <>
    <a className="skip-link" href="#main-content">Skip to content</a>
    <div className="app-shell">
      <aside className="sidebar" aria-label="Application sidebar">
        <div className="brand"><span className="brand-mark" aria-hidden="true" />ESP32 KVM</div>
        <nav className="nav-list" aria-label="Main navigation">{destinations.map(({ id, label }) => <button key={id} type="button" ref={(node) => { navRefs.current[id] = node; }} className={`nav-item${page === id ? " nav-item--selected" : ""}`} aria-current={page === id ? "page" : undefined} onClick={() => selectPage(id)} onKeyDown={(event) => handleNavKey(event, id)}>{label}</button>)}</nav>
        <div className="sidebar-bottom"><span className="sidebar-caption">ACTIVE TARGET</span><strong>{route.title}</strong><StatusPill tone={route.tone}>{snapshot.route.kind === "guest" ? "Guest controlling" : snapshot.route.kind === "switching" ? "Switch pending" : "Local control"}</StatusPill></div>
      </aside>
      <div className="workspace">
        <header className="topbar"><span>Your devices. One keyboard.</span><div className="topbar-right"><label className="preview-toggle"><input type="checkbox" checked={preview} onChange={(event) => setPreview(event.target.checked)} />Show design examples</label><StatusPill tone={snapshot.device.kind === "verified" ? "accent" : "warning"}>{snapshot.device.kind === "verified" ? "Device verified" : "No verified device"}</StatusPill></div></header>
        <div className="connection-status" role="status" aria-live="polite">{route.title}. {route.detail}</div>
        <main id="main-content" className="content" tabIndex={-1}>{setupOpen ? <SetupWizard preview={preview} onClose={() => { setSetupOpen(false); void setupSnapshot().then(setSnapshot); }} /> : editingGuest ? <ProfileEditor key={editingGuest.bondToken} guest={editingGuest} snapshot={snapshot} onClose={closeEditor} onChanged={() => { closeEditor(); void setupSnapshot().then(setSnapshot); }} /> : <>{page === "systems" && <SystemsPage preview={preview} headingRef={headingRef} snapshot={snapshot} onAddGuest={() => setSetupOpen(true)} onReturn={requestLocal} onEdit={setEditingToken} onSelect={requestGuest} keepAwake={keepAwake} keepAwakePending={keepAwakePending} onKeepAwake={changeKeepAwake} />}{page === "layout" && <LayoutEditor profiles={snapshot.profiles} profilesReady={snapshotLoaded} headingRef={headingRef} />}{page === "mappings" && <MappingPresets snapshot={snapshot} headingRef={headingRef} onChanged={() => { void setupSnapshot().then(setSnapshot); }} />}{page === "shortcuts" && <ShortcutsPage headingRef={headingRef} />}{page === "device" && <DevicePage headingRef={headingRef} snapshot={snapshot} onSetup={() => setSetupOpen(true)} />}</>}</main>
        <footer className="footer"><span>{snapshot.route.kind === "guest" ? "Guest route confirmed" : "Local control"} · Return with <KeyChord keys={["Ctrl", "Alt", "F10"]} /></span><span>Emergency: hold both Ctrl keys for one second</span></footer>
      </div>
    </div>
    {announcement && <div className={`switch-overlay${announcement.persistent ? " switch-overlay--persistent" : ""}`} role={announcement.persistent ? "alert" : "status"} aria-live={announcement.persistent ? "assertive" : "polite"}><strong>{announcement.title ?? (announcement.persistent ? "Connection lost" : "Input destination changed")}</strong><span>{announcement.message}</span>{announcement.persistent && <button type="button" onClick={() => setAnnouncement(null)} aria-label="Dismiss connection alert">Dismiss</button>}</div>}
    <span className="sr-only" aria-live="polite">{selectedLabel} page</span>
  </>;
}
