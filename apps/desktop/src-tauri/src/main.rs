// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Starts the native desktop shell and fail-closed setup command surface.
// The serial-owning host actor must be installed before live pairing is enabled.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod setup;

use tauri::Manager;

fn main() {
    tauri::Builder::default()
        .setup(|app| {
            let directory = app.path().app_config_dir()?;
            app.manage(setup::SetupService::new(
                directory,
                Box::new(setup::CandidateBackend),
            ));
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            setup::setup_snapshot,
            setup::setup_begin,
            setup::setup_cancel,
            setup::setup_confirm,
            setup::setup_save_profile,
            setup::setup_test_controls,
        ])
        .run(tauri::generate_context!())
        .expect("failed to run ESP32 KVM desktop shell");
}
