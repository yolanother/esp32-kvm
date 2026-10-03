# Board bring-up evidence (2026-10-02)

This record separates observations of the attached device from Waveshare's reference design. The USB interface and ESP32-S3 silicon have been identified, and a user photo confirms the touch-model enclosure label. The PCB revision and working touch panel have **not** been verified. Do not enable peripheral outputs from this table until the board assembly is checked against the schematic.

## Attached device: read-only observations

| Check | Result | What it establishes |
| --- | --- | --- |
| PlatformIO `device list --json-output` | `COM7`, `USB VID:PID=303A:1001`, serial `28:84:85:8D:30:FC` | An Espressif USB serial/JTAG interface is attached; this does not identify a Waveshare SKU. |
| `esptool.py v4.11.0 --chip esp32s3 --port COM7 flash_id` | Succeeded twice; ESP32-S3 QFN56 silicon revision `v0.2`, 40 MHz crystal, 8 MB embedded PSRAM, USB-Serial/JTAG mode; flash manufacturer `0x20`, device `0x4018`, detected 16 MB, quad, 3.3 V | ROM/tool access and flash capacity are repeatable. `v0.2` is **chip** revision, not PCB revision. |
| `esptool.py v4.11.0 --chip esp32s3 --port COM7 get_security_info` | Succeeded; security flags `0x00000000`, Secure Boot disabled, Flash Encryption disabled, SPI boot crypt count `0x0` | Software-initiated ROM connection is available with the current eFuse state. This does not prove physical BOOT button recovery, and future eFuse programming can change these properties. |
| User-supplied enclosure label photo | Reads `ESP32-S3-Touch-LCD-1.54`, Waveshare, `Touch CST816`, `Display ST7789`, `8MB PSRAM`, `16MB FLASH`, and `USB-C`; button labels include `+/KEY`, `PWR`, and `BOOT/-` | Confirms the sold touch-model enclosure and matches the probed memory capacities. The PCB revision and actual touch response are not visible or measured. The photo is retained outside the repository. |
| `esptool.py v4.12.0 read_flash 0x0 0x1000000` | Completed on COM7 in download mode; local ignored file `.tools/board-factory-backup.bin` is exactly 16,777,216 bytes, SHA-256 `7AAC67A6CB06ECAB5AC4B4D4AD9209ABE772E5E946C6D2951B8848E90DAFC300` | Preserves a byte-for-byte recovery source before any write. The backup may contain device configuration; keep it local and do not upload or commit it. COM7 re-enumerated after esptool's RTS reset. |

The backed-up factory partition table has NVS at `0x9000` (16 KiB), OTA data at `0xd000` (8 KiB), PHY data at `0xf000` (4 KiB), factory app at `0x20000` (6464 KiB), OTA app at `0x670000` (4 MiB), and assets at `0xa70000` (4416 KiB). The project partition CSV mirrors these offsets. This does not establish compatibility with factory NVS or assets.

`flash_id` uses a RAM stub and ends with an RTS hard reset. Both executions returned exit code 0. The identifier in the tool output is a device MAC; keep it out of user-facing diagnostic exports unless needed for support.

## Vendor reference, conditional on board match

Waveshare lists non-touch SKUs 33866/33867 and touch SKUs 33868/33869. The vendor describes the touch version as adding a CST816 touch controller to the shared ESP32-S3R8, 16 MB flash, 8 MB PSRAM, 240×240 ST7789 design. The [official schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.54/ESP32-S3-LCD-1.54-Schematic.pdf) was downloaded and inspected; its SHA-256 on this date is `3F551B71E2E80EAA4A766F3AA6CBA623332554068524E42DF629A51835C87102`. The PDF remains at the vendor URL because its separate redistribution terms were not established. The [Waveshare examples repository](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54) states Apache-2.0 for its code; bundled third-party components require their own license review before copying.

The supplied [purchase listing](https://www.amazon.com/dp/B0GV472MH4) is ASIN `B0GV472MH4`. An attempted direct fetch returned a generic Amazon page, so the selected order variation could not be verified there. The user photo reads `ESP32-S3-Touch-LCD-1.54`, which resolves the enclosure model but does not establish the PCB revision or demonstrate touch response.

The following GPIOs come from the vendor's [ESP-IDF 5.5.1 factory BSP](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54/tree/main/examples/ESP32-S3-Touch-LCD-1.54-demo/ESP-IDF-5.5.1/01_factory/components/esp_bsp). They are **reference mappings**, not proof that the attached assembly has these connections.

| Function | Reference GPIO | Source / caution |
| --- | --- | --- |
| LCD SPI MOSI, clock | 39, 38 | `bsp_display.h`; SPI2, no MISO. |
| LCD chip select, data/command, reset, backlight | 21, 45, 40, 46 | `bsp_display.h`; GPIO46 has ESP32-S3 boot/drive constraints, so use the BSP's verified backlight configuration only after board match. |
| Touch I²C SDA, SCL | 42, 41 | `bsp_i2c.h`; only relevant to the touch assembly. |
| Touch reset, interrupt | 47, 48 | `bsp_touch.h`; `bsp_touch.c` uses the CST816S driver address constant, but the address value was not independently checked here. |
| BOOT | 0 | Vendor button example uses GPIO0; it is also the download strap. Preserve its power-on function. |
| Other buttons | 5, 4 in vendor button example | The example labels three inputs only as buttons 1–3. Mapping GPIO5/4 to the physical PLUS/PWR controls has not been established. PWR is part of the power circuit in the schematic; do not treat it as a free application button. |
| USB D−, D+ | ESP32-S3 GPIO19, GPIO20 | Schematic shows native USB nets routed through series resistors and protection to Type-C; the attached interface reports USB-Serial/JTAG mode. No external USB-UART bridge is indicated in that path. |

The factory BSP creates an ST7789 panel with 16-bit RGB pixels and display inversion. Its SPI IO configuration specifies mode 3 while the vendor overview describes mode 0; use a display bring-up test to resolve the operating setting. Panel offset/orientation and touch transforms have not been measured on this device. The touch BSP uses orientation-dependent swap/mirror settings rather than a single fixed orientation.

## Recovery status

The [Waveshare user guide](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.54/Instructions-For-Use) says to hold BOOT while connecting USB, then release BOOT to enter download mode when the port is not recognized; power cycle after programming. It also suggests holding BOOT and power cycling if a flashing tool waits for synchronization. This is the documented manual recovery procedure, **not a completed test on this board**.

Automatic ROM access via esptool succeeded on three read-only queries (two `flash_id`, one `get_security_info`) and a full read-only backup on COM7. The user reported performing the BOOT-held reconnect sequence and returning the board to BOOT mode; COM7 was readable afterward. The responses did not explicitly confirm COM enumeration on *both* manual cycles or that the usual factory screen returned after ordinary boot. Do not claim repeatable manual recovery until both entry and normal reboot are logged.

The first project firmware image was flashed with ESP-IDF 5.5.1 at 460800 baud. Esptool reported verified hashes for the bootloader, partition table, OTA data, and factory app, then requested a hard reset. A live host HELLO on COM7 timed out with no bytes returned. A subsequent `esptool --before no_reset --after no_reset chip_id` succeeded and explicitly reported that it remained in the bootloader, establishing that the board was still in ROM download mode at the time of the failed handshake. The display is intentionally disabled in this first firmware.

After the user unplugged USB and reconnected without BOOT, COM7 returned with a blank display. The initial host HELLO still timed out, and esptool's hard reset and ROM `run` left the port silent or stalled. After a second ordinary power cycle, a no-reset ROM command received no serial response, while the app returned a framed CAPS response. Host `discover_system` then completed HELLO/CAPS/SESSION_OPEN three successive times: firmware `0.1.0-m1`, protocol minor 1, one advertised live guest slot, eight bond slots. A native `HostActor::connect_system` check polled STATUS for two seconds and reported `Local`, one slot, pairing closed, and no fault without arming capture. The first timeout's exact cause is not established; boot transition timing or the intervening ROM commands may have contributed. This proves USB protocol discovery and disarmed STATUS on the current firmware, not BLE routing, pairing, or touch.

## Next hardware checks

1. Inspect the PCB itself for its revision and touch assembly; the supplied enclosure photo confirms the printed touch model but does not expose the PCB.
2. With the factory image intact, observe display output and touch response. Record whether CST816 is present, its I²C address, screen offset/orientation, and backlight behavior with a non-destructive probe or vendor demo.
3. Finish logging the two reported manual BOOT-entry cycles and a repeatable ordinary BOOT-released boot; COM7 and the app handshake have now succeeded after the second normal power cycle, while the first normal cycle was inconclusive. Confirm PLUS and PWR behavior separately without repurposing PWR.
4. Only after identity and pins match, use these values in a board configuration and run the CDC, BLE, and display coexistence test.

## Commands and source files

```text
C:/Users/yolan/.platformio/penv/Scripts/platformio.exe device list --json-output
C:/Users/yolan/.platformio/penv/Scripts/python.exe C:/Users/yolan/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32s3 --port COM7 flash_id
C:/Users/yolan/.platformio/penv/Scripts/python.exe C:/Users/yolan/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32s3 --port COM7 get_security_info
```

Reference files: `bsp_display.h`, `bsp_display.c`, `bsp_touch.h`, `bsp_touch.c`, `bsp_i2c.h`, and the vendor `02_button_example.ino` under the touch example tree. See the [vendor resources page](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.54/Resources-And-Documents) for current originals.
