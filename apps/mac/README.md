# macOS USB host client

This command-line client uses the shared verified USB host actor. Its native event-tap helper captures physical keyboard and mouse input only after a READY guest route is acknowledged. An independent half-second lease in the helper restores local delivery if the actor stops responding. The helper starts disarmed and requires macOS Accessibility permission.

Build on a Mac from the repository root:

```sh
mkdir -p apps/mac/build
swiftc -O -o apps/mac/build/capture apps/mac/capture.swift
cargo test -p esp32-kvm-mac
cargo build -p esp32-kvm-mac
```

With the ESP32 KVM connected to that Mac by USB, run `target/debug/esp32-kvm-mac apps/mac/build/capture`. Grant Accessibility access to the compiled capture executable in System Settings if prompted, then restart it. The program confirms the board ID and protocol before accepting `guest 1`, `guest 2`, `guest 3`, `local`, `status`, or `quit`. A guest must be authenticated and subscribed before the shared actor can arm capture. Press both physical Control keys together for an emergency local return. The command-line `local` action and process exit also restore local input.

The current key translator covers common ANSI physical keys, standard modifiers, navigation keys, and F1–F12. Unknown keys are not forwarded. macOS secure input and protected system shortcuts may bypass or disable the tap; the actor then fails local. Native macOS capture, permission, mouse deltas, wheel direction, reconnect, and crash recovery still require hardware validation before this client is released.
