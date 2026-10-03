// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Verifies explicit preset direction, complete draft previews, and per-guest
// clone/edit/reset behavior without a live keyboard or device connection.
import assert from "node:assert/strict";
import test from "node:test";
import { clonePreset, editBinding, presetBindings, previewBindings, resetCustom } from "./mapping-presets.ts";

const guest = { bondToken: "ab".repeat(16), name: "Mac", os: "macos", profile: "windows-to-mac" };

test("directional presets show every changed side and leave Alt/Option alone", () => {
  assert.deepEqual(presetBindings("cmd-to-ctrl"), [
    { sourceUsage: 0xe3, targetUsage: 0xe0 }, { sourceUsage: 0xe7, targetUsage: 0xe4 },
  ]);
  assert.deepEqual(presetBindings("windows-to-mac"), [
    { sourceUsage: 0xe0, targetUsage: 0xe3 }, { sourceUsage: 0xe4, targetUsage: 0xe7 },
  ]);
  assert.equal(previewBindings({ ...guest, profile: "cmd-to-ctrl" }).length, 2);
  assert.ok(previewBindings(guest).every((row) => row.sourceUsage !== 0xe6));
});

test("a cloned preset edits and resets only the selected guest draft", () => {
  const cloned = clonePreset(guest);
  assert.equal(cloned.profile, "custom");
  assert.equal(cloned.customBasePreset, "windows-to-mac");
  const edited = editBinding(cloned, 0xe0, 0xe4);
  assert.equal(previewBindings(edited)[0].targetUsage, 0xe4);
  assert.equal(previewBindings(guest)[0].targetUsage, 0xe3);
  assert.deepEqual(resetCustom(edited).modifierBindings, presetBindings("windows-to-mac"));
});
