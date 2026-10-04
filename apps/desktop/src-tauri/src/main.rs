// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Starts the desktop shell, serial-owning actor and persistent system tray.
// Closing hides to tray; explicit quit disarms capture and closes the actor.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod actor_backend;
mod layout;
mod setup;
mod tray;

use std::sync::{Arc, Mutex};
use tauri::Manager;

fn main() {
    tauri::Builder::default()
        .setup(|app| {
            let layout = Arc::new(Mutex::new(layout::LayoutRuntime::default()));
            app.manage(Arc::clone(&layout));
            let directory = app.path().app_config_dir()?;
            app.manage(setup::SetupService::new(
                directory,
                Box::new(actor_backend::ActorBackend::start(layout)),
            ));
            tray::install(app)?;
            Ok(())
        })
        .on_window_event(|window, event| {
            if window.label() == "main"
                && let tauri::WindowEvent::CloseRequested { api, .. } = event
            {
                api.prevent_close();
                let _ = window.hide();
            }
        })
        .invoke_handler(tauri::generate_handler![
            setup::setup_snapshot,
            setup::setup_begin,
            setup::setup_cancel,
            setup::setup_confirm,
            setup::setup_save_profile,
            setup::setup_forget_guest,
            setup::setup_test_controls,
            setup::dashboard_return_local,
            setup::dashboard_select_guest,
            setup::dashboard_keep_awake_enabled,
            setup::dashboard_set_keep_awake,
            layout::layout_validate_draft,
            layout::layout_discover,
            layout::layout_apply,
            layout::layout_set_enabled,
        ])
        .run(tauri::generate_context!())
        .expect("failed to run ESP32 KVM desktop shell");
}
