// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Defines physical key choices, labels, and form validation for the Windows
// host's editable cycle-to-next shortcut; native capture remains authoritative.

/** One physical cycle binding; modifier bits are Ctrl=1, Alt=2, Shift=4, Win=8. */
export interface SwitchBinding { trigger: string; modifiers: number }

/** Current cycle shortcut used until the user saves a different binding. */
export const defaultSwitchBinding: SwitchBinding = { trigger: "F12", modifiers: 3 };

/** Physical non-modifier keys supported by the native set-one lookup. */
export const switchTriggerOptions = [
  ...Array.from({ length: 12 }, (_, index) => ({ code: `F${index + 1}`, label: `F${index + 1}` })),
  ..."ABCDEFGHIJKLMNOPQRSTUVWXYZ".split("").map((letter) => ({ code: `Key${letter}`, label: letter })),
  ..."0123456789".split("").map((digit) => ({ code: `Digit${digit}`, label: digit })),
];

/** Keyboard state needed to turn a physical key press into a switch binding. */
export interface SwitchKeyEvent {
  code: string;
  ctrlKey: boolean;
  altKey: boolean;
  shiftKey: boolean;
  metaKey: boolean;
}

const modifierCodes = new Set([
  "ControlLeft", "ControlRight", "AltLeft", "AltRight",
  "ShiftLeft", "ShiftRight", "MetaLeft", "MetaRight",
]);

/** Returns a binding once a non-modifier key is pressed, or null while modifiers are held. */
export function switchBindingFromKeyEvent(event: SwitchKeyEvent): SwitchBinding | null {
  if (modifierCodes.has(event.code)) return null;
  return {
    trigger: event.code,
    modifiers: (event.ctrlKey ? 1 : 0)
      | (event.altKey ? 2 : 0)
      | (event.shiftKey ? 4 : 0)
      | (event.metaKey ? 8 : 0),
  };
}

/** Explains invalid or reserved bindings before asking the native service to save. */
export function validateSwitchBinding(binding: SwitchBinding): string | null {
  if (!Number.isInteger(binding.modifiers) || binding.modifiers < 1 || binding.modifiers > 15) return "Choose at least one modifier.";
  if (!switchTriggerOptions.some((option) => option.code === binding.trigger)) return "Choose a supported physical key.";
  if (binding.modifiers === 3 && ["F10", "F11", "Digit1", "Digit2", "Digit3"].includes(binding.trigger)) {
    return "That combination is reserved for another routing action.";
  }
  return null;
}

/** Displays the exact modifier set and physical trigger shown in the editor. */
export function formatSwitchBinding(binding: SwitchBinding): string {
  const parts = [
    binding.modifiers & 1 ? "Ctrl" : null,
    binding.modifiers & 2 ? "Alt" : null,
    binding.modifiers & 4 ? "Shift" : null,
    binding.modifiers & 8 ? "Win" : null,
    switchTriggerOptions.find((option) => option.code === binding.trigger)?.label ?? binding.trigger,
  ];
  return parts.filter(Boolean).join("+");
}
