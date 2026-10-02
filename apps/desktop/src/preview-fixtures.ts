// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Holds illustrative guest names and mapping rules for an explicit design preview only.
// These fixtures are never used as device state, saved profiles, or routing targets.

/** A non-operational example guest shown only when preview is enabled. */
export interface ExampleGuest {
  readonly name: string;
  readonly os: string;
  readonly profile: string;
  readonly shortcut: readonly string[];
}

/** Sample guest cards from the design handoff, with no connection claim. */
export const exampleGuests: readonly ExampleGuest[] = [
  { name: "MacBook", os: "macOS", profile: "Windows shortcuts to Mac", shortcut: ["Ctrl", "Alt", "1"] },
  { name: "Studio PC", os: "Windows", profile: "Unchanged keys", shortcut: ["Ctrl", "Alt", "2"] },
  { name: "Linux workstation", os: "Linux", profile: "Custom profile", shortcut: ["Ctrl", "Alt", "3"] },
];

/** Sample source-to-guest rules, never loaded into the native mapper. */
export const exampleMappings = [
  { source: "Left Cmd / Win", target: "Left Ctrl", meaning: "Cmd/GUI to Ctrl" },
  { source: "Left Ctrl", target: "Left Cmd", meaning: "Ctrl to Cmd/GUI" },
  { source: "Alt", target: "Option", meaning: "Unchanged physical key" },
] as const;
