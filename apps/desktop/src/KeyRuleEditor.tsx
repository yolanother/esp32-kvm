// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Edits per-guest physical key and exact-chord draft rules with explicit sides,
// priorities and enablement. Its recorder and tester remain local to this view;
// no observed key or simulated report is sent, logged or persisted until save.
import { useEffect, useRef, useState, type JSX, type KeyboardEvent } from "react";
import { captureChordCodes, keyLabel, keymapConflicts, simulateKeymap } from "./keymap-editor";
import type { GuestProfile, KeyRule, KeySource } from "./setup-model";

const choices = [
  ...Array.from({ length: 26 }, (_, i) => 0x04 + i),
  ...Array.from({ length: 10 }, (_, i) => 0x1e + i),
  0x28, 0x29, 0x2c,
  ...Array.from({ length: 12 }, (_, i) => 0x3a + i),
  ...Array.from({ length: 8 }, (_, i) => 0xe0 + i),
];

function keyOf(usage: number): KeySource {
  return { usage, side: usage < 0xe0 ? "unspecified" : usage <= 0xe3 ? "left" : "right" };
}

function keyText(key: KeySource): string { return keyLabel(key.usage); }

/** One source or emitted key with explicit physical modifier side. */
function KeyPicker({ value, label, onChange, onRemove }: {
  value: KeySource; label: string; onChange: (key: KeySource) => void; onRemove: () => void;
}): JSX.Element {
  return <span className="key-rule-picker"><label>{label}<select value={value.usage} onChange={(event) => onChange(keyOf(Number(event.target.value)))}>{choices.map((usage) => <option key={usage} value={usage}>{keyLabel(usage)}</option>)}</select></label>
    {value.usage >= 0xe0 && <label>Side<select value={value.side} onChange={(event) => onChange(keyOf(value.usage + (event.target.value === "left" ? value.usage >= 0xe4 ? -4 : 0 : value.usage < 0xe4 ? 4 : 0)))}><option value="left">Left</option><option value="right">Right</option></select></label>}
    <button type="button" onClick={onRemove} aria-label={`Remove ${label}`}>Remove</button></span>;
}

/** Draft rule list and report simulator for one selected guest. */
export default function KeyRuleEditor({ guest, recordingAllowed, onChange }: { guest: GuestProfile; recordingAllowed: boolean; onChange: (guest: GuestProfile) => void }): JSX.Element {
  const [recording, setRecording] = useState<number | "test" | null>(null);
  const [testHeld, setTestHeld] = useState<KeySource[]>([]);
  const pressed = useRef<string[]>([]);
  const rules = guest.keyRules ?? [];
  const conflicts = keymapConflicts(rules);
  const report = simulateKeymap(guest, testHeld);
  useEffect(() => { setRecording(null); setTestHeld([]); pressed.current = []; }, [guest.bondToken, recordingAllowed]);

  function setRules(next: KeyRule[]): void { onChange({ ...guest, keyRules: next }); }
  function update(index: number, next: KeyRule): void { setRules(rules.map((rule, at) => at === index ? next : rule)); }
  function recordKeyDown(event: KeyboardEvent<HTMLButtonElement>, target: number | "test"): void {
    if (recording !== target) return;
    event.preventDefault();
    if (event.code === "Escape") { setRecording(null); pressed.current = []; return; }
    if (event.repeat) return;
    if (!pressed.current.includes(event.code)) pressed.current.push(event.code);
  }
  function recordKeyUp(event: KeyboardEvent<HTMLButtonElement>, target: number | "test"): void {
    if (recording !== target) return;
    event.preventDefault();
    if (!pressed.current.length) return;
    const captured = captureChordCodes(pressed.current);
    pressed.current = [];
    setRecording(null);
    if (!captured.length) return;
    if (target === "test") setTestHeld(captured);
    else update(target, { ...rules[target], source: captured });
  }
  function start(target: number | "test"): void { if (!recordingAllowed) return; pressed.current = []; setRecording(target); }

  return <section className="key-rules" aria-labelledby="key-rules-title">
    <div className="card-heading"><div><h2 id="key-rules-title">Physical key and chord rules</h2><p className="small muted">Guest overrides win over the preset. Exact chords win over single keys; each input is mapped once.</p></div><button type="button" disabled={rules.length >= 32} onClick={() => setRules([...rules, { source: [keyOf(0x04)], target: [keyOf(0x04)], priority: 0, enabled: true }])}>Add rule</button></div>
    {rules.map((rule, index) => <fieldset key={index} className="key-rule"><legend>Rule {index + 1}</legend>
      <div className="key-rule-columns"><div><strong>Physical host input</strong><div className="key-rule-keys">{rule.source.map((key, at) => <KeyPicker key={at} value={key} label={`Source key ${at + 1}`} onChange={(next) => update(index, { ...rule, source: rule.source.map((old, i) => i === at ? next : old) })} onRemove={() => update(index, { ...rule, source: rule.source.filter((_, i) => i !== at) })} />)}</div><button type="button" disabled={rule.source.length >= 4} onClick={() => update(index, { ...rule, source: [...rule.source, keyOf(0x04)] })}>Add source key</button><button type="button" disabled={!recordingAllowed} aria-pressed={recording === index} onClick={() => start(index)} onKeyDown={(event) => recordKeyDown(event, index)} onKeyUp={(event) => recordKeyUp(event, index)}>{recording === index ? "Press chord, then release" : "Record physical chord"}</button></div>
      <div><strong>Emitted guest input</strong><div className="key-rule-keys">{rule.target.map((key, at) => <KeyPicker key={at} value={key} label={`Guest key ${at + 1}`} onChange={(next) => update(index, { ...rule, target: rule.target.map((old, i) => i === at ? next : old) })} onRemove={() => update(index, { ...rule, target: rule.target.filter((_, i) => i !== at) })} />)}</div><button type="button" disabled={rule.target.length >= 4} onClick={() => update(index, { ...rule, target: [...rule.target, keyOf(0x04)] })}>Add guest key</button></div></div>
      <div className="key-rule-controls"><label>Priority<input type="number" min={-100} max={100} value={rule.priority} onChange={(event) => update(index, { ...rule, priority: Number(event.target.value) })} /></label><label><input type="checkbox" checked={rule.enabled} onChange={(event) => update(index, { ...rule, enabled: event.target.checked })} /> Enabled</label><button type="button" onClick={() => setRules(rules.filter((_, at) => at !== index))}>Delete rule</button></div>
    </fieldset>)}
    {conflicts.length ? <div className="notice notice--warning" role="alert"><strong>Resolve before saving</strong><ul>{conflicts.map((issue) => <li key={issue}>{issue}</li>)}</ul></div> : <p className="small muted" role="status">No rule conflicts. Both Ctrl keys remain reserved for emergency return.</p>}
    <div className="mapping-preview"><h2>Try your mapping locally</h2><p className="small muted">Record a chord to preview the emitted guest HID report. Nothing is sent to the guest or saved as a test log. Recording requires confirmed local control.</p><div className="setup-actions"><button type="button" disabled={!recordingAllowed} aria-pressed={recording === "test"} onClick={() => start("test")} onKeyDown={(event) => recordKeyDown(event, "test")} onKeyUp={(event) => recordKeyUp(event, "test")}>{recording === "test" ? "Press chord, then release" : "Record test chord"}</button><button type="button" onClick={() => { setTestHeld([]); setRecording(null); }}>Clear / all up</button></div><p>Physical: {testHeld.length ? testHeld.map(keyText).join(" + ") : "All up"}</p><p>Emitted: modifier byte 0x{report.modifiers.toString(16).padStart(2, "0")}; keys [{report.keys.map((usage) => usage ? keyLabel(usage) : "—").join(", ")}]</p></div>
  </section>;
}

/** Save-blocking rule issues for the parent editor's apply action. */
export { keymapConflicts } from "./keymap-editor";
