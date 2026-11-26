/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Implements the boot-time USB mode selection on ESP-IDF: the RTC one-shot
 * flag, the reset-reason mapping, the BOOT escape window and the application
 * of the pure decision (usb_mode.c) to the NVS store. Every failure path ends
 * in serial mode; nothing here starts a USB driver. */
#include "usb_boot.h"
#include "usb_mode_store.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#ifdef CONFIG_KVM_USB_COMPOSITE_SUPPORT
#define KVM_USB_BUILT true
#else
#define KVM_USB_BUILT false
#endif
#ifdef CONFIG_KVM_USB_FORCE_SERIAL
#define KVM_USB_FORCED true
#else
#define KVM_USB_FORCED false
#endif

#define BOOT_GPIO GPIO_NUM_0      /* active low, same pin and config as display.c */
#define ESCAPE_SAMPLE_MS 10u
#define ESCAPE_HOLD_MS 200u
#define SERIAL_ONCE_MAGIC 0x55534231u /* "USB1" */
#define RELEASE_FLUSH_MS 100u

static const char *TAG = "kvm-usb";

typedef struct {
    uint32_t magic;
    uint32_t inverse;
} kvm_usb_serial_once_t;

/* Not initialized at any boot: survives esp_restart(), random after power-on. */
static RTC_NOINIT_ATTR kvm_usb_serial_once_t serial_once_flag;

static kvm_usb_boot_decision_t last_decision = {
    KVM_USB_MODE_SERIAL_JTAG, KVM_USB_REASON_DEFAULT, false, false,
};
static bool decided;

bool kvm_usb_take_serial_once(void)
{
    volatile kvm_usb_serial_once_t *flag = &serial_once_flag;
    bool set = flag->magic == SERIAL_ONCE_MAGIC && flag->inverse == (uint32_t)~SERIAL_ONCE_MAGIC;
    flag->magic = 0;
    flag->inverse = 0;
    return set;
}

void kvm_usb_set_serial_once(void)
{
    volatile kvm_usb_serial_once_t *flag = &serial_once_flag;
    flag->magic = SERIAL_ONCE_MAGIC;
    flag->inverse = (uint32_t)~SERIAL_ONCE_MAGIC;
}

bool kvm_usb_reset_abnormal(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
    case ESP_RST_CPU_LOCKUP:
        return true;
    default:
        return false;
    }
}

/* Samples BOOT for the escape window; true means "serial for this boot". */
static bool escape_window(void)
{
    gpio_config_t boot_cfg = {
        .pin_bit_mask = 1ULL << BOOT_GPIO, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    if (gpio_config(&boot_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "BOOT input unavailable; staying serial for this boot");
        return true;
    }
    kvm_usb_escape_t escape;
    kvm_usb_escape_init(&escape, CONFIG_KVM_USB_ESCAPE_WINDOW_MS, ESCAPE_HOLD_MS);
    TickType_t delay = pdMS_TO_TICKS(ESCAPE_SAMPLE_MS);
    if (delay == 0) delay = 1;
    const int64_t start_us = esp_timer_get_time();
    for (;;) {
        int64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000;
        if (elapsed_ms < 0) elapsed_ms = 0;
        if (elapsed_ms > (int64_t)UINT32_MAX) elapsed_ms = UINT32_MAX;
        if (!kvm_usb_escape_sample(&escape, gpio_get_level(BOOT_GPIO) == 0, (uint32_t)elapsed_ms))
            break;
        vTaskDelay(delay);
    }
    return escape.held;
}

kvm_usb_boot_decision_t kvm_usb_boot_decide(void)
{
    if (decided) return last_decision;
    kvm_usb_boot_inputs_t in = {0};
    in.composite_built = KVM_USB_BUILT;
    in.force_serial = KVM_USB_FORCED;
    in.serial_once = kvm_usb_take_serial_once(); /* one-shot: consumed on every boot */
    in.abnormal_reset = kvm_usb_reset_abnormal();
    in.max_unconfirmed = (uint8_t)CONFIG_KVM_USB_MAX_UNCONFIRMED;
    /* A build that cannot start composite ignores the store completely. */
    if (in.composite_built && !in.force_serial) {
        esp_err_t err = kvm_usb_mode_store_load(&in.stored_mode, &in.unconfirmed);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "USB mode store unreadable (%d); serial", err);
            in.stored_mode = KVM_USB_MODE_SERIAL_JTAG;
            in.unconfirmed = 0;
        }
    }
    if (kvm_usb_mode_needs_window(&in)) in.escape_held = escape_window();
    kvm_usb_boot_decision_t d = kvm_usb_mode_decide(&in);
    if (d.persist_serial) {
        esp_err_t err = kvm_usb_mode_store_set_mode(KVM_USB_MODE_SERIAL_JTAG);
        if (err != ESP_OK) ESP_LOGE(TAG, "Persisting serial mode failed (%d)", err);
    }
    if (d.count_attempt) {
        /* decide() only counts when unconfirmed < max_unconfirmed <= 255. */
        esp_err_t err = in.unconfirmed < UINT8_MAX
            ? kvm_usb_mode_store_set_unconfirmed((uint8_t)(in.unconfirmed + 1))
            : ESP_ERR_INVALID_STATE;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Counting the composite attempt failed (%d); serial", err);
            d.mode = KVM_USB_MODE_SERIAL_JTAG;
            d.reason = KVM_USB_REASON_DEFAULT;
            d.count_attempt = false;
        }
    }
    last_decision = d;
    decided = true;
    ESP_LOGI(TAG, "USB boot: mode=%d reason=%d stored=%u unconf=%u", (int)d.mode, (int)d.reason,
             (unsigned)in.stored_mode, (unsigned)in.unconfirmed);
    return d;
}

kvm_usb_boot_decision_t kvm_usb_boot_last(void)
{
    return last_decision;
}

static void restart_after_release(void)
{
    vTaskDelay(pdMS_TO_TICKS(RELEASE_FLUSH_MS));
    esp_restart();
}

void kvm_usb_reboot_serial_once(void)
{
    kvm_usb_set_serial_once();
    ESP_LOGW(TAG, "Restarting once in USB Serial/JTAG mode");
    restart_after_release();
}

void kvm_usb_reboot_with_mode(kvm_usb_mode_t mode)
{
    if (mode == KVM_USB_MODE_COMPOSITE) {
        if (!KVM_USB_BUILT || KVM_USB_FORCED) {
            ESP_LOGW(TAG, "Composite USB is not in this build; request refused");
            return;
        }
        /* Fresh attempt budget first: if the mode write then fails, nothing changed. */
        esp_err_t err = kvm_usb_mode_store_set_unconfirmed(0);
        if (err == ESP_OK) err = kvm_usb_mode_store_set_mode(KVM_USB_MODE_COMPOSITE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Enabling composite USB failed (%d); not restarting", err);
            return;
        }
        ESP_LOGW(TAG, "Restarting in composite USB mode");
    } else {
        esp_err_t err = kvm_usb_mode_store_set_mode(KVM_USB_MODE_SERIAL_JTAG);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Persisting serial mode failed (%d); restarting serial once", err);
            kvm_usb_set_serial_once();
        } else {
            err = kvm_usb_mode_store_set_unconfirmed(0);
            if (err != ESP_OK) ESP_LOGE(TAG, "Clearing the attempt count failed (%d)", err);
        }
        ESP_LOGW(TAG, "Restarting in USB Serial/JTAG mode");
    }
    restart_after_release();
}
