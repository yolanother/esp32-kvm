// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Maps physical macOS ANSI virtual keycodes to USB HID usages for the shared host actor.
// Unrecognized keys are omitted rather than translated through the active text layout.

use esp32_kvm_host_actor::{KeyMapper, MappedKey};

/// Physical ANSI keycode translator for macOS keyboards.
pub struct MacKeyMapper;

impl KeyMapper for MacKeyMapper {
    fn map_key(&self, _virtual_key: u32, scan_code: u32, _extended: bool) -> Option<MappedKey> {
        let modifier = match scan_code {
            59 => Some(0x01), // left control
            56 => Some(0x02), // left shift
            58 => Some(0x04), // left option
            55 => Some(0x08), // left command
            62 => Some(0x10), // right control
            60 => Some(0x20), // right shift
            61 => Some(0x40), // right option
            54 => Some(0x80), // right command
            _ => None,
        };
        if let Some(bit) = modifier {
            return Some(MappedKey::Modifier(bit));
        }
        let usage = match scan_code {
            0 => 0x04,
            11 => 0x05,
            8 => 0x06,
            2 => 0x07,
            14 => 0x08,
            3 => 0x09,
            5 => 0x0a,
            4 => 0x0b,
            34 => 0x0c,
            38 => 0x0d,
            40 => 0x0e,
            37 => 0x0f,
            46 => 0x10,
            45 => 0x11,
            31 => 0x12,
            35 => 0x13,
            12 => 0x14,
            15 => 0x15,
            1 => 0x16,
            17 => 0x17,
            32 => 0x18,
            9 => 0x19,
            13 => 0x1a,
            7 => 0x1b,
            16 => 0x1c,
            6 => 0x1d,
            18 => 0x1e,
            19 => 0x1f,
            20 => 0x20,
            21 => 0x21,
            23 => 0x22,
            22 => 0x23,
            26 => 0x24,
            28 => 0x25,
            25 => 0x26,
            29 => 0x27,
            36 => 0x28,
            53 => 0x29,
            51 => 0x2a,
            48 => 0x2b,
            49 => 0x2c,
            27 => 0x2d,
            24 => 0x2e,
            33 => 0x2f,
            30 => 0x30,
            42 => 0x31,
            41 => 0x33,
            39 => 0x34,
            50 => 0x35,
            43 => 0x36,
            47 => 0x37,
            44 => 0x38,
            57 => 0x39,
            122 => 0x3a,
            120 => 0x3b,
            99 => 0x3c,
            118 => 0x3d,
            96 => 0x3e,
            97 => 0x3f,
            98 => 0x40,
            100 => 0x41,
            101 => 0x42,
            109 => 0x43,
            103 => 0x44,
            111 => 0x45,
            105 => 0x49,
            115 => 0x4a,
            116 => 0x4b,
            117 => 0x4c,
            119 => 0x4d,
            121 => 0x4e,
            123 => 0x50,
            124 => 0x4f,
            126 => 0x52,
            125 => 0x51,
            _ => return None,
        };
        Some(MappedKey::Usage(usage))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn physical_a_and_modifiers_map_without_text_translation() {
        let mapper = MacKeyMapper;
        assert_eq!(mapper.map_key(0, 0, false), Some(MappedKey::Usage(4)));
        assert_eq!(mapper.map_key(55, 55, false), Some(MappedKey::Modifier(8)));
        assert_eq!(mapper.map_key(62, 62, false), Some(MappedKey::Modifier(16)));
        assert_eq!(mapper.map_key(999, 999, false), None);
    }
}
