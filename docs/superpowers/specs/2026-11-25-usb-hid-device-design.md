# USB HID device (TinyUSB composite HID + CDC) design: stage 3

Date: 2026-11-25. Status: written for owner review. Builds on
`2026-11-24-doubtech-hid-library-design.md` (stage 1) and
`2026-11-25-device-input-central-design.md` (stage 2, branch
`feature/device-input-central` at `75fc706`). Branch: `feature/usb-hid-device`.

## Intent

The owner wants a device-centric KVM: the board is the central mechanism. The owner's own
Bluetooth keyboard and mouse stay paired to the board (stage 2) and the board shares them
with the BLE guests **and with the PC**, over a USB HID connection, so the Bluetooth links
stay with the board whichever computer is in use. The same USB cable also keeps the existing
control layer: the desktop app and its PC capture remain one more input source (option A).
The PC-app-driven path must keep working while the new layers are built.

Today hub target 0 ("This PC") is a stub that swallows input
(`firmware/components/input_hub/input_hub.c`, `kvm_input_hub_input`: `if (hub->target == 0)
return KVM_HUB_FORWARDED;`). Stage 3 makes it real.

## Goals

1. The board enumerates on the PC as one composite USB device: boot-compatible keyboard,
   relative mouse with wheel and horizontal pan, consumer control, and a CDC-ACM serial port
   that carries the existing control protocol unchanged (`transport_core.c` framing, COBS,
   CRC32C, `protocol/schema/v1.md`).
2. Device-owned input (stage 2 central) on hub target 0 reaches the PC as USB HID reports, with
   no desktop app running, before Windows login, and in BIOS/UEFI (boot keyboard).
3. The PC-driven path keeps working: the desktop app finds the board on the CDC port, captures
   PC input and routes it to a guest; while it has a guest armed the PC receives no HID from
   the board.
4. Flashing and recovery are always possible, and the first image with TinyUSB in it is
   flashable by today's known-good method.
5. The board survives host suspend, bus reset, re-enumeration, cable unplug and PC sleep/wake
   with no stuck keys.
6. Boards still in USB Serial/JTAG mode (`303A:1001`) keep working with the new host app.

## Non-goals

- Keyboard LED forwarding to the BLE keyboard (LED output reports are accepted and ignored).
- NKRO, absolute pointer, touch digitizer, gamepad, mass storage, DFU, WebUSB.
- Making the mode switch available from the desktop app (touchscreen only in this stage; the
  app only reads the mode).
- Dual-image/rollback boot (see Approaches, B). OTA data, partition table and bootloader stay
  untouched; `tools/firmware.ps1` keeps flashing the app partition only.
- Any change to guest pairing, bond storage, the 500 ms host lease or router semantics.
- A USB PID registration (an action for the owner, see Decisions 4).

## Constraints and verified facts

- Board: Waveshare ESP32-S3-Touch-LCD-1.54, one Type-C wired to native USB GPIO19/GPIO20
  (`docs/hardware/board-bringup.md`, vendor schematic row "USB D-, D+"). No external USB-UART.
  Today it enumerates as USB Serial/JTAG `303A:1001` (COM4) and the control protocol runs over it
  (`firmware/components/transport/transport_usb_serial_jtag.c`). The IDF console is on UART0,
  which is not wired to the connector (`firmware/sdkconfig`: `CONFIG_ESP_CONSOLE_UART=y`), and
  the transport refuses any USB console at compile time.
- One internal full-speed PHY is shared by the USB Serial/JTAG (USJ) controller and the USB-OTG
  controller. The mux lives in RTC registers: `RTCCNTL.usb_conf.sw_hw_usb_phy_sel` and
  `sw_usb_phy_sel` (`components/hal/esp32s3/include/hal/usb_serial_jtag_ll.h`,
  `usb_serial_jtag_ll_phy_enable_external`; `hal/esp32s3/include/hal/usb_wrap_ll.h`,
  `usb_wrap_ll_phy_enable_external`). While TinyUSB owns the PHY, COM4, JTAG and esptool
  auto-reset are gone.
- `usb_serial_jtag_driver_install()` maps the PHY back to USJ every time it runs
  (`components/esp_driver_usb_serial_jtag/src/usb_serial_jtag.c:213`,
  `usb_serial_jtag_ll_phy_enable_external(false)`). Today's firmware calls it at every start,
  so any boot that takes the serial path reclaims the PHY even after a software reset from
  TinyUSB mode.
- `esp_restart()` on the S3 resets the CPUs only (`esp_system/port/soc/esp32s3/system_internal.c`,
  `esp_rom_software_reset_cpu`); RTC registers, including the PHY mux and `RTC_NOINIT_ATTR`
  memory (`esp_common/include/esp_attr.h:98`), survive it. A power-on reset clears both.
- Software entry to ROM download mode exists in IDF:
  `REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT)` then restart
  (`esp_system/port/usb_console.c`, `esp_usb_console_before_restart`; register in
  `soc/esp32s3/register/soc/rtc_cntl_reg.h:3209`). Not used as the primary path (Decision 9).
- USB-OTG endpoint budget (`soc/esp32s3/include/soc/usb_dwc_cfg.h`): `OTG_NUM_EPS 6`,
  `OTG_NUM_IN_EPS 5`, `OTG_DFIFO_DEPTH 256` (words, shared RX and TX FIFO). TinyUSB upstream
  (`src/portable/synopsys/dwc2/dwc2_esp32.h`, fetched from GitHub, not in a local tree) uses
  `.ep_count = 7, .ep_in_count = 5`. The Synopsys GHWCFG4 INEPS field counts IN endpoints
  **including** EP0 (`soc/esp32h4/register/soc/usb_otgfs_core_ctrl_reg.h:2360`). This design
  assumes the conservative reading: EP0 plus **at most 4 IN endpoints**.
- Reset reasons available: `ESP_RST_PANIC`, `ESP_RST_INT_WDT`, `ESP_RST_TASK_WDT`, `ESP_RST_WDT`,
  `ESP_RST_BROWNOUT`, `ESP_RST_CPU_LOCKUP`, `ESP_RST_USB`, `ESP_RST_SW`, `ESP_RST_POWERON`
  (`esp_system/include/esp_system.h`).
- TinyUSB is **not** in ESP-IDF 5.5.1 (`D:\esp\esp-idf-v5.5.1\components` has `usb` (host stack
  and PHY driver) but no `tinyusb`; no `examples/` in the install). The project does not pin it:
  `firmware/main/idf_component.yml` does not exist; the only manifests are
  `components/display/idf_component.yml` (LVGL, touch) and the two doubtech manifests;
  `firmware/dependencies.lock` lists only LVGL, `esp_lvgl_port` and touch components.
  `espressif/esp_tinyusb` must be added from the component registry, which changes
  `firmware/dependencies.lock` (owner-gated, see Decisions 16).
- esp_tinyusb API, checked against the GitHub `espressif/esp-usb` master headers (version 3.0.1,
  manifest says IDF `>=5.3`), **not** against a local copy: `tinyusb_driver_install(const
  tinyusb_config_t *)`, `tinyusb_driver_uninstall()`, `tinyusb_remote_wakeup()`; config fields
  `port`, `phy {skip_setup, self_powered, vbus_monitor_io}`, `task {size, priority, xCoreID}`,
  `descriptor {device, qualifier, string, string_count, full_speed_config, high_speed_config}`,
  `event_cb` / `event_arg` with events `TINYUSB_EVENT_ATTACHED`, `_DETACHED`, `_SUSPENDED`
  (carries `remote_wakeup`), `_RESUMED`; CDC: `tinyusb_cdcacm_init(const tinyusb_config_cdcacm_t
  *)` with `callback_rx` and `callback_line_state_changed`, `tinyusb_cdcacm_read`,
  `tinyusb_cdcacm_write_queue`, `tinyusb_cdcacm_write_flush(itf, timeout_ticks)` (0 = non-blocking,
  returns `ESP_ERR_NOT_FINISHED`; must not be called with a timeout from CDC callbacks). HID goes
  through TinyUSB directly (`tud_hid_n_ready`, `tud_hid_n_report`, `tud_hid_descriptor_report_cb`,
  `tud_hid_get_report_cb`, `tud_hid_set_report_cb`, `tud_mounted`, `tud_suspended`): these are
  upstream TinyUSB names from knowledge, not verified in a local tree.
- Host side: `crates/usb-transport/src/lib.rs` finds the board with `candidate_ports`, which keeps
  only `(0x303a, 0x1001)`; `crates/host-actor`, `crates/update-runner` (`flasher.rs`, `reconnect.rs`)
  and `apps/desktop/src-tauri/src/actor_backend.rs` all go through it. `serialport 4.10.1` (pinned
  `=4.10.1` in usb-transport) exposes the composite interface number as
  `UsbPortInfo::interface: Option<u8>` behind feature `usbportinfo-interface`, parsed from the
  Windows hardware ID `MI_xx` (verified in the cargo registry copy, `src/lib.rs:833`,
  `src/windows/enumerate.rs:115`). On macOS it may report the data interface instead.
- The installed desktop app (MSI 0.1.1) only knows `303A:1001`, holds the port and auto-reconnects
  every second. It will not see a board in composite mode until stage 3 phase E is installed.
- Protocol: major 1, `KVM_PROTOCOL_MINOR_MAX 4` (`protocol/schema/protocol_v1.h`,
  `crates/protocol/src/lib.rs: MINOR = 4`). The schema reserves optional extension keys `>=128`
  for future minors (`protocol/schema/v1.md`, "Canonical CBOR payloads").
- Firmware threading: one USB worker task (`kvm_usb_loopback`, priority 10, 8 KB stack) owns the
  transport core, router, hub and pairing windows; it blocks at most 20 ms in
  `usb_serial_jtag_read_bytes` and one tick (10 ms, `CONFIG_FREERTOS_HZ=100`) when device input
  is queued. `send_binary` blocks at most 50 ms per write and resets the core on failure.
- Buttons: BOOT is GPIO0 and is the ROM download strap; PLUS is not mapped (`display.c`,
  `PLUS_GPIO GPIO_NUM_NC`). A BOOT hold of 1 s is the runtime emergency release, and a BOOT held
  at app start is blocked from triggering it until released (`display_model.c`,
  `kvm_display_buttons_init`).
- Partitions (`firmware/partitions.csv`): nvs `0x9000`, otadata `0xd000`, factory `0x20000`,
  ota_0 `0x670000`. The update runner requires the OTA selector to stay all `0xff`.
- Rules carried over: never erase flash or NVS; flashing needs the owner to quit the app; the
  first-ever TinyUSB build must be flashable by `tools/firmware.ps1 flash` exactly as today.

## Approaches considered

A. **One image, runtime USB mode, default USB Serial/JTAG (chosen).** TinyUSB support is
   compiled in behind a Kconfig switch. At boot the firmware decides the USB mode from an NVS
   flag (absent means Serial/JTAG) and several escape conditions; the owner turns on "USB HID
   mode" from the touchscreen. Every revert path ends in the serial app, which is today's
   proven state. Cost: a few seconds of boot logic, a 1.5 s escape window before composite
   enumeration, and a second byte-link behind the worker.
B. **Dual image with bootloader rollback.** Factory keeps a serial-only image, ota_0 holds the
   composite image, `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` reverts on a boot that does not mark
   itself valid. Strongest "a working image always exists" guarantee, but it needs a new
   bootloader (a `-Full` flash that rewrites `0x0`, the one write that can strand the board
   without ROM recovery), it breaks the update runner's erased-OTA-selector invariant, and
   rollback does not catch the common failure (the app runs but USB is unreachable). Deferred to
   a release-hardening stage.
C. **Compile-time switch only (separate HID build).** Least firmware code, but every return to
   serial needs a reflash, and with no USJ the only reflash path is BOOT ROM download, the one
   path never tested on this board. Rejected.

## Decisions

### Composite design

1. **Interfaces and endpoints (4 IN + 1 OUT besides EP0).**

   | Itf | Function | Class/Sub/Proto | Endpoints |
   |---|---|---|---|
   | 0 | Keyboard, boot-compatible | HID 03/01/01 | `0x81` IN interrupt, 8 B, 1 ms |
   | 1 | Mouse + consumer control | HID 03/00/00 | `0x82` IN interrupt, 16 B, 1 ms |
   | 2 | CDC-ACM communication (IAD first=2, count=2) | 02/02/00 | `0x83` IN interrupt, 8 B, 16 ms |
   | 3 | CDC-ACM data | 0A/00/00 | `0x04` OUT bulk 64 B, `0x84` IN bulk 64 B |

   Device descriptor: USB 2.0, class `EF/02/01` (IAD composite), EP0 64 bytes, bcdDevice
   `0x0100` (bumped on every interface-layout change so Windows does not reuse a stale cache).
   Configuration: bus powered, remote-wakeup bit set, 250 mA. Keyboard at interface 0 because
   some BIOSes and KVM-aware firmware look only at the first interface. IN endpoints = 4, at the
   conservative limit; the FIFO need (EP0 + four IN endpoints at 16 words each plus an RX FIFO of
   roughly 50 words) is well under 256 words. If enumeration or TinyUSB's FIFO allocation fails
   on the board, the fallback is one HID interface with report IDs 1 keyboard, 2 mouse,
   3 consumer (3 IN endpoints), losing BIOS boot-keyboard support; the builder records it.
2. **Keyboard: 6KRO boot format, no report ID.** 8 bytes (modifiers, reserved, six usages) plus a
   one-byte LED output report taken on EP0 SET_REPORT and ignored. NKRO is rejected: every source
   (BLE boot keyboards in stage 2, protocol `KEY_STATE` with six usages) is 6KRO already, so
   NKRO would add a second report format and lose boot compatibility for nothing. Boot and report
   protocol carry the same bytes, so SET_PROTOCOL needs no state change.
3. **Mouse and consumer reports (interface 1).** Report ID 1, mouse: 5 buttons + 3 pad, X and Y
   signed 16-bit (logical -32767..32767), wheel signed 8-bit, AC Pan signed 8-bit (7 data bytes).
   16-bit axes match `kvm_router_input_t.dx/dy` (int16) so no splitting is needed per report;
   pending motion accumulates in int32 and is emitted clamped. Report ID 2, consumer: one 16-bit
   usage (0 = release). Mouse and consumer share `0x82`; the sender alternates so a busy
   endpoint never loses the consumer release.
4. **VID/PID.** Espressif VID `0x303A` with a PID that is **not** `0x1001`, set by Kconfig
   `KVM_USB_COMPOSITE_PID`. Interim default `0x4009`, the TinyUSB example class-map formula
   `0x4000 | CDC<<0 | HID-count<<2` with one CDC and two HID interfaces (the builder confirms the
   value against esp_tinyusb's default-descriptor source). Espressif's `usb-pids` README says
   standard-class TinyUSB devices do not need a custom PID and that custom PIDs are granted per
   request with no listed test range (fetched from GitHub; no test range could be verified).
   Because the interim PID is shared with hobby devices, discovery never trusts it alone
   (Decision 13). Before any distribution the owner requests a dedicated PID through
   `espressif/usb-pids` (or uses an owner-owned VID); the host keeps an allow-list so both work.
   Strings: manufacturer "DoubTech", product "DoubTech HID", serial number = the same MAC-derived
   string USJ reports today (keeps Windows' COM number stable across replugs; no new exposure).
5. **Windows drivers.** No INF: Windows 10/11 binds `usbccgp` to the composite parent, `hidusb`
   with `kbdhid`/`mouhid`/HID consumer to interfaces 0 and 1, and the inbox `usbser.sys` to the
   CDC-ACM function (02/02, with IAD). The COM port's hardware ID is
   `USB\VID_303A&PID_4009&MI_02`, so the host matches interface 2 (or 3 on macOS).
   Not verified on the owner's PC; checked in the phase C drill.

### Flashing and recovery

6. **Default is Serial/JTAG.** NVS namespace `kvm_usb`, key `mode` (u8; absent or 0 = Serial/JTAG,
   1 = composite) and key `unconf` (u8, unconfirmed composite boots). Never erased. A build with
   `CONFIG_KVM_USB_COMPOSITE_SUPPORT=n` (the phase A/B default) ignores the key completely and
   behaves exactly like today. A rescue build with `CONFIG_KVM_USB_FORCE_SERIAL=y` ignores the key
   and offers "Serial mode" on the touchscreen to clear it.
7. **Boot decision** (pure function `kvm_usb_mode_decide`, first match wins):
   1. composite support not built, or `FORCE_SERIAL` → serial;
   2. RTC one-shot "serial once" flag present (Decision 9) → serial, flag consumed;
   3. BOOT pressed during the escape window → serial for this boot, screen offers "Keep serial";
   4. reset reason abnormal (panic, any watchdog, brownout, CPU lockup) → serial for this boot;
   5. `unconf >= 3` → serial and **persist** `mode = 0`, screen shows "USB HID turned off after 3
      failed starts";
   6. `mode == 1` → composite, `unconf += 1` written before TinyUSB starts;
   7. otherwise serial.
   A composite boot is confirmed (`unconf = 0`) after the device has been mounted continuously
   for 30 s. Consequence the owner can rely on: **unplugging and replugging three times, each
   within 30 s, always returns to serial mode**, with no button and no screen.
8. **Escape window.** Every boot first installs the USJ driver exactly as today, which maps the
   PHY to USJ even after a software reset from composite mode (Constraints). In composite mode
   the firmware then waits `KVM_USB_ESCAPE_WINDOW_MS` (1500 ms) with USJ live, sampling BOOT, and
   only then calls `usb_serial_jtag_driver_uninstall()` and `tinyusb_driver_install()` (whose
   `usb_new_phy` maps the PHY to OTG, `components/usb/usb_phy.c:284`); the screen (when the panel is enabled) shows "Hold BOOT for
   serial mode". The owner plugs in first and then presses BOOT (pressing BOOT before power is
   ROM download mode, which is also safe, Decision 10). The existing "BOOT held at startup does
   not trigger emergency release" rule keeps this hold from releasing the route.
9. **"Serial once" reboot.** A one-shot flag in `RTC_NOINIT_ATTR` memory (magic plus inverted copy,
   so power-on garbage is rejected) survives `esp_restart()` and the USJ chip reset, and is cleared
   when read. It is set by (a) the touchscreen action "Restart in serial mode", (b) a raw magic
   string `"\0KVM-USB-SERIAL-ONCE\n"` written to the CDC port (same matcher style as
   `kvm_transport_diagnostics_requested`, no session needed, parity with today's DTR/RTS reset
   power), before an emergency release and `esp_restart()`. The serial-once boot is today's
   firmware path, so `esptool --before default_reset` then works unchanged, and its final
   `hard_reset` boots back into composite mode. A second magic `"\0KVM-USB-SERIAL-MODE\n"` persists
   `mode = 0` and reboots. Turning composite **on** is touchscreen-only (physical presence).
   `RTC_CNTL_FORCE_DOWNLOAD_BOOT` (direct ROM download) is not used: whether the ROM clears the bit
   after use could not be verified, and the serial-once path needs nothing new from the ROM.
10. **ROM download is the floor, proven before any TinyUSB image.** Holding BOOT while plugging in
    enters ROM download mode over USJ regardless of the app. Phase B ends with an owner drill that
    proves it on this board: BOOT+plug shows `303A:1001`; `esptool --before no_reset --after
    no_reset chip_id` reports the chip stays in the bootloader; an app-only write works from there;
    an ordinary replug returns to the app; `espefuse.py summary` (read-only) shows USB download is
    not disabled. No composite image is flashed until this drill passes and is logged in
    `docs/hardware/board-bringup.md`.
11. **Staged rollout.** Phase B images contain the mode store, decision, escape window, one-shot
    flag, magic strings and touchscreen screen, with composite support still off: the owner can
    drill every serial-side path with zero risk. Phase C turns composite support on; its first
    image boots serial (NVS key absent) and is flashed by the existing method; the owner enables
    composite from the touchscreen and drills every revert path before daily use. Until phase E
    is installed, the owner returns to serial mode for daily use, because MSI 0.1.1 cannot find
    a composite board.
12. **Flash workflow in composite mode.** `tools/firmware.ps1 flash` and `ports` also look for the
    composite board (VID 303A, allow-listed PID, `MI_02`). If only the composite board is present
    it checks the port is free (app quit), writes the serial-once magic, waits up to 15 s for
    `303A:1001`, then runs `idf.py app-flash` exactly as today. The update runner does the same
    before its existing esptool sequence. The esp32-kvm-firmware skill and README document it.

### Input flow and arbitration

13. **Host discovery.** `PortIdentity` gains `interface: Option<u8>` (serialport feature
    `usbportinfo-interface`). `candidate_ports` accepts `303A:1001` (any interface) and
    `303A:<allow-listed composite PID>` with interface 2 or 3; the handshake (CAPS board id
    `esp32-kvm-s3`, minor range, SESSION_OPEN ACK) remains the real identity check, so a foreign
    TinyUSB device with the same PID is probed and rejected. New `usb_mode(&PortIdentity)` tells
    callers which flash handoff to use. Legacy boards keep working unchanged.
14. **Hub target 0 is the PC.** In composite mode the input hub gets a PC output (a
    `kvm_router_output_t` whose `send` updates the HID sink, `ready` = mounted and not suspended).
    `armed_slot == 0` already means "nothing armed", so the PC arm is a separate `pc_armed` flag.
    The PC uses the same held/want/dirty model as guests: a not-ready PC keeps the hub dirty and
    the wanted state is resent on resume; an unmounted PC (bus reset, re-enumeration, unplug) is
    "known disconnected" and its held state is abandoned without emitting, because the host
    dropped it. With composite support off, or in serial mode, target 0 stays the stage-2 stub and
    every stage-2 test passes unchanged.
15. **Arbitration (stage-2 rule kept, one addition).**
    - Router armed (the app drives a guest): device input is dropped and the hub yields: all-up
      keyboard, zero buttons and consumer to the PC, then `pc_armed = false`. The PC therefore
      never gets the device's HID while the app routes to a guest, and the app's low-level hooks
      never see board input to forward (no loop).
    - Router not armed (no app, or app in Local): device input goes to the hub target; the app
      is not involved. PC capture is not needed for device-owned input.
    - New: a chord typed while the router is armed is recognized (today it is dropped before
      chord detection) and sent to the app as the existing `DEVICE_SELECT_REQUEST` (slot 0 to 3),
      so the owner can always leave an app-armed guest from the device keyboard. The app still
      arbitrates.
    - Chord to a guest: all-up/zero to the PC first, then arm the guest; the PC sees every key
      released. Chord to This PC: all-up on the old guest, then PC.
    - Default target in composite mode: This PC, as an explicit choice (no stage-2 auto-target,
      which existed only because the PC was a stub). Serial mode keeps the stage-2 auto-target.
16. **Ownership and threading.** Only the USB worker calls the hub, the router and the HID sink.
    TinyUSB runs in esp_tinyusb's task (default priority and a 4 KB stack to start; the builder
    measures the high-water mark into the Health log line). Its event callback only stores
    atomics (attached, mounted, suspended, remote-wakeup allowed, reset count) and gives the CDC
    RX semaphore. HID reports are state-based: the worker calls `tud_hid_n_ready` then
    `tud_hid_n_report` (non-blocking); a busy endpoint leaves the sink dirty and the next worker
    iteration resends. No HID queue, never a blocking call. esp_tinyusb is added as a registry
    dependency of the new component; the resulting `firmware/dependencies.lock` change is
    committed only in its own commit with the owner's explicit approval.
17. **CDC transport.** The worker reads through a small byte-link interface
    (`connected`, `read(wait)`, `write(wait)`), implemented by USJ (today's calls) and by CDC.
    CDC `read` waits on a binary semaphore given by `callback_rx` for the same `wait` (20 ms, or one
    tick with device input queued), then calls `tinyusb_cdcacm_read`. `write` queues with
    `tinyusb_cdcacm_write_queue`, flushes with timeout 0, and retries for at most 50 ms before the
    core is reset, matching `send_binary`. `connected` = mounted and not suspended. The
    diagnostics dump and both magic strings go through the same link. `transport_core.c` is not
    changed for framing.
18. **Suspend, resume, reset, unplug.** Suspend: PC not ready, CDC disconnected, core reset (the
    app's session ends as on PC sleep today; the 500 ms lease already fails local). Resume: the
    hub flushes the wanted state (all-up when nothing is held), the app reopens its session.
    Bus reset or re-enumeration: PC known disconnected, held state abandoned. Unplug: the board
    is bus-powered and reboots; a composite boot within 30 s does not confirm (Decision 7).
    No VBUS sensing (`phy.self_powered = false`, no `vbus_monitor_io`). The board draws more than
    the 2.5 mA suspend budget; PCs do not enforce it, recorded as a known deviation.
19. **Remote wakeup: yes, key press only.** The configuration sets the remote-wakeup bit
    (`CONFIG_KVM_USB_REMOTE_WAKEUP`, default y). While suspended, if the host enabled remote
    wakeup (`TINYUSB_EVENT_SUSPENDED.remote_wakeup`), a newly pressed key or mouse button on the
    device, with hub target This PC, calls `tinyusb_remote_wakeup()` at most once per 2 s; the
    waking report itself is not delivered, held state follows on resume. Motion never wakes.
    This also covers Windows selective suspend of the HID functions.

### Protocol, host app and display

20. **Protocol minor 5, wire-compatible.** `MINOR_MAX` 4 to 5. STATUS gains optional extension key
    `128`, emitted only at negotiated minor >= 5: a map `{1: usb_mode 0 serial / 1 composite,
    2: pc_hid 0 off / 1 suspended / 2 ready, 3: device_target 0..3, 4: device_inputs bitset
    (1 keyboard, 2 mouse)}`. No new commands; mode changes are touchscreen-only and bootloader
    handoff uses the raw magic. New vectors `status-m5.cobs` / `.hex`; old vectors unchanged.
    Hosts at minor <= 4 never see key 128.
21. **Host app.** Discovery as Decision 13 in `usb-transport`; `host-actor`, desktop
    `actor_backend.rs`, `update-runner` (`flasher.rs` handoff, `reconnect.rs` accepts either
    identity after flashing) follow. The desktop app shows the USB mode and PC HID state from
    STATUS key 128; no mode control.
22. **Display.** `kvm_display_status_t` gains `usb_mode`, `pc_hid`, `usb_reason`. Serial mode:
    unchanged ("USB on/off" = host session, stage 2 dim rule). Composite mode: "USB off" now means
    **not enumerated**; badge `PC on` / `PC asleep` / `USB off`, followed by ` | App` when a host
    session is open and the existing ` | KB MS`; the view is dimmed only when the PC is not
    mounted; the "This PC: use Ctrl+Alt+1-3" hint is shown only when the PC is unavailable. A
    "USB mode" screen (from guest settings) shows the current mode and boot reason with actions
    "Use USB HID mode" (persist 1 and reboot), "Use serial mode" (persist 0 and reboot) and
    "Restart in serial mode" (one-shot).

### Testing

23. **Host-side C (MSVC, `run-msvc.ps1` pattern):** boot decision table; descriptor parser checks
    (lengths, IAD, interface numbers, endpoint addresses, IN count <= 4, boot keyboard without
    report ID); report builders (keyboard bytes, int16 clamping and int32 accumulation, consumer,
    release-all, pump alternation and busy endpoints); hub PC target (stage-2 suites unchanged,
    plus PC arm/yield/dirty/known-disconnected, chord while host armed); hub worker integration
    with a fake PC port (suspend/resume flush, remote-wakeup trigger and rate limit, router arm
    yields the PC); byte-link worker integration with a fake link; magic matchers; transport core
    STATUS key 128 at minor 5 only; display model badges.
24. **Rust:** discovery (`303A:1001` any interface; composite PID only with interface 2/3;
    foreign PIDs rejected; handshake still required), protocol STATUS key 128 decode and minor 5
    negotiation, update-runner handoff with a fake runner and fake ports (magic written, waits for
    `303A:1001`, times out cleanly, never flashes the composite port).
25. **Owner-run hardware checklists** at the end of phases B, C and E (plan tasks 9, 11, 16).

## Risks (recovery first)

- **R1 Recovery.** A composite image can make USJ, COM4 and esptool auto-reset unavailable, and
  BOOT ROM download, the last resort, has never been proven on this board. Mitigations: default
  serial; composite only after an owner action; ROM drill before the first composite image;
  escape window; replug three times; crash fallback; serial-once magic and touchscreen; rescue
  build; app-only flashes never touch bootloader, partition table or NVS. Residual: a build that
  panics before the decision code, which still falls to ROM download.
- **R2 Endpoint and FIFO budget.** If `ep_in_count = 5` turns out to exclude EP0 there is one spare;
  if the hardware allows fewer than four IN endpoints beside EP0, the merged-HID fallback
  (Decision 1) loses BIOS boot keyboard.
- **R3 Windows binding and discovery.** Composite with IAD on usbser, stale descriptor cache, the
  interim shared PID, COM number changes, and the installed app not seeing composite boards.
  Mitigations: bcdDevice bump, interface match plus handshake, serial for daily use until phase E.
- **R4 Suspend and power.** Stuck keys across suspend, lost first keystroke, remote wakeup not
  honoured, selective suspend, a PC that cuts VBUS on sleep (board reboots and may not confirm).
  Mitigations: dirty/flush model, all-up on resume, remote wakeup on press, owner checks.
- **R5 Arbitration with the app.** The app's edge switching can be triggered by the device mouse
  moving the PC cursor, after which device input is dropped until the owner chords (now possible
  while host-armed) or the app returns to Local. Watched in the phase E checklist.
- R6 esp_tinyusb 3.x on IDF 5.5.1 is not verified locally; it may define TinyUSB callbacks
  (`tud_mount_cb` and similar) itself, which constrains our glue; the lock-file change needs
  owner approval.
- R7 CPU and heap: TinyUSB task beside NimBLE, the display and the worker; internal RAM headroom
  after stage 2 is unknown. Measured from the Health line.
- R8 `RTC_NOINIT_ATTR` survival across the USJ chip reset used by esptool is assumed, not measured;
  if it fails, only the post-flash return to composite is affected (the board stays serial).

## Left to the builder

Exact esp_tinyusb version pin and Kconfig names (`CONFIG_TINYUSB_*` buffer sizes, at least 1024 B
CDC RX/TX), whether esp_tinyusb provides `tud_*_cb` symbols, task priority and core, report
descriptor byte layout within Decision 2 and 3, LVGL layout of the USB mode screen, order of
display start versus the escape window in `app_main` (the window must work with the panel off),
and the measurements (heap, stacks, HID latency). What could not be verified here: anything in
esp_tinyusb or TinyUSB (no local copy), ROM behaviour after `esp_restart()` with the PHY on OTG,
Windows driver binding, and every hardware behaviour; each has a drill or checklist step.
