// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Verifies authoritative display normalization and generation changes without
// depending on the connected Windows monitor arrangement.

use esp32_kvm_platform_windows::{DisplayRecord, MonitorInventory};

fn display(id: &str, x: i32, width: u32, dpi: u32, rotation: u32, primary: bool) -> DisplayRecord {
    DisplayRecord {
        id: id.into(),
        x,
        y: -100,
        width,
        height: 900,
        dpi_x: dpi,
        dpi_y: dpi,
        rotation_degrees: rotation,
        primary,
    }
}

#[test]
fn negative_origins_mixed_dpi_and_rotation_survive_normalization() {
    let mut inventory = MonitorInventory::new();
    assert!(
        inventory
            .update(vec![
                display("left", -1600, 1600, 144, 90, false),
                display("primary", 0, 1920, 96, 0, true)
            ])
            .unwrap()
    );
    let monitors = inventory.topology().unwrap().monitors();
    assert_eq!(monitors[0].rect.x, -1600);
    assert_eq!(monitors[0].dpi_x, 144);
    assert_eq!(format!("{:?}", monitors[0].rotation), "Deg90");
}

#[test]
fn hotplug_advances_generation_but_reordering_does_not() {
    let mut inventory = MonitorInventory::new();
    let first = vec![
        display("left", -1600, 1600, 144, 0, false),
        display("primary", 0, 1920, 96, 0, true),
    ];
    assert!(inventory.update(first.clone()).unwrap());
    let generation = inventory.topology().unwrap().generation();
    assert!(!inventory.update(first.into_iter().rev().collect()).unwrap());
    assert_eq!(inventory.topology().unwrap().generation(), generation);
    assert!(
        inventory
            .update(vec![display("primary", 0, 1920, 96, 0, true)])
            .unwrap()
    );
    assert!(inventory.topology().unwrap().generation() > generation);
}

#[test]
fn invalid_snapshot_keeps_previous_topology() {
    let mut inventory = MonitorInventory::new();
    inventory
        .update(vec![display("primary", 0, 1920, 96, 0, true)])
        .unwrap();
    let generation = inventory.topology().unwrap().generation();
    assert!(
        inventory
            .update(vec![display("primary", 0, 1920, 96, 45, true)])
            .is_err()
    );
    assert_eq!(inventory.topology().unwrap().generation(), generation);
}
