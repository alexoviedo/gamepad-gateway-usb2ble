#include "hid_device_manager.h"
#include "input_decoder.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>
#include <usb/hid_host.h>
#include <usb/usb_host.h>

static const char *TAG = "HID_MGR";

#define MAX_DEVICES 8
static HidDeviceContext g_devices[MAX_DEVICES];
static SemaphoreHandle_t g_state_mutex = NULL;
static GamepadState g_merged_state = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

static void print_device_caps(const HidDeviceContext *ctx) {
  ESP_LOGI(TAG, "Device registered with Role: %d, Fields: %d",
           (int)ctx->caps.role, (int)ctx->caps.num_fields);
  for (size_t i = 0; i < ctx->caps.num_fields; i++) {
    const auto &f = ctx->caps.fields[i];
    ESP_LOGI(TAG,
             " F: ID=%d, bit_ofs=%u, bit_sz=%u, usage_page=%04X, usage=%04X, "
             "min=%d, max=%d",
             f.report_id, (unsigned)f.bit_offset, (unsigned)f.bit_size,
             f.usage_page, f.usage, (int)f.logical_min, (int)f.logical_max);
  }
}

static void
hid_host_interface_callback(hid_host_device_handle_t hid_device_handle,
                            const hid_host_interface_event_t event, void *arg) {
  // Find context
  int ctx_idx = -1;
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (g_devices[i].active &&
        g_devices[i].dev_addr == ((uintptr_t)hid_device_handle & 0xFF)) {
      ctx_idx = i;
      break;
    }
  }

  if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
    size_t report_length = 0;
    uint8_t report_data[128];
    esp_err_t err = hid_host_device_get_raw_input_report_data(
        hid_device_handle, report_data, sizeof(report_data), &report_length);
    if (err == ESP_OK && ctx_idx >= 0) {
      xSemaphoreTake(g_state_mutex, portMAX_DELAY);
      hid_decode_report(report_data, report_length, &g_devices[ctx_idx]);
      hid_merge_states(g_devices, MAX_DEVICES, &g_merged_state);
      xSemaphoreGive(g_state_mutex);
    }
  } else if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
    ESP_LOGI(TAG, "Interface Disconnected");
    if (ctx_idx >= 0) {
      xSemaphoreTake(g_state_mutex, portMAX_DELAY);
      g_devices[ctx_idx].active = false;
      hid_merge_states(g_devices, MAX_DEVICES, &g_merged_state);
      xSemaphoreGive(g_state_mutex);
    }
    hid_host_device_close(hid_device_handle);
  } else if (event == HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR) {
    ESP_LOGI(TAG, "Transfer Error");
  }
}

static void hid_init_device_task(void *arg) {
  hid_host_device_handle_t hid_device_handle = (hid_host_device_handle_t)arg;

  hid_host_device_config_t dev_config = {
      .callback = hid_host_interface_callback, .callback_arg = NULL};
  esp_err_t err = hid_host_device_open(hid_device_handle, &dev_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open HID device");
    vTaskDelete(NULL);
    return;
  }

  // Some devices behind USB Hubs need extra time to process control requests
  // after enumeration
  vTaskDelay(pdMS_TO_TICKS(200));

  // Fetch descriptor
  size_t desc_len = 0;
  uint8_t *desc = hid_host_get_report_descriptor(hid_device_handle, &desc_len);
  if (desc) {
    ESP_LOGI(TAG, "Got Report Descriptor of length %d", (int)desc_len);

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < MAX_DEVICES; i++) {
      if (!g_devices[i].active) {
        slot = i;
        break;
      }
    }
    if (slot >= 0) {
      // Parse it!
      memset(&g_devices[slot], 0, sizeof(HidDeviceContext));
      hid_parse_report_descriptor(desc, desc_len, &g_devices[slot].caps);
      g_devices[slot].active = true;
      g_devices[slot].dev_addr = ((uintptr_t)hid_device_handle & 0xFF);

      print_device_caps(&g_devices[slot]);
    }
    xSemaphoreGive(g_state_mutex);
  }

  // Now that the device is fully parsed and activated, let's start pulling
  // reports!
  hid_host_device_start(hid_device_handle);

  vTaskDelete(NULL);
}

static void hid_host_driver_event_cb(hid_host_device_handle_t hid_device_handle,
                                     const hid_host_driver_event_t event,
                                     void *arg) {
  if (event == HID_HOST_DRIVER_EVENT_CONNECTED) {
    ESP_LOGI(TAG, "HID Device Connected");
    xTaskCreate(hid_init_device_task, "hid_init_dev", 8192,
                (void *)hid_device_handle, 5, NULL);
  }
}

void hid_device_manager_init(void) {
  g_state_mutex = xSemaphoreCreateMutex();
  memset(g_devices, 0, sizeof(g_devices));

  hid_host_driver_config_t driver_config = {.create_background_task = true,
                                            .task_priority = 5,
                                            .stack_size = 8192,
                                            .core_id = tskNO_AFFINITY,
                                            .callback =
                                                hid_host_driver_event_cb,
                                            .callback_arg = NULL};
  esp_err_t err = hid_host_install(&driver_config);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "HID Class Driver installed");
  } else {
    ESP_LOGE(TAG, "Failed to install HID Class Driver: %s",
             esp_err_to_name(err));
  }
}

void hid_device_manager_get_merged_state(struct GamepadState *out_state) {
  if (g_state_mutex) {
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    memcpy(out_state, &g_merged_state, sizeof(GamepadState));
    xSemaphoreGive(g_state_mutex);
  }
}
