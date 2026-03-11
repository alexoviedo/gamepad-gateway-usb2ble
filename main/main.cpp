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
// This is the *one place* you should edit when you want to change how USB HID
// inputs map to the outgoing BLE controller.
//
// USB side produces a normalized GamepadState:
//   -32767..32767 for axes/sliders
//   hat: 0=center, 1=N,2=NE,...8=NW
//   buttons: bit0=Button1 ... bit31=Button32
//
// BLE side consumes the same logical layout.
static inline int16_t apply_deadzone(int16_t v, int16_t dz) {
  if (v > -dz && v < dz) {
    return 0;
  }
  return v;
}

static void translate_usb_to_ble(const GamepadState *in, GamepadState *out) {
  *out = *in;

  // Example tweaks:
  //
  // out->y = (int16_t)(-out->y);
  //
  // const int16_t DZ = 800;
  // out->x = apply_deadzone(out->x, DZ);
  // out->y = apply_deadzone(out->y, DZ);
  //
  // int16_t tmp = out->z;
  // out->z = out->rz;
  // out->rz = tmp;
}

extern "C" void app_main() {
  printf("--- HOTAS USB to BLE Gamepad Bridge ---\n");
  printf("Target: ESP32-S3 (USB OTG Host + BLE)\n\n");

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  app_mode_init();

  if (app_mode_current() == APP_MODE_CONFIG) {
    ble_config_init();
  } else {
    ble_gamepad_init();
  }

  usb_host_manager_init();
  hid_device_manager_init();

  if (app_mode_current() == APP_MODE_RUN) {
    printf("RUN mode active mapping debug:\n");
    mapping::mapping_engine_log_profile();
  }

  GamepadState usb_state;
  GamepadState ble_state;
  memset(&usb_state, 0, sizeof(usb_state));
  memset(&ble_state, 0, sizeof(ble_state));

  const TickType_t period = pdMS_TO_TICKS(20);
  TickType_t last_wake = xTaskGetTickCount();

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