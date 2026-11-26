/* Bounded, opt-in BLE trace retrieval on boards without a connected UART. */
#include "transport_usb_serial_jtag.h"
#include "sdkconfig.h"
#ifdef CONFIG_DOUBTECH_HID_GATT_TRACE
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "usb_boot.h"
#define TRACE_SIZE 16384
static char trace[TRACE_SIZE];
static char snapshot[TRACE_SIZE];
static size_t position, count;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

static int capture_log(const char *format, va_list args)
{
    char line[512];
    va_list copy;
    va_copy(copy, args);
    int length = vsnprintf(line, sizeof(line), format, copy);
    va_end(copy);
    size_t used = length < 0 ? 0 : (size_t)length;
    if (used >= sizeof(line)) used = sizeof(line) - 1;
    portENTER_CRITICAL(&lock);
    for (size_t i = 0; i < used; ++i) {
        trace[position] = line[i];
        position = (position + 1) % TRACE_SIZE;
        if (count < TRACE_SIZE) ++count;
    }
    portEXIT_CRITICAL(&lock);
    /* UART polling at 115200 can consume the HID RPC's entire 20 ms budget.
     * Diagnostic logs are retrieved from the ring instead of duplicating them
     * synchronously to the unconnected UART console. */
    return length;
}

void kvm_transport_diagnostics_init(void)
{
    (void)esp_log_set_vprintf(capture_log);
    ESP_LOGI("kvm-hid-trace", "BLE trace capture initialized");
}

bool kvm_transport_diagnostics_requested(const uint8_t *bytes, size_t length)
{
    static const char command[] = "\0KVM-DIAG-LOGS\n";
    static size_t matched;
    for (size_t i = 0; i < length; ++i) {
        if (bytes[i] == (uint8_t)command[matched]) ++matched;
        else matched = bytes[i] == 0 ? 1 : 0;
        if (matched == sizeof(command) - 1) {
            matched = 0;
            return true;
        }
    }
    return false;
}

void kvm_transport_diagnostics_dump_to(const kvm_transport_link_t *link)
{
    if (!link || !link->write) return;
    char identity[256];
    char usb_boot[128];
    const esp_app_desc_t *app = esp_app_get_description();
    /* The USB boot facts sit in the header, not only in the ring, so a long
     * uptime that wraps the 16 KB trace still shows them (task 9 drill). */
    kvm_usb_boot_snapshot_t boot = kvm_usb_boot_last();
    (void)kvm_usb_boot_describe(&boot, usb_boot, sizeof(usb_boot));
    int identity_length = snprintf(identity, sizeof(identity),
        "\nKVM-TRACE-BEGIN\nbuild=%s %s uptime_ms=%lld trace_bytes=%u\nusb_boot %s\n",
        app->date, app->time, (long long)(esp_timer_get_time() / 1000), (unsigned)count, usb_boot);
    if (identity_length < 0) identity_length = 0;
    if ((size_t)identity_length >= sizeof(identity)) identity_length = (int)sizeof(identity) - 1;
    portENTER_CRITICAL(&lock);
    size_t length = count;
    size_t start = (position + TRACE_SIZE - count) % TRACE_SIZE;
    for (size_t i = 0; i < length; ++i)
        snapshot[i] = trace[(start + i) % TRACE_SIZE];
    portEXIT_CRITICAL(&lock);
    link->write(link->context, (const uint8_t *)identity, (size_t)identity_length, pdMS_TO_TICKS(100));
    for (size_t at = 0; at < length;) {
        size_t chunk = length - at > 256 ? 256 : length - at;
        int written = link->write(link->context, (const uint8_t *)snapshot + at, chunk, pdMS_TO_TICKS(100));
        if (written <= 0) break;
        at += (size_t)written;
    }
    link->write(link->context, (const uint8_t *)"\nKVM-TRACE-END\n", 15, pdMS_TO_TICKS(100));
}
#else
void kvm_transport_diagnostics_init(void) {}
bool kvm_transport_diagnostics_requested(const uint8_t *bytes, size_t length)
{ (void)bytes; (void)length; return false; }
void kvm_transport_diagnostics_dump_to(const kvm_transport_link_t *link) { (void)link; }
#endif

void kvm_transport_diagnostics_dump(void)
{
    kvm_transport_diagnostics_dump_to(kvm_transport_link_usj());
}
