/* Verify the real keyboard-only diagnostic can route with Mac subscriptions. */
#include <assert.h>
#include "hid_report.h"

static int send(void *context, uint16_t handle, uint8_t id,
                const uint8_t *bytes, size_t length)
{
    (void)context; (void)bytes;
    assert(handle == 1);
    assert(id == HID_REPORT_KEYBOARD || id == HID_REPORT_BOOT_KEYBOARD);
    assert(length == HID_KEYBOARD_REPORT_LEN);
    return 0;
}

int main(void)
{
    hid_channel_t channel;
    hid_channel_init(&channel, send, 0);
    hid_channel_connected(&channel, 1);
    /* macOS may subscribe while numeric pairing is still finishing. */
    hid_channel_subscribed(&channel, HID_REPORT_KEYBOARD, true);
    assert(!hid_channel_ready(&channel));
    hid_channel_encrypted(&channel, true);
    assert(hid_channel_ready(&channel));
    assert(hid_channel_arm(&channel));
    /* Match the desktop actor's keyboard/mouse/consumer startup baseline. */
    const uint8_t zero[8] = {0};
    assert(hid_channel_keyboard(&channel, zero));
    assert(hid_channel_mouse(&channel, 0, 0, 0, 0, 0));
    assert(hid_channel_consumer(&channel, 0));
    assert(channel.armed && !channel.needs_disconnect);
    assert(!hid_channel_mouse(&channel, 0, 1, 0, 0, 0));
    assert(!hid_channel_consumer(&channel, 1));
    assert(channel.armed && !channel.needs_disconnect);
    const uint8_t a[8] = {0, 0, 4};
    assert(hid_channel_keyboard(&channel, a));
    assert(hid_channel_release(&channel));
    assert(hid_channel_set_protocol_mode(&channel, 0));
    assert(!hid_channel_ready(&channel));
    hid_channel_subscribed(&channel, HID_REPORT_BOOT_KEYBOARD, true);
    assert(hid_channel_ready(&channel));
    assert(hid_channel_arm(&channel));
    assert(hid_channel_mouse(&channel, 0, 0, 0, 0, 0));
    assert(hid_channel_consumer(&channel, 0));
    hid_channel_encrypted(&channel, false);
    assert(!hid_channel_ready(&channel) && !channel.armed);
    return 0;
}
