use esp32_kvm_host_actor::{HostActor, HostState, SetOneKeyMapper};
use esp32_kvm_input_core::Action;
use esp32_kvm_platform_windows::{
    CaptureEvent, CaptureGate, MouseAxis, MouseButton, PhysicalEvent,
};
use esp32_kvm_usb_transport::probe_stream;
use std::thread;
use std::time::{Duration, Instant};

/// Routes to the first ready guest, then sends a key, pointer motion, a click and a wheel tick.
fn main() -> Result<(), String> {
    let (capture, receiver) = CaptureGate::new(64);
    let started = Instant::now();
    let now = || started.elapsed().as_millis() as u64;
    let mut port = serialport::new("COM4", 115_200)
        .timeout(Duration::from_millis(20))
        .open()
        .map_err(|error| format!("open failed: {error:?}"))?;
    let device = probe_stream(&mut port, "esp32-kvm-s3", "send-input")
        .map_err(|error| format!("connect failed: {error:?}"))?;
    let mut actor = HostActor::from_confirmed(
        port,
        Box::new(capture.clone()),
        device,
        Vec::new(),
        Box::new(SetOneKeyMapper),
        0,
    )
    .map_err(|error| format!("connect failed: {error:?}"))?;
    let pump = |actor: &mut HostActor<_>, millis: u64| {
        let until = Instant::now() + Duration::from_millis(millis);
        while Instant::now() < until {
            actor.drive(&receiver, now());
            thread::sleep(Duration::from_millis(5));
        }
    };

    let deadline = Instant::now() + Duration::from_secs(4);
    while Instant::now() < deadline && actor.state() != HostState::Local {
        actor.drive(&receiver, now());
        actor.observe_all_up();
        thread::sleep(Duration::from_millis(5));
    }
    let before = actor.setup_snapshot();
    println!("snapshot slots={:?}", before.slots);
    let slot = before
        .slots
        .iter()
        .find(|slot| slot.ready && slot.subscribed)
        .map(|slot| slot.slot)
        .ok_or("no ready and subscribed guest")?;
    let before_count = before
        .slots
        .iter()
        .find(|seen| seen.slot == slot)
        .map_or(0, |seen| seen.notifications_delivered);

    actor.request(Action::Direct(slot), now());
    let deadline = Instant::now() + Duration::from_secs(4);
    while Instant::now() < deadline && !matches!(actor.state(), HostState::Guest(_)) {
        actor.drive(&receiver, now());
        actor.observe_all_up();
        thread::sleep(Duration::from_millis(5));
    }
    if !matches!(actor.state(), HostState::Guest(_)) {
        return Err(format!(
            "route failed: state={:?} fault={:?}",
            actor.state(),
            actor.fault()
        ));
    }
    let generation = capture.generation();
    let mut send = |actor: &mut HostActor<_>, event: PhysicalEvent| {
        actor.on_capture(CaptureEvent { generation, event }, now());
        pump(actor, 30);
    };

    // Keyboard: 'b' down/up.
    for down in [true, false] {
        send(
            &mut actor,
            PhysicalEvent::Key {
                virtual_key: 0x42,
                scan_code: 0x30,
                extended: false,
                down,
                repeat: false,
            },
        );
        pump(&mut actor, 60);
    }
    // Pointer: a square of relative motion, then a left click and one wheel tick.
    for (dx, dy) in [(40, 0), (0, 40), (-40, 0), (0, -40)] {
        for _ in 0..5 {
            send(&mut actor, PhysicalEvent::Motion(dx, dy));
        }
    }
    for down in [true, false] {
        send(&mut actor, PhysicalEvent::Button(MouseButton::Left, down));
        pump(&mut actor, 60);
    }
    send(&mut actor, PhysicalEvent::Wheel(MouseAxis::Vertical, 120));
    pump(&mut actor, 1500);

    let after = actor.setup_snapshot();
    let after_count = after
        .slots
        .iter()
        .find(|seen| seen.slot == slot)
        .map_or(0, |seen| seen.notifications_delivered);
    println!(
        "delivered delta={} state={:?} fault={:?} trace={:?}",
        after_count.wrapping_sub(before_count),
        actor.state(),
        actor.fault(),
        actor.input_trace()
    );
    let ok = matches!(actor.state(), HostState::Guest(_)) && actor.fault().is_none();
    actor.shutdown();
    if ok { Ok(()) } else { Err("input test faulted".into()) }
}
