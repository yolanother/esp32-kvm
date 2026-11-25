/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Drives the Waveshare 240-by-240 ST7789 through ESP-IDF LCD and LVGL, using
 * small DMA draw buffers and a low-rate derived screen update. The panel stays
 * opt-in behind a validated board profile. A separate GPIO sampler emits BOOT
 * emergency events even if panel startup fails. Validated CST816 touches send
 * exact pairing requests through the transport worker; no touch routes HID. */
#include "display.h"
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LCD_WIDTH 240
#define LCD_HEIGHT 240
#define LCD_DRAW_LINES 20
#define BOOT_GPIO GPIO_NUM_0
#define PLUS_GPIO GPIO_NUM_NC
#define BACKLIGHT_GPIO GPIO_NUM_46

static kvm_display_event_fn event_callback;
static kvm_display_pair_fn pair_callback;
static kvm_display_target_fn target_callback;
static kvm_display_input_fn input_callback;
static kvm_display_status_t latest_status;
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static bool started;
static bool panel_started;
static bool backlight_started;
static bool touch_started;
static esp_lcd_touch_handle_t touch_handle;
static TaskHandle_t display_task;
static lv_obj_t *title_label;
static lv_obj_t *primary_label;
static lv_obj_t *detail_label;
static lv_obj_t *footer_label;
static lv_obj_t *row_labels[3];
static lv_obj_t *target_cards[4];
static lv_obj_t *target_icons[4];
static lv_obj_t *target_names[4];
static lv_obj_t *left_action;
static lv_obj_t *right_action;
static lv_obj_t *guest_settings_action;
/* Input-device pairing: the entry beside the gear, the close control and the
   device rows (geometry from display_model.h, the touch zones' source). */
static lv_obj_t *input_entry_action;
static lv_obj_t *input_cancel_action;
static lv_obj_t *input_rows[4];
static lv_obj_t *input_row_labels[4];

static void touch_interrupt(esp_lcd_touch_handle_t touch)
{
    (void)touch;
    if (!display_task) return;
    BaseType_t wake = pdFALSE;
    vTaskNotifyGiveFromISR(display_task, &wake);
    if (wake) portYIELD_FROM_ISR();
}

static bool init_touch(void)
{
#if CONFIG_KVM_DISPLAY_CST816_TOUCH_VERIFIED
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0, .sda_io_num = GPIO_NUM_42, .scl_io_num = GPIO_NUM_41,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) return false;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    esp_lcd_panel_io_handle_t io;
    if (esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io) != ESP_OK) return false;
    esp_lcd_touch_config_t cfg = {
        .x_max = LCD_WIDTH, .y_max = LCD_HEIGHT,
        .rst_gpio_num = GPIO_NUM_47, .int_gpio_num = GPIO_NUM_48,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
        .interrupt_callback = touch_interrupt,
    };
    if (esp_lcd_touch_new_i2c_cst816s(io, &cfg, &touch_handle) != ESP_OK)
        return false;
    return true;
#else
    return false;
#endif
}

static lv_obj_t *make_action(lv_obj_t *screen, int16_t x, int16_t width)
{
    lv_obj_t *box = lv_obj_create(screen);
    lv_obj_set_size(box, width, 46);
    lv_obj_align(box, LV_ALIGN_TOP_LEFT, x, 160);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x193c48), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x75e2c3), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    return box;
}

static void set_action(lv_obj_t *box, const char *label)
{
    lv_obj_clean(box);
    if (!label) {
        lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *text = lv_label_create(box);
    lv_label_set_text(text, label);
    lv_obj_set_style_text_color(text, lv_color_hex(0xeef5f7), 0);
    lv_obj_center(text);
}

static lv_obj_t *make_label(lv_obj_t *screen, int16_t y, uint32_t color)
{
    lv_obj_t *label = lv_label_create(screen);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_width(label, LCD_WIDTH - 24);
    lv_obj_set_height(label, 28);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

static void make_target_card(lv_obj_t *screen, unsigned index)
{
    static const int16_t x[4] = {12, 124, 12, 124};
    static const int16_t y[4] = {42, 42, 120, 120};
    target_cards[index] = lv_obj_create(screen);
    lv_obj_set_size(target_cards[index], 104, 70);
    lv_obj_align(target_cards[index], LV_ALIGN_TOP_LEFT, x[index], y[index]);
    lv_obj_set_style_bg_color(target_cards[index], lv_color_hex(0x15242e), 0);
    lv_obj_set_style_border_color(target_cards[index], lv_color_hex(0x405560), 0);
    lv_obj_set_style_border_width(target_cards[index], 1, 0);
    lv_obj_set_style_radius(target_cards[index], 9, 0);
    lv_obj_set_style_pad_all(target_cards[index], 7, 0);
    target_icons[index] = lv_label_create(target_cards[index]);
    char icon[3] = {'G', (char)('0' + index), '\0'};
    if (index == 0) { icon[0] = 'P'; icon[1] = 'C'; }
    lv_label_set_text(target_icons[index], icon);
    lv_obj_set_style_text_color(target_icons[index], lv_color_hex(0x75e2c3), 0);
    lv_obj_align(target_icons[index], LV_ALIGN_TOP_MID, 0, 0);
    target_names[index] = lv_label_create(target_cards[index]);
    lv_obj_set_width(target_names[index], 88);
    lv_label_set_long_mode(target_names[index], LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(target_names[index], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(target_names[index], lv_color_hex(0xeef5f7), 0);
    lv_obj_align(target_names[index], LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(target_cards[index], LV_OBJ_FLAG_HIDDEN);
}

static void update_target_card(unsigned index, const kvm_display_view_t *view)
{
    lv_obj_t *card = target_cards[index];
    if (view->screen != KVM_DISPLAY_ACTIVE || !(view->card_visible_mask & (1u << index))) {
        lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(card, LV_OBJ_FLAG_HIDDEN);
    /* No host session: dim only This PC (card 0, the USB-attached host); guest
       cards stay prominent. Every redraw sets opacity explicitly, so it restores
       when usb_dim clears. */
    lv_obj_set_style_opa(card, (view->usb_dim && index == 0) ? LV_OPA_50 : LV_OPA_COVER, 0);
    bool selected = view->selected_card == index;
    bool ready = view->card_ready_mask & (1u << index);
    lv_obj_set_style_bg_color(card, lv_color_hex(selected ? 0x1d4948 : 0x15242e), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(selected ? 0x75e2c3 : 0x405560), 0);
    lv_obj_set_style_border_width(card, selected ? 3 : 1, 0);
    lv_obj_set_style_text_color(target_icons[index],
        lv_color_hex(ready ? 0x75e2c3 : 0x6f7f87), 0);
    lv_obj_set_style_text_color(target_names[index],
        lv_color_hex(ready ? 0xeef5f7 : 0x82939b), 0);
    lv_label_set_text(target_names[index], view->card_labels[index]);
}

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
    gpio_config_t backlight_cfg = {
        .pin_bit_mask = 1ULL << BACKLIGHT_GPIO, .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_set_level(BACKLIGHT_GPIO, 0);
    if (err != ESP_OK) return err;
    if ((err = gpio_config(&backlight_cfg)) != ESP_OK) return err;
    spi_bus_config_t bus = {
        .sclk_io_num = GPIO_NUM_38, .mosi_io_num = GPIO_NUM_39,
        .miso_io_num = GPIO_NUM_NC, .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_WIDTH * LCD_DRAW_LINES * 2 + 8,
    };
    err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = GPIO_NUM_21, .dc_gpio_num = GPIO_NUM_45,
        .spi_mode = 0, .pclk_hz = 40000000, .trans_queue_depth = 10,
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
    title_label = make_label(screen, 12, 0xb4c5ce);
    primary_label = make_label(screen, 42, 0xeef5f7);
    lv_obj_set_height(primary_label, 42);
    detail_label = make_label(screen, 94, 0xb4c5ce);
    footer_label = make_label(screen, 210, 0x75e2c3);
    for (unsigned i = 0; i < 3; ++i)
        row_labels[i] = make_label(screen, 64 + 44 * i, 0xeef5f7);
    for (unsigned i = 0; i < 4; ++i) make_target_card(screen, i);
    left_action = make_action(screen, 12, 103);
    right_action = make_action(screen, 125, 103);
    guest_settings_action = make_action(screen, 190, 42);
    lv_obj_set_size(guest_settings_action, 42, 42);
    lv_obj_align(guest_settings_action, LV_ALIGN_TOP_LEFT, 190, 194);
    lv_obj_add_flag(left_action, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(right_action, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(guest_settings_action, LV_OBJ_FLAG_HIDDEN);
    input_entry_action = make_action(screen, KVM_DISPLAY_INPUT_ENTRY_X, KVM_DISPLAY_INPUT_ENTRY_SIZE);
    lv_obj_set_size(input_entry_action, KVM_DISPLAY_INPUT_ENTRY_SIZE, KVM_DISPLAY_INPUT_ENTRY_SIZE);
    lv_obj_align(input_entry_action, LV_ALIGN_TOP_LEFT, KVM_DISPLAY_INPUT_ENTRY_X,
                 KVM_DISPLAY_INPUT_ENTRY_Y);
    lv_obj_add_flag(input_entry_action, LV_OBJ_FLAG_HIDDEN);
    input_cancel_action = make_action(screen, KVM_DISPLAY_INPUT_CANCEL_X, KVM_DISPLAY_INPUT_CANCEL_W);
    lv_obj_set_size(input_cancel_action, KVM_DISPLAY_INPUT_CANCEL_W, KVM_DISPLAY_INPUT_CANCEL_W);
    lv_obj_align(input_cancel_action, LV_ALIGN_TOP_LEFT, KVM_DISPLAY_INPUT_CANCEL_X, 1);
    lv_obj_add_flag(input_cancel_action, LV_OBJ_FLAG_HIDDEN);
    for (unsigned i = 0; i < 4; ++i) {
        /* A 42 px box inside each 44 px touch row. */
        input_rows[i] = make_action(screen, KVM_DISPLAY_INPUT_ROW_X, KVM_DISPLAY_INPUT_ROW_W);
        lv_obj_set_size(input_rows[i], KVM_DISPLAY_INPUT_ROW_W, KVM_DISPLAY_INPUT_ROW_H - 2);
        lv_obj_align(input_rows[i], LV_ALIGN_TOP_LEFT, KVM_DISPLAY_INPUT_ROW_X,
                     (int32_t)(KVM_DISPLAY_INPUT_ROW_Y + KVM_DISPLAY_INPUT_ROW_H * i + 1u));
        input_row_labels[i] = lv_label_create(input_rows[i]);
        lv_obj_set_width(input_row_labels[i], KVM_DISPLAY_INPUT_ROW_W - 16);
        lv_label_set_long_mode(input_row_labels[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_color(input_row_labels[i], lv_color_hex(0xeef5f7), 0);
        lv_label_set_text(input_row_labels[i], "");
        lv_obj_center(input_row_labels[i]);
        lv_obj_add_flag(input_rows[i], LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
    touch_started = pair_callback && init_touch();
    panel_started = true;
    return ESP_OK;
}

static void display_worker(void *context)
{
    (void)context;
    kvm_display_view_t shown = {0};
    char shown_badge[48] = "";
    uint64_t last_touch_ms = 0;
    for (;;) {
        kvm_display_status_t next;
        portENTER_CRITICAL(&status_lock);
        next = latest_status;
        portEXIT_CRITICAL(&status_lock);
        next.touch_available = touch_started;
        kvm_display_view_t view;
        kvm_display_make_view(&next, (uint64_t)esp_timer_get_time() / 1000, &view);
        char badge[48];
        kvm_display_badge_text(&next, badge, sizeof badge);
        /* The badge only replaces the footer on the dimmed active screen. Input
           changes do not alter the view, so compare the badge text as well. */
        bool badge_changed = view.usb_dim && strcmp(badge, shown_badge) != 0;
        if (panel_started && (memcmp(&shown, &view, sizeof(view)) != 0 || badge_changed) &&
            lvgl_port_lock(20)) {
            lv_label_set_text(title_label, view.title);
#if CONFIG_LV_FONT_MONTSERRAT_28
            lv_obj_set_style_text_font(primary_label,
                (view.screen == KVM_DISPLAY_PAIRING || view.screen == KVM_DISPLAY_INPUT_PAIR) &&
                strlen(view.primary) == 6u ? &lv_font_montserrat_28 : LV_FONT_DEFAULT, 0);
#endif
            lv_label_set_text(primary_label, view.primary);
            lv_label_set_text(detail_label, view.detail);
            lv_label_set_text(footer_label, view.usb_dim ? badge : view.footer);
            strcpy(shown_badge, view.usb_dim ? badge : "");
            bool input_screen = view.screen == KVM_DISPLAY_INPUT_PAIR;
            /* The input rows reach y 220; its countdown footer sits below them.
               Beside the entry and gear buttons the footer ends before x 142.
               (TOP_LEFT at x 12 equals the original TOP_MID for width 216.) */
            lv_obj_set_width(footer_label, view.show_input_entry && touch_started
                                               ? KVM_DISPLAY_INPUT_ENTRY_X - 16 : LCD_WIDTH - 24);
            lv_obj_align(footer_label, LV_ALIGN_TOP_LEFT, 12, input_screen ? 220 : 210);
            bool cards = view.screen == KVM_DISPLAY_ACTIVE;
            bool device_rows = input_screen && view.input_row_count > 0;
            if (cards || device_rows) {
                lv_obj_add_flag(primary_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(detail_label, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(primary_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(detail_label, LV_OBJ_FLAG_HIDDEN);
            }
            for (unsigned i = 0; i < 3; ++i) {
                lv_label_set_text(row_labels[i], view.rows[i]);
                if (cards || input_screen) lv_obj_add_flag(row_labels[i], LV_OBJ_FLAG_HIDDEN);
                else lv_obj_remove_flag(row_labels[i], LV_OBJ_FLAG_HIDDEN);
            }
            for (unsigned i = 0; i < 4; ++i) {
                if (input_screen && i < view.input_row_count) {
                    lv_label_set_text(input_row_labels[i], view.input_rows[i]);
                    lv_obj_remove_flag(input_rows[i], LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(input_rows[i], LV_OBJ_FLAG_HIDDEN);
                }
            }
            for (unsigned i = 0; i < 4; ++i) update_target_card(i, &view);
            set_action(input_entry_action, view.show_input_entry && touch_started
                                               ? LV_SYMBOL_KEYBOARD : NULL);
            set_action(input_cancel_action, input_screen && view.input_cancel && touch_started
                                                ? LV_SYMBOL_CLOSE : NULL);
            if (touch_started && !next.recovery_required && !next.updating &&
                !next.fault && !next.armed && !input_screen &&
                next.input_pair_state == KVM_DISPLAY_INPUT_CLOSED) {
                switch (next.pairing_state) {
                case KVM_DISPLAY_PAIRING_CLOSED:
                case KVM_DISPLAY_PAIRING_REJECTED:
                case KVM_DISPLAY_PAIRING_TIMEOUT:
                    set_action(left_action, next.paired_guests ? NULL : "START PAIRING");
                    set_action(right_action, NULL);
                    set_action(guest_settings_action,
                               next.paired_guests ? LV_SYMBOL_SETTINGS : NULL);
                    lv_obj_set_width(left_action, 216);
                    break;
                case KVM_DISPLAY_PAIRING_WAITING:
                    lv_obj_set_width(left_action, 103);
                    set_action(left_action, next.pairing_local_owner ? "CANCEL" : NULL);
                    set_action(right_action, NULL);
                    set_action(guest_settings_action, NULL);
                    break;
                case KVM_DISPLAY_PAIRING_CHALLENGE:
                    lv_obj_set_width(left_action, 103);
                    set_action(left_action, NULL);
                    set_action(right_action, NULL);
                    set_action(guest_settings_action, NULL);
                    break;
                default:
                    set_action(left_action, NULL);
                    set_action(right_action, NULL);
                    set_action(guest_settings_action, NULL);
                    break;
                }
            } else {
                set_action(left_action, NULL);
                set_action(right_action, NULL);
                set_action(guest_settings_action, NULL);
            }
            shown = view;
            lvgl_port_unlock();
            if (!backlight_started && gpio_set_level(BACKLIGHT_GPIO, 1) == ESP_OK)
                backlight_started = true;
        }
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)) && touch_started &&
            esp_lcd_touch_read_data(touch_handle) == ESP_OK) {
            esp_lcd_touch_point_data_t point;
            uint8_t count = 0;
            if (esp_lcd_touch_get_data(touch_handle, &point, &count, 1) == ESP_OK && count) {
                kvm_display_pair_action_t action;
                kvm_display_pair_request_t request;
                kvm_display_input_action_t input_action;
                kvm_display_input_request_t input_request;
                uint8_t input_row = 0;
                uint64_t touch_ms = (uint64_t)esp_timer_get_time() / 1000;
                bool accepted = false;
                /* Input-device pairing first: its zones only exist on its own
                   screen or (the entry) where no card or guest zone lies. */
                if ((!last_touch_ms || touch_ms - last_touch_ms >= 750u) && input_callback &&
                    kvm_display_input_touch_action(&next, point.x, point.y,
                                                   &input_action, &input_row) &&
                    kvm_display_input_request(&next, input_action, input_row, &input_request)) {
                    input_callback(input_request);
                    accepted = true;
                }
                if (!accepted && (!last_touch_ms || touch_ms - last_touch_ms >= 750u) &&
                    view.screen == KVM_DISPLAY_ACTIVE && target_callback) {
                    int card = -1;
                    if (point.x >= 12 && point.x < 116 && point.y >= 42 && point.y < 112) card = 0;
                    else if (point.x >= 124 && point.x < 228 && point.y >= 42 && point.y < 112) card = 1;
                    else if (point.x >= 12 && point.x < 116 && point.y >= 120 && point.y < 190) card = 2;
                    else if (point.x >= 124 && point.x < 228 && point.y >= 120 && point.y < 190) card = 3;
                    if (card >= 0 && (view.card_visible_mask & (1u << card)) &&
                        (view.card_ready_mask & (1u << card))) {
                        target_callback((uint8_t)card);
                        accepted = true;
                    }
                }
                if (!accepted && (!last_touch_ms || touch_ms - last_touch_ms >= 750u) &&
                    kvm_display_pair_touch_action(&next, point.x, point.y, &action) &&
                    kvm_display_pair_request(&next, touch_ms, action, &request) && pair_callback) {
                    pair_callback(request);
                    accepted = true;
                }
                if (accepted) {
                    last_touch_ms = touch_ms;
                }
            }
        }
    }
}

void kvm_display_set_input_fn(kvm_display_input_fn input_fn)
{
    if (!started) input_callback = input_fn; /* read by the display task it precedes */
}

esp_err_t kvm_display_start(bool enable_panel, kvm_display_event_fn event_fn,
                            kvm_display_pair_fn pair_fn, kvm_display_target_fn target_fn)
{
    if (started) return ESP_ERR_INVALID_STATE;
    event_callback = event_fn;
    pair_callback = pair_fn;
    target_callback = target_fn;
    gpio_config_t boot_cfg = {
        .pin_bit_mask = 1ULL << BOOT_GPIO, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    esp_err_t err = gpio_config(&boot_cfg);
    if (err != ESP_OK) return err;
    if (xTaskCreate(button_worker, "kvm_buttons", 3072, NULL, 8, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    started = true;
    if (!enable_panel) return ESP_OK;
    if ((err = init_panel()) != ESP_OK) return err;
    if (xTaskCreate(display_worker, "kvm_display", 4096, NULL, 4, &display_task) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void kvm_display_post_status(const kvm_display_status_t *status)
{
    if (!status) return;
    portENTER_CRITICAL(&status_lock);
    latest_status = *status;
    portEXIT_CRITICAL(&status_lock);
}
