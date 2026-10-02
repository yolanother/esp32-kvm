# M0 USB transport

The attached ESP32-S3 exposes the fixed USB Serial/JTAG CDC interface. The M0 transport reserves that byte stream for COBS framed binary protocol messages; ESP-IDF console and secondary logs must use another channel. `firmware/components/transport` starts disarmed, rotates the session after USB disconnect, and implements HELLO/CAPS/SESSION_OPEN loopback without forwarding input.

The host crate `crates/usb-transport` first filters ports by the Espressif VID/PID observed during bring-up, then requires a valid protocol handshake and expected board identity. A COM name alone is never trusted. Version, board, malformed frame, timeout, and disconnect outcomes are reported separately.

The firmware entry point does not start this component yet. Source tests cover the host discovery policy and firmware frame core, but physical loopback, unplug/re-enumeration, latency, and ESP-IDF build are pending board identification and a verified recovery path. Adding another USB interface or moving to TinyUSB requires a new identity/capability check.
