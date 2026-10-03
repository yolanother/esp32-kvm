// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Bridges a macOS event-tap helper to the shared bounded capture gate. The helper owns
// suppression and its independent lease; this process owns route generations and USB I/O.

use esp32_kvm_host_actor::CaptureControl;
use esp32_kvm_input_core::Action;
use esp32_kvm_platform_windows::{
    CaptureEvent, CaptureFault, CaptureGate, MouseAxis, MouseButton, PhysicalEvent,
};
use std::io::{self, BufRead, BufReader, Write};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, Receiver, Sender};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

/// Owns the Mac event-tap process and fails local if its event pipe closes.
pub struct MacCapture {
    gate: Arc<CaptureGate>,
    stdin: Mutex<ChildStdin>,
    child: Mutex<Child>,
    replies: Mutex<Receiver<bool>>,
    alive: Arc<AtomicBool>,
}

impl MacCapture {
    /// Starts the prebuilt helper in disarmed mode and waits for tap readiness.
    pub fn start(helper: &str) -> io::Result<(Arc<Self>, Receiver<CaptureEvent>)> {
        let mut child = Command::new(helper)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit())
            .spawn()?;
        let stdin = child
            .stdin
            .take()
            .ok_or_else(|| io::Error::other("missing helper stdin"))?;
        let stdout = child
            .stdout
            .take()
            .ok_or_else(|| io::Error::other("missing helper stdout"))?;
        let (gate, events) = CaptureGate::new(256);
        let (replies, reply_rx) = mpsc::channel();
        let (ready, ready_rx) = mpsc::sync_channel(1);
        let alive = Arc::new(AtomicBool::new(true));
        let reader_gate = Arc::clone(&gate);
        let reader_alive = Arc::clone(&alive);
        thread::Builder::new()
            .name("esp32-kvm-mac-events".into())
            .spawn(move || read_events(stdout, reader_gate, replies, ready, reader_alive))?;
        if !ready_rx
            .recv_timeout(Duration::from_secs(3))
            .unwrap_or(false)
        {
            let _ = child.kill();
            let _ = child.wait();
            return Err(io::Error::other(
                "Mac event tap unavailable; grant Accessibility permission",
            ));
        }
        Ok((
            Arc::new(Self {
                gate,
                stdin: Mutex::new(stdin),
                child: Mutex::new(child),
                replies: Mutex::new(reply_rx),
                alive,
            }),
            events,
        ))
    }

    fn command(&self, line: &str) -> bool {
        self.stdin
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .write_all(line.as_bytes())
            .is_ok()
    }
}

impl CaptureControl for MacCapture {
    fn arm(&self, generation: u32) -> bool {
        if !self.alive.load(Ordering::Acquire) || !self.gate.arm(generation) {
            return false;
        }
        if !self.command(&format!("ARM {generation}\n"))
            || !self
                .replies
                .lock()
                .unwrap_or_else(|error| error.into_inner())
                .recv_timeout(Duration::from_millis(150))
                .unwrap_or(false)
        {
            self.disarm();
            return false;
        }
        true
    }

    fn disarm(&self) {
        self.gate.disarm();
        let _ = self.command("LOCAL\n");
    }

    fn generation(&self) -> u32 {
        self.gate.generation()
    }

    fn fault(&self) -> Option<CaptureFault> {
        self.gate.fault()
    }

    fn physical_all_up(&self) -> bool {
        self.gate.physical_all_up()
    }

    fn actor_heartbeat(&self) {
        if !self.command("BEAT\n") {
            self.gate.mark_fault(CaptureFault::MessagePumpFailure);
        }
    }
}

impl Drop for MacCapture {
    fn drop(&mut self) {
        self.gate.disarm();
        let _ = self.command("LOCAL\n");
        let mut child = self.child.lock().unwrap_or_else(|error| error.into_inner());
        let _ = child.kill();
        let _ = child.wait();
    }
}

fn read_events(
    output: impl io::Read,
    gate: Arc<CaptureGate>,
    replies: Sender<bool>,
    ready: mpsc::SyncSender<bool>,
    alive: Arc<AtomicBool>,
) {
    let mut ready = Some(ready);
    for line in BufReader::new(output).lines() {
        let Ok(line) = line else { break };
        match line.as_str() {
            "READY" => {
                if let Some(sender) = ready.take() {
                    let _ = sender.send(true);
                }
            }
            "ARMED" => {
                let _ = replies.send(true);
            }
            "DENIED" => {
                let _ = replies.send(false);
            }
            "FAULT" => gate.mark_fault(CaptureFault::MessagePumpFailure),
            _ => parse_input(&line, &gate),
        }
    }
    alive.store(false, Ordering::Release);
    gate.mark_fault(CaptureFault::MessagePumpFailure);
    if let Some(sender) = ready {
        let _ = sender.send(false);
    }
}

fn parse_input(line: &str, gate: &CaptureGate) {
    if let Some(event) = parse_event(line, gate) {
        let _ = gate.offer(event);
    }
}

fn parse_event(line: &str, gate: &CaptureGate) -> Option<PhysicalEvent> {
    let fields: Vec<&str> = line.split_ascii_whitespace().collect();
    let number = |index: usize| fields.get(index).and_then(|s| s.parse::<i32>().ok());
    match fields.first().copied() {
        Some("K") if fields.len() == 4 => {
            let (key, down, repeat) = (number(1)?, number(2)?, number(3)?);
            if !(0..=255).contains(&key) || !(0..=1).contains(&down) || !(0..=1).contains(&repeat) {
                return None;
            }
            gate.record_physical_key(key as u32, false, down != 0);
            Some(PhysicalEvent::Key {
                virtual_key: key as u32,
                scan_code: key as u32,
                extended: false,
                down: down != 0,
                repeat: repeat != 0,
            })
        }
        Some("M") if fields.len() == 3 => Some(PhysicalEvent::Motion(number(1)?, number(2)?)),
        Some("B") if fields.len() == 3 => {
            let button = match number(1)? {
                0 => MouseButton::Left,
                1 => MouseButton::Right,
                2 => MouseButton::Middle,
                3 => MouseButton::X1,
                4 => MouseButton::X2,
                _ => return None,
            };
            let down = number(2)? != 0;
            gate.record_physical_button(button, down);
            Some(PhysicalEvent::Button(button, down))
        }
        Some("W") if fields.len() == 3 => {
            let axis = match number(1)? {
                0 => MouseAxis::Vertical,
                1 => MouseAxis::Horizontal,
                _ => return None,
            };
            Some(PhysicalEvent::Wheel(
                axis,
                number(2)?.clamp(i16::MIN as i32, i16::MAX as i32) as i16,
            ))
        }
        Some("H") if fields.len() == 1 => Some(PhysicalEvent::Hotkey(Action::Local)),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_mouse_and_key_events_with_generation() {
        let (gate, events) = CaptureGate::new(8);
        parse_input("K 0 0 0", &gate);
        assert!(gate.arm(7));
        parse_input("K 0 1 0", &gate);
        parse_input("M 3 -2", &gate);
        assert_eq!(events.recv().unwrap().generation, 7);
        assert_eq!(events.recv().unwrap().event, PhysicalEvent::Motion(3, -2));
        assert!(!gate.physical_all_up());
    }
}
