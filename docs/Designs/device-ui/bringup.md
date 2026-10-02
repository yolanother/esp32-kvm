# Device status UI and physical controls

The target is the Waveshare ESP32-S3-Touch-LCD-1.54. Its reference design has a 240×240 ST7789 display on SPI2 and a CST816 touch controller. The attached enclosure identifies that model, but the PCB revision, actual panel orientation, touch alignment, PLUS pin, and backlight polarity have not been measured. Firmware therefore calls `kvm_display_start(false, ...)`: BOOT is sampled as an input, while the LCD, touch bus, and backlight remain off. PWR is never repurposed.

The opt-in LCD path uses the factory BSP reference pins: MOSI 39, clock 38, CS 21, DC 45, reset 40, and backlight 46. It follows the factory BSP's SPI mode 3, 40 MHz, RGB565, inversion setting, and begins at unrotated 240×240. The vendor overview mentions mode 0, so a physical pixel grid test must resolve that discrepancy before enabling the panel. LVGL uses two 240×20 DMA buffers (19,200 bytes total) and changes labels only when status changes. The status copy is lock bounded and the UI task never runs in the USB router or NimBLE callback.

The confirmed 16 MB flash size is set in `sdkconfig.defaults`. The custom partition CSV preserves offsets observed in the read-only factory backup (NVS, OTA data, PHY, factory app, OTA app, assets) so a future image does not accidentally adopt IDF's 1 MB default app layout. This does not authorize flashing or using pre-existing NVS, OTA data, or assets; their application format is unknown. PSRAM configuration is deferred until its electrical mode is checked against the board factory settings.

BOOT GPIO0 is a recovery strap. A press held during startup is ignored until release; a new runtime hold of at least one second requests emergency release. The USB worker executes that request even when the host is absent, releases/disarms the router, closes its session, and marks local fault. The next normal host command needs a fresh session handshake. PLUS stays unassigned (`GPIO_NUM_NC`) because vendor examples do not establish whether GPIO5 or GPIO4 is the physical PLUS contact. Once mapped, a short PLUS press emits `DEVICE_SELECT_REQUEST` and the host arbitrates; the board never arms directly from the button.

## Verification

- `firmware/tests/display/run-msvc.ps1`: status labels, startup BOOT exclusion, PLUS debounce, and one-shot BOOT emergency.
- `firmware/tests/router/run-msvc.ps1`: emergency fail-local release and stale-session rejection.
- `firmware/tests/integration/run-msvc.ps1`: binary select request and existing USB/router fixture behavior.
- ESP-IDF 5.5.1 `idf.py -C firmware build`: passed after regenerating local `sdkconfig` from defaults. The image is `0xe26e0` bytes; the smallest app partition is 4 MB, with 78% free. The first parallel build hit an Xtensa GCC internal compiler segmentation fault in ESP-IDF's unused RGB LCD source; a retry compiled that object and the complete build passed. No flash was performed.
- Physical acceptance: inspect PCB revision; test LCD pixels and backlight polarity at opt-in startup; confirm SPI mode/offset/rotation; probe CST816 address, touch axes, and interrupt; map PLUS without touching PWR; measure redraw time and free DMA memory while BLE input flows; repeat BOOT recovery and normal screen boot. Flash only after board recovery and identity gates are cleared by the coordinator.

Reference: [Waveshare board resources](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.54/Resources-And-Documents), [Waveshare factory BSP](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54/tree/main/examples/ESP32-S3-Touch-LCD-1.54-demo/ESP-IDF-5.5.1/01_factory/components/esp_bsp), and [Espressif LVGL port](https://github.com/espressif/esp-bsp/tree/master/components/esp_lvgl_port).
