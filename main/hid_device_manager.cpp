#include "hid_device_manager.h"
#include "input_decoder.h"
#include "hid_verbose.h"
#include "hid_device_identity.h"
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

static uint32_t fnv1a32_u8(const uint8_t *data, size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    h ^= data[i];
    h *= 16777619u;
  }
  return h;
}

static int find_ctx_by_handle_tag(uintptr_t tag) {
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (g_devices[i].active && g_devices[i].hid_handle_tag == tag) return i;
  }
  return -1;
}

static int find_ctx_by_usb_addr_iface(uint8_t addr, uint8_t iface) {
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (!g_devices[i].active) continue;
    if (g_devices[i].usb_addr == addr && g_devices[i].iface_num == iface) return i;
  }
  return -1;
}

static void refresh_identity(HidDeviceContext *ctx, const uint8_t *report_desc, size_t report_desc_len) {
  if (!ctx) return;
  hid_identity_init(&ctx->identity);
  ctx->identity.session_handle_tag = ctx->hid_handle_tag;
  ctx->identity.dev_addr = ctx->usb_addr;
  ctx->identity.iface_num = ctx->iface_num;
  ctx->identity.vid = ctx->vid;
  ctx->identity.pid = ctx->pid;
  if (report_desc && report_desc_len) {
    ctx->identity.report_desc_crc32 = fnv1a32_u8(report_desc, report_desc_len);
  }
  // manufacturer/product are best-effort; leave empty unless populated elsewhere
  hid_identity_refresh_hash(&ctx->identity);
}

static void print_device_caps(const HidDeviceContext *ctx) {
  ESP_LOGI(TAG,
           "HID device DEV[%u] addr=%u VID=0x%04X PID=0x%04X iface=%u role=%d fields=%u",
           (unsigned)ctx->slot_id, (unsigned)ctx->usb_addr, (unsigned)ctx->vid,
           (unsigned)ctx->pid, (unsigned)ctx->iface_num, (int)ctx->caps.role,
           (unsigned)ctx->caps.num_fields);

#if HID_VERBOSE_HID_DEBUG
  ESP_LOGI(TAG, "Descriptor snapshot DEV[%u] (parsed input elements)", (unsigned)ctx->slot_id);
  for (size_t i = 0; i < ctx->caps.num_fields; i++) {
    const auto &f = ctx->caps.fields[i];
    ESP_LOGI(TAG,
             "  IN elem: rpt_id=%u bit_ofs=%u bit_sz=%u usage_page=%04X usage=%04X log=[%d..%d] signed=%d",
             (unsigned)f.report_id, (unsigned)f.bit_offset, (unsigned)f.bit_size,
             (unsigned)f.usage_page, (unsigned)f.usage, (int)f.logical_min,
             (int)f.logical_max, (int)f.is_signed);
  }
#endif
}

static void
hid_host_interface_callback(hid_host_device_handle_t hid_device_handle,
                            const hid_host_interface_event_t event, void *arg) {
  (void)arg;

  if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
    size_t report_length = 0;
    uint8_t report_data[128];
    esp_err_t err = hid_host_device_get_raw_input_report_data(
        hid_device_handle, report_data, sizeof(report_data), &report_length);
    if (err != ESP_OK || report_length == 0) return;

    // Best-effort params to support a safe fallback match.
    hid_host_dev_params_t p = {};
    bool have_p = (hid_host_device_get_params(hid_device_handle, &p) == ESP_OK);

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);

    int ctx_idx = find_ctx_by_handle_tag((uintptr_t)hid_device_handle);
    if (ctx_idx < 0 && have_p) {
      ctx_idx = find_ctx_by_usb_addr_iface(p.addr, p.iface_num);
    }

    if (ctx_idx >= 0) {
      // Timestamp used by verbose paths (milliseconds)
      uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
      (void)now;  // avoid -Werror=unused-variable when verbose is disabled

#if HID_VERBOSE_HID_DEBUG
      // Track per-device report metadata/cadence
      uint8_t rid = 0;
      bool uses_report_ids = false;
      for (size_t i = 0; i < g_devices[ctx_idx].caps.num_fields; i++) {
        if (g_devices[ctx_idx].caps.fields[i].report_id > 0) {
          uses_report_ids = true;
          break;
        }
      }
      if (uses_report_ids && report_length > 0) {
        rid = report_data[0];
      }
      hid_verbose_note_report(&g_devices[ctx_idx], rid, report_length,
                              report_data, report_length, uses_report_ids);
#endif

      hid_decode_report(report_data, report_length, &g_devices[ctx_idx]);

#if HID_VERBOSE_HID_DEBUG
      // Per-device decoded values BEFORE merging
      if (hid_verbose_maybe_dump_device_state(&g_devices[ctx_idx])) {
        g_devices[ctx_idx].dbg_last_state_dump_ms = now;
      }
#endif

      hid_merge_states(g_devices, MAX_DEVICES, &g_merged_state);

#if HID_VERBOSE_HID_DEBUG
      // Collision diagnostics + winner device per axis in merged state
      hid_verbose_log_merge(g_devices, MAX_DEVICES, &g_merged_state);
#endif
    } else {
      ESP_LOGW(TAG, "INPUT_REPORT: no matching device slot for handle=%p", hid_device_handle);
    }

    xSemaphoreGive(g_state_mutex);
    return;
  }

  if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
    // Best-effort params to support safe fallback match.
    hid_host_dev_params_t p = {};
    bool have_p = (hid_host_device_get_params(hid_device_handle, &p) == ESP_OK);

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);

    int ctx_idx = find_ctx_by_handle_tag((uintptr_t)hid_device_handle);
    if (ctx_idx < 0 && have_p) {
      ctx_idx = find_ctx_by_usb_addr_iface(p.addr, p.iface_num);
    }

    if (ctx_idx >= 0) {
      char idbuf[196];
      hid_identity_to_string(&g_devices[ctx_idx].identity, idbuf, sizeof(idbuf));
      ESP_LOGI(TAG, "HID Device Disconnected slot=%d (%s)", ctx_idx, idbuf);

      g_devices[ctx_idx].active = false;
      // Clear the rest so stale fields cannot be matched.
      HidDeviceContext tmp = {};
      tmp.slot_id = ctx_idx;
      g_devices[ctx_idx] = tmp;

      hid_merge_states(g_devices, MAX_DEVICES, &g_merged_state);
    } else {
      ESP_LOGW(TAG, "DISCONNECTED: no matching device slot for handle=%p", hid_device_handle);
    }

    xSemaphoreGive(g_state_mutex);

    hid_host_device_close(hid_device_handle);
    return;
  }

  if (event == HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR) {
    ESP_LOGW(TAG, "HID transfer error for handle=%p", hid_device_handle);
    return;
  }
}

static void hid_init_device_task(void *arg) {
  hid_host_device_handle_t hid_device_handle = (hid_host_device_handle_t)arg;

  // Query best-effort HID/USB params
  hid_host_dev_params_t dev_params = {};
  (void)hid_host_device_get_params(hid_device_handle, &dev_params);

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

      // Stable-enough identification for Phase 0
      g_devices[slot].slot_id = (uint8_t)slot;
      g_devices[slot].usb_addr = dev_params.addr;
      g_devices[slot].iface_num = dev_params.iface_num;
      g_devices[slot].hid_handle_tag = (uintptr_t)hid_device_handle;

#if HID_VERBOSE_HID_DEBUG
      // Ensure verbose helpers are running
      hid_verbose_init();

      // Best-effort VID/PID lookup (may populate slightly after connect)
      uint16_t vid = 0, pid = 0;
      if (hid_verbose_get_vidpid(g_devices[slot].usb_addr, &vid, &pid)) {
        g_devices[slot].vid = vid;
        g_devices[slot].pid = pid;
      }
#endif

      // Canonical identity (stable-ish across reconnects where possible)
      refresh_identity(&g_devices[slot], desc, desc_len);

      hid_parse_report_descriptor(desc, desc_len, &g_devices[slot].caps);
      g_devices[slot].active = true;

      // FIX: do not use \" inside C++ string literals; build id string first
      char idbuf[196];
      hid_identity_to_string(&g_devices[slot].identity, idbuf, sizeof(idbuf));
      ESP_LOGI(TAG, "HID device initialized slot=%d (%s)", slot, idbuf);

      g_devices[slot].dev_addr = g_devices[slot].usb_addr;  // legacy field now tracks USB address

#if HID_VERBOSE_HID_DEBUG
      // Try once more for VID/PID now that cache may have been filled.
      uint16_t vid2 = 0, pid2 = 0;
      if (hid_verbose_get_vidpid(g_devices[slot].usb_addr, &vid2, &pid2)) {
        g_devices[slot].vid = vid2;
        g_devices[slot].pid = pid2;
      }
#endif

      // VID/PID may have changed; refresh identity hash
      refresh_identity(&g_devices[slot], desc, desc_len);

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
  (void)arg;
  if (event == HID_HOST_DRIVER_EVENT_CONNECTED) {
    ESP_LOGI(TAG, "HID Device Connected");
    xTaskCreate(hid_init_device_task, "hid_init_dev", 8192,
                (void *)hid_device_handle, 5, NULL);
  }
}

void hid_device_manager_init(void) {
  g_state_mutex = xSemaphoreCreateMutex();
  memset(g_devices, 0, sizeof(g_devices));

#if HID_VERBOSE_HID_DEBUG
  // Start debug-only helpers early (e.g., VID/PID cache)
  hid_verbose_init();
#endif

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