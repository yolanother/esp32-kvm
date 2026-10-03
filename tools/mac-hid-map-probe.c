/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Check whether macOS accepts the firmware's exact HID report map without BLE.
 * Build: clang -I firmware/components/hid tools/mac-hid-map-probe.c \
 *        firmware/components/hid/hid_report.c -framework IOKit \
 *        -framework CoreFoundation -o /tmp/mac-hid-map-probe
 * Run: /tmp/mac-hid-map-probe & sleep 2; hidutil list | grep 'ESP32 KVM Map Probe'; wait
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hidsystem/IOHIDUserDevice.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "hid_report.h"

/* The macOS 26 SDK exports this symbol but omits its declaration. */
extern IOHIDUserDeviceRef IOHIDUserDeviceCreate(CFAllocatorRef allocator,
                                                CFDictionaryRef properties);

static void set_number(CFMutableDictionaryRef properties, CFStringRef key, int value)
{
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);
    CFDictionarySetValue(properties, key, number);
    CFRelease(number);
}

int main(int argc, char **argv)
{
    /* Control: if this fails too, virtual-device policy is the likely cause. */
    static const uint8_t minimal_keyboard[] = {
        0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,
        0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
        0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
        0x75, 0x08, 0x95, 0x01, 0x81, 0x03,
        0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x15, 0x00, 0x25, 0x65,
        0x75, 0x08, 0x95, 0x06, 0x81, 0x00, 0xc0
    };
    bool control = argc > 1 && strcmp(argv[1], "--minimal") == 0;
    const uint8_t *bytes = control ? minimal_keyboard : hid_report_map;
    size_t length = control ? sizeof(minimal_keyboard) : hid_report_map_len;
    CFMutableDictionaryRef properties = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFDataRef map = CFDataCreate(kCFAllocatorDefault, bytes, (CFIndex)length);
    CFDictionarySetValue(properties, CFSTR(kIOHIDReportDescriptorKey), map);
    CFDictionarySetValue(properties, CFSTR(kIOHIDProductKey),
                         CFSTR("ESP32 KVM Map Probe"));
    CFDictionarySetValue(properties, CFSTR(kIOHIDTransportKey), CFSTR("Virtual"));
    set_number(properties, CFSTR(kIOHIDVendorIDKey), 0x303a);
    set_number(properties, CFSTR(kIOHIDProductIDKey), 0x1002);

    IOHIDUserDeviceRef device = IOHIDUserDeviceCreate(kCFAllocatorDefault, properties);
    CFRelease(map);
    CFRelease(properties);
    if (device == NULL) {
        fprintf(stderr, "IOHIDUserDeviceCreate failed for %zu-byte map\n",
                length);
        return 1;
    }
    printf("Created temporary virtual HID device with %zu-byte firmware map; "
           "checking registration for 20 seconds\n", length);
    fflush(stdout);
    sleep(20);
    CFRelease(device);
    return 0;
}
