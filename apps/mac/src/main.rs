// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Runs the macOS USB host controller: verified board discovery, native capture,
// local/guest route requests, and text status. Input timing stays outside the UI.

mod awake;
mod capture;
mod keymap;

use awake::{AwakeBump, paired_motion};
use capture::MacCapture;
use esp32_kvm_host_actor::{CaptureControl, HostState, connect_system};
use esp32_kvm_input_core::Action;
use keymap::MacKeyMapper;
use std::env;
use std::io::{self, BufRead};
use std::sync::mpsc::{self, TryRecvError};
use std::thread;
use std::time::{Duration, Instant};

/// Opens a verified USB session and accepts route, status, and idle-motion commands.
fn main() -> Result<(), Box<dyn std::error::Error>> {
    let helper = env::args()
        .nth(1)
        .ok_or("usage: esp32-kvm-mac /path/to/capture-helper")?;
    let (capture, events) = MacCapture::start(&helper)?;
    let started = Instant::now();
    let mut actor = connect_system(
        "esp32-kvm-s3",
        "0.1.0-m1",
        Box::new(capture.clone()),
        Vec::new(),
        Box::new(MacKeyMapper),
        0,
    )
    .map_err(|error| format!("verified board connection failed: {error:?}"))?;
    let (commands, command_rx) = mpsc::channel();
    thread::spawn(move || {
        for line in io::stdin().lock().lines() {
            let Ok(line) = line else { break };
            if commands.send(line).is_err() {
                break;
            }
        }
        let _ = commands.send("quit".into());
    });
    println!(
        "ESP32 KVM Mac host ready. Commands: guest 1..3, local, status, awake on|off|status, quit"
    );
    let mut previous = None;
    let mut last_status = Instant::now();
    let mut awake = AwakeBump::new();
    loop {
        let now_ms = started.elapsed().as_millis() as u64;
        actor.drive(&events, now_ms);
        match command_rx.try_recv() {
            Ok(command) => match command.trim() {
                "local" => {
                    capture.disarm();
                    actor.request(Action::Local, now_ms);
                }
                "status" => {
                    print_status(&actor.setup_snapshot());
                    println!("awake={}", if awake.enabled() { "on" } else { "off" });
                }
                "awake on" => {
                    awake.set_enabled(true);
                    println!(
                        "awake=on (one paired one-pixel bump every 30 seconds on an all-up guest route)"
                    );
                }
                "awake off" => {
                    awake.set_enabled(false);
                    println!("awake=off");
                }
                "awake status" => {
                    println!("awake={}", if awake.enabled() { "on" } else { "off" });
                }
                "quit" => break,
                text if text.starts_with("guest ") => {
                    if let Ok(slot @ 1..=3) = text[6..].trim().parse::<u8>() {
                        actor.request(Action::Direct(slot), now_ms);
                    } else {
                        eprintln!("expected guest slot 1, 2, or 3");
                    }
                }
                _ => eprintln!("commands: guest 1..3, local, status, awake on|off|status, quit"),
            },
            Err(TryRecvError::Empty) => {}
            Err(TryRecvError::Disconnected) => break,
        }
        let state = actor.state();
        let generation = capture.generation();
        let route_ready = match state {
            HostState::Guest(active) => actor
                .setup_snapshot()
                .slots
                .iter()
                .any(|slot| slot.slot == active && slot.ready && slot.subscribed),
            _ => false,
        };
        if awake.due(
            now_ms,
            state,
            generation,
            route_ready,
            capture.fault().is_none() && capture.physical_all_up(),
        ) {
            for event in paired_motion(generation) {
                actor.on_capture(event, now_ms);
                if actor.state() != state || capture.generation() != generation {
                    break;
                }
            }
        }
        if previous != Some(state) || last_status.elapsed() >= Duration::from_secs(5) {
            print_status(&actor.setup_snapshot());
            previous = Some(state);
            last_status = Instant::now();
        }
        if state == HostState::Failed {
            break;
        }
        thread::sleep(Duration::from_millis(10));
    }
    capture.disarm();
    actor.shutdown();
    Ok(())
}

fn print_status(snapshot: &esp32_kvm_host_actor::SetupSnapshot) {
    println!(
        "route={:?} pairing={:?} guests={}",
        snapshot.state,
        snapshot.pairing_status,
        snapshot.slots.len()
    );
    for slot in &snapshot.slots {
        println!(
            "  slot={} ready={} subscribed={}",
            slot.slot, slot.ready, slot.subscribed
        );
    }
}
