// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Verifies keyboard navigation across the five desktop shell destinations.
import assert from "node:assert/strict";
import test from "node:test";
import { destinations, moveSelection } from "./navigation.ts";

test("all required destinations appear in the intended order", () => {
  assert.deepEqual(destinations.map(({ id }) => id), [
    "systems", "layout", "mappings", "shortcuts", "device",
  ]);
});

test("arrow keys wrap and Home/End select boundaries", () => {
  assert.equal(moveSelection("systems", "ArrowUp"), "device");
  assert.equal(moveSelection("device", "ArrowDown"), "systems");
  assert.equal(moveSelection("mappings", "Home"), "systems");
  assert.equal(moveSelection("mappings", "End"), "device");
});

test("other keys leave the selected destination unchanged", () => {
  assert.equal(moveSelection("layout", "Tab"), "layout");
});
