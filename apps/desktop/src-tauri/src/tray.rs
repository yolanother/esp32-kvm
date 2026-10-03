// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Creates the Windows tray's current target choices and window lifecycle.
// Guest selection and pause stay disabled until native capture is integrated.

use crate::setup::{RouteState, SetupService, SetupSnapshot};
use std::time::Duration;
use tauri::{
    Emitter, Manager,
    menu::{CheckMenuItem, Menu, MenuItem},
    tray::TrayIconBuilder,
};

struct TrayGuest {
    label: String,
    checked: bool,
    enabled: bool,
}

struct TrayModel {
    host_checked: bool,
    guests: Vec<TrayGuest>,
    tooltip: String,
}

impl TrayModel {
    fn from_snapshot(snapshot: &SetupSnapshot) -> Self {
        let host_checked = !matches!(snapshot.route, RouteState::Guest { .. });
        let guests = (0..3).map(|index| {
            let Some(profile) = snapshot.profiles.get(index) else {
                return TrayGuest { label: format!("Guest {} — Offline", index + 1), checked: false, enabled: false };
            };
            let checked = matches!(&snapshot.route, RouteState::Guest { bond_token, .. } if bond_token == &profile.bond_token);
            let ready = snapshot.ready_tokens.contains(&profile.bond_token) && matches!(snapshot.device, crate::setup::DeviceState::Verified { .. }) && !matches!(snapshot.route, RouteState::Failed { .. });
            let connected = snapshot.connected_tokens.contains(&profile.bond_token)
                && matches!(snapshot.device, crate::setup::DeviceState::Verified { .. });
            let state = if checked { "Controlling" } else if ready { "Ready" } else if connected { "Connected" } else { "Offline" };
            TrayGuest { label: format!("{} — {state}", profile.name), checked, enabled: false }
        }).collect();
        let tooltip = if let RouteState::Guest { bond_token, slot } = &snapshot.route {
            let target = snapshot
                .profiles
                .iter()
                .find(|entry| entry.bond_token == *bond_token)
                .map(|entry| entry.name.as_str())
                .unwrap_or("Guest");
            format!("ESP32 KVM — {target} (slot {slot})")
        } else if let RouteState::Failed { reason } = &snapshot.route {
            format!("ESP32 KVM — Local after {reason} failure")
        } else {
            "ESP32 KVM — This computer".into()
        };
        Self {
            host_checked,
            guests,
            tooltip,
        }
    }
}

/// Installs a persistent tray, current-target choices, settings, and safe quit.
pub fn install(app: &mut tauri::App) -> tauri::Result<()> {
    let host = CheckMenuItem::with_id(app, "host", "This computer", true, true, None::<&str>)?;
    let guest_items = [
        CheckMenuItem::with_id(
            app,
            "guest_1",
            "Guest 1 — Offline",
            false,
            false,
            None::<&str>,
        )?,
        CheckMenuItem::with_id(
            app,
            "guest_2",
            "Guest 2 — Offline",
            false,
            false,
            None::<&str>,
        )?,
        CheckMenuItem::with_id(
            app,
            "guest_3",
            "Guest 3 — Offline",
            false,
            false,
            None::<&str>,
        )?,
    ];
    let pause = MenuItem::with_id(
        app,
        "pause",
        "Pause capture — unavailable",
        false,
        None::<&str>,
    )?;
    let shortcuts = MenuItem::with_id(
        app,
        "shortcuts",
        "Switch shortcuts — not registered",
        false,
        None::<&str>,
    )?;
    let settings = MenuItem::with_id(app, "settings", "Open settings", true, None::<&str>)?;
    let quit = MenuItem::with_id(app, "quit", "Quit and release", true, None::<&str>)?;
    let menu = Menu::with_items(
        app,
        &[
            &host,
            &guest_items[0],
            &guest_items[1],
            &guest_items[2],
            &pause,
            &shortcuts,
            &settings,
            &quit,
        ],
    )?;
    let mut builder = TrayIconBuilder::new()
        .menu(&menu)
        .tooltip("ESP32 KVM — This computer")
        .on_menu_event(|handle, event| match event.id().as_ref() {
            "host" => {
                let _ = handle.state::<SetupService>().return_local();
            }
            "settings" => {
                if let Some(window) = handle.get_webview_window("main") {
                    let _ = window.show();
                    let _ = window.set_focus();
                    let _ = window.emit("tray-open-settings", ());
                }
            }
            "quit" => {
                let _ = handle.state::<SetupService>().release_for_exit();
                handle.exit(0);
            }
            _ => {}
        });
    if let Some(icon) = app.default_window_icon() {
        builder = builder.icon(icon.clone());
    }
    let tray = builder.build(app)?;
    let handle = app.handle().clone();
    std::thread::Builder::new()
        .name("esp32-kvm-tray-status".into())
        .spawn(move || {
            loop {
                if let Ok(snapshot) = handle.state::<SetupService>().snapshot() {
                    let model = TrayModel::from_snapshot(&snapshot);
                    let _ = host.set_checked(model.host_checked);
                    for (item, value) in guest_items.iter().zip(model.guests.iter()) {
                        let _ = item.set_text(&value.label);
                        let _ = item.set_checked(value.checked);
                        let _ = item.set_enabled(value.enabled);
                    }
                    let _ = tray.set_tooltip(Some(model.tooltip));
                }
                std::thread::sleep(Duration::from_secs(2));
            }
        })
        .map_err(tauri::Error::Io)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::setup::{DeviceState, GuestProfile, PairingState, RouteState, SetupSnapshot};

    fn snapshot(route: RouteState) -> SetupSnapshot {
        SetupSnapshot {
            device: DeviceState::Verified {
                board_id: "esp32-kvm-s3".into(),
                firmware_version: None,
                max_bonds: 8,
                max_connections: 1,
            },
            route,
            pairing: PairingState::Closed,
            bond_tokens: vec!["abababababababababababababababab".into()],
            connected_tokens: vec!["abababababababababababababababab".into()],
            ready_tokens: vec!["abababababababababababababababab".into()],
            profiles: vec![GuestProfile {
                bond_token: "abababababababababababababababab".into(),
                name: "Work Mac".into(),
                os: "macos".into(),
                profile: "unchanged".into(),
                direct_shortcut: None,
                mapping_profile_id: None,
                layout_link_id: None,
            }],
            pairing_available: true,
        }
    }

    #[test]
    fn tray_marks_only_confirmed_guest_active_and_never_enables_selection() {
        let local = TrayModel::from_snapshot(&snapshot(RouteState::Local));
        assert!(local.host_checked);
        assert_eq!(local.guests[0].label, "Work Mac — Ready");
        assert_eq!(local.guests[1].label, "Guest 2 — Offline");
        assert!(!local.guests[0].enabled);
        let guest = TrayModel::from_snapshot(&snapshot(RouteState::Guest {
            slot: 1,
            bond_token: "abababababababababababababababab".into(),
        }));
        assert!(!guest.host_checked);
        assert!(guest.guests[0].checked);
    }

    #[test]
    fn tray_clears_guest_choice_after_transport_failure() {
        let failed = TrayModel::from_snapshot(&snapshot(RouteState::Failed {
            reason: "transport".into(),
        }));
        assert!(failed.host_checked);
        assert!(!failed.guests[0].checked);
        assert!(failed.tooltip.contains("transport"));
    }
}
