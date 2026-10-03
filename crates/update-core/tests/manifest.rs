// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Exercises strict firmware manifest parsing and board, protocol, size, and SHA-256 preflight.

use esp32_kvm_update_core::{
    DeviceIdentity, ManifestError, VerificationError, parse_manifest, verify_image,
};

const HASH_ABC: &str = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

fn manifest(hash: &str, size: u64) -> Vec<u8> {
    format!(r#"{{"schema":1,"board_id":"esp32-kvm-s3","protocol_major":1,"protocol_minor_min":0,"protocol_minor_max":1,"firmware_version":"0.2.0","partition":"app","image_size":{size},"image_sha256":"{hash}"}}"#).into_bytes()
}

fn device() -> DeviceIdentity {
    DeviceIdentity {
        board_id: "esp32-kvm-s3".into(),
        protocol_major: 1,
        protocol_minor: 1,
        app_partition_bytes: 0x400000,
        firmware_version: "0.1.0".into(),
        local: false,
        armed: true,
    }
}

#[test]
fn exact_board_protocol_size_and_digest_yield_an_app_only_plan() {
    let parsed = parse_manifest(&manifest(HASH_ABC, 3)).unwrap();
    let verified = verify_image(&parsed, b"abc", &device()).unwrap();
    assert_eq!(verified.firmware_version(), "0.2.0");
    assert_eq!(verified.image_len(), 3);
    assert_eq!(verified.partition(), "app");
}

#[test]
fn manifest_parser_rejects_unknown_duplicate_and_unsafe_fields() {
    let mut extra = String::from_utf8(manifest(HASH_ABC, 3)).unwrap();
    extra.insert_str(extra.len() - 1, ",\"erase_nvs\":true");
    assert_eq!(parse_manifest(extra.as_bytes()), Err(ManifestError::Schema));
    let duplicate = String::from_utf8(manifest(HASH_ABC, 3))
        .unwrap()
        .replace("\"schema\":1", "\"schema\":1,\"schema\":1");
    assert_eq!(
        parse_manifest(duplicate.as_bytes()),
        Err(ManifestError::Schema)
    );
    assert_eq!(
        parse_manifest(&vec![b' '; 4097]),
        Err(ManifestError::TooLarge)
    );
    let wrong_partition = String::from_utf8(manifest(HASH_ABC, 3))
        .unwrap()
        .replace("\"partition\":\"app\"", "\"partition\":\"nvs\"");
    assert_eq!(
        parse_manifest(wrong_partition.as_bytes()),
        Err(ManifestError::Schema)
    );
}

#[test]
fn preflight_rejects_wrong_board_protocol_partition_capacity_and_hash() {
    let parsed = parse_manifest(&manifest(HASH_ABC, 3)).unwrap();
    let mut identity = device();
    identity.board_id = "different".into();
    assert_eq!(
        verify_image(&parsed, b"abc", &identity),
        Err(VerificationError::Board)
    );
    identity = device();
    identity.protocol_major = 2;
    assert_eq!(
        verify_image(&parsed, b"abc", &identity),
        Err(VerificationError::Protocol)
    );
    identity = device();
    identity.app_partition_bytes = 2;
    assert_eq!(
        verify_image(&parsed, b"abc", &identity),
        Err(VerificationError::PartitionCapacity)
    );
    assert_eq!(
        verify_image(&parsed, b"ab", &device()),
        Err(VerificationError::Size)
    );
    assert_eq!(
        verify_image(&parsed, b"abd", &device()),
        Err(VerificationError::Digest)
    );
}
