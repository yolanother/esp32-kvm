// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Lets users preview, clone, edit, reset, and save physical modifier mappings
// per opaque guest identity. It never records keys or sends a test to hardware.
import { useEffect, useState, type JSX, type RefObject } from "react";
import { saveGuestProfile } from "./setup-api";
import { clonePreset, editBinding, modifierLabel, previewBindings, resetCustom, type PresetId } from "./mapping-presets";
import type { GuestProfile, SetupSnapshot } from "./setup-model";

const presets: { id: PresetId; label: string; detail: string }[] = [
  { id: "unchanged", label: "Unchanged physical keys", detail: "No modifier replacement." },
  { id: "cmd-to-ctrl", label: "Cmd / Windows key → Ctrl", detail: "For an Apple keyboard on a Windows host controlling a Ctrl-shortcut guest." },
  { id: "windows-to-mac", label: "Windows Ctrl → Mac Cmd", detail: "For familiar Windows shortcuts when controlling a Mac." },
];
const modifierUsages = [0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7];

/** Per-guest preset editor with a complete draft change preview before apply. */
export default function MappingPresets({ snapshot, headingRef, onChanged }: {
  snapshot: SetupSnapshot; headingRef: RefObject<HTMLHeadingElement | null>; onChanged: () => void;
}): JSX.Element {
  const [selected, setSelected] = useState(snapshot.profiles[0]?.bondToken ?? "");
  const stored = snapshot.profiles.find((guest) => guest.bondToken === selected);
  const [draft, setDraft] = useState<GuestProfile | null>(stored ?? null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  useEffect(() => {
    if (!snapshot.profiles.some((guest) => guest.bondToken === selected)) {
      setSelected(snapshot.profiles[0]?.bondToken ?? "");
      setDraft(snapshot.profiles[0] ?? null);
    }
  }, [selected, snapshot.profiles]);
  const changes = draft ? previewBindings(draft) : [];
  const dirty = !!stored && !!draft && JSON.stringify(stored) !== JSON.stringify(draft);
  const pending = snapshot.mappingPendingTokens.includes(selected);

  async function apply(): Promise<void> {
    if (!draft || !dirty) return;
    setBusy(true); setError("");
    try { await saveGuestProfile(draft); onChanged(); }
    catch (failure) { setError(String(failure)); }
    finally { setBusy(false); }
  }

  return <section aria-labelledby="mapping-title">
    <header className="page-intro"><div><p className="eyebrow">INPUT ROUTING</p><h1 id="mapping-title" ref={headingRef} tabIndex={-1}>Key mappings</h1><p>Choose the physical modifier direction for one saved guest. Local Windows input is unchanged.</p></div></header>
    {!draft ? <div className="card empty-panel"><h2>No saved guest</h2><p>Pair and name a guest before setting its mapping.</p></div> : <div className="card mapping-editor">
      <label>Guest profile<select value={selected} onChange={(event) => { const next = snapshot.profiles.find((guest) => guest.bondToken === event.target.value); setSelected(event.target.value); setDraft(next ?? null); setError(""); }}>{snapshot.profiles.map((guest) => <option key={guest.bondToken} value={guest.bondToken}>{guest.name}</option>)}</select></label>
      {pending && <div className="notice notice--warning" role="status">Mapping saved locally; native actor installation is pending. Input remains disarmed while this is unresolved.</div>}
      <fieldset><legend>Preset direction</legend>{presets.map((preset) => <label className="mapping-choice" key={preset.id}><input type="radio" name="mapping-preset" checked={draft.profile === preset.id} onChange={() => setDraft({ ...draft, profile: preset.id, customBasePreset: undefined, modifierBindings: undefined })} /><span><strong>{preset.label}</strong><small>{preset.detail}</small></span></label>)}<label className="mapping-choice"><input type="radio" name="mapping-preset" checked={draft.profile === "custom"} onChange={() => setDraft(clonePreset(draft))} /><span><strong>Custom copy</strong><small>Clone the selected preset, then edit each physical modifier.</small></span></label></fieldset>
      {draft.profile === "custom" && <div className="mapping-custom"><div className="card-heading"><h2>Custom modifier bindings</h2><button type="button" onClick={() => setDraft(resetCustom(draft))}>Reset copy</button></div>{modifierUsages.map((usage) => <label key={usage}>{modifierLabel(usage)}<select value={changes.find((row) => row.sourceUsage === usage)?.targetUsage ?? usage} onChange={(event) => setDraft(editBinding(draft, usage, Number(event.target.value)))}>{modifierUsages.map((target) => <option value={target} key={target}>{modifierLabel(target)}</option>)}</select></label>)}</div>}
      <div className="mapping-preview"><h2>Preview before apply</h2>{changes.length ? <div className="table-scroll"><table><thead><tr><th scope="col">Physical host input</th><th scope="col">Guest output</th></tr></thead><tbody>{changes.map((row) => <tr key={row.sourceUsage}><td>{modifierLabel(row.sourceUsage)}</td><td>{modifierLabel(row.targetUsage)}</td></tr>)}</tbody></table></div> : <p>No changed bindings. Physical keys pass through unchanged.</p>}<p className="small muted">Alt remains Option/Alt. Right Alt with Windows-synthesized Left Ctrl is preserved for AltGr; guest layout determines the character. This preview is local and sends no keys.</p></div>
      <div className="card-tail"><button type="button" onClick={() => { setDraft(stored ?? null); setError(""); }} disabled={!dirty || busy}>Discard draft</button><button type="button" onClick={() => { void apply(); }} disabled={!dirty || busy}>Apply to {draft.name}</button></div>
      {error && <div className="notice notice--warning" role="alert">{error}</div>}
    </div>}
  </section>;
}
