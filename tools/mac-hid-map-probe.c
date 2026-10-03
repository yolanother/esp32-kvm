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

int main(void)
{
    CFMutableDictionaryRef properties = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFDataRef map = CFDataCreate(kCFAllocatorDefault, hid_report_map,
                                (CFIndex)hid_report_map_len);
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
                hid_report_map_len);
        return 1;
    }
    printf("Created temporary virtual HID device with %zu-byte firmware map; "
           "checking registration for 20 seconds\n", hid_report_map_len);
    fflush(stdout);
    sleep(20);
    CFRelease(device);
    return 0;
}
