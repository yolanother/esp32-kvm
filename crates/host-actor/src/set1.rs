// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Converts known Windows set-one physical scan codes to USB HID keyboard usages.
// Unknown or synthetic scan sequences are omitted rather than inferred from text or virtual keys.

use crate::{KeyMapper, MappedKey};

/// Conservative physical scan-code translator for a standard Windows keyboard.
pub struct SetOneKeyMapper;

impl KeyMapper for SetOneKeyMapper {
    fn map_key(&self, virtual_key: u32, scan_code: u32, extended: bool) -> Option<MappedKey> {
        // Pause can share Num Lock's base scan code after the E1 sequence.
        if virtual_key == 0x13 {
            return None;
        }
        let modifier = match (scan_code, extended) {
            (0x1d, false) => Some(1),
            (0x2a, false) => Some(2),
            (0x38, false) => Some(4),
            (0x5b, true) => Some(8),
            (0x1d, true) => Some(0x10),
            (0x36, false) => Some(0x20),
            (0x38, true) => Some(0x40),
            (0x5c, true) => Some(0x80),
            _ => None,
        };
        if let Some(bit) = modifier {
            return Some(MappedKey::Modifier(bit));
        }
        let usage = if extended {
            match scan_code {
                0x1c => 0x58,
                0x35 => 0x54,
                0x47 => 0x4a,
                0x48 => 0x52,
                0x49 => 0x4b,
                0x4b => 0x50,
                0x4d => 0x4f,
                0x4f => 0x4d,
                0x50 => 0x51,
                0x51 => 0x4e,
                0x52 => 0x49,
                0x53 => 0x4c,
                0x5d => 0x65,
                _ => return None,
            }
        } else {
            match scan_code {
                0x01 => 0x29,
                0x02..=0x0b => 0x1e + (scan_code as u8 - 0x02),
                0x0c => 0x2d,
                0x0d => 0x2e,
                0x0e => 0x2a,
                0x0f => 0x2b,
                0x10 => 0x14,
                0x11 => 0x1a,
                0x12 => 0x08,
                0x13 => 0x15,
                0x14 => 0x17,
                0x15 => 0x1c,
                0x16 => 0x18,
                0x17 => 0x0c,
                0x18 => 0x12,
                0x19 => 0x13,
                0x1a => 0x2f,
                0x1b => 0x30,
                0x1c => 0x28,
                0x1e => 0x04,
                0x1f => 0x16,
                0x20 => 0x07,
                0x21 => 0x09,
                0x22 => 0x0a,
                0x23 => 0x0b,
                0x24 => 0x0d,
                0x25 => 0x0e,
                0x26 => 0x0f,
                0x27 => 0x33,
                0x28 => 0x34,
                0x29 => 0x35,
                0x2b => 0x31,
                0x2c => 0x1d,
                0x2d => 0x1b,
                0x2e => 0x06,
                0x2f => 0x19,
                0x30 => 0x05,
                0x31 => 0x11,
                0x32 => 0x10,
                0x33 => 0x36,
                0x34 => 0x37,
                0x35 => 0x38,
                0x37 => 0x55,
                0x39 => 0x2c,
                0x3a => 0x39,
                0x3b..=0x44 => 0x3a + (scan_code as u8 - 0x3b),
                0x45 => 0x53,
                0x46 => 0x47,
                0x47 => 0x5f,
                0x48 => 0x60,
                0x49 => 0x61,
                0x4a => 0x56,
                0x4b => 0x5c,
                0x4c => 0x5d,
                0x4d => 0x5e,
                0x4e => 0x57,
                0x4f => 0x59,
                0x50 => 0x5a,
                0x51 => 0x5b,
                0x52 => 0x62,
                0x53 => 0x63,
                0x56 => 0x64,
                0x57 => 0x44,
                0x58 => 0x45,
                _ => return None,
            }
        };
        Some(MappedKey::Usage(usage))
    }
}
