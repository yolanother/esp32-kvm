# Device-owned input path

The primary route is **Bluetooth LE keyboard and mouse → ESP32-S3 → selected output**. Output zero is the Windows PC over USB HID; outputs 1–3 are bonded BLE HID guests. The board owns selection, release, and report forwarding. The Windows app configures names, mappings, pairing, and optional host-capture features; closing it, sleeping Windows, or rebooting Windows must not interrupt a selected Mac route. The existing PC-capture → USB protocol → BLE route remains an optional input source, admitted only by an explicit source-mode choice and never concurrently with the paired physical input source.

## Hardware and stack boundaries

- The current firmware uses the ESP32-S3's fixed USB Serial/JTAG CDC controller. It cannot add a HID interface. Change to USB-OTG TinyUSB **device** mode with a composite keyboard, mouse, and CDC/configuration interface on the existing PC-facing cable. Keep a separately verified ROM BOOT recovery route before replacing the working USB transport. USB HID and CDC endpoint allocation, VID/PID, Windows enumeration, app discovery, and update handoff all need an integration test.
- The board's one USB-C connector is PC-facing. Bluetooth input avoids needing a second USB host port, hub, VBUS source, or external PHY. Wired or proprietary 2.4 GHz receiver input would require different hardware.
- The ESP32-S3 is Bluetooth LE only. The input host should be a generic encrypted HID-over-GATT client: discover report maps and report references, parse standard keyboard/pointer usages, handle reconnect and bonded identity, and reject unsupported reports explicitly. It must not depend on a model whitelist. Vendor-specific buttons may remain unsupported until mapped.
- Current firmware allocates three NimBLE connections and four controller activities for output guests. Two BLE input sources plus up to three output guests require a larger measured budget. Pairing storage must distinguish trusted input devices from output hosts and preserve both across reboot.
- USB power from a sleeping or rebooting PC is not a guarantee. The board needs an independently verified power source if the Mac route must survive loss of PC VBUS.

## Route invariants

The portable `hardware_route` policy begins disarmed. A target change releases the old output and waits for keyboard and pointer all-up before admitting new reports. It then sends an all-up baseline to the selected output and arms it before forwarding input. A source disconnect releases held state and returns to local USB. Loss of USB output while a BLE guest is selected leaves that guest route untouched. Each output adapter must be able to send all-up; a failed release disconnects that BLE output. The USB session heartbeat may govern the optional PC-capture source, never the hardware-primary source.

## Integration sequence

1. Implement and test a generic BLE HID input client on the board with one keyboard and one mouse, including encrypted bonding, report-map parsing, all-up on disconnect, and reconnection after power cycle.
2. Wire the device-owned route gate to those input reports and the existing BLE HID guest output. Prove switching to the Mac while the PC USB cable is disconnected; no Windows app may be running.
3. Add TinyUSB composite USB HID + CDC device output. Verify keyboard, pointer, wheel, LED output, USB suspend/re-enumeration, and BOOT recovery before changing the installed host app's USB discovery.
4. Keep PC capture behind explicit source selection. The board must arbitrate one source and one output, so software and BLE devices cannot send simultaneous held states.
5. Validate two input devices plus three output guests at the chosen connection intervals. Measure report latency, free internal memory, reconnect time, and switching release behavior on physical hardware.

The MSI's login startup supports the current app and optional PC route. It is not a boot-time hardware service; the device-owned route is what removes that service from the essential input path.
