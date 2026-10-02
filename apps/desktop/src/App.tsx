// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Renders five desktop destinations and an explicit setup wizard with accessible
// navigation. Native status owns device truth; the webview never routes input.
import { useEffect, useRef, useState, type KeyboardEvent, type JSX } from "react";
import { destinations, moveSelection, type Destination } from "./navigation";
import { exampleGuests, exampleMappings } from "./preview-fixtures";
import SetupWizard from "./SetupWizard";
import { setupSnapshot, unavailableSnapshot } from "./setup-api";
import { profileState, type SetupSnapshot } from "./setup-model";

/** A readable text badge whose label also carries its meaning. */
function StatusPill({ children, tone = "neutral" }: { children: string; tone?: "neutral" | "warning" | "accent" }): JSX.Element {
  return <span className={`status-pill status-pill--${tone}`}>{children}</span>;
}

/** Display a proposed physical shortcut without activating global capture. */
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
function SystemsPage({ preview, headingRef, snapshot, onAddGuest }: { preview: boolean; headingRef: React.RefObject<HTMLHeadingElement | null>; snapshot: SetupSnapshot; onAddGuest: () => void }): JSX.Element {
  return <>
    <PageIntro title="Systems" description="Choose where your keyboard and mouse go after setup." headingRef={headingRef} />
    <div className="toolbar"><StatusPill>Local control only</StatusPill><span className="muted">Setup starts disarmed.</span><button type="button" onClick={onAddGuest}>Add guest</button></div>
    <div className="card-grid">
      <article className="card card--local"><div className="card-heading"><h2>This computer</h2><StatusPill tone="accent">Local host</StatusPill></div><p>Keyboard and mouse remain with Windows.</p><div className="card-tail"><span className="small muted">No active guest route</span><span className="small">Current state</span></div></article>
      {preview ? exampleGuests.map((guest) => <article className="card" key={guest.name}><div className="card-heading"><h2>{guest.name}</h2><StatusPill>Example only</StatusPill></div><p>{guest.os} · {guest.profile}</p><div className="card-tail"><KeyChord keys={guest.shortcut} /><span className="small muted">Selection unavailable</span></div></article>) : snapshot.profiles.length ? snapshot.profiles.map((guest) => <article className="card" key={guest.bondToken}><div className="card-heading"><h2>{guest.name}</h2><StatusPill tone={profileState(snapshot.profiles, guest.bondToken, snapshot.readyTokens) === "ready" ? "accent" : "warning"}>{profileState(snapshot.profiles, guest.bondToken, snapshot.readyTokens) === "ready" ? "Ready" : "Offline"}</StatusPill></div><p>{guest.os} · {guest.profile === "unchanged" ? "Unchanged keys" : "Windows shortcuts to Mac"}</p><div className="card-tail"><span className="small muted">Saved identity</span><span className="small muted">Input selection unavailable</span></div></article>) : <article className="card card--empty"><h2>No guest profiles yet</h2><p>Connect the device and pair a guest to add a saved profile.</p><StatusPill tone="warning">Device unverified</StatusPill></article>}
    </div>
    {preview && <PreviewNotice />}
    <div className="notice"><strong>Stay in control</strong><span>Routing starts only after the firmware and guest report ready. This shell cannot arm input capture.</span></div>
  </>;
}

/** Screen layout destination; preview geometry is deliberately non-operational. */
function LayoutPage({ preview, headingRef }: { preview: boolean; headingRef: React.RefObject<HTMLHeadingElement | null> }): JSX.Element {
  return <>
    <PageIntro title="Screen layout" description="Place systems and assign exposed host edges to guests." headingRef={headingRef} />
    <div className="toolbar"><StatusPill>Layout unavailable</StatusPill><span className="muted">Host monitor detection is not connected yet.</span></div>
    {preview ? <div className="layout-preview card" aria-label="Example monitor arrangement, not detected displays"><div className="monitor monitor--side"><span>EXAMPLE HOST</span><strong>Display 2</strong><small>Illustrative geometry</small></div><div className="monitor monitor--main"><span>EXAMPLE HOST</span><strong>Main display</strong><small>Illustrative right edge</small></div><span className="layout-arrow" aria-hidden="true">→</span><div className="monitor monitor--guest"><span>EXAMPLE GUEST</span><strong>MacBook</strong><small>Manual placeholder</small></div></div> : <div className="card empty-panel"><h2>No layout detected</h2><p>Detected monitor geometry and portal editing will appear when the Windows topology feature is connected.</p></div>}
    {preview && <PreviewNotice />}
    <div className="notice notice--warning"><strong>Standard BLE limit</strong><span>A guest keeps its existing cursor position. Return by a configured host shortcut when switching is available; automatic return needs the optional helper.</span></div>
  </>;
}

/** Key mapping destination with explicit, sample-only direction labels. */
function MappingsPage({ preview, headingRef }: { preview: boolean; headingRef: React.RefObject<HTMLHeadingElement | null> }): JSX.Element {
  return <>
    <PageIntro title="Key mappings" description="Choose exactly which physical keys a guest receives." headingRef={headingRef} />
    <div className="toolbar"><StatusPill>Nothing applied</StatusPill><span className="muted">Host keys remain unchanged.</span></div>
    {preview ? <div className="card table-card"><div className="card-heading"><h2>Example rules for MacBook</h2><StatusPill>Sample only</StatusPill></div><div className="table-scroll"><table><thead><tr><th scope="col">Physical host input</th><th scope="col">Direction</th><th scope="col">Emitted guest input</th><th scope="col">Meaning</th></tr></thead><tbody>{exampleMappings.map((rule) => <tr key={rule.source}><td><kbd>{rule.source}</kbd></td><td aria-label="maps to">→</td><td><kbd>{rule.target}</kbd></td><td>{rule.meaning}</td></tr>)}</tbody></table></div></div> : <div className="card empty-panel"><h2>No guest selected</h2><p>Guest profiles and mapping controls will become available after pairing and native profile storage are implemented.</p></div>}
    {preview && <PreviewNotice />}
    <div className="notice"><strong>Direction is explicit</strong><span>Cmd/GUI to Ctrl and Ctrl to Cmd/GUI are separate rules. The examples above are never sent to a guest.</span></div>
  </>;
}

const proposedShortcuts = [
  { action: "Next connected system", keys: ["Ctrl", "Alt", "F12"] },
  { action: "Previous connected system", keys: ["Ctrl", "Alt", "F11"] },
  { action: "Return to host", keys: ["Ctrl", "Alt", "F10"] },
] as const;

/** Shortcuts destination displays proposed defaults without registering hotkeys. */
function ShortcutsPage({ headingRef }: { headingRef: React.RefObject<HTMLHeadingElement | null> }): JSX.Element {
  return <>
    <PageIntro title="Shortcuts" description="Review the proposed physical-key controls for switching systems." headingRef={headingRef} />
    <div className="notice notice--warning"><strong>Shortcuts are inactive</strong><span>The native hotkey recognizer and recording flow are not connected in this build. These combinations are design defaults only.</span></div>
    <div className="card table-card"><h2>Proposed defaults</h2><div className="table-scroll"><table><thead><tr><th scope="col">Action</th><th scope="col">Physical key combination</th><th scope="col">State</th></tr></thead><tbody>{proposedShortcuts.map(({ action, keys }) => <tr key={action}><td>{action}</td><td><KeyChord keys={keys} /></td><td className="muted">Not registered</td></tr>)}</tbody></table></div></div>
    <div className="card emergency-card"><h2>Planned emergency return</h2><p>Hold both Ctrl keys for one second. This safety path is not active until native routing is integrated and tested.</p></div>
  </>;
}

/** Device destination reports only facts available without a native handshake. */
function DevicePage({ headingRef, snapshot, onSetup }: { headingRef: React.RefObject<HTMLHeadingElement | null>; snapshot: SetupSnapshot; onSetup: () => void }): JSX.Element {
  return <>
    <PageIntro title="Device" description="Connection health, firmware and diagnostics will appear here." headingRef={headingRef} />
    <div className="card card-heading"><div><h2>ESP32-S3 input bridge</h2><p>{snapshot.device.kind === "verified" ? `Firmware reports ${snapshot.device.boardId}.` : "Attached variant and revision are unverified."}</p></div><StatusPill tone={snapshot.device.kind === "verified" ? "accent" : "warning"}>{snapshot.device.kind === "verified" ? "Verified" : "Not verified"}</StatusPill></div>
    <div className="card-grid device-grid"><section className="card"><h2>Connection</h2><dl className="detail-list"><div><dt>USB session</dt><dd>{snapshot.device.kind === "verified" ? "Verified" : snapshot.device.kind === "candidate" ? "Candidate only" : "Unavailable"}</dd></div><div><dt>BLE guests ready</dt><dd>{snapshot.device.kind === "verified" ? snapshot.readyTokens.length : "Unavailable"}</dd></div><div><dt>Input destination</dt><dd>Not reported</dd></div><div><dt>Guest latency</dt><dd>Not measured</dd></div></dl></section><section className="card"><h2>Firmware</h2><p>Board identity and capabilities require a device handshake.</p><dl className="detail-list"><div><dt>Installed version</dt><dd>{snapshot.device.kind === "verified" ? snapshot.device.firmwareVersion ?? "Unavailable" : "Unknown"}</dd></div><div><dt>Protocol</dt><dd>{snapshot.device.kind === "verified" ? "Verified" : "Unverified"}</dd></div></dl><button type="button" onClick={onSetup}>Open setup</button></section></div>
    <div className="notice"><strong>Diagnostics</strong><span>Connection events and aggregate counters will be available later. Typed keys, pairing codes and bond secrets must never appear in exports.</span></div>
  </>;
}

/** Provide persistent navigation and truthful status around the five destinations. */
export default function App(): JSX.Element {
  const [page, setPage] = useState<Destination>("systems");
  const [preview, setPreview] = useState(false);
  const [setupOpen, setSetupOpen] = useState(false);
  const [snapshot, setSnapshot] = useState<SetupSnapshot>(unavailableSnapshot());
  const headingRef = useRef<HTMLHeadingElement>(null);
  const focusHeading = useRef(false);
  const navRefs = useRef<Record<Destination, HTMLButtonElement | null>>({ systems: null, layout: null, mappings: null, shortcuts: null, device: null });

  useEffect(() => {
    if (focusHeading.current) headingRef.current?.focus();
    focusHeading.current = false;
  }, [page]);

  useEffect(() => {
    let active = true;
    async function refresh(): Promise<void> { const next = await setupSnapshot(); if (active) setSnapshot(next); }
    void refresh();
    const timer = window.setInterval(() => { void refresh(); }, 2000);
    return () => { active = false; window.clearInterval(timer); };
  }, []);

  function selectPage(destination: Destination): void {
    setSetupOpen(false);
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
  return <>
    <a className="skip-link" href="#main-content">Skip to content</a>
    <div className="app-shell">
      <aside className="sidebar" aria-label="Application sidebar">
        <div className="brand"><span className="brand-mark" aria-hidden="true" />ESP32 KVM</div>
        <nav className="nav-list" aria-label="Main navigation">{destinations.map(({ id, label }) => <button key={id} type="button" ref={(node) => { navRefs.current[id] = node; }} className={`nav-item${page === id ? " nav-item--selected" : ""}`} aria-current={page === id ? "page" : undefined} onClick={() => selectPage(id)} onKeyDown={(event) => handleNavKey(event, id)}>{label}</button>)}</nav>
        <div className="sidebar-bottom"><span className="sidebar-caption">WINDOWS HOST</span><strong>This computer</strong><StatusPill tone={snapshot.device.kind === "verified" ? "accent" : "warning"}>{snapshot.device.kind === "verified" ? "Device verified" : "Connection unavailable"}</StatusPill></div>
      </aside>
      <div className="workspace">
        <header className="topbar"><span>Your devices. One keyboard.</span><div className="topbar-right"><label className="preview-toggle"><input type="checkbox" checked={preview} onChange={(event) => setPreview(event.target.checked)} />Show design examples</label><StatusPill tone={snapshot.device.kind === "verified" ? "accent" : "warning"}>{snapshot.device.kind === "verified" ? "Device verified" : "No verified device"}</StatusPill></div></header>
        <div className="connection-status" role="status" aria-live="polite">{snapshot.device.kind === "verified" ? "Device verified. Guest control still requires an acknowledged route." : snapshot.device.kind === "candidate" ? "USB interface detected; firmware is not verified. Input stays with this computer." : "Device connection is unavailable. Input stays with this computer."}</div>
        <main id="main-content" className="content" tabIndex={-1}>{setupOpen ? <SetupWizard preview={preview} onClose={() => { setSetupOpen(false); void setupSnapshot().then(setSnapshot); }} /> : <>{page === "systems" && <SystemsPage preview={preview} headingRef={headingRef} snapshot={snapshot} onAddGuest={() => setSetupOpen(true)} />}{page === "layout" && <LayoutPage preview={preview} headingRef={headingRef} />}{page === "mappings" && <MappingsPage preview={preview} headingRef={headingRef} />}{page === "shortcuts" && <ShortcutsPage headingRef={headingRef} />}{page === "device" && <DevicePage headingRef={headingRef} snapshot={snapshot} onSetup={() => setSetupOpen(true)} />}</>}</main>
        <footer className="footer"><span>Local control only · Proposed return shortcut <KeyChord keys={["Ctrl", "Alt", "F10"]} /> is inactive</span><span>Emergency shortcut is not active yet</span></footer>
      </div>
    </div>
    <span className="sr-only" aria-live="polite">{selectedLabel} page</span>
  </>;
}
