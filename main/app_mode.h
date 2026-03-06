#pragma once

#include <stdint.h>

// Application BLE mode.
//
// RUN:
//  - Advertises as a BLE HID Gamepad (HOGP)
//  - WebBLE config service disabled
//
// CONFIG:
//  - Advertises a custom GATT service for Web Bluetooth configuration
//  - HID gamepad output is paused/disabled
//
typedef enum {
  APP_MODE_RUN = 0,
  APP_MODE_CONFIG = 1,
} app_mode_t;

// Read persisted boot mode from NVS and set the current in-RAM mode.
// Defaults to RUN.
void app_mode_init(void);

// Current mode for this boot.
app_mode_t app_mode_current(void);

// Persist the boot mode for the next restart.
void app_mode_set_boot_mode(app_mode_t mode);

// Persist boot mode and reboot immediately.
void app_mode_reboot_to(app_mode_t mode);

// Human-readable name.
const char *app_mode_name(app_mode_t mode);