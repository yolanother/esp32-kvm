// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Recognizes editable physical scan-code shortcuts before guest key mapping.
// It consumes activating key down/up events and reserves the both-Control
// hold as an emergency local-return gesture.

/// A host routing action produced by a physical shortcut or another control source.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Action {
    /// Advance through ready guests and the host in configured order.
    Next,
    /// Move backward through ready guests and the host.
    Previous,
    /// Return input to the Windows host immediately.
    Local,
    /// Select a specific configured guest slot.
    Direct(u8),
}

/// A physical set-one scan code and its extended-key flag.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Key {
    /// Scan code, excluding the extended prefix.
    pub scan: u16,
    /// Whether the key has the E0 extended prefix.
    pub extended: bool,
}

impl Key {
    fn index(self) -> Option<usize> {
        (self.scan <= 255).then_some(self.scan as usize + if self.extended { 256 } else { 0 })
    }

    fn modifier(self) -> Option<u8> {
        match self.scan {
            0x1d => Some(Modifiers::CTRL.0),
            0x38 => Some(Modifiers::ALT.0),
            0x2a | 0x36 if !self.extended => Some(Modifiers::SHIFT.0),
            0x5b | 0x5c if self.extended => Some(Modifiers::GUI.0),
            _ => None,
        }
    }
}

/// Logical modifier classes; either physical side satisfies a shortcut.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Modifiers(u8);

impl Modifiers {
    /// No modifiers.
    pub const NONE: Self = Self(0);
    /// Left or right Control.
    pub const CTRL: Self = Self(1);
    /// Left or right Alt.
    pub const ALT: Self = Self(2);
    /// Left or right Shift.
    pub const SHIFT: Self = Self(4);
    /// Left or right GUI/Windows.
    pub const GUI: Self = Self(8);

    /// Combines modifier classes for an exact shortcut match.
    pub const fn union(self, other: Self) -> Self {
        Self(self.0 | other.0)
    }
}

/// One editable shortcut with an exact modifier set and one non-modifier trigger.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Shortcut {
    /// Physical trigger that must be consumed before key mapping.
    pub trigger: Key,
    /// Required modifier classes; extra modifiers prevent activation.
    pub modifiers: Modifiers,
    /// Routing action triggered on the first key-down.
    pub action: Action,
}

/// Why a shortcut configuration cannot be installed.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ShortcutError {
    /// Two shortcuts have the same trigger and exact modifier set.
    Conflict,
    /// The trigger is a modifier or lacks a supported scan-code index.
    ModifierTrigger,
    /// Direct selection used slot zero, which is reserved for local control.
    InvalidSlot,
}

/// Validated, editable shortcut configuration.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct HotkeyConfig {
    shortcuts: Vec<Shortcut>,
}

impl HotkeyConfig {
    /// Validates shortcuts before they become active.
    pub fn try_new(shortcuts: Vec<Shortcut>) -> Result<Self, ShortcutError> {
        for (index, shortcut) in shortcuts.iter().enumerate() {
            if shortcut.trigger.index().is_none() || shortcut.trigger.modifier().is_some() {
                return Err(ShortcutError::ModifierTrigger);
            }
            if matches!(shortcut.action, Action::Direct(0)) {
                return Err(ShortcutError::InvalidSlot);
            }
            if shortcuts[..index].iter().any(|prior| {
                prior.trigger == shortcut.trigger && prior.modifiers == shortcut.modifiers
            }) {
                return Err(ShortcutError::Conflict);
            }
        }
        Ok(Self { shortcuts })
    }

    /// Returns the editable Ctrl+Alt+F12/F11/F10 and Ctrl+Alt+1/2/3 defaults.
    pub fn defaults() -> Self {
        let modifiers = Modifiers::CTRL.union(Modifiers::ALT);
        let shortcuts = [
            (0x58, Action::Next),
            (0x57, Action::Previous),
            (0x44, Action::Local),
            (0x02, Action::Direct(1)),
            (0x03, Action::Direct(2)),
            (0x04, Action::Direct(3)),
        ]
        .into_iter()
        .map(|(scan, action)| Shortcut {
            trigger: Key {
                scan,
                extended: false,
            },
            modifiers,
            action,
        })
        .collect();
        Self { shortcuts }
    }
}

/// Result of classifying one physical key event before guest mapping.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct KeyOutcome {
    /// Routing action to send to the serialized request actor.
    pub action: Option<Action>,
    /// Whether neither Windows nor the guest may receive this trigger event.
    pub consume: bool,
}

/// Tracks physical key state and detects shortcuts without translated text.
pub struct HotkeyMatcher {
    config: HotkeyConfig,
    held: [bool; 512],
    consumed: [bool; 512],
    both_ctrl_since: Option<u64>,
    emergency_fired: bool,
}

impl HotkeyMatcher {
    /// Creates a matcher with a validated shortcut configuration.
    pub fn new(config: HotkeyConfig) -> Self {
        Self {
            config,
            held: [false; 512],
            consumed: [false; 512],
            both_ctrl_since: None,
            emergency_fired: false,
        }
    }

    /// Replaces editable shortcuts while preserving physical held-key state.
    pub fn set_config(&mut self, config: HotkeyConfig) {
        self.config = config;
    }

    /// Classifies one physical key before mapping; repeats never retrigger.
    pub fn on_key(&mut self, key: Key, down: bool, repeat: bool, now_ms: u64) -> KeyOutcome {
        let Some(index) = key.index() else {
            return KeyOutcome {
                action: None,
                consume: false,
            };
        };
        if !down {
            self.held[index] = false;
            let consume = std::mem::take(&mut self.consumed[index]);
            self.update_emergency(now_ms);
            return KeyOutcome {
                action: None,
                consume,
            };
        }
        let first_down = !self.held[index] && !repeat;
        self.held[index] = true;
        self.update_emergency(now_ms);
        if self.consumed[index] {
            return KeyOutcome {
                action: None,
                consume: true,
            };
        }
        if first_down {
            let modifiers = self.modifiers();
            if let Some(shortcut) = self
                .config
                .shortcuts
                .iter()
                .find(|shortcut| shortcut.trigger == key && shortcut.modifiers == modifiers)
            {
                self.consumed[index] = true;
                return KeyOutcome {
                    action: Some(shortcut.action),
                    consume: true,
                };
            }
        }
        KeyOutcome {
            action: None,
            consume: false,
        }
    }

    /// Recognizes a one-second simultaneous hold of both physical Control keys.
    pub fn tick(&mut self, now_ms: u64) -> Option<Action> {
        if !self.emergency_fired
            && self
                .both_ctrl_since
                .is_some_and(|since| now_ms.saturating_sub(since) >= 1000)
        {
            self.emergency_fired = true;
            return Some(Action::Local);
        }
        None
    }

    fn modifiers(&self) -> Modifiers {
        let present =
            |scan: usize, extended: bool| self.held[scan + if extended { 256 } else { 0 }];
        let mut bits = 0;
        if present(0x1d, false) || present(0x1d, true) {
            bits |= Modifiers::CTRL.0;
        }
        if present(0x38, false) || present(0x38, true) {
            bits |= Modifiers::ALT.0;
        }
        if present(0x2a, false) || present(0x36, false) {
            bits |= Modifiers::SHIFT.0;
        }
        if present(0x5b, true) || present(0x5c, true) {
            bits |= Modifiers::GUI.0;
        }
        Modifiers(bits)
    }

    fn update_emergency(&mut self, now_ms: u64) {
        if self.held[0x1d] && self.held[0x1d + 256] {
            if self.both_ctrl_since.is_none() {
                self.both_ctrl_since = Some(now_ms);
            }
        } else {
            self.both_ctrl_since = None;
            self.emergency_fired = false;
        }
    }
}
