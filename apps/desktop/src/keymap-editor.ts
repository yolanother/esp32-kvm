// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Validates per-guest physical HID key and exact-chord drafts, captures common
// browser physical codes, and simulates emitted reports locally without I/O.
import { presetBindings } from "./mapping-presets.ts";
import type { GuestProfile, KeySource, KeyRule } from "./setup-model";

/** Labels a physical HID usage without implying guest character translation. */
export function keyLabel(usage: number): string {
  if (usage >= 0x04 && usage <= 0x1d) return String.fromCharCode(65 + usage - 0x04);
  if (usage >= 0x1e && usage <= 0x27) return "1234567890"[usage - 0x1e];
  if (usage >= 0x3a && usage <= 0x45) return `F${usage - 0x39}`;
  return ({ 0x28: "Enter", 0x29: "Esc", 0x2c: "Space", 0xe0: "Left Ctrl", 0xe1: "Left Shift", 0xe2: "Left Alt", 0xe3: "Left Cmd / Win", 0xe4: "Right Ctrl", 0xe5: "Right Shift", 0xe6: "Right Alt", 0xe7: "Right Cmd / Win" } as Record<number, string>)[usage] ?? `HID ${usage.toString(16).padStart(2, "0")}`;
}

/** Converts common KeyboardEvent.code values to physical usages and sides. */
export function sourceFromCode(code: string): KeySource | null {
  if (/^Key[A-Z]$/.test(code)) return { usage: 0x04 + code.charCodeAt(3) - 65, side: "unspecified" };
  if (/^Digit[0-9]$/.test(code)) return { usage: 0x1e + (Number(code[5]) + 9) % 10, side: "unspecified" };
  if (/^F([1-9]|1[0-2])$/.test(code)) return { usage: 0x39 + Number(code.slice(1)), side: "unspecified" };
  const fixed: Record<string, KeySource> = {
    ControlLeft: { usage: 0xe0, side: "left" }, ShiftLeft: { usage: 0xe1, side: "left" }, AltLeft: { usage: 0xe2, side: "left" }, MetaLeft: { usage: 0xe3, side: "left" },
    ControlRight: { usage: 0xe4, side: "right" }, ShiftRight: { usage: 0xe5, side: "right" }, AltRight: { usage: 0xe6, side: "right" }, MetaRight: { usage: 0xe7, side: "right" },
    Enter: { usage: 0x28, side: "unspecified" }, Escape: { usage: 0x29, side: "unspecified" }, Space: { usage: 0x2c, side: "unspecified" },
  };
  return fixed[code] ?? null;
}

/** Captures a bounded exact chord from physical key-down codes. */
export function captureChordCodes(codes: readonly string[]): KeySource[] {
  const result: KeySource[] = [];
  for (const code of codes) {
    const source = sourceFromCode(code);
    if (source && !result.some((item) => sameSource(item, source)) && result.length < 4) result.push(source);
  }
  return result;
}

function sameSource(a: KeySource, b: KeySource): boolean { return a.usage === b.usage && a.side === b.side; }
function trigger(rule: KeyRule): string { return [...rule.source].map((key) => `${key.usage}:${key.side}`).sort().join("+"); }
function validKey(key: KeySource): boolean {
  return Number.isInteger(key.usage) && key.usage >= 0x04 && key.usage <= 0xe7 &&
    ["unspecified", "left", "right"].includes(key.side) &&
    (key.usage < 0xe0 ? key.side === "unspecified" : key.side === (key.usage <= 0xe3 ? "left" : "right"));
}

/** Returns save-blocking conflicts for a bounded rule list. */
export function keymapConflicts(rules: readonly KeyRule[]): string[] {
  const issues: string[] = [];
  if (rules.length > 32) issues.push("At most 32 rules can be saved.");
  const seen = new Set<string>();
  for (const [index, rule] of rules.entries()) {
    if (!Number.isInteger(rule.priority) || rule.priority < -100 || rule.priority > 100) issues.push(`Rule ${index + 1}: priority must be -100 to 100.`);
    if (rule.source.length < 1 || rule.source.length > 4 || rule.target.length < 1 || rule.target.length > 4 ||
        !rule.source.every(validKey) || !rule.target.every(validKey) ||
        new Set(rule.source.map((key) => `${key.usage}:${key.side}`)).size !== rule.source.length ||
        new Set(rule.target.map((key) => `${key.usage}:${key.side}`)).size !== rule.target.length)
      issues.push(`Rule ${index + 1}: source or output keys are invalid.`);
    if (rule.source.some((key) => key.usage === 0xe0) && rule.source.some((key) => key.usage === 0xe4))
      issues.push(`Rule ${index + 1}: the both-Ctrl emergency escape is reserved.`);
    if (rule.enabled) {
      const key = `${trigger(rule)}@${rule.priority}`;
      if (seen.has(key)) issues.push(`Rule ${index + 1}: duplicate same-priority trigger.`);
      seen.add(key);
    }
  }
  return issues;
}

function effectiveRules(guest: GuestProfile): KeyRule[] {
  const bindings = guest.profile === "custom" ? guest.modifierBindings ?? [] : presetBindings(guest.profile);
  return bindings.map((binding) => ({
    source: [{ usage: binding.sourceUsage, side: binding.sourceUsage <= 0xe3 ? "left" as const : "right" as const }],
    target: [{ usage: binding.targetUsage, side: binding.targetUsage <= 0xe3 ? "left" as const : "right" as const }],
    priority: 0, enabled: true,
  }));
}

function matches(rule: KeyRule, held: readonly KeySource[]): boolean {
  return rule.source.length === held.length && rule.source.every((item) => held.some((key) => sameSource(item, key)));
}

/** Computes a boot-style guest keyboard report from a local draft and held physical keys. */
export function simulateKeymap(guest: GuestProfile, physicalHeld: readonly KeySource[]): { modifiers: number; keys: number[] } {
  const held = physicalHeld.filter((item, index) => physicalHeld.findIndex((other) => sameSource(item, other)) === index);
  const custom = (guest.keyRules ?? []).filter((rule) => rule.enabled);
  const preset = effectiveRules(guest);
  const best = (rules: readonly KeyRule[], key: KeySource) => rules.filter((rule) => rule.source.length === 1 && sameSource(rule.source[0], key)).sort((a, b) => b.priority - a.priority)[0];
  const exactChord = (rules: readonly KeyRule[]) => rules.filter((rule) => rule.source.length > 1 && matches(rule, held)).sort((a, b) => b.priority - a.priority)[0];
  const chord = exactChord(custom) ?? exactChord(preset);
  const outputs: KeySource[] = [];
  if (chord) outputs.push(...chord.target);
  else for (const key of held) {
    if (key.usage === 0xe0 && held.some((other) => other.usage === 0xe6)) { outputs.push(key); continue; }
    const selected = best(custom, key) ?? best(preset, key);
    outputs.push(...(selected?.target ?? [key]));
  }
  const usages = [...new Set(outputs.map((item) => item.usage))];
  const modifiers = usages.filter((usage) => usage >= 0xe0 && usage <= 0xe7).reduce((bits, usage) => bits | (1 << (usage - 0xe0)), 0);
  const ordinary = usages.filter((usage) => usage < 0xe0).sort((a, b) => a - b);
  return { modifiers, keys: ordinary.length > 6 ? Array(6).fill(1) : [...ordinary, ...Array(6 - ordinary.length).fill(0)] };
}
