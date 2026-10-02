/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Drives the Waveshare 240-by-240 ST7789 through ESP-IDF LCD and LVGL, using
 * small DMA draw buffers and a low-rate status update. A separate GPIO sampler
 * emits BOOT emergency events; PLUS is disabled until its physical pin is proven. */
#include "display.h"
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LCD_WIDTH 240
#define LCD_HEIGHT 240
#define LCD_DRAW_LINES 20
#define BOOT_GPIO GPIO_NUM_0
#define PLUS_GPIO GPIO_NUM_NC

static kvm_display_event_fn event_callback;
static kvm_display_status_t latest_status;
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static bool started;
static bool panel_started;
static lv_obj_t *target_label;
static lv_obj_t *connection_label;

static void button_worker(void *context)
{
    (void)context;
    kvm_display_buttons_t buttons;
    uint64_t time_ms = (uint64_t)esp_timer_get_time() / 1000;
    kvm_display_buttons_init(&buttons, false, gpio_get_level(BOOT_GPIO) == 0, time_ms);
    for (;;) {
        time_ms = (uint64_t)esp_timer_get_time() / 1000;
        bool plus_down = PLUS_GPIO != GPIO_NUM_NC && gpio_get_level(PLUS_GPIO) == 0;
        bool boot_down = gpio_get_level(BOOT_GPIO) == 0;
        kvm_display_event_t event = kvm_display_buttons_sample(&buttons, plus_down, boot_down, time_ms);
        if (event != KVM_DISPLAY_NO_EVENT && event_callback) event_callback(event);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static esp_err_t init_panel(void)
{
    spi_bus_config_t bus = {
        .sclk_io_num = GPIO_NUM_38, .mosi_io_num = GPIO_NUM_39,
        .miso_io_num = GPIO_NUM_NC, .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_WIDTH * LCD_DRAW_LINES * 2 + 8,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = GPIO_NUM_21, .dc_gpio_num = GPIO_NUM_45,
        .spi_mode = 3, .pclk_hz = 40000000, .trans_queue_depth = 10,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io);
    if (err != ESP_OK) return err;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_40,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_handle_t panel = NULL;
    err = esp_lcd_new_panel_st7789(io, &panel_cfg, &panel);
    if (err != ESP_OK) return err;
    if ((err = esp_lcd_panel_reset(panel)) != ESP_OK ||
        (err = esp_lcd_panel_init(panel)) != ESP_OK ||
        (err = esp_lcd_panel_invert_color(panel, true)) != ESP_OK ||
        (err = esp_lcd_panel_disp_on_off(panel, true)) != ESP_OK) return err;
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    if ((err = lvgl_port_init(&lvgl_cfg)) != ESP_OK) return err;
    lvgl_port_display_cfg_t display_cfg = {
        .io_handle = io, .panel_handle = panel,
        .buffer_size = LCD_WIDTH * LCD_DRAW_LINES,
        .double_buffer = true, .hres = LCD_WIDTH, .vres = LCD_HEIGHT,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = { .buff_dma = true, .swap_bytes = true },
    };
    lv_display_t *display = lvgl_port_add_disp(&display_cfg);
    if (!display) return ESP_ERR_NO_MEM;
    if (!lvgl_port_lock(100)) return ESP_ERR_TIMEOUT;
    lv_obj_t *screen = lv_display_get_screen_active(display);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), 0);
    target_label = lv_label_create(screen);
    lv_obj_set_style_text_color(target_label, lv_color_hex(0xffffff), 0);
    lv_label_set_text(target_label, "HOST");
    lv_obj_align(target_label, LV_ALIGN_CENTER, 0, -18);
    connection_label = lv_label_create(screen);
    lv_obj_set_style_text_color(connection_label, lv_color_hex(0xb0bec5), 0);
    lv_label_set_text(connection_label, "USB offline | BLE idle");
    lv_obj_align(connection_label, LV_ALIGN_CENTER, 0, 20);
    lvgl_port_unlock();
    gpio_config_t backlight_cfg = {
        .pin_bit_mask = 1ULL << GPIO_NUM_46, .mode = GPIO_MODE_OUTPUT,
    };
    if ((err = gpio_config(&backlight_cfg)) != ESP_OK) return err;
    if ((err = gpio_set_level(GPIO_NUM_46, 1)) != ESP_OK) return err;
    panel_started = true;
    return ESP_OK;
}

static void display_worker(void *context)
{
    (void)context;
    kvm_display_status_t shown = {0};
    for (;;) {
        kvm_display_status_t next;
        portENTER_CRITICAL(&status_lock);
        next = latest_status;
        portEXIT_CRITICAL(&status_lock);
        if (panel_started && memcmp(&shown, &next, sizeof(next)) != 0 && lvgl_port_lock(20)) {
            char line[48];
            lv_label_set_text(target_label, kvm_display_target_label(&next));
            snprintf(line, sizeof(line), "USB %s | BLE %s", next.usb_connected ? "online" : "offline",
                     next.guest_ready ? "ready" : "idle");
            lv_label_set_text(connection_label, line);
            shown = next;
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t kvm_display_start(bool enable_panel, kvm_display_event_fn event_fn)
{
    if (started) return ESP_ERR_INVALID_STATE;
    event_callback = event_fn;
    gpio_config_t boot_cfg = {
        .pin_bit_mask = 1ULL << BOOT_GPIO, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    esp_err_t err = gpio_config(&boot_cfg);
    if (err != ESP_OK) return err;
    if (enable_panel && (err = init_panel()) != ESP_OK) return err;
    if (xTaskCreate(button_worker, "kvm_buttons", 3072, NULL, 8, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    if (enable_panel && xTaskCreate(display_worker, "kvm_display", 4096, NULL, 4, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    started = true;
    return ESP_OK;
}

void kvm_display_post_status(const kvm_display_status_t *status)
{
    if (!status) return;
    portENTER_CRITICAL(&status_lock);
    latest_status = *status;
    portEXIT_CRITICAL(&status_lock);
}
