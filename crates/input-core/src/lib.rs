// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Exposes the native input routing state machine independently of the Tauri
// webview and Windows capture adapter.
#![forbid(unsafe_code)]

mod routing;

pub use routing::{Command, Router, SWITCH_TIMEOUT_MS, State};
