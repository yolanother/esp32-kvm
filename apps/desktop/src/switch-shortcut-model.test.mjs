// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Specifies the Shortcuts page's physical cycle-binding choices, display, and
// invalid or reserved combinations used by the settings UI.
import assert from "node:assert/strict";
import test from "node:test";
import { defaultSwitchBinding, formatSwitchBinding, switchBindingFromKeyEvent, validateSwitchBinding } from "./switch-shortcut-model.ts";

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

test("shortcut recorder derives modifier bits from a physical key event", () => {
  assert.deepEqual(switchBindingFromKeyEvent({
    code: "KeyK", ctrlKey: true, altKey: true, shiftKey: false, metaKey: false,
  }), { trigger: "KeyK", modifiers: 3 });
  assert.equal(switchBindingFromKeyEvent({
    code: "ControlLeft", ctrlKey: true, altKey: false, shiftKey: false, metaKey: false,
  }), null);
  assert.deepEqual(switchBindingFromKeyEvent({
    code: "F8", ctrlKey: false, altKey: false, shiftKey: true, metaKey: true,
  }), { trigger: "F8", modifiers: 12 });
});
