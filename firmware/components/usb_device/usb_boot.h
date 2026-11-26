/* Copyright (c) ESP32 KVM contributors. Use is governed by the root LICENSE.
 * Declares the boot-time USB mode selection (spec Decisions 7 to 9): the RTC
 * "serial once" flag, the reset-reason check, the escape window and the boot
 * decision applied to the NVS mode store, plus the worker's reboot helpers.
 * With CONFIG_KVM_USB_COMPOSITE_SUPPORT=n the decision is always serial
 * (NOT_BUILT), never waits, never reads or writes NVS and never touches GPIO0;
 * the boot snapshot still records the inputs (once=1 after a serial-once
 * restart), which is the drill's proof that the RTC flag survived. */
#ifndef ESP32_KVM_USB_BOOT_H
#define ESP32_KVM_USB_BOOT_H

#include <stdbool.h>
#include "usb_mode.h"

/** Returns whether the one-shot flag was set, and clears it before returning,
 *  so it is honoured at most once. The flag lives in RTC_NOINIT memory as a
 *  magic word plus its bitwise inverse: power-on garbage is never read as set. */
bool kvm_usb_take_serial_once(void);

/** Sets the one-shot flag; it survives esp_restart() but not a power cycle. */
void kvm_usb_set_serial_once(void);

/** True for PANIC, INT_WDT, TASK_WDT, WDT, BROWNOUT, CPU_LOCKUP and
 *  PWR_GLITCH resets (the last two are never reported by the S3 port). */
bool kvm_usb_reset_abnormal(void);

/** Gathers the boot facts, runs the escape window (GPIO0 sampled every 10 ms,
 *  BOOT counted once held for 200 ms, KVM_USB_ESCAPE_WINDOW_MS in total) only
 *  when kvm_usb_mode_needs_window says the boot would otherwise be composite,
 *  decides, and applies the decision to NVS before returning: persist_serial
 *  writes mode 0; count_attempt writes unconf + 1, and if that write fails the
 *  boot falls back to serial (reason DEFAULT) so composite never starts with an
 *  uncounted attempt. The serial-once flag is consumed on every boot. Call once
 *  from app_main after nvs_flash_init (doubtech_hid_start) and before any USB
 *  driver starts; later calls return the first result without side effects.
 *  Known limit (for task 10): the window runs before the transport installs the
 *  USB Serial/JTAG driver, so after a software reset from composite mode the
 *  PHY is still routed to OTG during the window (spec Decision 8 wants USJ live
 *  there); the BOOT escape itself does not depend on it. */
kvm_usb_boot_decision_t kvm_usb_boot_decide(void);

/** This boot's snapshot: the decision as applied plus the inputs it used
 *  (serial_once, escape_held, abnormal_reset, stored_mode, unconfirmed) and the
 *  esp_reset_reason() value, for the screen, STATUS and the diagnostics dump.
 *  Before kvm_usb_boot_decide has run: serial / DEFAULT, all inputs 0 and
 *  reset_reason -1. Written once in app_main before the tasks that read it
 *  start. kvm_usb_boot_decide logs the same facts (kvm_usb_boot_describe). */
kvm_usb_boot_snapshot_t kvm_usb_boot_last(void);

/** Worker only, after it has done the emergency release (router, hub, HID
 *  sink) and reset the transport core: sets the serial-once flag, waits 100 ms
 *  so queued release reports can leave, and restarts. Does not return. */
void kvm_usb_reboot_serial_once(void);

/** Worker only, same preconditions. SERIAL: in a build with composite support
 *  or the rescue build, persists mode 0 and clears unconf (if the mode write
 *  fails the serial-once flag is set instead, so the next boot is still
 *  serial); in a build without either it writes nothing (Decision 6: the key is
 *  ignored completely); then restarts in every build. COMPOSITE: refused (returns, nothing
 *  written) unless the build has composite support and is not the rescue
 *  build; otherwise clears unconf, then persists mode 1, and restarts; if a
 *  write fails it returns without restarting. Any other value is SERIAL. */
void kvm_usb_reboot_with_mode(kvm_usb_mode_t mode);

#endif
