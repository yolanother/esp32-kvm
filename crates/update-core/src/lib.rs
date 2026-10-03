// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Verifies firmware update manifests and models release-first USB update/recovery
// transitions without opening a serial port or invoking a flasher.

#![forbid(unsafe_code)]

mod manifest;
mod transaction;
pub use manifest::{
    DeviceIdentity, Manifest, ManifestError, VerificationError, VerifiedImage, parse_manifest,
    verify_image,
};
pub use transaction::{UpdateAction, UpdateError, UpdateMachine, UpdateState};
