// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Starts the desktop shell, serial-owning actor and persistent system tray.
// Closing hides to tray; explicit quit disarms capture and closes the actor.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod actor_backend;
mod layout;
mod setup;
mod switch_shortcut;
mod tray;

use std::sync::{Arc, Mutex};
use tauri::Manager;

#[cfg(windows)]
struct InstanceMutex(windows_sys::Win32::Foundation::HANDLE);

#[cfg(windows)]
impl InstanceMutex {
    fn acquire() -> Option<Self> {
        use windows_sys::Win32::Foundation::{
            ERROR_ALREADY_EXISTS, GetLastError, WAIT_ABANDONED, WAIT_OBJECT_0,
        };
        use windows_sys::Win32::System::Threading::{CreateMutexW, WaitForSingleObject};

        let name: Vec<u16> = "Local\\ESP32KVMDesktop\0".encode_utf16().collect();
        let handle = unsafe { CreateMutexW(std::ptr::null(), 1, name.as_ptr()) };
        if handle.is_null() {
            return None;
        }
        if unsafe { GetLastError() } == ERROR_ALREADY_EXISTS {
            let result = unsafe { WaitForSingleObject(handle, 5_000) };
            if result != WAIT_OBJECT_0 && result != WAIT_ABANDONED {
                unsafe { windows_sys::Win32::Foundation::CloseHandle(handle) };
                return None;
            }
        }
        Some(Self(handle))
    }
}

#[cfg(windows)]
impl Drop for InstanceMutex {
    fn drop(&mut self) {
        unsafe {
            windows_sys::Win32::System::Threading::ReleaseMutex(self.0);
            windows_sys::Win32::Foundation::CloseHandle(self.0);
        }
    }
}

fn main() {
    #[cfg(windows)]
    let Some(_instance) = InstanceMutex::acquire() else {
        return;
    };
    tauri::Builder::default()
        .setup(|app| {
            let layout = Arc::new(Mutex::new(layout::LayoutRuntime::default()));
            app.manage(Arc::clone(&layout));
            let directory = app.path().app_config_dir()?;
            app.manage(setup::SetupService::new(
                directory.clone(),
                Box::new(actor_backend::ActorBackend::start(layout, directory)),
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
            setup::setup_set_guest_enabled,
            setup::setup_test_controls,
            setup::dashboard_return_local,
            setup::dashboard_select_guest,
            setup::dashboard_keep_awake_enabled,
            setup::dashboard_set_keep_awake,
            setup::shortcut_get_cycle,
            setup::shortcut_set_cycle,
            layout::layout_validate_draft,
            layout::layout_discover,
            layout::layout_apply,
            layout::layout_set_enabled,
        ])
        .run(tauri::generate_context!())
        .expect("failed to run ESP32 KVM desktop shell");
}
