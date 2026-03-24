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

static constexpr UBaseType_t kBleRunTxTaskPriority = 5;
static constexpr BaseType_t kBleRunTxTaskCore = 0;
static constexpr TickType_t kBleRunTxPeriod = pdMS_TO_TICKS(16);
static constexpr int16_t kBleAnalogNotifyThreshold = 96;

static void translate_usb_to_ble(const GamepadState *in, GamepadState *out) {
  *out = *in;
}

static inline int16_t abs_axis_delta(int16_t a, int16_t b) {
  return (a >= b) ? (a - b) : (b - a);
}

static bool has_digital_delta(const GamepadState &a, const GamepadState &b) {
  return a.buttons != b.buttons || a.hat != b.hat;
}

static bool has_significant_analog_delta(const GamepadState &a, const GamepadState &b) {
  return abs_axis_delta(a.x, b.x) >= kBleAnalogNotifyThreshold ||
         abs_axis_delta(a.y, b.y) >= kBleAnalogNotifyThreshold ||
         abs_axis_delta(a.z, b.z) >= kBleAnalogNotifyThreshold ||
         abs_axis_delta(a.rx, b.rx) >= kBleAnalogNotifyThreshold ||
         abs_axis_delta(a.ry, b.ry) >= kBleAnalogNotifyThreshold ||
         abs_axis_delta(a.rz, b.rz) >= kBleAnalogNotifyThreshold ||
         abs_axis_delta(a.slider1, b.slider1) >= kBleAnalogNotifyThreshold ||
         abs_axis_delta(a.slider2, b.slider2) >= kBleAnalogNotifyThreshold;
}

static GamepadState apply_analog_hysteresis(const GamepadState &candidate, const GamepadState &reference) {
  GamepadState filtered = candidate;
  if (abs_axis_delta(filtered.x, reference.x) < kBleAnalogNotifyThreshold) filtered.x = reference.x;
  if (abs_axis_delta(filtered.y, reference.y) < kBleAnalogNotifyThreshold) filtered.y = reference.y;
  if (abs_axis_delta(filtered.z, reference.z) < kBleAnalogNotifyThreshold) filtered.z = reference.z;
  if (abs_axis_delta(filtered.rx, reference.rx) < kBleAnalogNotifyThreshold) filtered.rx = reference.rx;
  if (abs_axis_delta(filtered.ry, reference.ry) < kBleAnalogNotifyThreshold) filtered.ry = reference.ry;
  if (abs_axis_delta(filtered.rz, reference.rz) < kBleAnalogNotifyThreshold) filtered.rz = reference.rz;
  if (abs_axis_delta(filtered.slider1, reference.slider1) < kBleAnalogNotifyThreshold) filtered.slider1 = reference.slider1;
  if (abs_axis_delta(filtered.slider2, reference.slider2) < kBleAnalogNotifyThreshold) filtered.slider2 = reference.slider2;
  return filtered;
}

static void ble_run_tx_task(void *) {
  GamepadState usb_state = {};
  GamepadState candidate = {};
  GamepadState pending = {};
  GamepadState last_sent = {};
  bool have_pending = false;
  bool have_last_sent = false;
  TickType_t last_send_tick = 0;

  while (true) {
    const TickType_t now = xTaskGetTickCount();
    const TickType_t wait_ticks = have_pending
        ? ((now - last_send_tick) >= kBleRunTxPeriod ? 0 : (kBleRunTxPeriod - (now - last_send_tick)))
        : portMAX_DELAY;

    if (hid_device_manager_wait_for_next_state_ticks(&usb_state, wait_ticks)) {
      translate_usb_to_ble(&usb_state, &candidate);

      if (!have_last_sent) {
        pending = candidate;
        have_pending = true;
      } else {
        GamepadState filtered = apply_analog_hysteresis(candidate, last_sent);
        const bool digital_changed = has_digital_delta(filtered, last_sent);
        const bool analog_changed = has_significant_analog_delta(filtered, last_sent);

        if (digital_changed) {
          ble_gamepad_send_state(&filtered);
          last_sent = filtered;
          have_last_sent = true;
          have_pending = false;
          last_send_tick = xTaskGetTickCount();
        } else if (analog_changed) {
          pending = filtered;
          have_pending = true;
        }
      }
    }

    if (have_pending) {
      const TickType_t send_now = xTaskGetTickCount();
      if (!have_last_sent || (send_now - last_send_tick) >= kBleRunTxPeriod) {
        ble_gamepad_send_state(&pending);
        last_sent = pending;
        have_last_sent = true;
        have_pending = false;
        last_send_tick = send_now;
      }
    }
  }
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
    xTaskCreatePinnedToCore(ble_run_tx_task, "ble_run_tx", 4096, nullptr,
                            kBleRunTxTaskPriority, nullptr, kBleRunTxTaskCore);
  }

  if (app_mode_current() == APP_MODE_CONFIG) {
    xTaskCreatePinnedToCore(
        [](void *) {
          const TickType_t p = pdMS_TO_TICKS(1000 / 60);
          TickType_t lw = xTaskGetTickCount();
          while (true) {
            ble_config_service_stream_tick();
            vTaskDelayUntil(&lw, p);
          }
        },
        "cfg_stream", 4096, nullptr, 4, nullptr, 0);
  }

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
