#include "hid_device_manager.h"
#include "input_decoder.h"
#include "input_elements.h"
#include "mapping_engine.h"
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
  ESP_LOGI(TAG, "Device registered. Role=%d Elements=%d", (int)ctx->caps.role,
           (int)ctx->caps.num_elements);
  for (size_t i = 0; i < ctx->caps.num_elements; i++) {
    const InputElement &e = ctx->caps.elements[i];
    const char *friendly = ie_friendly_usage(e.usage_page, e.usage);
    if (friendly && e.usage_page == 0x09) {
      // Buttons share the same friendly label; include the number
      ESP_LOGI(TAG,
               " E[%03d]: id=%08X kind=%s usage=%s#%u up=%04X u=%04X rid=%u "
               "bit_ofs=%u bit_sz=%u lmin=%d lmax=%d signed=%d rel=%d var=%d",
               (int)i, (unsigned)e.element_id, ie_kind_str(e.kind), friendly,
               (unsigned)e.usage, e.usage_page, e.usage, (unsigned)e.report_id,
               (unsigned)e.bit_offset, (unsigned)e.bit_size, (int)e.logical_min,
               (int)e.logical_max, (int)e.is_signed, (int)e.is_relative,
               (int)e.is_variable);
    } else if (friendly) {
      ESP_LOGI(TAG,
               " E[%03d]: id=%08X kind=%s usage=%s up=%04X u=%04X rid=%u "
               "bit_ofs=%u bit_sz=%u lmin=%d lmax=%d signed=%d rel=%d var=%d",
               (int)i, (unsigned)e.element_id, ie_kind_str(e.kind), friendly,
               e.usage_page, e.usage, (unsigned)e.report_id,
               (unsigned)e.bit_offset, (unsigned)e.bit_size, (int)e.logical_min,
               (int)e.logical_max, (int)e.is_signed, (int)e.is_relative,
               (int)e.is_variable);
    } else {
      ESP_LOGI(TAG,
               " E[%03d]: id=%08X kind=%s up=%04X u=%04X rid=%u bit_ofs=%u "
               "bit_sz=%u lmin=%d lmax=%d signed=%d rel=%d var=%d",
               (int)i, (unsigned)e.element_id, ie_kind_str(e.kind),
               e.usage_page, e.usage, (unsigned)e.report_id,
               (unsigned)e.bit_offset, (unsigned)e.bit_size, (int)e.logical_min,
               (int)e.logical_max, (int)e.is_signed, (int)e.is_relative,
               (int)e.is_variable);
    }
  }
}

// Optional: periodically log element changes so you can visually confirm
// *all* physical controls are producing updates (including unknown usages).
static void log_element_samples(HidDeviceContext *ctx) {
  if (!ctx) return;
  uint32_t now = ctx->last_report_ms;
  if (now == 0) return;

  // Rate limit per device to avoid log spam.
  const uint32_t kMinIntervalMs = 500;
  if (ctx->last_sample_log_ms != 0 && (now - ctx->last_sample_log_ms) < kMinIntervalMs)
    return;

  int printed = 0;
  const int kMaxPrint = 12;
  for (size_t i = 0; i < ctx->caps.num_elements && printed < kMaxPrint; i++) {
    const InputElement &e = ctx->caps.elements[i];
    // Print elements updated since the last sample window
    if (e.last_update_ms == 0) continue;
    if (ctx->last_sample_log_ms != 0 && e.last_update_ms <= ctx->last_sample_log_ms)
      continue;

    const char *friendly = ie_friendly_usage(e.usage_page, e.usage);
    if (friendly && e.usage_page == 0x09) {
      ESP_LOGI(TAG,
               "Sample: dev=%d elem[%03d] %s#%u raw=%d n01=%.3f n11=%.3f",
               (int)ctx->dev_addr, (int)i, friendly, (unsigned)e.usage, (int)e.raw,
               (double)e.norm_0_1, (double)e.norm_m1_1);
    } else if (friendly) {
      ESP_LOGI(TAG,
               "Sample: dev=%d elem[%03d] %s raw=%d n01=%.3f n11=%.3f",
               (int)ctx->dev_addr, (int)i, friendly, (int)e.raw, (double)e.norm_0_1,
               (double)e.norm_m1_1);
    } else {
      ESP_LOGI(TAG,
               "Sample: dev=%d elem[%03d] up=%04X u=%04X raw=%d n01=%.3f n11=%.3f",
               (int)ctx->dev_addr, (int)i, e.usage_page, e.usage, (int)e.raw,
               (double)e.norm_0_1, (double)e.norm_m1_1);
    }
    printed++;
  }

  // Only advance the window if we printed at least one update.
  if (printed > 0) ctx->last_sample_log_ms = now;
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
      log_element_samples(&g_devices[ctx_idx]);
      // Deterministic mapping (replaces the legacy per-axis max-abs merge).
      mapping::mapping_engine_compute(g_devices, MAX_DEVICES, &g_merged_state);
      xSemaphoreGive(g_state_mutex);
    }
  } else if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
    ESP_LOGI(TAG, "Interface Disconnected");
    if (ctx_idx >= 0) {
      xSemaphoreTake(g_state_mutex, portMAX_DELAY);
      g_devices[ctx_idx].active = false;
      // caps.elements is a fixed-size array; memset(caps) already clears all element values.
      memset(&g_devices[ctx_idx].caps, 0, sizeof(g_devices[ctx_idx].caps));
      memset(&g_devices[ctx_idx].state, 0, sizeof(g_devices[ctx_idx].state));
      g_devices[ctx_idx].report_desc_len = 0;

      mapping::mapping_engine_notify_devices_changed();
      mapping::mapping_engine_compute(g_devices, MAX_DEVICES, &g_merged_state);
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

      // Cache raw report descriptor for WebBLE inspection.
      const size_t max_desc = HidDeviceContext::MAX_HID_REPORT_DESC_LEN;
      g_devices[slot].report_desc_len = (uint16_t)((desc_len > max_desc) ? max_desc : desc_len);
      if (g_devices[slot].report_desc_len > 0) {
        memcpy(g_devices[slot].report_desc, desc, g_devices[slot].report_desc_len);
      }

      hid_parse_report_descriptor(desc, desc_len, &g_devices[slot].caps);
      g_devices[slot].active = true;
      g_devices[slot].dev_addr = ((uintptr_t)hid_device_handle & 0xFF);

      print_device_caps(&g_devices[slot]);

      // New device set -> rebuild deterministic mapping.
      mapping::mapping_engine_notify_devices_changed();
      mapping::mapping_engine_compute(g_devices, MAX_DEVICES, &g_merged_state);
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
  memset(&g_merged_state, 0, sizeof(g_merged_state));
  mapping::mapping_engine_init();

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

// -----------------------------------------------------------------------------
// WebBLE / Config Introspection
// -----------------------------------------------------------------------------

static inline uint32_t make_device_id_from_ctx(const HidDeviceContext &ctx) {
  // Reserve 0 as invalid.
  return (uint32_t)ctx.dev_addr + 1;
}

size_t hid_device_manager_list_devices(HidDeviceInfo *out_infos, size_t max_infos) {
  if (!out_infos || max_infos == 0 || !g_state_mutex) return 0;
  size_t n = 0;
  xSemaphoreTake(g_state_mutex, portMAX_DELAY);
  for (int i = 0; i < MAX_DEVICES && n < max_infos; i++) {
    if (!g_devices[i].active) continue;
    out_infos[n].device_id = make_device_id_from_ctx(g_devices[i]);
    out_infos[n].dev_addr = g_devices[i].dev_addr;
    out_infos[n].role = (uint8_t)g_devices[i].caps.role;
    out_infos[n].num_elements = (uint16_t)g_devices[i].caps.num_elements;
    out_infos[n].report_desc_len = g_devices[i].report_desc_len;
    n++;
  }
  xSemaphoreGive(g_state_mutex);
  return n;
}

static HidDeviceContext *find_device_by_id(uint32_t device_id) {
  if (device_id == 0) return nullptr;
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (!g_devices[i].active) continue;
    if (make_device_id_from_ctx(g_devices[i]) == device_id) return &g_devices[i];
  }
  return nullptr;
}

size_t hid_device_manager_get_report_descriptor(uint32_t device_id, uint8_t *out_buf, size_t max_len) {
  if (!out_buf || max_len == 0 || !g_state_mutex) return 0;
  size_t copied = 0;
  xSemaphoreTake(g_state_mutex, portMAX_DELAY);
  HidDeviceContext *ctx = find_device_by_id(device_id);
  if (ctx && ctx->report_desc_len > 0) {
    copied = (ctx->report_desc_len > max_len) ? max_len : (size_t)ctx->report_desc_len;
    memcpy(out_buf, ctx->report_desc, copied);
  }
  xSemaphoreGive(g_state_mutex);
  return copied;
}

size_t hid_device_manager_get_elements(uint32_t device_id, InputElement *out_elems, size_t max_elems) {
  if (!out_elems || max_elems == 0 || !g_state_mutex) return 0;
  size_t copied = 0;
  xSemaphoreTake(g_state_mutex, portMAX_DELAY);
  HidDeviceContext *ctx = find_device_by_id(device_id);
  if (ctx && ctx->caps.num_elements > 0) {
    copied = (ctx->caps.num_elements > max_elems) ? max_elems : ctx->caps.num_elements;
    memcpy(out_elems, ctx->caps.elements, copied * sizeof(InputElement));
  }
  xSemaphoreGive(g_state_mutex);
  return copied;
}

bool hid_device_manager_get_device_state(uint32_t device_id, struct GamepadState *out_state) {
  if (!out_state || !g_state_mutex) return false;
  bool ok = false;
  xSemaphoreTake(g_state_mutex, portMAX_DELAY);
  HidDeviceContext *ctx = find_device_by_id(device_id);
  if (ctx) {
    memcpy(out_state, &ctx->state, sizeof(GamepadState));
    ok = true;
  }
  xSemaphoreGive(g_state_mutex);
  return ok;
}
