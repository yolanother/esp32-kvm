// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Defines the five desktop shell destinations and deterministic keyboard movement between them.

/** Stable destination identifier for shell navigation. */
export type Destination = "systems" | "layout" | "mappings" | "shortcuts" | "device";

/** Sidebar destinations in visual and keyboard order. */
export const destinations: ReadonlyArray<{ id: Destination; label: string }> = [
  { id: "systems", label: "Systems" },
  { id: "layout", label: "Screen layout" },
  { id: "mappings", label: "Key mappings" },
  { id: "shortcuts", label: "Shortcuts" },
  { id: "device", label: "Device" },
];

/** Return the destination reached by an arrow or boundary key. */
export function moveSelection(current: Destination, key: string): Destination {
  const index = destinations.findIndex(({ id }) => id === current);
  if (key === "Home") return destinations[0].id;
  if (key === "End") return destinations[destinations.length - 1].id;
  if (key === "ArrowDown" || key === "ArrowRight") {
    return destinations[(index + 1) % destinations.length].id;
  }
  if (key === "ArrowUp" || key === "ArrowLeft") {
    return destinations[(index - 1 + destinations.length) % destinations.length].id;
  }
  return current;
}
