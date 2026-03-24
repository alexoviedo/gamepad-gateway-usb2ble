#include "ble_gamepad.h"
#include "ble_config_service.h"
#include "app_mode.h"
#include "hid_device_manager.h"
#include "usb_host_manager.h"
#include "mapping_engine.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <limits.h>
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
  *out = *in;
}

#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
static constexpr BaseType_t kBleTaskCore = 0;
#else
static constexpr BaseType_t kBleTaskCore = tskNO_AFFINITY;
#endif

static constexpr UBaseType_t kBleTxTaskPriority = 6;
static constexpr UBaseType_t kCfgStreamTaskPriority = 6;
static constexpr TickType_t kBleTxMinIntervalTicks = pdMS_TO_TICKS(16);
static constexpr TickType_t kBleTxIdlePollTicks = pdMS_TO_TICKS(4);
static constexpr TickType_t kCfgStreamPeriod = pdMS_TO_TICKS(1000 / 60);

static void cfg_stream_task(void *) {
  TickType_t last_wake = xTaskGetTickCount();
  while (true) {
    ble_config_service_stream_tick();
    vTaskDelayUntil(&last_wake, kCfgStreamPeriod);
  }
}

static void ble_tx_task(void *) {
  GamepadState latest_usb_state = {0, 0, 0, 0, 0, 0, 0, 0, HatDirection::CENTER, 0};
  GamepadState ble_state = latest_usb_state;
  bool have_pending_state = false;
  TickType_t last_tx_tick = 0;

  while (true) {
    TickType_t now = xTaskGetTickCount();
    TickType_t wait_ticks = kBleTxIdlePollTicks;
    if (have_pending_state && last_tx_tick != 0) {
      const TickType_t elapsed = now - last_tx_tick;
      if (elapsed < kBleTxMinIntervalTicks) {
        wait_ticks = kBleTxMinIntervalTicks - elapsed;
      } else {
        wait_ticks = 0;
      }
    }

    const uint32_t wait_ms = (wait_ticks == 0) ? 0u : (uint32_t)(wait_ticks * portTICK_PERIOD_MS);
    GamepadState queued_state = latest_usb_state;
    if (hid_device_manager_wait_for_next_state(&queued_state, wait_ms)) {
      latest_usb_state = queued_state;
      have_pending_state = true;

      while (hid_device_manager_wait_for_next_state(&queued_state, 0)) {
        latest_usb_state = queued_state;
      }
    }

    now = xTaskGetTickCount();
    const bool interval_elapsed = (last_tx_tick == 0) || ((now - last_tx_tick) >= kBleTxMinIntervalTicks);
    if (have_pending_state && interval_elapsed) {
      translate_usb_to_ble(&latest_usb_state, &ble_state);
      ble_gamepad_send_state(&ble_state);
      last_tx_tick = xTaskGetTickCount();
      have_pending_state = false;
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
    xTaskCreatePinnedToCore(ble_tx_task, "ble_tx", 4096, nullptr,
                            kBleTxTaskPriority, nullptr, kBleTaskCore);
  } else {
    xTaskCreatePinnedToCore(cfg_stream_task, "cfg_stream", 4096, nullptr,
                            kCfgStreamTaskPriority, nullptr, kBleTaskCore);
  }

  vTaskDelete(nullptr);
}
