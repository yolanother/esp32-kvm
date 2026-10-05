// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Specifies the Shortcuts page's physical cycle-binding choices, display, and
// invalid or reserved combinations before the settings UI is implemented.
import assert from "node:assert/strict";
import test from "node:test";
import { defaultSwitchBinding, formatSwitchBinding, validateSwitchBinding } from "./switch-shortcut-model.ts";

test("cycle shortcut defaults to the current physical Ctrl+Alt+F12 binding", () => {
  assert.deepEqual(defaultSwitchBinding, { trigger: "F12", modifiers: 3 });
  assert.equal(formatSwitchBinding(defaultSwitchBinding), "Ctrl+Alt+F12");
});

test("shortcut editor rejects missing modifiers and reserved routing chords", () => {
  assert.equal(validateSwitchBinding({ trigger: "F8", modifiers: 5 }), null);
  assert.notEqual(validateSwitchBinding({ trigger: "F8", modifiers: 0 }), null);
  assert.notEqual(validateSwitchBinding({ trigger: "F10", modifiers: 3 }), null);
  assert.notEqual(validateSwitchBinding({ trigger: "F11", modifiers: 3 }), null);
  assert.notEqual(validateSwitchBinding({ trigger: "Digit1", modifiers: 3 }), null);
  assert.notEqual(validateSwitchBinding({ trigger: "ControlLeft", modifiers: 3 }), null);
});
