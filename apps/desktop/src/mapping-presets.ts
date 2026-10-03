// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Defines the desktop's physical modifier preset drafts and complete change
// previews. Drafts are per guest and never alter native input until saved.
import type { GuestProfile, ModifierBinding } from "./setup-model";

/** Built-in mapping direction; Alt/Option remains an identity binding. */
export type PresetId = "unchanged" | "cmd-to-ctrl" | "windows-to-mac";

/** Short label for a saved mapping choice in system summaries. */
export function mappingLabel(profile: GuestProfile["profile"]): string {
  return ({ unchanged: "Unchanged keys", "cmd-to-ctrl": "Cmd / Win to Ctrl", "windows-to-mac": "Windows Ctrl to Mac Cmd", custom: "Custom modifier mapping" })[profile];
}

/** All changed physical modifier bindings for a built-in preset. */
export function presetBindings(preset: PresetId): ModifierBinding[] {
  switch (preset) {
    case "cmd-to-ctrl": return [{ sourceUsage: 0xe3, targetUsage: 0xe0 }, { sourceUsage: 0xe7, targetUsage: 0xe4 }];
    case "windows-to-mac": return [{ sourceUsage: 0xe0, targetUsage: 0xe3 }, { sourceUsage: 0xe4, targetUsage: 0xe7 }];
    case "unchanged": return [];
  }
}

/** Every effective changed binding in the draft, in physical source order. */
export function previewBindings(guest: GuestProfile): ModifierBinding[] {
  const bindings = guest.profile === "custom" ? guest.modifierBindings ?? [] : presetBindings(guest.profile);
  return [...bindings].sort((a, b) => a.sourceUsage - b.sourceUsage);
}

/** Copies one built-in preset into this guest's editable custom draft. */
export function clonePreset(guest: GuestProfile): GuestProfile {
  const base: PresetId = guest.profile === "custom" ? guest.customBasePreset ?? "unchanged" : guest.profile;
  return { ...guest, profile: "custom", customBasePreset: base, modifierBindings: previewBindings(guest) };
}

/** Edits one physical modifier; selecting identity removes its custom rule. */
export function editBinding(guest: GuestProfile, sourceUsage: number, targetUsage: number): GuestProfile {
  if (guest.profile !== "custom") return guest;
  if (sourceUsage < 0xe0 || sourceUsage > 0xe7 || targetUsage < 0xe0 || targetUsage > 0xe7) return guest;
  const bindings = (guest.modifierBindings ?? []).filter((row) => row.sourceUsage !== sourceUsage);
  if (sourceUsage !== targetUsage) bindings.push({ sourceUsage, targetUsage });
  return { ...guest, modifierBindings: bindings.sort((a, b) => a.sourceUsage - b.sourceUsage) };
}

/** Restores a custom draft to the built-in preset it was cloned from. */
export function resetCustom(guest: GuestProfile): GuestProfile {
  if (guest.profile !== "custom") return guest;
  return { ...guest, modifierBindings: presetBindings(guest.customBasePreset ?? "unchanged") };
}

/** Physical HID modifier names for the preview and editor. */
export function modifierLabel(usage: number): string {
  return ["Left Ctrl", "Left Shift", "Left Alt / Option", "Left Cmd / Win", "Right Ctrl", "Right Shift", "Right Alt / AltGr", "Right Cmd / Win"][usage - 0xe0] ?? "Unknown";
}
