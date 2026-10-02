// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Reserves the Windows native adapter boundary for physical input capture, suppression, and recovery.
// The desktop webview must never serve as the timing-critical input path.
#![forbid(unsafe_code)]

/// Windows capture implementation will follow hardware and routing contracts.
pub const CAPTURE_IMPLEMENTED: bool = false;
