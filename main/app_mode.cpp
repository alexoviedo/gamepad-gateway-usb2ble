#include "app_mode.h"

#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "APP_MODE";

static constexpr const char *kNvsNamespace = "gg";
static constexpr const char *kKeyBootMode = "boot_mode";

static app_mode_t g_mode = APP_MODE_RUN;

const char *app_mode_name(app_mode_t mode) {
  switch (mode) {
    case APP_MODE_RUN: return "RUN";
    case APP_MODE_CONFIG: return "CONFIG";
    default: return "?";
  }
}

void app_mode_init(void) {
  // NVS must already be initialized by app_main.
  nvs_handle_t h;
  esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &h);
  if (err != ESP_OK) {
    // Namespace missing is normal on first boot.
    // Default to CONFIG so a WebBLE client can connect and select RUN explicitly.
    g_mode = APP_MODE_CONFIG;
    ESP_LOGI(TAG, "No NVS namespace yet; defaulting mode=%s", app_mode_name(g_mode));
    return;
  }

  uint8_t v = 0;
  err = nvs_get_u8(h, kKeyBootMode, &v);
  nvs_close(h);

  if (err == ESP_OK && (v == (uint8_t)APP_MODE_RUN || v == (uint8_t)APP_MODE_CONFIG)) {
    g_mode = (app_mode_t)v;
  } else {
    g_mode = APP_MODE_RUN;
  }

  ESP_LOGI(TAG, "Boot mode=%s", app_mode_name(g_mode));
}

app_mode_t app_mode_current(void) { return g_mode; }

void app_mode_set_boot_mode(app_mode_t mode) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
    return;
  }

  err = nvs_set_u8(h, kKeyBootMode, (uint8_t)mode);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nvs_set_u8 failed: %s", esp_err_to_name(err));
    nvs_close(h);
    return;
  }

  err = nvs_commit(h);
  nvs_close(h);

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
    return;
  }

  ESP_LOGI(TAG, "Persisted next boot mode=%s", app_mode_name(mode));
}

void app_mode_reboot_to(app_mode_t mode) {
  ESP_LOGI(TAG, "Rebooting to mode=%s", app_mode_name(mode));
  app_mode_set_boot_mode(mode);
  // Small delay to flush logs.
  vTaskDelay(pdMS_TO_TICKS(50));
  esp_restart();
}