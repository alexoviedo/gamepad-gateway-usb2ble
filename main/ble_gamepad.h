#pragma once
#include "shared_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize NimBLE and start advertising as a Gamepad
void ble_gamepad_init(void);

// Send updated gamepad state over BLE (should be called from a task)
void ble_gamepad_send_state(const struct GamepadState *state);

#ifdef __cplusplus
}
#endif
