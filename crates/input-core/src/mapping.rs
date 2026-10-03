// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Applies per-guest physical HID key, sided-modifier, and exact-chord rules once.
// Tracks all held sources and derives reference-counted six-key HID state after each transition.

use std::collections::{BTreeMap, BTreeSet};

/// Physical side of a key as identified before guest mapping.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq, Ord, PartialOrd)]
pub enum Side {
    /// Side does not distinguish this key.
    #[default]
    Unspecified,
    /// Left physical key.
    Left,
    /// Right physical key.
    Right,
}

/// One physical key represented by USB HID usage and optional location.
#[derive(Clone, Copy, Debug, Eq, PartialEq, Ord, PartialOrd)]
pub struct SourceKey {
    /// USB HID keyboard usage obtained from the physical scan code.
    pub usage: u8,
    /// Physical side for a sided rule.
    pub side: Side,
}

/// One destination in a guest keyboard report.
#[derive(Clone, Copy, Debug, Eq, PartialEq, Ord, PartialOrd)]
pub enum Destination {
    /// Nonzero USB keyboard usage.
    Usage(u8),
    /// One nonzero bit of the USB modifier byte.
    Modifier(u8),
}

/// One enabled physical source set mapped to a destination chord.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct MappingRule {
    /// Exactly these physical sources must be held for a chord; one source is a key rule.
    pub source: Vec<SourceKey>,
    /// Keys and modifiers emitted together when this rule is active.
    pub target: Vec<Destination>,
    /// Larger priority wins among otherwise equal-specificity rules.
    pub priority: i16,
    /// Disabled rules are ignored without deleting their saved definition.
    pub enabled: bool,
}

/// Selected preset plus explicit guest overrides.
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct MappingProfile {
    /// Lower-precedence preset rules.
    pub preset: Vec<MappingRule>,
    /// Guest rules, which override preset rules for the same physical trigger.
    pub rules: Vec<MappingRule>,
}

/// Direction chosen by the user for the built-in physical modifier preset.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MappingPreset {
    /// Emit the physical modifiers unchanged.
    Unchanged,
    /// Apple Command or Windows GUI sends Control to the guest.
    CmdToCtrl,
    /// Windows Control sends Command or GUI to a Mac guest.
    WindowsToMac,
}

/// Builds a validated built-in mapping without reversing the selected direction.
pub fn preset_profile(preset: MappingPreset) -> MappingProfile {
    let sources: &[(u8, Side, u8)] = match preset {
        MappingPreset::Unchanged => &[],
        MappingPreset::CmdToCtrl => &[(0xe3, Side::Left, 0x01), (0xe7, Side::Right, 0x10)],
        MappingPreset::WindowsToMac => &[(0xe0, Side::Left, 0x08), (0xe4, Side::Right, 0x80)],
    };
    MappingProfile {
        preset: sources
            .iter()
            .map(|&(usage, side, bit)| MappingRule {
                source: vec![SourceKey { usage, side }],
                target: vec![Destination::Modifier(bit)],
                priority: 0,
                enabled: true,
            })
            .collect(),
        rules: Vec::new(),
    }
}

/// Invalid mapping data that must not replace the active profile.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MappingError {
    /// A rule has no source or contains the same source twice.
    InvalidSource,
    /// A destination is zero or a modifier with multiple bits.
    InvalidDestination,
    /// Two enabled rules in one layer have identical sources and priority.
    DuplicateTrigger,
}

/// Full eight-byte boot-style keyboard state.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct KeyboardReport {
    /// USB modifier bits.
    pub modifiers: u8,
    /// Six non-modifier usages, or six rollover indicators when over capacity.
    pub keys: [u8; 6],
}

impl KeyboardReport {
    /// Serializes the USB KEY_STATE payload.
    pub fn bytes(self) -> [u8; 8] {
        let mut result = [0; 8];
        result[0] = self.modifiers;
        result[2..].copy_from_slice(&self.keys);
        result
    }
}

/// Stateful per-route mapper with deferred edits and physical press tracking.
pub struct MappingEngine {
    profile: MappingProfile,
    pending_profile: Option<MappingProfile>,
    held: BTreeSet<SourceKey>,
    report: KeyboardReport,
}

impl MappingEngine {
    /// Validates a profile before it can emit guest input.
    pub fn new(profile: MappingProfile) -> Result<Self, MappingError> {
        validate(&profile)?;
        Ok(Self {
            profile,
            pending_profile: None,
            held: BTreeSet::new(),
            report: KeyboardReport::default(),
        })
    }

    /// Defers a valid profile edit until every physical key is released.
    pub fn set_profile(&mut self, profile: MappingProfile) -> Result<(), MappingError> {
        validate(&profile)?;
        if self.held.is_empty() {
            self.profile = profile;
            self.pending_profile = None;
        } else {
            self.pending_profile = Some(profile);
        }
        Ok(())
    }

    /// Records a first physical press; repeated down events leave state unchanged.
    pub fn press(&mut self, key: SourceKey) -> KeyboardReport {
        if key.usage != 0 && self.held.insert(key) {
            self.recompute();
        }
        self.report
    }

    /// Releases one physical source and its remembered mapping contribution.
    pub fn release(&mut self, key: SourceKey) -> KeyboardReport {
        if self.held.remove(&key) {
            self.recompute();
            if self.held.is_empty()
                && let Some(profile) = self.pending_profile.take()
            {
                self.profile = profile;
            }
        }
        self.report
    }

    /// Returns the current full keyboard report.
    pub fn report(&self) -> KeyboardReport {
        self.report
    }

    /// Whether the mapper has no physical sources held.
    pub fn all_up(&self) -> bool {
        self.held.is_empty()
    }

    /// Clears old route output; the capture gate must independently prove all-up before arming.
    pub fn reset_for_route(&mut self) {
        self.held.clear();
        self.report = KeyboardReport::default();
        if let Some(profile) = self.pending_profile.take() {
            self.profile = profile;
        }
    }

    fn recompute(&mut self) {
        let mut counts = BTreeMap::<Destination, usize>::new();
        // An exact chord owns the complete physical set. Extra keys disable it.
        let chord = self
            .profile
            .rules
            .iter()
            .filter(|r| r.enabled && r.source.len() > 1 && source_matches(&r.source, &self.held))
            .max_by_key(|r| r.priority)
            .or_else(|| {
                self.profile
                    .preset
                    .iter()
                    .filter(|r| {
                        r.enabled && r.source.len() > 1 && source_matches(&r.source, &self.held)
                    })
                    .max_by_key(|r| r.priority)
            });
        if let Some(rule) = chord {
            for target in &rule.target {
                *counts.entry(*target).or_default() += 1;
            }
        } else {
            for key in &self.held {
                // Windows reports many AltGr presses as left Control followed by right Alt.
                // Preserve that physical pair, including while a printable key is held.
                if key.usage == 0xe0 && self.held.iter().any(|held| held.usage == 0xe6) {
                    *counts.entry(Destination::Modifier(0x01)).or_default() += 1;
                    continue;
                }
                let selected = self
                    .profile
                    .rules
                    .iter()
                    .filter(|r| r.enabled && r.source.len() == 1 && r.source[0] == *key)
                    .max_by_key(|r| r.priority)
                    .or_else(|| {
                        self.profile
                            .preset
                            .iter()
                            .filter(|r| r.enabled && r.source.len() == 1 && r.source[0] == *key)
                            .max_by_key(|r| r.priority)
                    });
                if let Some(rule) = selected {
                    for target in &rule.target {
                        *counts.entry(*target).or_default() += 1;
                    }
                } else {
                    let identity = if (0xe0..=0xe7).contains(&key.usage) {
                        Destination::Modifier(1 << (key.usage - 0xe0))
                    } else {
                        Destination::Usage(key.usage)
                    };
                    *counts.entry(identity).or_default() += 1;
                }
            }
        }
        let mut report = KeyboardReport::default();
        let mut usages = Vec::new();
        for (destination, count) in counts {
            if count == 0 {
                continue;
            }
            match destination {
                Destination::Modifier(bit) => report.modifiers |= bit,
                Destination::Usage(usage) => usages.push(usage),
            }
        }
        if usages.len() > 6 {
            report.keys = [1; 6];
        } else {
            report.keys[..usages.len()].copy_from_slice(&usages);
        }
        self.report = report;
    }
}

fn source_matches(source: &[SourceKey], held: &BTreeSet<SourceKey>) -> bool {
    source.len() == held.len() && source.iter().all(|key| held.contains(key))
}

fn validate(profile: &MappingProfile) -> Result<(), MappingError> {
    for layer in [&profile.preset, &profile.rules] {
        for (index, rule) in layer.iter().enumerate().filter(|(_, r)| r.enabled) {
            if rule.source.is_empty()
                || rule.source.iter().any(|key| key.usage == 0)
                || rule.source.iter().collect::<BTreeSet<_>>().len() != rule.source.len()
            {
                return Err(MappingError::InvalidSource);
            }
            if rule.target.iter().any(|target| match target {
                Destination::Usage(usage) => *usage == 0 || (0xe0..=0xe7).contains(usage),
                Destination::Modifier(bit) => !bit.is_power_of_two(),
            }) {
                return Err(MappingError::InvalidDestination);
            }
            if layer[..index].iter().any(|earlier| {
                earlier.enabled
                    && earlier.priority == rule.priority
                    && earlier.source.len() == rule.source.len()
                    && earlier.source.iter().all(|key| rule.source.contains(key))
            }) {
                return Err(MappingError::DuplicateTrigger);
            }
        }
    }
    Ok(())
}
