// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Starts the native desktop shell and one serial-owning setup actor thread.
// Pairing requests use that actor; challenge confirmation and HID tests remain closed.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod actor_backend;
mod setup;

use tauri::Manager;

fn main() {
    tauri::Builder::default()
        .setup(|app| {
            let directory = app.path().app_config_dir()?;
            app.manage(setup::SetupService::new(
                directory,
                Box::new(actor_backend::ActorBackend::start()),
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
