// Copyright (c) ESP32 KVM contributors. Use is governed by the repository LICENSE.
// Parses bounded canonical CBOR control maps and enforces the version-one key,
// type, and range contracts before variable payloads affect routing or pairing.

use crate::{MessageKind, ProtocolError};

#[derive(Debug)]
enum Value<'a> {
    Uint(u64),
    Text(&'a [u8]),
    Bytes(&'a [u8]),
    Bool,
    Array(Vec<Value<'a>>),
    Map(Vec<(u64, Value<'a>)>),
}

struct Reader<'a> {
    bytes: &'a [u8],
    at: usize,
}

impl<'a> Reader<'a> {
    fn byte(&mut self) -> Result<u8, ProtocolError> {
        let value = *self.bytes.get(self.at).ok_or(ProtocolError::Payload)?;
        self.at += 1;
        Ok(value)
    }

    fn take(&mut self, n: usize) -> Result<&'a [u8], ProtocolError> {
        let end = self.at.checked_add(n).ok_or(ProtocolError::Payload)?;
        let value = self.bytes.get(self.at..end).ok_or(ProtocolError::Payload)?;
        self.at = end;
        Ok(value)
    }

    fn argument(&mut self, additional: u8) -> Result<u64, ProtocolError> {
        match additional {
            0..=23 => Ok(u64::from(additional)),
            24 => {
                let n = u64::from(self.byte()?);
                if n < 24 {
                    Err(ProtocolError::Payload)
                } else {
                    Ok(n)
                }
            }
            25 => {
                let n = u64::from(u16::from_be_bytes(self.take(2)?.try_into().unwrap()));
                if n <= 255 {
                    Err(ProtocolError::Payload)
                } else {
                    Ok(n)
                }
            }
            26 => {
                let n = u64::from(u32::from_be_bytes(self.take(4)?.try_into().unwrap()));
                if n <= 65535 {
                    Err(ProtocolError::Payload)
                } else {
                    Ok(n)
                }
            }
            27 => {
                let n = u64::from_be_bytes(self.take(8)?.try_into().unwrap());
                if n <= u32::MAX as u64 {
                    Err(ProtocolError::Payload)
                } else {
                    Ok(n)
                }
            }
            _ => Err(ProtocolError::Payload),
        }
    }

    fn value(&mut self, depth: u8) -> Result<Value<'a>, ProtocolError> {
        if depth > 3 {
            return Err(ProtocolError::Payload);
        }
        let initial = self.byte()?;
        let major = initial >> 5;
        let additional = initial & 31;
        if major == 7 {
            return match additional {
                20 | 21 => Ok(Value::Bool),
                _ => Err(ProtocolError::Payload),
            };
        }
        let n = self.argument(additional)?;
        match major {
            0 => Ok(Value::Uint(n)),
            2 | 3 => {
                let bytes = self.take(usize::try_from(n).map_err(|_| ProtocolError::Payload)?)?;
                if major == 2 {
                    Ok(Value::Bytes(bytes))
                } else if bytes.contains(&0) || std::str::from_utf8(bytes).is_err() {
                    Err(ProtocolError::Payload)
                } else {
                    Ok(Value::Text(bytes))
                }
            }
            4 => {
                if n > 16 {
                    return Err(ProtocolError::Payload);
                }
                let mut items = Vec::with_capacity(n as usize);
                for _ in 0..n {
                    items.push(self.value(depth + 1)?);
                }
                Ok(Value::Array(items))
            }
            5 => {
                if n > 16 {
                    return Err(ProtocolError::Payload);
                }
                let mut entries = Vec::with_capacity(n as usize);
                let mut previous = None;
                for _ in 0..n {
                    let Value::Uint(key) = self.value(depth + 1)? else {
                        return Err(ProtocolError::Payload);
                    };
                    if previous.is_some_and(|prev| key <= prev) {
                        return Err(ProtocolError::Payload);
                    }
                    previous = Some(key);
                    entries.push((key, self.value(depth + 1)?));
                }
                Ok(Value::Map(entries))
            }
            _ => Err(ProtocolError::Payload),
        }
    }
}

fn map<'a>(
    value: &'a Value<'a>,
    required: &[u64],
    allowed: &[u64],
) -> Result<&'a [(u64, Value<'a>)], ProtocolError> {
    let Value::Map(entries) = value else {
        return Err(ProtocolError::Payload);
    };
    if required
        .iter()
        .any(|key| !entries.iter().any(|(found, _)| found == key))
        || entries.iter().any(|(key, _)| !allowed.contains(key))
    {
        return Err(ProtocolError::Payload);
    }
    Ok(entries)
}

fn field<'a>(entries: &'a [(u64, Value<'a>)], key: u64) -> Option<&'a Value<'a>> {
    entries
        .iter()
        .find(|(found, _)| *found == key)
        .map(|(_, value)| value)
}

fn keys_exact(entries: &[(u64, Value<'_>)], expected: &[u64]) -> Result<(), ProtocolError> {
    if entries.len() != expected.len()
        || entries
            .iter()
            .zip(expected)
            .any(|((key, _), wanted)| key != wanted)
    {
        return Err(ProtocolError::Payload);
    }
    Ok(())
}

fn uint(entries: &[(u64, Value<'_>)], key: u64, min: u64, max: u64) -> Result<u64, ProtocolError> {
    match field(entries, key) {
        Some(Value::Uint(n)) if (min..=max).contains(n) => Ok(*n),
        _ => Err(ProtocolError::Payload),
    }
}

fn text(entries: &[(u64, Value<'_>)], key: u64) -> Result<(), ProtocolError> {
    match field(entries, key) {
        Some(Value::Text(bytes)) if (1..=32).contains(&bytes.len()) => Ok(()),
        _ => Err(ProtocolError::Payload),
    }
}

fn bytes16(entries: &[(u64, Value<'_>)], key: u64) -> Result<(), ProtocolError> {
    match field(entries, key) {
        Some(Value::Bytes(bytes)) if bytes.len() == 16 => Ok(()),
        _ => Err(ProtocolError::Payload),
    }
}

fn boolean(entries: &[(u64, Value<'_>)], key: u64) -> Result<(), ProtocolError> {
    match field(entries, key) {
        Some(Value::Bool) => Ok(()),
        _ => Err(ProtocolError::Payload),
    }
}

/// Validates one complete variable payload against the major-one map contract.
pub(crate) fn validate(kind: MessageKind, payload: &[u8]) -> Result<(), ProtocolError> {
    if payload.is_empty() || payload.len() > crate::MAX_PAYLOAD {
        return Err(ProtocolError::Payload);
    }
    let mut reader = Reader {
        bytes: payload,
        at: 0,
    };
    let value = reader.value(0)?;
    if reader.at != payload.len() {
        return Err(ProtocolError::Payload);
    }
    match kind {
        MessageKind::Caps => {
            let fields = map(&value, &[1, 2, 3, 4, 5, 6, 7, 8], &[1, 2, 3, 4, 5, 6, 7, 8])?;
            text(fields, 1)?;
            text(fields, 2)?;
            let min = uint(fields, 3, 0, u16::MAX as u64)?;
            let max = uint(fields, 4, min, u16::MAX as u64)?;
            if min > max {
                return Err(ProtocolError::Payload);
            }
            uint(fields, 5, 1, 3)?;
            uint(fields, 6, 1, 8)?;
            uint(fields, 7, 0, u32::MAX as u64)?;
            uint(fields, 8, 1, u64::MAX)?;
        }
        MessageKind::SessionOpen => {
            let fields = map(&value, &[1, 2], &[1, 2])?;
            text(fields, 1)?;
            uint(fields, 2, 0, u64::MAX)?;
        }
        MessageKind::Status => {
            let fields = map(&value, &[1, 2, 3, 4, 5], &[1, 2, 3, 4, 5, 6])?;
            uint(fields, 1, 0, 6)?;
            uint(fields, 2, 0, 3)?;
            let Some(Value::Array(slots)) = field(fields, 3) else {
                return Err(ProtocolError::Payload);
            };
            if slots.len() > 3 {
                return Err(ProtocolError::Payload);
            }
            let mut previous_slot = 0;
            for slot in slots {
                let fields = map(slot, &[1, 2, 3, 4, 5], &[1, 2, 3, 4, 5])?;
                let number = uint(fields, 1, 1, 3)?;
                if number <= previous_slot {
                    return Err(ProtocolError::Payload);
                }
                previous_slot = number;
                bytes16(fields, 2)?;
                boolean(fields, 3)?;
                boolean(fields, 4)?;
                uint(fields, 5, 0, u16::MAX as u64)?;
            }
            uint(fields, 4, 0, u32::MAX as u64)?;
            uint(fields, 5, 0, u32::MAX as u64)?;
            if let Some(pairing) = field(fields, 6) {
                let pairing = map(pairing, &[1], &[1, 2, 3, 4])?;
                let state = uint(pairing, 1, 0, 5)?;
                match state {
                    1 => {
                        keys_exact(pairing, &[1, 2])?;
                        uint(pairing, 2, 1, 60_000)?;
                    }
                    2 => {
                        keys_exact(pairing, &[1, 2, 3, 4])?;
                        uint(pairing, 2, 1, 60_000)?;
                        uint(pairing, 3, 1, u32::MAX as u64)?;
                        uint(pairing, 4, 0, 999_999)?;
                    }
                    _ if pairing.len() != 1 => return Err(ProtocolError::Payload),
                    _ => {}
                }
            }
        }
        MessageKind::ForgetBond => {
            bytes16(map(&value, &[1], &[1])?, 1)?;
        }
        MessageKind::PairReply => {
            let fields = map(&value, &[1, 2, 3], &[1, 2, 3])?;
            uint(fields, 1, 1, u32::MAX as u64)?;
            uint(fields, 2, 0, 0)?;
            boolean(fields, 3)?;
        }
        _ => return Err(ProtocolError::Payload),
    }
    Ok(())
}
