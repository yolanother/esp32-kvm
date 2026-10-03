// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Edits host-local guest labels and shortcut preferences without promoting
// offline profiles to live connections. Forget waits for firmware confirmation.
import { useEffect, useRef, useState, type FormEvent, type JSX, type KeyboardEvent } from "react";
import { forgetGuestProfile, saveGuestProfile } from "./setup-api";
import { validDirectShortcut, validGuestName, type GuestProfile, type SetupSnapshot } from "./setup-model";

/** Keyboard-accessible editor for a previously firmware-proven bond token. */
export default function ProfileEditor({ guest, snapshot, onClose, onChanged }: {
  guest: GuestProfile; snapshot: SetupSnapshot; onClose: () => void; onChanged: () => void;
}): JSX.Element {
  const [name, setName] = useState(guest.name);
  const [os, setOs] = useState(guest.os);
  const [profile, setProfile] = useState(guest.profile);
  const [directShortcut, setDirectShortcut] = useState(guest.directShortcut ?? "");
  const [confirmForget, setConfirmForget] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const heading = useRef<HTMLHeadingElement>(null);
  useEffect(() => { heading.current?.focus(); }, []);

  function escape(event: KeyboardEvent<HTMLElement>): void {
    if (event.key !== "Escape") return;
    event.preventDefault();
    if (confirmForget) setConfirmForget(false);
    else onClose();
  }

  async function save(event: FormEvent<HTMLFormElement>): Promise<void> {
    event.preventDefault();
    if (!validGuestName(name) || !validDirectShortcut(directShortcut)) return;
    setBusy(true); setError("");
    try {
      await saveGuestProfile({ ...guest, name: name.trim(), os, profile, directShortcut: directShortcut || null });
      onChanged();
    } catch (failure) { setError(String(failure)); }
    finally { setBusy(false); }
  }

  async function forget(): Promise<void> {
    setBusy(true); setError("");
    try { await forgetGuestProfile(guest.bondToken); onChanged(); }
    catch (failure) { setError(String(failure)); }
    finally { setBusy(false); }
  }

  const canForget = snapshot.device.kind === "verified" && snapshot.route.kind === "local";
  return <section className="profile-editor card" aria-labelledby="profile-title" onKeyDown={escape}>
    <div className="card-heading"><div><p className="eyebrow">SAVED GUEST</p><h2 id="profile-title" ref={heading} tabIndex={-1}>Edit {guest.name}</h2></div><button type="button" onClick={onClose}>Close</button></div>
    <p>A saved name and preference stay on this computer. Editing them does not connect or select the guest.</p>
    <form className="profile-form" onSubmit={(event) => { void save(event); }}>
      <label>Guest name<input value={name} maxLength={64} onChange={(event) => setName(event.target.value)} autoComplete="off" required /></label>
      <label>Guest OS<select value={os} onChange={(event) => setOs(event.target.value as GuestProfile["os"])}><option value="macos">macOS</option><option value="windows">Windows</option><option value="linux">Linux</option><option value="other">Other</option></select></label>
      <label>Key profile preference<select value={profile} onChange={(event) => setProfile(event.target.value as GuestProfile["profile"])}><option value="unchanged">Unchanged keys</option><option value="windows-to-mac">Windows shortcuts to Mac</option></select></label>
      <label>Direct shortcut preference<input value={directShortcut} maxLength={64} onChange={(event) => setDirectShortcut(event.target.value)} placeholder="Ctrl+Alt+1" aria-describedby="shortcut-help" /></label>
      <p className="small muted" id="shortcut-help">Saved for future shortcut setup. No shortcut is registered in this build.</p>
      <button type="submit" disabled={busy || !validGuestName(name) || !validDirectShortcut(directShortcut)}>Save profile</button>
    </form>
    <div className="profile-danger"><h3>Forget this guest</h3><p>The device bond and local name must both be removed. The name is kept if firmware cannot confirm bond removal.</p>
      {!confirmForget ? <button type="button" onClick={() => setConfirmForget(true)} disabled={busy || !canForget}>Review forget request</button> : <div className="notice notice--warning" role="alert"><span>Forget {guest.name} on the device and this computer?</span><button type="button" onClick={() => setConfirmForget(false)} disabled={busy}>Cancel</button><button type="button" onClick={() => { void forget(); }} disabled={busy}>Confirm forget</button></div>}
      {!canForget && <p className="small muted">Connect a verified device and return locally before forgetting a bond.</p>}
    </div>
    {error && <div className="notice notice--warning" role="alert">{error}</div>}
  </section>;
}
