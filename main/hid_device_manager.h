#pragma once
#include "shared_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the HID Host Driver and connection callbacks
void hid_device_manager_init(void);

// Get the latest merged state (thread-safe, lock-free or protected)
void hid_device_manager_get_merged_state(struct GamepadState *out_state);

#ifdef __cplusplus
}
#endif
