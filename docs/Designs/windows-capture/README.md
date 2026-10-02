# Windows capture adapter

The `esp32-kvm-platform-windows` crate owns one dedicated Win32 message-loop thread. It installs low-level keyboard and mouse hooks and registers a message-only window for mouse Raw Input. Capture starts disarmed. The desktop webview has no part in input timing or suppression.

## Event ownership

| Source | Used for | Suppression while armed |
|---|---|---|
| `WH_KEYBOARD_LL` | Physical key down/up, scan code, side/extended bit, repeat metadata | Hook consumes accepted physical transitions |
| `WH_MOUSE_LL` | Button transitions and vertical/horizontal wheel deltas | Hook consumes accepted transitions and legacy mouse movement |
| `WM_INPUT` mouse Raw Input | Relative X/Y motion | Raw Input cannot suppress Windows input; the mouse hook handles local legacy movement |

The hook never forwards legacy `WM_MOUSEMOVE`, and the Raw Input handler ignores button/wheel fields. This prevents duplicate input. Injected hook events pass through locally and are not forwarded. One exception is the Windows AltGr synthetic left Ctrl scan code `0x021D`: while armed it is consumed without forwarding, leaving the physical right Alt event distinct. Real keyboard-layout validation is still required.

The hook callback classifies a fixed-size event and makes a nonblocking `try_send` to a bounded queue. It does no USB, BLE, logging or webview I/O. Queue overflow, receiver closure, invalid Raw Input, or absolute mouse motion records a fault and atomically disarms local suppression. The native routing actor must observe `fault()`, stop guest forwarding, release guest state, and report the reason. `clear_fault()` requires the gate to be disarmed. The event receiver must discard entries whose `generation` differs from the active route. Each arm must use a fresh nonzero routing generation.

The owning actor must arm only after the firmware confirms the selected guest is ready and the all-up baseline is established. It must disarm before a switch, pairing, update, session transition or shutdown. This adapter does not select guests or keep the firmware lease alive. The host watchdog and cursor restoration are separate native routing work.

## Verification

```powershell
& 'C:/Users/yolan/.cargo/bin/cargo.exe' fmt --all -- --check
& 'C:/Users/yolan/.cargo/bin/cargo.exe' test -p esp32-kvm-platform-windows --offline
& 'C:/Users/yolan/.cargo/bin/cargo.exe' clippy -p esp32-kvm-platform-windows --all-targets --offline -- -D warnings
```

Unit tests cover local pass-through, repeat metadata, injected input, the AltGr marker, one-path mouse delivery, and queue overflow. The following live gates remain open: high-poll mice and wheel/button ordering; multiple physical devices; AltGr on non-US layouts; elevated applications; UAC secure desktop, lock/sleep/resume, and protected shortcuts; hook timeout/removal; cursor parking/restoration and crash recovery. Windows does not guarantee that ordinary global hooks intercept secure desktop or protected system combinations. The release matrix must record those cases separately, and route recovery must fail local.

## Win32 references

- [LowLevelKeyboardProc](https://learn.microsoft.com/en-us/windows/win32/winmsg/lowlevelkeyboardproc) and [LowLevelMouseProc](https://learn.microsoft.com/en-us/windows/win32/winmsg/lowlevelmouseproc) define hook callback and message-loop requirements.
- [Using Raw Input](https://learn.microsoft.com/en-us/windows/win32/inputdev/using-raw-input) describes `WM_INPUT`, `GetRawInputData`, and background registration.
- [Raw Input overview](https://learn.microsoft.com/en-us/windows/win32/inputdev/about-raw-input) warns that traditional and Raw Input paths can both report mouse events.
- [Chromium Windows modifier hook](https://chromium.googlesource.com/chromium/src/+/5aa35d6fa3ef41091f480b9e4254c494a1e153da/ui/events/win/modifier_keyboard_hook_win.cc) documents the observed AltGr synthetic Ctrl scan code; the physical-layout gate remains necessary.
