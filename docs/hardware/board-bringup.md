# Board bring-up evidence (2026-10-02)

This record separates observations of the attached device from Waveshare's reference design. The USB interface and ESP32-S3 silicon have been identified. The physical board SKU, PCB revision, and presence of a touch panel have **not** been verified. Do not select a firmware image or enable peripheral outputs from this table until the board marking and assembly are checked against the schematic.

## Attached device: read-only observations

| Check | Result | What it establishes |
| --- | --- | --- |
| PlatformIO `device list --json-output` | `COM7`, `USB VID:PID=303A:1001`, serial `28:84:85:8D:30:FC` | An Espressif USB serial/JTAG interface is attached; this does not identify a Waveshare SKU. |
| `esptool.py v4.11.0 --chip esp32s3 --port COM7 flash_id` | Succeeded twice; ESP32-S3 QFN56 silicon revision `v0.2`, 40 MHz crystal, 8 MB embedded PSRAM, USB-Serial/JTAG mode; flash manufacturer `0x20`, device `0x4018`, detected 16 MB, quad, 3.3 V | ROM/tool access and flash capacity are repeatable. `v0.2` is **chip** revision, not PCB revision. No flash contents were read, erased, or written. |

`flash_id` uses a RAM stub and ends with an RTS hard reset. Both executions returned exit code 0. The identifier in the tool output is a device MAC; keep it out of user-facing diagnostic exports unless needed for support.

## Vendor reference, conditional on board match

Waveshare lists non-touch SKUs 33866/33867 and touch SKUs 33868/33869. The vendor describes the touch version as adding a CST816 touch controller to the shared ESP32-S3R8, 16 MB flash, 8 MB PSRAM, 240×240 ST7789 design. The [official schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.54/ESP32-S3-LCD-1.54-Schematic.pdf) was downloaded and inspected; its SHA-256 on this date is `3F551B71E2E80EAA4A766F3AA6CBA623332554068524E42DF629A51835C87102`. The PDF remains at the vendor URL because its separate redistribution terms were not established. The [Waveshare examples repository](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54) states Apache-2.0 for its code; bundled third-party components require their own license review before copying.

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

Automatic ROM access via esptool succeeded twice on COM7. Manual BOOT-at-power-on recovery and return to the factory application still need a person to operate the button while observing USB enumeration. Do not claim repeatable manual recovery until both entry and normal reboot are logged. No firmware was flashed during this check.

## Next hardware checks

1. Photograph front and back so the printed model, touch flex/panel, and PCB revision are legible; compare with the exact vendor schematic and SKU listing.
2. With the factory image intact, observe display output and touch response. Record whether CST816 is present, its I²C address, screen offset/orientation, and backlight behavior with a non-destructive probe or vendor demo.
3. Perform and log two manual BOOT-entry cycles, then ordinary boot, including COM port/VID:PID changes and the exact button sequence. Confirm PLUS and PWR behavior separately without repurposing PWR.
4. Only after identity and pins match, use these values in a board configuration and run the CDC, BLE, and display coexistence test.

## Commands and source files

```text
C:/Users/yolan/.platformio/penv/Scripts/platformio.exe device list --json-output
C:/Users/yolan/.platformio/penv/Scripts/python.exe C:/Users/yolan/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32s3 --port COM7 flash_id
```

Reference files: `bsp_display.h`, `bsp_display.c`, `bsp_touch.h`, `bsp_touch.c`, `bsp_i2c.h`, and the vendor `02_button_example.ino` under the touch example tree. See the [vendor resources page](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.54/Resources-And-Documents) for current originals.
