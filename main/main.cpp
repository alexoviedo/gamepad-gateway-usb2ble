#include "ble_gamepad.h"
#include "hid_device_manager.h"
#include "usb_host_manager.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <stdio.h>
#include <string.h>

// -----------------------------------------------------------------------------
// Translation Layer
// -----------------------------------------------------------------------------
// This is the *one place* you should edit when you want to change how USB HID
// inputs map to the outgoing BLE controller.
//
// USB side (Project A) produces a normalized GamepadState:
//   -32767..32767 for axes/sliders
//   hat: 0=center, 1=N,2=NE,...8=NW
//   buttons: bit0=Button1 ... bit31=Button32
//
// BLE side (Project B style / HOGP) consumes the same logical layout, but you
// may want to:
//   * invert axes
//   * swap axes
//   * apply deadzones
//   * remap sliders to different axes
//
// Keep this function fast and non-blocking.
static inline int16_t apply_deadzone(int16_t v, int16_t dz) {
  if (v > -dz && v < dz)
    return 0;
  return v;
}

static void translate_usb_to_ble(const GamepadState *in, GamepadState *out) {
  // Start with an identity mapping.
  *out = *in;

  // Example tweaks (uncomment if desired):
  //
  // 1) Invert Y axis (many sticks are "up is negative")
  // out->y = (int16_t)(-out->y);
  //
  // 2) Apply deadzone to stick axes
  // const int16_t DZ = 800; // ~2.4% of full scale
  // out->x = apply_deadzone(out->x, DZ);
  // out->y = apply_deadzone(out->y, DZ);
  //
  // 3) Swap rudder (Z) and twist (Rz)
  // int16_t tmp = out->z;
  // out->z = out->rz;
  // out->rz = tmp;
}

extern "C" void app_main() {
  printf("--- HOTAS USB to BLE Gamepad Bridge ---\n");
  printf("Target: ESP32-S3 (USB OTG Host + BLE)\n\n");

  // NVS is required for BLE bonding / key storage.
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // 1) Init BLE (NimBLE host runs in its own FreeRTOS task)
  ble_gamepad_init();

  // 2) Init USB Host (daemon task handles host events)
  usb_host_manager_init();

  // 3) Init HID Host Class Driver (creates its own background task)
  hid_device_manager_init();

  // 4) Main loop: translate + notify BLE at a fixed cadence.
  //    All heavy lifting is done in the background tasks.
  GamepadState usb_state;
  GamepadState ble_state;
  memset(&usb_state, 0, sizeof(usb_state));
  memset(&ble_state, 0, sizeof(ble_state));

  const TickType_t period = pdMS_TO_TICKS(20); // 50 Hz
  TickType_t last_wake = xTaskGetTickCount();

  while (1) {
    hid_device_manager_get_merged_state(&usb_state);
    translate_usb_to_ble(&usb_state, &ble_state);
    ble_gamepad_send_state(&ble_state);

    vTaskDelayUntil(&last_wake, period);
  }
}
