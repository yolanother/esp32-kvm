// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Creates the Windows tray's current target choices and window lifecycle.
// Guest choices and route status come from the actor; ready destinations can be selected.

use crate::setup::{DeviceState, RouteState, SetupService, SetupSnapshot};
use std::path::Path;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
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

const RECOVERY_WINDOW: Duration = Duration::from_secs(30);
const RECOVERY_COOLDOWN: Duration = Duration::from_secs(300);
const STATUS_POLL: Duration = Duration::from_millis(250);

/// Two losses of a verified session in a short interval indicate the USB
/// off/connected cycle seen after sleep. A single unplug does not qualify.
#[derive(Default)]
struct RecoveryDetector {
    verified: bool,
    first_loss: Option<Instant>,
    losses: u8,
}

impl RecoveryDetector {
    fn observe(&mut self, verified: bool, now: Instant) -> bool {
        if self
            .first_loss
            .is_some_and(|first| now.duration_since(first) > RECOVERY_WINDOW)
        {
            self.first_loss = None;
            self.losses = 0;
        }
        if self.verified && !verified {
            if self.first_loss.is_none() {
                self.first_loss = Some(now);
            }
            self.losses += 1;
        }
        self.verified = verified;
        if self.losses >= 2 {
            self.first_loss = None;
            self.losses = 0;
            return true;
        }
        false
    }
}

/// Persist the attempt before restarting, since the replacement process must
/// honor the same cooldown if the USB device remains unstable.
fn reserve_recovery(path: &Path, now: SystemTime) -> bool {
    let Ok(now_ms) = now
        .duration_since(UNIX_EPOCH)
        .map(|elapsed| elapsed.as_millis())
    else {
        return false;
    };
    if let Ok(previous) = std::fs::read_to_string(path)
        && let Ok(previous_ms) = previous.trim().parse::<u128>()
        && now_ms.saturating_sub(previous_ms) < RECOVERY_COOLDOWN.as_millis()
    {
        return false;
    }
    if let Some(parent) = path.parent()
        && std::fs::create_dir_all(parent).is_err()
    {
        return false;
    }
    std::fs::write(path, now_ms.to_string()).is_ok()
}

fn guest_tokens(snapshot: &SetupSnapshot) -> Vec<String> {
    let mut tokens = Vec::new();
    for token in snapshot
        .bond_tokens
        .iter()
        .chain(snapshot.connected_tokens.iter())
        .chain(snapshot.ready_tokens.iter())
        .chain(snapshot.profiles.iter().map(|profile| &profile.bond_token))
    {
        if !tokens.contains(token) {
            tokens.push(token.clone());
        }
    }
    if let RouteState::Guest { bond_token, .. } = &snapshot.route
        && !tokens.contains(bond_token)
    {
        tokens.push(bond_token.clone());
    }
    tokens
}

impl TrayModel {
    fn from_snapshot(snapshot: &SetupSnapshot) -> Self {
        let host_checked = !matches!(snapshot.route, RouteState::Guest { .. });
        let tokens = guest_tokens(snapshot);
        let guests = (0..3).map(|index| {
            let Some(token) = tokens.get(index) else {
                return TrayGuest { label: format!("Guest {} — Offline", index + 1), checked: false, enabled: false };
            };
            let profile = snapshot.profiles.iter().find(|profile| profile.bond_token == *token);
            let name = profile
                .map(|profile| profile.name.clone())
                .unwrap_or_else(|| format!("Guest {}", index + 1));
            let checked = matches!(&snapshot.route, RouteState::Guest { bond_token, .. } if bond_token == token);
            let ready = snapshot.ready_tokens.contains(token) && matches!(snapshot.device, crate::setup::DeviceState::Verified { .. }) && !matches!(snapshot.route, RouteState::Failed { .. });
            let connected = snapshot.connected_tokens.contains(token)
                && matches!(snapshot.device, crate::setup::DeviceState::Verified { .. });
            let state = if checked { "Controlling" } else if ready { "Ready" } else if connected { "Connected" } else { "Offline" };
            let enabled = ready
                && matches!(snapshot.route, RouteState::Local)
                && !snapshot.mapping_pending_tokens.contains(token);
            TrayGuest { label: format!("{name} — {state}"), checked, enabled }
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
    let recovery_marker = app.path().app_config_dir()?.join("usb-recovery-timestamp");
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
        "Return: Ctrl+Alt+F10 or hold both Ctrl keys",
        false,
        None::<&str>,
    )?;
    let settings = MenuItem::with_id(app, "settings", "Open settings", true, None::<&str>)?;
    let restart = MenuItem::with_id(app, "restart", "Restart ESP32 KVM", true, None::<&str>)?;
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
            &restart,
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
            "restart" => {
                let _ = handle.state::<SetupService>().release_for_exit();
                handle.restart();
            }
            "quit" => {
                let _ = handle.state::<SetupService>().release_for_exit();
                handle.exit(0);
            }
            guest_id => {
                let Some(index) = guest_id
                    .strip_prefix("guest_")
                    .and_then(|value| value.parse::<usize>().ok())
                    .and_then(|value| value.checked_sub(1))
                else {
                    return;
                };
                let service = handle.state::<SetupService>();
                if let Ok(snapshot) = service.snapshot()
                    && let Some(token) = guest_tokens(&snapshot).get(index)
                {
                    let _ = service.select_guest(token);
                }
            }
        });
    if let Some(icon) = app.default_window_icon() {
        builder = builder.icon(icon.clone());
    }
    let tray = builder.build(app)?;
    let handle = app.handle().clone();
    std::thread::Builder::new()
        .name("esp32-kvm-tray-status".into())
        .spawn(move || {
            let mut recovery = RecoveryDetector::default();
            let mut next_menu_update = Instant::now();
            loop {
                if let Ok(snapshot) = handle.state::<SetupService>().snapshot() {
                    let now = Instant::now();
                    if recovery
                        .observe(matches!(snapshot.device, DeviceState::Verified { .. }), now)
                        && reserve_recovery(&recovery_marker, SystemTime::now())
                    {
                        let _ = handle.state::<SetupService>().release_for_exit();
                        handle.restart();
                    }
                    if now >= next_menu_update {
                        let model = TrayModel::from_snapshot(&snapshot);
                        let _ = host.set_checked(model.host_checked);
                        for (item, value) in guest_items.iter().zip(model.guests.iter()) {
                            let _ = item.set_text(&value.label);
                            let _ = item.set_checked(value.checked);
                            let _ = item.set_enabled(value.enabled);
                        }
                        let _ = tray.set_tooltip(Some(model.tooltip));
                        next_menu_update = now + Duration::from_secs(2);
                    }
                }
                std::thread::sleep(STATUS_POLL);
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
            retained_bond_tokens: None,
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
                custom_base_preset: None,
                modifier_bindings: Vec::new(),
                key_rules: Vec::new(),
                enabled: true,
            }],
            mapping_pending_tokens: Vec::new(),
            pairing_available: true,
        }
    }

    #[test]
    fn tray_marks_only_confirmed_guest_active_and_enables_ready_local_selection() {
        let local = TrayModel::from_snapshot(&snapshot(RouteState::Local));
        assert!(local.host_checked);
        assert_eq!(local.guests[0].label, "Work Mac — Ready");
        assert_eq!(local.guests[1].label, "Guest 2 — Offline");
        assert!(local.guests[0].enabled);
        let mut unnamed = snapshot(RouteState::Local);
        unnamed.profiles.clear();
        let unnamed = TrayModel::from_snapshot(&unnamed);
        assert_eq!(unnamed.guests[0].label, "Guest 1 — Ready");
        assert!(unnamed.guests[0].enabled);
        let guest = TrayModel::from_snapshot(&snapshot(RouteState::Guest {
            slot: 1,
            bond_token: "abababababababababababababababab".into(),
        }));
        assert!(!guest.host_checked);
        assert!(guest.guests[0].checked);
        assert!(!guest.guests[0].enabled);
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

    #[test]
    fn recovery_requires_two_verified_session_losses() {
        let start = Instant::now();
        let mut detector = RecoveryDetector::default();
        assert!(!detector.observe(false, start));
        assert!(!detector.observe(true, start));
        assert!(!detector.observe(false, start + Duration::from_secs(1)));
        assert!(!detector.observe(false, start + Duration::from_secs(2)));
        assert!(!detector.observe(true, start + Duration::from_secs(3)));
        assert!(detector.observe(false, start + Duration::from_secs(4)));
        assert!(!detector.observe(false, start + Duration::from_secs(5)));

        let mut separated = RecoveryDetector::default();
        assert!(!separated.observe(true, start));
        assert!(!separated.observe(false, start + Duration::from_secs(1)));
        assert!(!separated.observe(true, start + Duration::from_secs(32)));
        assert!(!separated.observe(false, start + Duration::from_secs(33)));
    }
}
