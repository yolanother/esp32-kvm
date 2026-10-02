// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Runs one Windows message-loop thread with low-level keyboard/mouse hooks and a message-only
// Raw Input window. Hooks handle key/button/wheel transitions and local suppression; only Raw
// Input supplies relative motion. The worker starts disarmed and never routes through the UI.
use crate::{
    CaptureEvent, CaptureFault, CaptureGate, MouseAxis, MouseButton, MouseInput, PhysicalEvent,
    RawMotionOutcome, classify_keyboard, classify_mouse, classify_raw_motion,
};
use std::cell::RefCell;
use std::io;
use std::mem::{size_of, zeroed};
use std::ptr::{null, null_mut};
use std::sync::Arc;
use std::sync::mpsc::{Receiver, sync_channel};
use std::thread::{self, JoinHandle};
use windows_sys::Win32::Foundation::{GetLastError, HINSTANCE, HWND, LPARAM, LRESULT, WPARAM};
use windows_sys::Win32::System::LibraryLoader::GetModuleHandleW;
use windows_sys::Win32::System::Threading::GetCurrentThreadId;
use windows_sys::Win32::UI::Input::{
    GetRawInputData, MOUSE_MOVE_ABSOLUTE, RAWINPUT, RAWINPUTDEVICE, RAWINPUTHEADER, RID_INPUT,
    RIDEV_INPUTSINK, RIDEV_REMOVE, RIM_TYPEMOUSE, RegisterRawInputDevices,
};
use windows_sys::Win32::UI::WindowsAndMessaging::*;

const CLASS_NAME: &[u16] = &[
    69, 83, 80, 51, 50, 75, 86, 77, 67, 97, 112, 116, 117, 114, 101, 0,
];
const SCAN_SLOTS: usize = 512;

/// Failure to start or retain the dedicated Windows capture worker.
#[derive(Debug)]
pub enum CaptureStartError {
    /// The operating system could not create the worker thread.
    Thread(io::Error),
    /// A Windows registration or hook call failed with GetLastError.
    Windows { operation: &'static str, code: u32 },
    /// The worker exited before reporting its setup result.
    WorkerExited,
}

impl std::fmt::Display for CaptureStartError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Thread(error) => write!(f, "capture thread: {error}"),
            Self::Windows { operation, code } => {
                write!(f, "{operation} failed: Win32 error {code}")
            }
            Self::WorkerExited => write!(f, "capture worker exited during setup"),
        }
    }
}
impl std::error::Error for CaptureStartError {}

struct ThreadContext {
    gate: Arc<CaptureGate>,
    held: [bool; SCAN_SLOTS],
    generation: u32,
}
thread_local! { static CONTEXT: RefCell<Option<ThreadContext>> = const { RefCell::new(None) }; }

/// Owns the capture worker; dropping it disarms, unhooks, and joins the thread.
pub struct CaptureService {
    gate: Arc<CaptureGate>,
    thread_id: u32,
    worker: Option<JoinHandle<()>>,
}

impl CaptureService {
    /// Start a disarmed worker and return its bounded event receiver.
    pub fn start(
        queue_capacity: usize,
    ) -> Result<(Self, Receiver<CaptureEvent>), CaptureStartError> {
        let (gate, receiver) = CaptureGate::new(queue_capacity);
        let worker_gate = Arc::clone(&gate);
        let (ready_sender, ready_receiver) = sync_channel(1);
        let worker = thread::Builder::new()
            .name("esp32-kvm-capture".into())
            .spawn(move || {
                let result = Runtime::setup(Arc::clone(&worker_gate));
                match result {
                    Ok(runtime) => {
                        let thread_id = unsafe { GetCurrentThreadId() };
                        let _ = ready_sender.send(Ok(thread_id));
                        let mut message: MSG = unsafe { zeroed() };
                        loop {
                            let outcome = unsafe { GetMessageW(&mut message, null_mut(), 0, 0) };
                            if outcome <= 0 {
                                worker_gate.mark_fault(CaptureFault::MessagePumpFailure);
                                break;
                            }
                            unsafe {
                                TranslateMessage(&message);
                                DispatchMessageW(&message);
                            }
                        }
                        worker_gate.disarm();
                        drop(runtime);
                    }
                    Err(error) => {
                        let _ = ready_sender.send(Err(error));
                    }
                }
            })
            .map_err(CaptureStartError::Thread)?;
        match ready_receiver.recv() {
            Ok(Ok(thread_id)) => Ok((
                Self {
                    gate,
                    thread_id,
                    worker: Some(worker),
                },
                receiver,
            )),
            Ok(Err(error)) => {
                let _ = worker.join();
                Err(error)
            }
            Err(_) => {
                let _ = worker.join();
                Err(CaptureStartError::WorkerExited)
            }
        }
    }

    /// Arm a fresh route only when no capture fault is pending.
    pub fn arm(&self, generation: u32) -> bool {
        self.gate.arm(generation)
    }
    /// Restore local pass-through immediately.
    pub fn disarm(&self) {
        self.gate.disarm();
    }
    /// Read the active route generation, or zero when locally disarmed.
    pub fn generation(&self) -> u32 {
        self.gate.generation()
    }
    /// Read the first capture fault; the actor must release remote state on faults.
    pub fn fault(&self) -> Option<CaptureFault> {
        self.gate.fault()
    }
    /// Clear a diagnosed fault while disarmed before starting a new route.
    pub fn clear_fault(&self) -> bool {
        self.gate.clear_fault()
    }
}
impl Drop for CaptureService {
    fn drop(&mut self) {
        self.gate.disarm();
        unsafe {
            PostThreadMessageW(self.thread_id, WM_QUIT, 0, 0);
        }
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

struct Runtime {
    instance: HINSTANCE,
    window: HWND,
    keyboard: HHOOK,
    mouse: HHOOK,
    registered: bool,
    raw_registered: bool,
}
impl Runtime {
    fn setup(gate: Arc<CaptureGate>) -> Result<Self, CaptureStartError> {
        let instance = unsafe { GetModuleHandleW(null()) };
        if instance.is_null() {
            return Err(win_error("GetModuleHandleW"));
        }
        let mut runtime = Self {
            instance,
            window: null_mut(),
            keyboard: null_mut(),
            mouse: null_mut(),
            registered: false,
            raw_registered: false,
        };
        let class = WNDCLASSW {
            lpfnWndProc: Some(window_proc),
            hInstance: instance,
            lpszClassName: CLASS_NAME.as_ptr(),
            ..unsafe { zeroed() }
        };
        if unsafe { RegisterClassW(&class) } == 0 {
            return Err(win_error("RegisterClassW"));
        }
        runtime.registered = true;
        runtime.window = unsafe {
            CreateWindowExW(
                0,
                CLASS_NAME.as_ptr(),
                CLASS_NAME.as_ptr(),
                0,
                0,
                0,
                0,
                0,
                HWND_MESSAGE,
                null_mut(),
                instance,
                null(),
            )
        };
        if runtime.window.is_null() {
            return Err(win_error("CreateWindowExW"));
        }
        CONTEXT.with(|cell| {
            *cell.borrow_mut() = Some(ThreadContext {
                gate,
                held: [false; SCAN_SLOTS],
                generation: 0,
            })
        });
        let device = RAWINPUTDEVICE {
            usUsagePage: 1,
            usUsage: 2,
            dwFlags: RIDEV_INPUTSINK,
            hwndTarget: runtime.window,
        };
        if unsafe { RegisterRawInputDevices(&device, 1, size_of::<RAWINPUTDEVICE>() as u32) } == 0 {
            return Err(win_error("RegisterRawInputDevices"));
        }
        runtime.raw_registered = true;
        runtime.keyboard =
            unsafe { SetWindowsHookExW(WH_KEYBOARD_LL, Some(keyboard_proc), instance, 0) };
        if runtime.keyboard.is_null() {
            return Err(win_error("SetWindowsHookExW keyboard"));
        }
        runtime.mouse = unsafe { SetWindowsHookExW(WH_MOUSE_LL, Some(mouse_proc), instance, 0) };
        if runtime.mouse.is_null() {
            return Err(win_error("SetWindowsHookExW mouse"));
        }
        Ok(runtime)
    }
}
impl Drop for Runtime {
    fn drop(&mut self) {
        CONTEXT.with(|cell| {
            if let Ok(mut context) = cell.try_borrow_mut() {
                if let Some(context) = context.as_ref() {
                    context.gate.disarm();
                }
                *context = None;
            }
        });
        unsafe {
            if !self.mouse.is_null() {
                UnhookWindowsHookEx(self.mouse);
            }
            if !self.keyboard.is_null() {
                UnhookWindowsHookEx(self.keyboard);
            }
            if self.raw_registered {
                let remove = RAWINPUTDEVICE {
                    usUsagePage: 1,
                    usUsage: 2,
                    dwFlags: RIDEV_REMOVE,
                    hwndTarget: null_mut(),
                };
                RegisterRawInputDevices(&remove, 1, size_of::<RAWINPUTDEVICE>() as u32);
            }
            if !self.window.is_null() {
                DestroyWindow(self.window);
            }
            if self.registered {
                UnregisterClassW(CLASS_NAME.as_ptr(), self.instance);
            }
        }
    }
}

fn win_error(operation: &'static str) -> CaptureStartError {
    CaptureStartError::Windows {
        operation,
        code: unsafe { GetLastError() },
    }
}

unsafe extern "system" fn keyboard_proc(code: i32, message: WPARAM, param: LPARAM) -> LRESULT {
    if code < 0 {
        return unsafe { CallNextHookEx(null_mut(), code, message, param) };
    }
    let down = match message as u32 {
        WM_KEYDOWN | WM_SYSKEYDOWN => true,
        WM_KEYUP | WM_SYSKEYUP => false,
        _ => return unsafe { CallNextHookEx(null_mut(), code, message, param) },
    };
    let input = unsafe { &*(param as *const KBDLLHOOKSTRUCT) };
    let mut suppress = false;
    CONTEXT.with(|cell| {
        if let Ok(mut slot) = cell.try_borrow_mut()
            && let Some(context) = slot.as_mut()
        {
            let generation = context.gate.generation();
            if generation != context.generation {
                context.held.fill(false);
                context.generation = generation;
            }
            let index = ((input.scanCode as usize) & 0xff)
                | if input.flags & LLKHF_EXTENDED != 0 {
                    256
                } else {
                    0
                };
            let injected = input.flags & LLKHF_INJECTED != 0;
            let repeat = down && context.held[index];
            let decision = classify_keyboard(
                generation != 0,
                injected,
                input.vkCode,
                input.scanCode,
                input.flags & LLKHF_EXTENDED != 0,
                down,
                repeat,
            );
            if !decision.pass_to_os {
                if decision.event.is_none() {
                    suppress = true;
                }
                if let Some(event) = decision.event
                    && context.gate.offer(event)
                {
                    context.held[index] = down;
                    suppress = true;
                }
            }
        }
    });
    if suppress {
        1
    } else {
        unsafe { CallNextHookEx(null_mut(), code, message, param) }
    }
}

unsafe extern "system" fn mouse_proc(code: i32, message: WPARAM, param: LPARAM) -> LRESULT {
    if code < 0 {
        return unsafe { CallNextHookEx(null_mut(), code, message, param) };
    }
    let input = unsafe { &*(param as *const MSLLHOOKSTRUCT) };
    let button = |down| match (input.mouseData >> 16) as u16 {
        1 => Some(MouseInput::Button(MouseButton::X1, down)),
        2 => Some(MouseInput::Button(MouseButton::X2, down)),
        _ => None,
    };
    let kind = match message as u32 {
        WM_MOUSEMOVE => Some(MouseInput::Move),
        WM_LBUTTONDOWN => Some(MouseInput::Button(MouseButton::Left, true)),
        WM_LBUTTONUP => Some(MouseInput::Button(MouseButton::Left, false)),
        WM_RBUTTONDOWN => Some(MouseInput::Button(MouseButton::Right, true)),
        WM_RBUTTONUP => Some(MouseInput::Button(MouseButton::Right, false)),
        WM_MBUTTONDOWN => Some(MouseInput::Button(MouseButton::Middle, true)),
        WM_MBUTTONUP => Some(MouseInput::Button(MouseButton::Middle, false)),
        WM_XBUTTONDOWN => button(true),
        WM_XBUTTONUP => button(false),
        WM_MOUSEWHEEL => Some(MouseInput::Wheel(
            MouseAxis::Vertical,
            (input.mouseData >> 16) as i16,
        )),
        WM_MOUSEHWHEEL => Some(MouseInput::Wheel(
            MouseAxis::Horizontal,
            (input.mouseData >> 16) as i16,
        )),
        _ => None,
    };
    let Some(kind) = kind else {
        return unsafe { CallNextHookEx(null_mut(), code, message, param) };
    };
    let mut suppress = false;
    CONTEXT.with(|cell| {
        if let Ok(slot) = cell.try_borrow()
            && let Some(context) = slot.as_ref()
        {
            let decision = classify_mouse(
                context.gate.generation() != 0,
                input.flags & LLMHF_INJECTED != 0,
                kind,
            );
            if !decision.pass_to_os {
                suppress = decision.event.is_none_or(|event| context.gate.offer(event));
            }
        }
    });
    if suppress {
        1
    } else {
        unsafe { CallNextHookEx(null_mut(), code, message, param) }
    }
}

unsafe extern "system" fn window_proc(
    window: HWND,
    message: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    if message == WM_INPUT {
        process_raw_input(lparam);
    }
    unsafe { DefWindowProcW(window, message, wparam, lparam) }
}

fn process_raw_input(param: LPARAM) {
    let mut raw: RAWINPUT = unsafe { zeroed() };
    let mut size = size_of::<RAWINPUT>() as u32;
    let actual = unsafe {
        GetRawInputData(
            param as _,
            RID_INPUT,
            (&mut raw as *mut RAWINPUT).cast(),
            &mut size,
            size_of::<RAWINPUTHEADER>() as u32,
        )
    };
    let gate = CONTEXT.with(|cell| {
        cell.try_borrow()
            .ok()
            .and_then(|slot| slot.as_ref().map(|context| Arc::clone(&context.gate)))
    });
    let Some(gate) = gate else {
        return;
    };
    if actual == u32::MAX
        || actual < size_of::<RAWINPUTHEADER>() as u32
        || actual > size_of::<RAWINPUT>() as u32
    {
        gate.mark_fault(CaptureFault::RawInputFailure);
        return;
    }
    if raw.header.dwType != RIM_TYPEMOUSE || gate.generation() == 0 {
        return;
    }
    if actual < size_of::<RAWINPUT>() as u32 || raw.header.dwSize != actual {
        gate.mark_fault(CaptureFault::RawInputFailure);
        return;
    }
    let mouse = unsafe { raw.data.mouse };
    match classify_raw_motion(
        true,
        mouse.usFlags & MOUSE_MOVE_ABSOLUTE != 0,
        mouse.lLastX,
        mouse.lLastY,
    ) {
        RawMotionOutcome::Motion(dx, dy) => {
            gate.offer(PhysicalEvent::Motion(dx, dy));
        }
        RawMotionOutcome::UnsupportedAbsolute => {
            gate.mark_fault(CaptureFault::UnsupportedAbsoluteInput)
        }
        RawMotionOutcome::Ignore => {}
    }
}
