#include "ble_gamepad.h"
#include "ble_config_service.h"
#include "app_mode.h"
#include "hid_device_manager.h"
#include "usb_host_manager.h"
#include "mapping_engine.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <stdio.h>
#include <string.h>

// -----------------------------------------------------------------------------
// Translation Layer
// -----------------------------------------------------------------------------
// Current implementation note:
//   - translate_usb_to_ble() is intentionally identity-only on current main.
//   - The actual deterministic mapping from descriptor-derived HID inputs to the
//     outgoing canonical GamepadState happens earlier in mapping_engine_compute(),
//     reached through hid_device_manager_get_merged_state().
//   - This function currently exists only as a final pass-through seam between
//     the merged USB-side GamepadState and the BLE sender.
//
// USB side produces a canonical GamepadState:
//   -32767..32767 for axes/sliders
//   hat: 0=center, 1=N,2=NE,...8=NW
//   buttons: bit0=Button1 ... bit31=Button32
//
// BLE RUN mode currently consumes that same logical layout without further
// remapping in this function.
//
// Keep this function fast and non-blocking.
static void translate_usb_to_ble(const GamepadState *in, GamepadState *out) {
  // Identity mapping only on current main.
  *out = *in;
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

  // Determine boot mode (RUN vs CONFIG). This is persisted in NVS.
  app_mode_init();

  // 1) Init BLE (NimBLE host runs in its own FreeRTOS task)
  if (app_mode_current() == APP_MODE_CONFIG) {
    ble_config_init();
  } else {
    ble_gamepad_init();
  }

  // 2) Init USB Host (daemon task handles host events)
  usb_host_manager_init();

  // 3) Init HID Host Class Driver (creates its own background task)
  hid_device_manager_init();

  if (app_mode_current() == APP_MODE_RUN) {
    printf("RUN mode active mapping debug:\n");
    mapping::mapping_engine_log_profile();
  }

  // 4) Main loop: translate + notify BLE at a fixed cadence.
  //    All heavy lifting is done in the background tasks.
  GamepadState usb_state;
  GamepadState ble_state;
  memset(&usb_state, 0, sizeof(usb_state));
  memset(&ble_state, 0, sizeof(ble_state));

  const TickType_t period = pdMS_TO_TICKS(20); // 50 Hz
  TickType_t last_wake = xTaskGetTickCount();

  // In CONFIG mode, stream samples to WebBLE at 60Hz (separate cadence from HID).
  // HID output is paused by skipping ble_gamepad_send_state.
  if (app_mode_current() == APP_MODE_CONFIG) {
    xTaskCreate(
        [](void *) {
          const TickType_t p = pdMS_TO_TICKS(1000 / 60);
          TickType_t lw = xTaskGetTickCount();
          while (true) {
            ble_config_service_stream_tick();
            vTaskDelayUntil(&lw, p);
          }
        },
        "cfg_stream", 4096, nullptr, 4, nullptr);
  }

  while (1) {
    hid_device_manager_get_merged_state(&usb_state);
    translate_usb_to_ble(&usb_state, &ble_state);
    if (app_mode_current() == APP_MODE_RUN) {
      ble_gamepad_send_state(&ble_state);
    }

    vTaskDelayUntil(&last_wake, period);
  }
}
