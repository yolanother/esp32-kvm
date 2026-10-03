// Copyright (c) ESP32 KVM contributors. Use is governed by the repository LICENSE.
// These independent wire fixtures guard framing, corruption handling, and control
// replay semantics against changes to the protocol implementation.

use esp32_kvm_protocol::{
    BondInventory, Frame, FrameDecoder, MessageKind, ProtocolError, RetryCache, cobs_decode, crc32c,
};

#[test]
fn retained_bond_inventory_is_bounded_versioned_and_unique() {
    let tokens = vec![[1; 16], [2; 16]];
    let inventory = BondInventory::new(tokens.clone()).unwrap();
    let payload = inventory.encode();
    assert_eq!(payload.len(), 34);
    assert_eq!(BondInventory::decode(&payload).unwrap().tokens, tokens);
    assert_eq!(BondInventory::decode(&[2, 0]), Err(ProtocolError::Version));
    assert_eq!(BondInventory::decode(&[1, 1]), Err(ProtocolError::Payload));
    assert_eq!(BondInventory::new(vec![[0; 16]]), Err(ProtocolError::Payload));
    assert_eq!(BondInventory::new(vec![[1; 16]; 2]), Err(ProtocolError::Payload));
    assert_eq!(BondInventory::new(vec![[1; 16]; 9]), Err(ProtocolError::Payload));
    assert!(Frame::new(MessageKind::GetBonds, 5, 9, 0, vec![1]).encode().is_ok());
    assert!(Frame::new(MessageKind::Bonds, 5, 9, 0, payload).encode().is_ok());
}

#[test]
fn crc32c_standard_check_value() {
    assert_eq!(crc32c(b"123456789"), 0xe3069283);
}

#[test]
fn independent_fixtures_cover_every_message_kind() {
    let fixtures: &[(&str, u8)] = &[
        ("hello-v1", 0x01),
        ("hello-m1", 0x01),
        ("hello-m2", 0x01),
        ("caps", 0x02),
        ("caps-m1", 0x02),
        ("caps-m2", 0x02),
        ("session-open", 0x03),
        ("heartbeat", 0x10),
        ("get-status", 0x11),
        ("status", 0x12),
        ("status-pairing-m1", 0x12),
        ("switch", 0x20),
        ("release-all", 0x21),
        ("arm", 0x22),
        ("key-state", 0x30),
        ("pointer", 0x31),
        ("consumer-state", 0x32),
        ("pair-begin", 0x40),
        ("pair-cancel", 0x41),
        ("forget-bond", 0x42),
        ("pair-reply", 0x43),
        ("get-bonds", 0x44),
        ("bonds", 0x45),
        ("device-select-request", 0x50),
        ("update-prepare", 0x60),
        ("ack", 0x70),
        ("nack", 0x71),
        ("input-progress", 0x72),
    ];
    for (name, kind) in fixtures {
        let path = format!(
            "{}/../../tests/vectors/{name}.cobs",
            env!("CARGO_MANIFEST_DIR")
        );
        let wire = std::fs::read(path).unwrap();
        assert_eq!(wire.last(), Some(&0), "{name}");
        let frame = Frame::decode_decoded(&cobs_decode(&wire[..wire.len() - 1]).unwrap()).unwrap();
        assert_eq!(frame.kind as u8, *kind, "{name}");
        assert_eq!(frame.encode().unwrap(), wire, "{name}");
    }
}

#[test]
fn hello_golden_wire_round_trip_across_split_reads() {
    let frame = Frame::new(MessageKind::Hello, 0, 7, 0, vec![0, 0, 0, 0, 0, 0]);
    let encoded = frame.encode().unwrap();
    assert_eq!(
        encoded,
        include_bytes!("../../../tests/vectors/hello-v1.cobs")
    );
    let mut decoder = FrameDecoder::new();
    for byte in &encoded[..encoded.len() - 1] {
        assert!(decoder.push(*byte).is_none());
    }
    assert_eq!(decoder.push(0), Some(Ok(frame)));
}

#[test]
fn coalesced_frames_and_invalid_lengths_are_isolated() {
    let one = Frame::new(
        MessageKind::Heartbeat,
        5,
        1,
        2,
        42_u64.to_le_bytes().to_vec(),
    );
    let two = Frame::new(MessageKind::GetStatus, 5, 2, 2, vec![]);
    let mut decoder = FrameDecoder::new();
    let results: Vec<_> = one
        .encode()
        .unwrap()
        .into_iter()
        .chain(two.encode().unwrap())
        .filter_map(|byte| decoder.push(byte))
        .collect();
    assert_eq!(results, vec![Ok(one), Ok(two)]);

    // The payload length field is deliberately inconsistent with the body.
    let malformed = include_bytes!("../../../tests/vectors/bad-length.cobs").to_vec();
    let result = malformed
        .into_iter()
        .filter_map(|byte| decoder.push(byte))
        .next()
        .unwrap();
    assert_eq!(result, Err(ProtocolError::Length));
}

#[test]
fn stale_generation_and_duplicate_switch_are_rejected_without_reexecution() {
    let mut cache = RetryCache::new(8);
    assert_eq!(cache.observe(9, 10, MessageKind::Switch, 3, 3), Ok(true));
    assert_eq!(cache.observe(9, 10, MessageKind::Switch, 3, 3), Ok(false));
    assert_eq!(
        cache.observe(9, 11, MessageKind::Switch, 2, 3),
        Err(ProtocolError::StaleRoute)
    );
    assert_eq!(
        cache.observe(8, 12, MessageKind::Switch, 3, 3),
        Err(ProtocolError::StaleSession)
    );
    assert_eq!(
        cache.observe(9, 10, MessageKind::Arm, 3, 3),
        Err(ProtocolError::StaleSequence)
    );
}

#[test]
fn sequence_wrap_and_evicted_replay_follow_modular_order() {
    let mut cache = RetryCache::new(2);
    assert_eq!(
        cache.observe(9, u32::MAX - 1, MessageKind::Switch, 0, 0),
        Ok(true)
    );
    assert_eq!(cache.observe(9, u32::MAX, MessageKind::Arm, 0, 0), Ok(true));
    assert_eq!(cache.observe(9, 0, MessageKind::ReleaseAll, 0, 0), Ok(true));
    assert_eq!(
        cache.observe(9, u32::MAX - 1, MessageKind::Switch, 0, 0),
        Err(ProtocolError::StaleSequence)
    );
}

#[test]
fn canonical_cbor_rejects_missing_keys_duplicates_and_trailing_data() {
    let samples: &[&[u8]] = &[
        &[0xa0],                         // required fields absent
        &[0xa2, 1, 0x61, b'1', 1, 0],    // repeated key
        &[0xa2, 1, 0x61, b'1', 2, 0, 0], // trailing byte
        &[0xa2, 1, 0x78, 1, b'1', 2, 0], // non-minimal length
        &[0xa2, 1, 0x61, b'1', 2, 0x9f], // indefinite array
    ];
    for payload in samples {
        let frame = Frame::new(MessageKind::SessionOpen, 5, 1, 0, payload.to_vec());
        assert_eq!(frame.encode(), Err(ProtocolError::Payload));
    }
}

#[test]
fn minor_one_pairing_status_requires_complete_numeric_challenge() {
    let mut status = vec![
        0xa6, 1, 5, 2, 0, 3, 0x80, 4, 0, 5, 0, 6, 0xa4, 1, 2, 2, 0x19, 0xea, 0x60, 3, 7, 4, 0x1a,
        0x00, 0x01, 0xe2, 0x40,
    ];
    assert!(
        Frame::new(MessageKind::Status, 5, 1, 0, status.clone())
            .encode()
            .is_ok()
    );
    status[20] = 0; // Challenge ID zero is never valid.
    assert_eq!(
        Frame::new(MessageKind::Status, 5, 1, 0, status.clone()).encode(),
        Err(ProtocolError::Payload)
    );
    status[20] = 7;
    status.truncate(21);
    status[12] = 0xa3;
    assert_eq!(
        Frame::new(MessageKind::Status, 5, 1, 0, status).encode(),
        Err(ProtocolError::Payload)
    );
}

#[test]
fn forget_bond_rejects_zero_or_wrong_length_token() {
    let mut payload = vec![0xa1, 1, 0x50];
    payload.extend_from_slice(&[0; 16]);
    assert_eq!(
        Frame::new(MessageKind::ForgetBond, 5, 1, 0, payload.clone()).encode(),
        Err(ProtocolError::Payload)
    );
    payload[3] = 1;
    assert!(
        Frame::new(MessageKind::ForgetBond, 5, 1, 0, payload.clone())
            .encode()
            .is_ok()
    );
    payload[2] = 0x4f;
    payload.pop();
    assert_eq!(
        Frame::new(MessageKind::ForgetBond, 5, 1, 0, payload).encode(),
        Err(ProtocolError::Payload)
    );
}

#[test]
fn bounded_decoder_drains_oversize_and_recovers_after_delimiter() {
    let mut decoder = FrameDecoder::new();
    for _ in 0..1024 {
        assert!(decoder.push(1).is_none());
    }
    assert_eq!(decoder.push(0), Some(Err(ProtocolError::Length)));
    let valid = Frame::new(MessageKind::GetStatus, 5, 2, 0, vec![]);
    assert_eq!(
        valid
            .encode()
            .unwrap()
            .into_iter()
            .filter_map(|byte| decoder.push(byte))
            .next(),
        Some(Ok(valid))
    );
}

#[test]
fn deterministic_random_bytes_cannot_panic_and_later_frame_recovers() {
    let mut decoder = FrameDecoder::new();
    let mut state = 0x1234_5678_u32;
    for _ in 0..20_000 {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        decoder.push((state & 255) as u8);
    }
    decoder.push(0);
    let valid = Frame::new(MessageKind::GetStatus, 5, 3, 0, vec![]);
    assert_eq!(
        valid
            .encode()
            .unwrap()
            .into_iter()
            .filter_map(|byte| decoder.push(byte))
            .next(),
        Some(Ok(valid))
    );
}

#[test]
fn unplug_discards_partial_frame_and_crc_error_recovers() {
    let frame = Frame::new(MessageKind::GetStatus, 5, 3, 2, vec![]);
    let bytes = frame.encode().unwrap();
    let mut decoder = FrameDecoder::new();
    for byte in &bytes[..8] {
        decoder.push(*byte);
    }
    decoder.reset();
    for byte in &bytes[8..] {
        assert!(decoder.push(*byte).is_none() || *byte == 0);
    }
    let bad = include_bytes!("../../../tests/vectors/bad-crc.cobs");
    assert_eq!(
        bad.iter().filter_map(|byte| decoder.push(*byte)).next(),
        Some(Err(ProtocolError::Crc))
    );
    assert_eq!(
        bytes.iter().filter_map(|byte| decoder.push(*byte)).next(),
        Some(Ok(frame))
    );
}
