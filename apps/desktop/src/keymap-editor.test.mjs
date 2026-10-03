// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Verifies physical key rule validation, reserved escape protection, chord
// recording and local-only emitted-report simulation before UI integration.
import assert from "node:assert/strict";
import test from "node:test";
import { captureChordCodes, keymapConflicts, simulateKeymap } from "./keymap-editor.ts";

const guest = { bondToken: "ab".repeat(16), name: "Mac", os: "macos", profile: "windows-to-mac" };
const key = (usage, side = "unspecified") => ({ usage, side });
const rule = (source, target, priority = 0, enabled = true) => ({ source, target, priority, enabled });

test("recorder uses physical codes and preserves modifier side", () => {
  assert.deepEqual(captureChordCodes(["ControlLeft", "ShiftRight", "KeyS"]), [key(0xe0, "left"), key(0xe5, "right"), key(0x16)]);
  assert.deepEqual(captureChordCodes(["KeyA", "KeyA", "Unknown"]), [key(0x04)]);
});

test("same-priority duplicate and reserved two-Ctrl trigger block save", () => {
  const ctrl = key(0xe0, "left");
  const copy = rule([ctrl, key(0x06)], [key(0xe3, "left"), key(0x06)]);
  assert.match(keymapConflicts([copy, { ...copy, source: [...copy.source].reverse() }]).join(" "), /duplicate/i);
  assert.match(keymapConflicts([rule([ctrl, key(0xe4, "right")], [key(0x04)])]).join(" "), /emergency/i);
  assert.deepEqual(keymapConflicts([copy, { ...copy, priority: 1 }]), []);
});

test("local simulator applies exact chord once before single-key rule, without changing host input", () => {
  const ctrl = key(0xe0, "left");
  const c = key(0x06);
  const profile = { ...guest, keyRules: [
    rule([c], [key(0x19)]),
    rule([ctrl, c], [key(0xe3, "left"), c], 2),
  ] };
  const held = [ctrl, c];
  assert.deepEqual(simulateKeymap(profile, held), { modifiers: 0x08, keys: [0x06, 0, 0, 0, 0, 0] });
  assert.deepEqual(held, [ctrl, c]);
  assert.deepEqual(simulateKeymap({ ...profile, keyRules: profile.keyRules.map((r) => ({ ...r, enabled: false })) }, held), { modifiers: 0x08, keys: [0x06, 0, 0, 0, 0, 0] });
});

test("disabled rules pass through and a mapped output is never remapped recursively", () => {
  const a = key(0x04), b = key(0x05), c = key(0x06);
  const profile = { ...guest, profile: "unchanged", keyRules: [rule([a], [b]), rule([b], [c])] };
  assert.deepEqual(simulateKeymap(profile, [a]), { modifiers: 0, keys: [0x05, 0, 0, 0, 0, 0] });
  assert.deepEqual(simulateKeymap({ ...profile, keyRules: [{ ...profile.keyRules[0], enabled: false }] }, [a]), { modifiers: 0, keys: [0x04, 0, 0, 0, 0, 0] });
});
