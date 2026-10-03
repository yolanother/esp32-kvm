// Copyright (c) ESP32 KVM contributors. Use of this file is governed by the root LICENSE.
// Captures physical macOS keyboard and mouse events for the USB host client. A CGEvent tap
// suppresses local delivery only while armed; pipe failure or a missed lease fails local.

import ApplicationServices
import Darwin
import Foundation

private final class CaptureState {
    let lock = NSLock()
    var armed = false
    var lastBeat = 0.0
    var heldKeys = Set<Int64>()
    var heldButtons = Set<Int64>()

    func emit(_ line: String) -> Bool {
        let bytes = Array((line + "\n").utf8)
        return bytes.withUnsafeBytes { raw in
            guard let base = raw.baseAddress else { return false }
            return Darwin.write(STDOUT_FILENO, base, bytes.count) == bytes.count
        }
    }

    func command(_ line: String) {
        lock.lock()
        defer { lock.unlock() }
        if line == "LOCAL" {
            armed = false
        } else if line == "BEAT" {
            lastBeat = ProcessInfo.processInfo.systemUptime
        } else if line.hasPrefix("ARM ") {
            let osKeysUp = (0..<128).allSatisfy {
                !CGEventSource.keyState(.combinedSessionState, key: CGKeyCode($0))
            }
            let osButtonsUp = !CGEventSource.buttonState(.combinedSessionState, button: .left)
                && !CGEventSource.buttonState(.combinedSessionState, button: .right)
                && !CGEventSource.buttonState(.combinedSessionState, button: .center)
            if heldKeys.isEmpty && heldButtons.isEmpty && osKeysUp && osButtonsUp {
                armed = true
                lastBeat = ProcessInfo.processInfo.systemUptime
                if !emit("ARMED") { armed = false }
            } else {
                _ = emit("DENIED")
            }
        }
    }

    func event(_ type: CGEventType, _ event: CGEvent) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        if type == .tapDisabledByTimeout || type == .tapDisabledByUserInput {
            armed = false
            _ = emit("FAULT")
            return false
        }
        if armed && ProcessInfo.processInfo.systemUptime - lastBeat > 0.5 {
            armed = false
            _ = emit("FAULT")
        }
        var record: String?
        let key = event.getIntegerValueField(.keyboardEventKeycode)
        switch type {
        case .keyDown:
            let repeatKey = heldKeys.contains(key)
            heldKeys.insert(key)
            record = "K \(key) 1 \(repeatKey ? 1 : 0)"
        case .keyUp:
            heldKeys.remove(key)
            record = "K \(key) 0 0"
        case .flagsChanged:
            let down = !heldKeys.contains(key)
            if down { heldKeys.insert(key) } else { heldKeys.remove(key) }
            record = "K \(key) \(down ? 1 : 0) 0"
            if armed && heldKeys.contains(59) && heldKeys.contains(62) {
                if !emit("H") { _ = emit("FAULT") }
                armed = false
                return true
            }
        case .mouseMoved, .leftMouseDragged, .rightMouseDragged, .otherMouseDragged:
            let dx = event.getIntegerValueField(.mouseEventDeltaX)
            let dy = event.getIntegerValueField(.mouseEventDeltaY)
            if dx != 0 || dy != 0 { record = "M \(dx) \(dy)" }
        case .leftMouseDown, .leftMouseUp, .rightMouseDown, .rightMouseUp,
             .otherMouseDown, .otherMouseUp:
            let button = event.getIntegerValueField(.mouseEventButtonNumber)
            let down = type == .leftMouseDown || type == .rightMouseDown || type == .otherMouseDown
            if down { heldButtons.insert(button) } else { heldButtons.remove(button) }
            record = "B \(button) \(down ? 1 : 0)"
        case .scrollWheel:
            let vertical = event.getIntegerValueField(.scrollWheelEventDeltaAxis1)
            let horizontal = event.getIntegerValueField(.scrollWheelEventDeltaAxis2)
            if vertical != 0 { record = "W 0 \(vertical * 120)" }
            if horizontal != 0 && !emit("W 1 \(horizontal * 120)") {
                armed = false
                return false
            }
        default:
            break
        }
        if let record, !emit(record) {
            armed = false
            return false
        }
        return armed
    }
}

private func callback(
    _ proxy: CGEventTapProxy,
    type: CGEventType,
    event: CGEvent,
    userInfo: UnsafeMutableRawPointer?
) -> Unmanaged<CGEvent>? {
    guard let userInfo else { return Unmanaged.passUnretained(event) }
    let state = Unmanaged<CaptureState>.fromOpaque(userInfo).takeUnretainedValue()
    let suppressed = state.event(type, event)
    return suppressed ? nil : Unmanaged.passUnretained(event)
}

guard AXIsProcessTrusted() else {
    fputs("ESP32 KVM needs Accessibility permission for this capture helper.\n", stderr)
    exit(1)
}
let state = CaptureState()
let flags = fcntl(STDOUT_FILENO, F_GETFL)
_ = fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK)
let types: [CGEventType] = [
    .keyDown, .keyUp, .flagsChanged, .mouseMoved,
    .leftMouseDragged, .rightMouseDragged, .otherMouseDragged,
    .leftMouseDown, .leftMouseUp, .rightMouseDown, .rightMouseUp,
    .otherMouseDown, .otherMouseUp, .scrollWheel,
]
let mask = types.reduce(CGEventMask(0)) { $0 | (CGEventMask(1) << $1.rawValue) }
guard let tap = CGEvent.tapCreate(
    tap: .cgSessionEventTap,
    place: .headInsertEventTap,
    options: .defaultTap,
    eventsOfInterest: mask,
    callback: callback,
    userInfo: Unmanaged.passUnretained(state).toOpaque()
) else {
    fputs("ESP32 KVM could not create a HID event tap.\n", stderr)
    exit(1)
}
let source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0)
CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes)
CGEvent.tapEnable(tap: tap, enable: true)
guard state.emit("READY") else { exit(1) }
DispatchQueue.global(qos: .userInteractive).async {
    while let line = readLine() { state.command(line) }
    state.command("LOCAL")
    exit(0)
}
CFRunLoopRun()
