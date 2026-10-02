// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Starts the native desktop shell. Future capture and routing workers run in Rust, outside the webview.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    tauri::Builder::default()
        .run(tauri::generate_context!())
        .expect("failed to run ESP32 KVM desktop shell");
}
