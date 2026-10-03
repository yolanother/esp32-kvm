// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Holds an optional helper enrollment pending explicit matching code approval on both machines.
// It passes only a public identity binding to the credential owner and contains no private key.

use crate::{AuthenticatedChannel, TrustedBinding};

/// Persistence boundary for approved public bindings and revocation.
/// Production implementations must use OS credential storage and close active sessions on revoke.
pub trait TrustedCredentialStore {
    /// Store error type, never containing private credential material.
    type Error;

    /// Persist an approved profile, BLE bond, and helper public fingerprint binding.
    fn save(&mut self, binding: TrustedBinding) -> Result<(), Self::Error>;

    /// Remove the approved binding and invalidate all live sessions for this profile.
    fn revoke(&mut self, profile_id: u128) -> Result<(), Self::Error>;
}

/// Why enrollment did not produce a trusted binding.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum EnrollmentError {
    /// The secure channel has not proved encryption and peer identity.
    Untrusted,
    /// Comparison code is outside the six-digit range.
    InvalidCode,
    /// A profile, BLE bond, or helper public fingerprint is missing.
    InvalidBinding,
    /// An approval came from a different verified peer or connection.
    WrongPeer,
    /// An operator saw a different comparison code.
    CodeMismatch,
    /// Both machines have not explicitly approved the same code.
    ApprovalMissing,
    /// Enrollment was canceled or already committed.
    Closed,
    /// The credential owner could not persist the approved binding.
    StoreFailed,
}

/// Pending explicit enrollment; Debug redacts the comparison code and bond token.
#[derive(Eq, PartialEq)]
pub struct Enrollment {
    binding: TrustedBinding,
    session_id: [u8; 16],
    code: u32,
    local_approved: bool,
    remote_approved: bool,
    closed: bool,
}

impl std::fmt::Debug for Enrollment {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("Enrollment")
            .field("binding", &self.binding)
            .field("code", &"<redacted>")
            .field("local_approved", &self.local_approved)
            .field("remote_approved", &self.remote_approved)
            .field("closed", &self.closed)
            .finish()
    }
}

impl Enrollment {
    /// Starts a proposed binding after a verified encrypted handshake supplied a comparison code.
    pub fn begin(
        profile_id: u128,
        bond_token: [u8; 16],
        channel: &impl AuthenticatedChannel,
        comparison_code: u32,
    ) -> Result<Self, EnrollmentError> {
        if !channel.is_encrypted_and_peer_verified() || channel.session_id() == [0; 16] {
            return Err(EnrollmentError::Untrusted);
        }
        if comparison_code > 999_999 {
            return Err(EnrollmentError::InvalidCode);
        }
        let version = channel.protocol_version();
        let binding = TrustedBinding::new(
            profile_id,
            bond_token,
            channel.peer_identity(),
            version,
            version,
        )
        .map_err(|_| EnrollmentError::InvalidBinding)?;
        Ok(Self {
            binding,
            session_id: channel.session_id(),
            code: comparison_code,
            local_approved: false,
            remote_approved: false,
            closed: false,
        })
    }

    /// Records explicit approval of the displayed code on this host.
    pub fn confirm_local(&mut self, observed_code: u32) -> Result<(), EnrollmentError> {
        self.check_code(observed_code)?;
        self.local_approved = true;
        Ok(())
    }

    /// Records explicit remote approval only from the same verified secure connection.
    pub fn confirm_remote(
        &mut self,
        observed_code: u32,
        channel: &impl AuthenticatedChannel,
    ) -> Result<(), EnrollmentError> {
        if self.closed {
            return Err(EnrollmentError::Closed);
        }
        if !channel.is_encrypted_and_peer_verified() {
            return Err(EnrollmentError::Untrusted);
        }
        if channel.session_id() != self.session_id
            || channel.peer_identity() != self.binding.helper_identity()
            || channel.protocol_version() != self.binding.min_version()
        {
            return Err(EnrollmentError::WrongPeer);
        }
        self.check_code(observed_code)?;
        self.remote_approved = true;
        Ok(())
    }

    /// Persists the public binding only after both approvals; a storage failure is retryable.
    pub fn commit(
        &mut self,
        store: &mut impl TrustedCredentialStore,
    ) -> Result<(), EnrollmentError> {
        if self.closed {
            return Err(EnrollmentError::Closed);
        }
        if !self.local_approved || !self.remote_approved {
            return Err(EnrollmentError::ApprovalMissing);
        }
        store
            .save(self.binding)
            .map_err(|_| EnrollmentError::StoreFailed)?;
        self.closed = true;
        Ok(())
    }

    /// Cancels pending approval without storing a binding.
    pub fn cancel(&mut self) {
        self.closed = true;
    }

    fn check_code(&self, observed_code: u32) -> Result<(), EnrollmentError> {
        if self.closed {
            return Err(EnrollmentError::Closed);
        }
        if observed_code != self.code {
            return Err(EnrollmentError::CodeMismatch);
        }
        Ok(())
    }
}
