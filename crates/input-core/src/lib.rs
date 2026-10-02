// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Exposes physical shortcut recognition and the native input routing policy
// independently of the Tauri webview and Windows capture adapter.
#![forbid(unsafe_code)]

mod hotkeys;
mod requests;
mod routing;

pub use hotkeys::{
    Action, HotkeyConfig, HotkeyMatcher, Key, KeyOutcome, Modifiers, Shortcut, ShortcutError,
};
pub use requests::RequestActor;
pub use routing::{Command, Router, SWITCH_TIMEOUT_MS, State};
