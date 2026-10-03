// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Parses a bounded versioned JSON firmware manifest and verifies the app image
// against the connected device's board, protocol, partition capacity, and SHA-256.

use serde::Deserialize;
use sha2::{Digest, Sha256};

/// Maximum accepted manifest document size in bytes.
pub const MAX_MANIFEST_BYTES: usize = 4096;
/// Maximum app image size accepted before device partition checks.
pub const MAX_IMAGE_BYTES: u64 = 8 * 1024 * 1024;

/// One exact app-only release manifest; unknown and duplicate JSON keys fail parsing.
/// Construction is restricted to `parse_manifest`, including the app-only rule:
/// ```compile_fail
/// use esp32_kvm_update_core::Manifest;
/// let _unsafe_manifest = Manifest {
///     schema: 1, board_id: "esp32-kvm-s3".into(), protocol_major: 1,
///     protocol_minor_min: 0, protocol_minor_max: 1,
///     firmware_version: "0.2.0".into(), partition: "nvs".into(),
///     image_size: 9 * 1024 * 1024, image_sha256: "0".repeat(64),
/// };
/// ```
#[derive(Clone, Debug, Deserialize, Eq, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct Manifest {
    /// Manifest schema revision, currently one.
    schema: u8,
    /// Board ID advertised by verified USB CAPS.
    board_id: String,
    /// Incompatible protocol major required by the new image.
    protocol_major: u8,
    /// Minimum host-compatible protocol minor for this update.
    protocol_minor_min: u16,
    /// Maximum host-compatible protocol minor for this update.
    protocol_minor_max: u16,
    /// Exact firmware version expected after reboot.
    firmware_version: String,
    /// Must be `app`; no NVS or full-flash operation is expressed.
    partition: String,
    /// Exact app image byte count.
    image_size: u64,
    /// Lowercase hexadecimal SHA-256 of the app image bytes.
    image_sha256: String,
}

/// Facts obtained from a verified USB session and a board partition manifest.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DeviceIdentity {
    /// Verified firmware board ID.
    pub board_id: String,
    /// Verified protocol major.
    pub protocol_major: u8,
    /// Negotiated protocol minor.
    pub protocol_minor: u16,
    /// Verified writable app partition capacity; never infer from the COM port.
    pub app_partition_bytes: u64,
    /// Firmware version returned by CAPS.
    pub firmware_version: String,
    /// Whether STATUS reports local routing.
    pub local: bool,
    /// Whether STATUS reports an armed guest route.
    pub armed: bool,
}

/// A parsed manifest could not be safely used.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ManifestError {
    /// Document exceeds the bounded parser input.
    TooLarge,
    /// JSON shape, version, value, or app-only policy is invalid.
    Schema,
}

/// Preflight rejected a release artifact for this device.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum VerificationError {
    /// Manifest board ID differs from verified device identity.
    Board,
    /// Connected protocol is outside the declared compatible range.
    Protocol,
    /// Image exceeds the verified app partition capacity.
    PartitionCapacity,
    /// Image length differs from the manifest.
    Size,
    /// SHA-256 differs from the manifest.
    Digest,
}

/// Verified app-only artifact information retained across update phases.
/// This plan can only be built by `verify_image` after manifest and byte checks:
/// ```compile_fail
/// use esp32_kvm_update_core::VerifiedImage;
/// let _unverified = VerifiedImage {
///     firmware_version: "0.2.0".into(), image_len: 3,
///     image_sha256: [0; 32], partition: "nvs",
///     board_id: "esp32-kvm-s3".into(), protocol_major: 1,
///     protocol_minor_min: 0, protocol_minor_max: 1,
/// };
/// ```
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct VerifiedImage {
    /// Expected version after reconnect.
    pub(crate) firmware_version: String,
    /// Exact bytes that may be flashed to the app partition.
    pub(crate) image_len: usize,
    /// SHA-256 bytes verified at preflight.
    pub(crate) image_sha256: [u8; 32],
    /// Fixed safe partition target.
    pub(crate) partition: &'static str,
    /// Board ID expected again after re-enumeration.
    pub(crate) board_id: String,
    /// Compatible protocol major.
    pub(crate) protocol_major: u8,
    /// Lowest compatible minor after reboot.
    pub(crate) protocol_minor_min: u16,
    /// Highest compatible minor after reboot.
    pub(crate) protocol_minor_max: u16,
}

impl VerifiedImage {
    /// Returns the firmware version required on reconnect.
    pub fn firmware_version(&self) -> &str {
        &self.firmware_version
    }

    /// Returns the exact verified image byte count.
    pub fn image_len(&self) -> usize {
        self.image_len
    }

    /// Returns the only permitted flash partition.
    pub fn partition(&self) -> &str {
        self.partition
    }
}

/// Parses and validates a bounded, strict app-only JSON release manifest.
pub fn parse_manifest(bytes: &[u8]) -> Result<Manifest, ManifestError> {
    if bytes.len() > MAX_MANIFEST_BYTES {
        return Err(ManifestError::TooLarge);
    }
    let manifest: Manifest = serde_json::from_slice(bytes).map_err(|_| ManifestError::Schema)?;
    let safe_name = |name: &str| {
        !name.is_empty()
            && name.len() <= 32
            && name
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'-'))
    };
    if manifest.schema != 1
        || !safe_name(&manifest.board_id)
        || !safe_name(&manifest.firmware_version)
        || manifest.partition != "app"
        || manifest.protocol_major == 0
        || manifest.protocol_minor_min > manifest.protocol_minor_max
        || manifest.image_size == 0
        || manifest.image_size > MAX_IMAGE_BYTES
        || decode_digest(&manifest.image_sha256).is_none()
    {
        return Err(ManifestError::Schema);
    }
    Ok(manifest)
}

/// Checks an app image against the parsed manifest and independently verified device facts.
pub fn verify_image(
    manifest: &Manifest,
    image: &[u8],
    device: &DeviceIdentity,
) -> Result<VerifiedImage, VerificationError> {
    if manifest.board_id != device.board_id {
        return Err(VerificationError::Board);
    }
    if manifest.protocol_major != device.protocol_major
        || device.protocol_minor < manifest.protocol_minor_min
        || device.protocol_minor > manifest.protocol_minor_max
    {
        return Err(VerificationError::Protocol);
    }
    if manifest.image_size > device.app_partition_bytes {
        return Err(VerificationError::PartitionCapacity);
    }
    if image.len() as u64 != manifest.image_size {
        return Err(VerificationError::Size);
    }
    let expected = decode_digest(&manifest.image_sha256).ok_or(VerificationError::Digest)?;
    if Sha256::digest(image).as_slice() != expected {
        return Err(VerificationError::Digest);
    }
    Ok(VerifiedImage {
        firmware_version: manifest.firmware_version.clone(),
        image_len: image.len(),
        image_sha256: expected,
        partition: "app",
        board_id: manifest.board_id.clone(),
        protocol_major: manifest.protocol_major,
        protocol_minor_min: manifest.protocol_minor_min,
        protocol_minor_max: manifest.protocol_minor_max,
    })
}

fn decode_digest(value: &str) -> Option<[u8; 32]> {
    if value.len() != 64 {
        return None;
    }
    let mut bytes = [0_u8; 32];
    for (index, pair) in value.as_bytes().chunks_exact(2).enumerate() {
        let digit = |byte: u8| match byte {
            b'0'..=b'9' => Some(byte - b'0'),
            b'a'..=b'f' => Some(byte - b'a' + 10),
            _ => None,
        };
        bytes[index] = digit(pair[0])? << 4 | digit(pair[1])?;
    }
    Some(bytes)
}
