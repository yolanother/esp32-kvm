// Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
// Diagnostic CoreBluetooth client for the ESP32 KVM HID over GATT peripheral.
// Run on macOS: swift tools/mac-ble-probe.swift
import CoreBluetooth
import Foundation

final class BLEProbe: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private var central: CBCentralManager!
    private var target: CBPeripheral?
    private let hid = CBUUID(string: "1812")
    private let battery = CBUUID(string: "180F")
    private let pairViaBattery = CommandLine.arguments.contains("--pair-via-battery")
    private let deadline = DispatchTime.now() + .seconds(90)
    private var pendingServices = 0
    private var pendingReads = 0

    private func finishIfComplete() {
        if pendingServices == 0 && pendingReads == 0 {
            DispatchQueue.main.asyncAfter(deadline: .now() + .seconds(2)) { exit(0) }
        }
    }

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
        DispatchQueue.main.asyncAfter(deadline: deadline) {
            print("TIMEOUT: no completed HID GATT read after 90 seconds")
            exit(2)
        }
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        guard central.state == .poweredOn else {
            print("Bluetooth state: \(central.state.rawValue)")
            if central.state != .unknown && central.state != .resetting { exit(3) }
            return
        }
        print("Scanning for ESP32 KVM")
        central.scanForPeripherals(withServices: nil, options: [
            CBCentralManagerScanOptionAllowDuplicatesKey: false
        ])
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        let advertised = advertisementData[CBAdvertisementDataLocalNameKey] as? String
        guard advertised == "ESP32 KVM" || peripheral.name == "ESP32 KVM" else { return }
        print("Found ESP32 KVM, RSSI \(RSSI), identifier \(peripheral.identifier)")
        central.stopScan()
        target = peripheral
        peripheral.delegate = self
        central.connect(peripheral, options: nil)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        print("BLE connected; discovering GATT services")
        peripheral.discoverServices(nil)
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral,
                        error: Error?) {
        print("BLE connect failed: \(String(describing: error))")
        exit(4)
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral,
                        error: Error?) {
        print("BLE disconnected: \(String(describing: error))")
        exit(5)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error { print("Service discovery failed: \(error)"); exit(6) }
        pendingServices = (peripheral.services ?? []).count
        for service in peripheral.services ?? [] {
            print("Service \(service.uuid)")
            peripheral.discoverCharacteristics(nil, for: service)
        }
        if !(peripheral.services ?? []).contains(where: { $0.uuid == hid }) {
            print("HID service 1812 not exposed to this CoreBluetooth client")
        }
        finishIfComplete()
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService,
                    error: Error?) {
        pendingServices -= 1
        if let error {
            print("Characteristics for \(service.uuid) failed: \(error)")
            finishIfComplete()
            return
        }
        for characteristic in service.characteristics ?? [] {
            print("Characteristic \(service.uuid)/\(characteristic.uuid) properties=\(characteristic.properties.rawValue)")
            if pairViaBattery && service.uuid == battery && characteristic.uuid == CBUUID(string: "2A19") &&
                characteristic.properties.contains(.read) {
                pendingReads += 1
                print("Reading protected Battery Level to request BLE pairing")
                peripheral.readValue(for: characteristic)
            } else if !pairViaBattery && service.uuid == hid &&
                ["2A4A", "2A4B", "2A4D", "2A4E"].contains(characteristic.uuid.uuidString.uppercased()) &&
                characteristic.properties.contains(.read) {
                pendingReads += 1
                peripheral.readValue(for: characteristic)
            }
        }
        finishIfComplete()
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic,
                    error: Error?) {
        pendingReads -= 1
        if let error {
            print("Read \(characteristic.uuid) failed: \(error)")
            finishIfComplete()
            return
        }
        let hex = (characteristic.value ?? Data()).map { String(format: "%02x", $0) }.joined()
        print("Read \(characteristic.uuid) = \(hex)")
        finishIfComplete()
    }
}

let probe = BLEProbe()
RunLoop.main.run()
