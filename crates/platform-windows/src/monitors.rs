// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Reads physical Windows display bounds, effective per-monitor DPI and rotation.
// Normalizes snapshots through topology-core so hotplug changes advance a generation.

use esp32_kvm_topology_core::{Monitor, Rect, Rotation, Topology};

/// One raw OS display record before topology validation.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DisplayRecord {
    /// Windows display device name; stable for a connected display arrangement.
    pub id: String,
    /// Left physical pixel, possibly negative.
    pub x: i32,
    /// Top physical pixel, possibly negative.
    pub y: i32,
    /// Physical width.
    pub width: u32,
    /// Physical height.
    pub height: u32,
    /// Effective horizontal DPI.
    pub dpi_x: u32,
    /// Effective vertical DPI.
    pub dpi_y: u32,
    /// Orientation in degrees.
    pub rotation_degrees: u32,
    /// Whether Windows identifies this display as primary.
    pub primary: bool,
}

/// Last validated Windows monitor snapshot and its change generation.
#[derive(Default)]
pub struct MonitorInventory {
    topology: Option<Topology>,
}

impl MonitorInventory {
    /// Creates an empty inventory until the first successful OS enumeration.
    pub fn new() -> Self {
        Self::default()
    }

    /// Returns the current validated topology, if one was discovered.
    pub fn topology(&self) -> Option<&Topology> {
        self.topology.as_ref()
    }

    /// Atomically validates and replaces a snapshot; errors leave the old snapshot intact.
    pub fn update(&mut self, records: Vec<DisplayRecord>) -> Result<bool, String> {
        let monitors = records
            .into_iter()
            .map(|record| {
                let rotation = match record.rotation_degrees {
                    0 => Rotation::Deg0,
                    90 => Rotation::Deg90,
                    180 => Rotation::Deg180,
                    270 => Rotation::Deg270,
                    _ => return Err("Windows returned an unsupported display rotation.".into()),
                };
                Ok(Monitor {
                    id: record.id,
                    rect: Rect {
                        x: record.x,
                        y: record.y,
                        width: record.width,
                        height: record.height,
                    },
                    dpi_x: record.dpi_x,
                    dpi_y: record.dpi_y,
                    rotation,
                    primary: record.primary,
                })
            })
            .collect::<Result<Vec<_>, String>>()?;
        if let Some(topology) = &mut self.topology {
            topology
                .replace(monitors)
                .map_err(|error| format!("Invalid Windows display topology: {error:?}"))
        } else {
            self.topology = Some(
                Topology::new(monitors)
                    .map_err(|error| format!("Invalid Windows display topology: {error:?}"))?,
            );
            Ok(true)
        }
    }
}

/// Enumerates the current Windows desktop; errors never invent a fallback display.
#[cfg(windows)]
pub fn discover_monitors() -> Result<Vec<DisplayRecord>, String> {
    windows::discover()
}

/// Reads the physical virtual-desktop cursor location for edge-policy sampling.
#[cfg(windows)]
pub fn physical_cursor_position() -> Result<(i32, i32), String> {
    windows::cursor_position()
}

/// Conservatively reports whether the foreground window covers its host monitor.
#[cfg(windows)]
pub fn foreground_fullscreen() -> Result<bool, String> {
    windows::foreground_fullscreen()
}

/// Reports unavailable native cursor sampling outside Windows.
#[cfg(not(windows))]
pub fn physical_cursor_position() -> Result<(i32, i32), String> {
    Err("Windows physical cursor sampling requires Windows.".into())
}

/// Reports unavailable foreground geometry outside Windows.
#[cfg(not(windows))]
pub fn foreground_fullscreen() -> Result<bool, String> {
    Err("Windows foreground geometry requires Windows.".into())
}

/// Reports unavailable native enumeration on non-Windows test hosts.
#[cfg(not(windows))]
pub fn discover_monitors() -> Result<Vec<DisplayRecord>, String> {
    Err("Windows monitor discovery requires Windows.".into())
}

#[cfg(windows)]
mod windows {
    use super::DisplayRecord;
    use std::mem::size_of;
    use std::ptr::{null, null_mut};
    use windows_sys::Win32::Foundation::LPARAM;
    use windows_sys::Win32::Foundation::POINT;
    use windows_sys::Win32::Graphics::Gdi::{
        DEVMODEW, ENUM_CURRENT_SETTINGS, EnumDisplayMonitors, EnumDisplaySettingsW,
        GetMonitorInfoW, HDC, HMONITOR, MONITOR_DEFAULTTONULL, MONITORINFO, MONITORINFOEXW,
        MonitorFromWindow,
    };
    use windows_sys::Win32::UI::HiDpi::{
        DPI_AWARENESS_PER_MONITOR_AWARE, GetAwarenessFromDpiAwarenessContext, GetDpiForMonitor,
        GetThreadDpiAwarenessContext, MDT_EFFECTIVE_DPI,
    };
    use windows_sys::Win32::UI::WindowsAndMessaging::{
        GetClassNameW, GetCursorPos, GetForegroundWindow, GetWindowRect,
    };

    struct Enumeration {
        records: Vec<DisplayRecord>,
        error: Option<String>,
    }

    /// Captures each callback record on the calling thread; no pointer escapes the call.
    unsafe extern "system" fn enumerate(
        monitor: HMONITOR,
        _dc: HDC,
        _rect: *mut windows_sys::Win32::Foundation::RECT,
        data: LPARAM,
    ) -> i32 {
        let result = unsafe { &mut *(data as *mut Enumeration) };
        let mut info = MONITORINFOEXW::default();
        info.monitorInfo.cbSize = size_of::<MONITORINFOEXW>() as u32;
        if unsafe { GetMonitorInfoW(monitor, &mut info.monitorInfo as *mut MONITORINFO) } == 0 {
            result.error = Some("GetMonitorInfoW failed.".into());
            return 0;
        }
        let len = info
            .szDevice
            .iter()
            .position(|ch| *ch == 0)
            .unwrap_or(info.szDevice.len());
        let id = String::from_utf16_lossy(&info.szDevice[..len]);
        let mut mode = DEVMODEW {
            dmSize: size_of::<DEVMODEW>() as u16,
            ..Default::default()
        };
        if unsafe { EnumDisplaySettingsW(info.szDevice.as_ptr(), ENUM_CURRENT_SETTINGS, &mut mode) }
            == 0
        {
            result.error = Some(format!("Could not read display orientation for {id}."));
            return 0;
        }
        let mut dpi_x = 0;
        let mut dpi_y = 0;
        if unsafe { GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &mut dpi_x, &mut dpi_y) } < 0 {
            result.error = Some(format!("Could not read effective DPI for {id}."));
            return 0;
        }
        let bounds = info.monitorInfo.rcMonitor;
        let width = i64::from(bounds.right) - i64::from(bounds.left);
        let height = i64::from(bounds.bottom) - i64::from(bounds.top);
        if width <= 0 || height <= 0 || width > i64::from(u32::MAX) || height > i64::from(u32::MAX)
        {
            result.error = Some(format!("Invalid Windows display bounds for {id}."));
            return 0;
        }
        result.records.push(DisplayRecord {
            id,
            x: bounds.left,
            y: bounds.top,
            width: width as u32,
            height: height as u32,
            dpi_x,
            dpi_y,
            rotation_degrees: unsafe { mode.Anonymous1.Anonymous2.dmDisplayOrientation } * 90,
            primary: info.monitorInfo.dwFlags & 1 != 0,
        });
        1
    }

    pub(super) fn discover() -> Result<Vec<DisplayRecord>, String> {
        // GetDpiForMonitor returns virtualized values outside a per-monitor-aware context.
        if unsafe { GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()) }
            != DPI_AWARENESS_PER_MONITOR_AWARE
        {
            return Err(
                "Windows thread is not per-monitor DPI aware; physical layout is unavailable."
                    .into(),
            );
        }
        let mut result = Enumeration {
            records: Vec::new(),
            error: None,
        };
        let ok = unsafe {
            EnumDisplayMonitors(
                null_mut(),
                null(),
                Some(enumerate),
                &mut result as *mut Enumeration as LPARAM,
            )
        };
        if let Some(error) = result.error {
            return Err(error);
        }
        if ok == 0 || result.records.is_empty() {
            return Err("Windows display enumeration failed or found no monitors.".into());
        }
        Ok(result.records)
    }

    pub(super) fn cursor_position() -> Result<(i32, i32), String> {
        if unsafe { GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()) }
            != DPI_AWARENESS_PER_MONITOR_AWARE
        {
            return Err("Windows thread is not per-monitor DPI aware; physical cursor coordinates are unavailable.".into());
        }
        let mut point = POINT::default();
        if unsafe { GetCursorPos(&mut point) } == 0 {
            return Err("Could not read the physical Windows cursor position.".into());
        }
        Ok((point.x, point.y))
    }

    pub(super) fn foreground_fullscreen() -> Result<bool, String> {
        let window = unsafe { GetForegroundWindow() };
        if window.is_null() {
            return Err("Windows foreground window is unavailable.".into());
        }
        let mut class = [0u16; 64];
        let length = unsafe { GetClassNameW(window, class.as_mut_ptr(), class.len() as i32) };
        if length == 0 {
            return Err("Could not identify the foreground window.".into());
        }
        let class = String::from_utf16_lossy(&class[..length as usize]);
        if matches!(class.as_str(), "Progman" | "WorkerW" | "Shell_TrayWnd") {
            return Ok(false);
        }
        let monitor = unsafe { MonitorFromWindow(window, MONITOR_DEFAULTTONULL) };
        if monitor.is_null() {
            return Err("Foreground window has no current monitor.".into());
        }
        let mut window_rect = windows_sys::Win32::Foundation::RECT::default();
        if unsafe { GetWindowRect(window, &mut window_rect) } == 0 {
            return Err("Could not read foreground window bounds.".into());
        }
        let mut info = MONITORINFO {
            cbSize: size_of::<MONITORINFO>() as u32,
            ..Default::default()
        };
        if unsafe { GetMonitorInfoW(monitor, &mut info) } == 0 {
            return Err("Could not read foreground monitor bounds.".into());
        }
        let screen = info.rcMonitor;
        Ok(window_rect.left <= screen.left
            && window_rect.top <= screen.top
            && window_rect.right >= screen.right
            && window_rect.bottom >= screen.bottom)
    }
}
