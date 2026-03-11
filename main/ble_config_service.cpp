// main/ble_config_service.cpp
#include "ble_config_service.h"

#include "app_mode.h"
#include "hid_device_manager.h"
#include "input_decoder.h"
#include "input_elements.h"
#include "mapping_engine.h"
#include "nvs_profile_store.h"

#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <math.h>
#include <string>
#include <string.h>

// NimBLE headers are C; wrap them for C++.
extern "C" {
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/ble_uuid.h>
#include <host/util/util.h>
#include <os/os_mbuf.h>
}

#include <cJSON.h>

static const char *TAG = "BLE_CFG";

// -----------------------------------------------------------------------------
// UUIDs
// -----------------------------------------------------------------------------
// Service UUID (canonical): 6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a1
// Characteristics are the same base with last byte incremented.
//
// NOTE: BLE UUIDs are little-endian in the underlying bytes.

static const ble_uuid128_t UUID_SVC_CFG = BLE_UUID128_INIT(
    0xa1, 0xc1, 0xb2, 0xf5, 0xd8, 0x4f, 0xe0, 0xb1,
    0x3f, 0x4a, 0x1c, 0x9c, 0x90, 0x2d, 0x1a, 0x6b);

static const ble_uuid128_t UUID_CHR_CMD = BLE_UUID128_INIT(
    0xa2, 0xc1, 0xb2, 0xf5, 0xd8, 0x4f, 0xe0, 0xb1,
    0x3f, 0x4a, 0x1c, 0x9c, 0x90, 0x2d, 0x1a, 0x6b);

static const ble_uuid128_t UUID_CHR_EVT = BLE_UUID128_INIT(
    0xa3, 0xc1, 0xb2, 0xf5, 0xd8, 0x4f, 0xe0, 0xb1,
    0x3f, 0x4a, 0x1c, 0x9c, 0x90, 0x2d, 0x1a, 0x6b);

static const ble_uuid128_t UUID_CHR_STREAM = BLE_UUID128_INIT(
    0xa4, 0xc1, 0xb2, 0xf5, 0xd8, 0x4f, 0xe0, 0xb1,
    0x3f, 0x4a, 0x1c, 0x9c, 0x90, 0x2d, 0x1a, 0x6b);

static const ble_uuid128_t UUID_CHR_CFG = BLE_UUID128_INIT(
    0xa5, 0xc1, 0xb2, 0xf5, 0xd8, 0x4f, 0xe0, 0xb1,
    0x3f, 0x4a, 0x1c, 0x9c, 0x90, 0x2d, 0x1a, 0x6b);

// -----------------------------------------------------------------------------
// GATT state
// -----------------------------------------------------------------------------

static uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;

static uint16_t g_cmd_handle = 0;
static uint16_t g_evt_handle = 0;
static uint16_t g_stream_handle = 0;
static uint16_t g_cfg_handle = 0;

static bool g_evt_notify_enabled = false;
static bool g_stream_notify_enabled = false;

static bool g_stream_active = false;
static uint32_t g_stream_rr = 0;
static uint16_t g_stream_elem_cursor = 0;

static uint16_t g_msg_id = 0;

// Pace chunked EVT notifications so we do not exhaust NimBLE mbufs when
// sending larger JSON payloads like descriptors or config.
static SemaphoreHandle_t g_evt_tx_sem = nullptr;

static std::string g_config_json = "{}";
static constexpr size_t kMaxConfigJsonBytes = 4096;

// Cache the most recent STREAM sample so the characteristic can be READ.
// Some NimBLE builds reject NOTIFY-only characteristics without an access_cb.
static bool g_last_stream_valid = false;
static constexpr size_t kStreamSampleLen = 16; // sizeof(StreamSample) when packed
static uint8_t g_last_stream_sample_buf[kStreamSampleLen];

// -----------------------------------------------------------------------------
// Small chunked notify framing (works even at MTU=23)
// -----------------------------------------------------------------------------
// EVT notifications are framed as:
//   u8  version (1)
//   u8  type    (1=json, 2=descriptor, 3=config)
//   u16 msg_id
//   u16 offset
//   u16 total_len
//   u8  payload[0..(MTU-3-8)]
static void notify_evt_chunked(uint8_t type, const uint8_t *data, uint16_t total_len) {
  if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
  if (!g_evt_notify_enabled) return;
  if (g_evt_handle == 0) return;

  const uint16_t msg_id = ++g_msg_id;

  constexpr size_t kHdrLen = 8;
  constexpr size_t kMaxValueCap = 244;
  uint16_t mtu = ble_att_mtu(g_conn_handle);
  size_t max_value_len = (mtu > 3) ? (size_t)(mtu - 3) : 20;
  if (max_value_len > kMaxValueCap) max_value_len = kMaxValueCap;
  if (max_value_len < kHdrLen + 1) max_value_len = kHdrLen + 1;
  const size_t max_payload = max_value_len - kHdrLen;

  uint16_t offset = 0;
  while (offset < total_len) {
    if (g_evt_tx_sem) {
      if (xSemaphoreTake(g_evt_tx_sem, pdMS_TO_TICKS(750)) != pdTRUE) {
        ESP_LOGW(TAG, "EVT pacing timeout msg=%u off=%u/%u", (unsigned)msg_id,
                 (unsigned)offset, (unsigned)total_len);
        return;
      }
    }

    uint8_t buf[kMaxValueCap];
    const uint16_t remaining = (uint16_t)(total_len - offset);
    const uint16_t chunk_len = (remaining > max_payload) ? (uint16_t)max_payload : remaining;

    buf[0] = 1;
    buf[1] = type;
    buf[2] = (uint8_t)(msg_id & 0xFF);
    buf[3] = (uint8_t)(msg_id >> 8);
    buf[4] = (uint8_t)(offset & 0xFF);
    buf[5] = (uint8_t)(offset >> 8);
    buf[6] = (uint8_t)(total_len & 0xFF);
    buf[7] = (uint8_t)(total_len >> 8);

    if (chunk_len > 0) {
      memcpy(&buf[kHdrLen], data + offset, chunk_len);
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, kHdrLen + chunk_len);
    if (!om) {
      ESP_LOGW(TAG, "EVT mbuf alloc failed at off=%u/%u", (unsigned)offset, (unsigned)total_len);
      if (g_evt_tx_sem) xSemaphoreGive(g_evt_tx_sem);
      return;
    }

    int rc = ble_gatts_notify_custom(g_conn_handle, g_evt_handle, om);
    if (rc != 0) {
      ESP_LOGW(TAG, "EVT notify failed rc=%d msg=%u off=%u/%u", rc, (unsigned)msg_id,
               (unsigned)offset, (unsigned)total_len);
      if (g_evt_tx_sem) xSemaphoreGive(g_evt_tx_sem);
      return;
    }

    offset = (uint16_t)(offset + chunk_len);
  }

  ESP_LOGI(TAG, "EVT send complete msg=%u len=%u type=%u", (unsigned)msg_id,
           (unsigned)total_len, (unsigned)type);
}

static void notify_evt_json(const char *json) {
  if (!json) return;
  const size_t len = strlen(json);
  if (len == 0 || len > 0xFFFF) return;
  notify_evt_chunked(1, (const uint8_t *)json, (uint16_t)len);
}

// -----------------------------------------------------------------------------
// Command router
// -----------------------------------------------------------------------------

static const char *role_to_str(uint8_t role) {
  switch ((DeviceRole)role) {
    case DeviceRole::STICK: return "stick";
    case DeviceRole::THROTTLE: return "throttle";
    case DeviceRole::PEDALS: return "pedals";
    default: return "unknown";
  }
}

static const char *element_kind_code(InputElementKind kind) {
  switch (kind) {
    case InputElementKind::BUTTON: return "b";
    case InputElementKind::AXIS: return "a";
    case InputElementKind::HAT: return "h";
    default: return "o";
  }
}

static void append_element_metadata(cJSON *arr, const InputElement &e) {
  cJSON *obj = cJSON_CreateObject();
  cJSON_AddNumberToObject(obj, "i", (double)e.element_id);
  cJSON_AddStringToObject(obj, "k", element_kind_code(e.kind));
  cJSON_AddNumberToObject(obj, "p", (double)e.usage_page);
  cJSON_AddNumberToObject(obj, "u", (double)e.usage);
  const char *friendly = ie_friendly_usage(e.usage_page, e.usage);
  cJSON_AddStringToObject(obj, "n", friendly ? friendly : "unknown");
  cJSON_AddItemToArray(arr, obj);
}

static void cmd_get_devices(cJSON *resp) {
  HidDeviceInfo infos[8];
  const size_t n = hid_device_manager_list_devices(infos, 8);

  cJSON *arr = cJSON_AddArrayToObject(resp, "devices");
  for (size_t i = 0; i < n; i++) {
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "device_id", (double)infos[i].device_id);
    cJSON_AddNumberToObject(d, "dev_addr", (double)infos[i].dev_addr);
    cJSON_AddStringToObject(d, "role", role_to_str(infos[i].role));
    cJSON_AddNumberToObject(d, "num_elements", (double)infos[i].num_elements);
    cJSON_AddNumberToObject(d, "report_desc_len", (double)infos[i].report_desc_len);
    cJSON_AddItemToArray(arr, d);
  }
}

static void cmd_get_descriptor(cJSON *req, cJSON *resp) {
  cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "device_id");
  if (!cJSON_IsNumber(id)) {
    cJSON_AddStringToObject(resp, "error", "device_id required");
    return;
  }

  const uint32_t device_id = (uint32_t)id->valuedouble;
  uint8_t tmp[HidDeviceContext::MAX_HID_REPORT_DESC_LEN];
  const size_t got = hid_device_manager_get_report_descriptor(device_id, tmp, sizeof(tmp));

  cJSON_AddNumberToObject(resp, "device_id", (double)device_id);
  cJSON_AddNumberToObject(resp, "descriptor_len", (double)got);

  if (got > 0 && got <= 0xFFFF) {
    notify_evt_chunked(2, tmp, (uint16_t)got);
  }
}

static void cmd_get_elements(cJSON *req, cJSON *resp) {
  cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "device_id");
  if (!cJSON_IsNumber(id)) {
    cJSON_AddStringToObject(resp, "error", "device_id required");
    return;
  }

  const uint32_t device_id = (uint32_t)id->valuedouble;
  InputElement elems[MAX_INPUT_ELEMENTS];
  const size_t elem_count = hid_device_manager_get_elements(device_id, elems, MAX_INPUT_ELEMENTS);

  cJSON_AddNumberToObject(resp, "device_id", (double)device_id);
  cJSON *arr = cJSON_AddArrayToObject(resp, "elements");
  for (size_t i = 0; i < elem_count; i++) {
    append_element_metadata(arr, elems[i]);
  }
}

static void cmd_start_stream(cJSON *resp) {
  g_stream_active = true;
  cJSON_AddBoolToObject(resp, "streaming", true);
}

static void cmd_stop_stream(cJSON *resp) {
  g_stream_active = false;
  cJSON_AddBoolToObject(resp, "streaming", false);
}

static void cmd_get_config(cJSON *resp) {
  g_config_json = mapping::mapping_engine_profile_to_json();
  cJSON *cfg = cJSON_Parse(g_config_json.c_str());
  if (cfg) {
    cJSON_AddItemToObject(resp, "config", cfg);
  } else {
    cJSON_AddStringToObject(resp, "config_json", g_config_json.c_str());
  }
}

static void cmd_set_config(cJSON *req, cJSON *resp) {
  cJSON *cfg = cJSON_GetObjectItemCaseSensitive(req, "config");
  if (!cfg) {
    cJSON_AddStringToObject(resp, "error", "config required");
    return;
  }

  char *printed = cJSON_PrintUnformatted(cfg);
  if (!printed) {
    cJSON_AddStringToObject(resp, "error", "failed to serialize config");
    return;
  }

  const size_t len = strlen(printed);
  if (len > kMaxConfigJsonBytes) {
    cJSON_free(printed);
    cJSON_AddStringToObject(resp, "error", "config too large");
    return;
  }

  std::string error;
  if (!mapping::mapping_engine_apply_profile_json(printed, len, &error)) {
    cJSON_free(printed);
    cJSON_AddStringToObject(resp, "error", error.empty() ? "invalid config" : error.c_str());
    return;
  }
  cJSON_free(printed);

  hid_device_manager_recompute_mapping();
  g_config_json = mapping::mapping_engine_profile_to_json();

  cJSON_AddBoolToObject(resp, "ok", true);
  cJSON *normalized = cJSON_Parse(g_config_json.c_str());
  if (normalized) cJSON_AddItemToObject(resp, "config", normalized);
}

static void cmd_save_profile(cJSON *resp) {
  g_config_json = mapping::mapping_engine_profile_to_json();
  std::string error;
  if (!nvs_profile_store::save_json(g_config_json.c_str(), g_config_json.size(), &error)) {
    cJSON_AddBoolToObject(resp, "ok", false);
    cJSON_AddStringToObject(resp, "error", error.empty() ? "failed to save profile" : error.c_str());
    return;
  }

  cJSON_AddBoolToObject(resp, "ok", true);
  cJSON_AddStringToObject(resp, "note", "profile saved to NVS");
  cJSON_AddNumberToObject(resp, "bytes", (double)g_config_json.size());
  cJSON *cfg = cJSON_Parse(g_config_json.c_str());
  if (cfg) cJSON_AddItemToObject(resp, "config", cfg);
}

static void schedule_reboot(app_mode_t mode) {
  xTaskCreate(
      [](void *arg) {
        const app_mode_t m = (app_mode_t)(uintptr_t)arg;
        vTaskDelay(pdMS_TO_TICKS(150));
        app_mode_reboot_to(m);
      },
      "reboot", 3072, (void *)(uintptr_t)mode, 5, nullptr);
}

static void cmd_reboot_to(app_mode_t mode, cJSON *resp) {
  cJSON_AddStringToObject(resp, "rebooting_to", app_mode_name(mode));
  cJSON_AddBoolToObject(resp, "ok", true);
  schedule_reboot(mode);
}

static void handle_cmd_json(const char *json, size_t len) {
  cJSON *req = cJSON_ParseWithLength(json, len);
  cJSON *resp = cJSON_CreateObject();

  cJSON_AddStringToObject(resp, "evt", "resp");

  if (!req) {
    cJSON_AddStringToObject(resp, "error", "invalid_json");
  } else {
    cJSON *rid = cJSON_GetObjectItemCaseSensitive(req, "rid");
    if (cJSON_IsNumber(rid)) {
      cJSON_AddNumberToObject(resp, "rid", rid->valuedouble);
    }

    cJSON *cmd = cJSON_GetObjectItemCaseSensitive(req, "cmd");
    if (!cJSON_IsString(cmd) || !cmd->valuestring) {
      cJSON_AddStringToObject(resp, "error", "cmd required");
    } else {
      const char *c = cmd->valuestring;
      cJSON_AddStringToObject(resp, "cmd", c);

      if (strcmp(c, "get_devices") == 0) {
        cmd_get_devices(resp);
      } else if (strcmp(c, "get_descriptor") == 0) {
        cmd_get_descriptor(req, resp);
      } else if (strcmp(c, "get_elements") == 0) {
        cmd_get_elements(req, resp);
      } else if (strcmp(c, "start_stream") == 0) {
        cmd_start_stream(resp);
      } else if (strcmp(c, "stop_stream") == 0) {
        cmd_stop_stream(resp);
      } else if (strcmp(c, "get_config") == 0) {
        cmd_get_config(resp);
      } else if (strcmp(c, "set_config") == 0) {
        cmd_set_config(req, resp);
      } else if (strcmp(c, "save_profile") == 0) {
        cmd_save_profile(resp);
      } else if (strcmp(c, "reboot_to_run") == 0) {
        cmd_reboot_to(APP_MODE_RUN, resp);
      } else if (strcmp(c, "reboot_to_config") == 0) {
        cmd_reboot_to(APP_MODE_CONFIG, resp);
      } else {
        cJSON_AddStringToObject(resp, "error", "unknown_cmd");
      }
    }
  }

  char *out = cJSON_PrintUnformatted(resp);
  if (out) {
    notify_evt_json(out);
    cJSON_free(out);
  }

  if (req) cJSON_Delete(req);
  cJSON_Delete(resp);
}

// -----------------------------------------------------------------------------
// GATT access
// -----------------------------------------------------------------------------

static int gatt_read_bytes(struct ble_gatt_access_ctxt *ctxt, const void *data, size_t len) {
  if (ctxt->offset > len) {
    return BLE_ATT_ERR_INVALID_OFFSET;
  }

  const uint8_t *ptr = static_cast<const uint8_t *>(data);
  const size_t remaining = len - ctxt->offset;
  int rc = os_mbuf_append(ctxt->om, ptr + ctxt->offset, remaining);
  return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int gatt_access_cfg(uint16_t, uint16_t, struct ble_gatt_access_ctxt *ctxt, void *) {
  // CMD
  if (ble_uuid_cmp(ctxt->chr->uuid, (const ble_uuid_t *)&UUID_CHR_CMD) == 0) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;

    constexpr size_t kMaxCmd = 1024;
    char buf[kMaxCmd + 1];

    const size_t om_len = OS_MBUF_PKTLEN(ctxt->om);
    if (om_len == 0 || om_len > kMaxCmd) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, om_len, nullptr);
    if (rc != 0) return BLE_ATT_ERR_UNLIKELY;
    buf[om_len] = '\0';

    ESP_LOGI(TAG, "CMD write (%u bytes)", (unsigned)om_len);
    handle_cmd_json(buf, om_len);
    return 0;
  }

  // CFG (read/write)
  if (ble_uuid_cmp(ctxt->chr->uuid, (const ble_uuid_t *)&UUID_CHR_CFG) == 0) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
      g_config_json = mapping::mapping_engine_profile_to_json();
      return gatt_read_bytes(ctxt, g_config_json.data(), g_config_json.size());
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
      const uint16_t off = ctxt->offset;
      const size_t incoming = OS_MBUF_PKTLEN(ctxt->om);
      if (incoming == 0) return 0;

      if (off == 0) {
        g_config_json.clear();
      } else if (off != g_config_json.size()) {
        return BLE_ATT_ERR_INVALID_OFFSET;
      }

      if (g_config_json.size() + incoming > kMaxConfigJsonBytes) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
      }

      std::string tmp;
      tmp.resize(incoming);
      int rc = ble_hs_mbuf_to_flat(ctxt->om, tmp.data(), incoming, nullptr);
      if (rc != 0) return BLE_ATT_ERR_UNLIKELY;

      g_config_json.append(tmp);

      std::string error;
      if (mapping::mapping_engine_apply_profile_json(g_config_json.c_str(), g_config_json.size(), &error)) {
        hid_device_manager_recompute_mapping();
        g_config_json = mapping::mapping_engine_profile_to_json();
      }
      return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
  }

  return BLE_ATT_ERR_UNLIKELY;
}

// Access handler for EVT / STREAM characteristics.
// We keep these characteristics readable to satisfy some NimBLE validation rules
// (and to make debugging easier from WebBLE).
static int gatt_access_evt_stream(uint16_t, uint16_t, struct ble_gatt_access_ctxt *ctxt, void *) {
  // EVT (read returns empty; notifications carry framed messages).
  if (ble_uuid_cmp(ctxt->chr->uuid, (const ble_uuid_t *)&UUID_CHR_EVT) == 0) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
      return 0; // 0-length read
    }
    return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
  }

  // STREAM (read returns the last sample emitted).
  if (ble_uuid_cmp(ctxt->chr->uuid, (const ble_uuid_t *)&UUID_CHR_STREAM) == 0) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
      if (!g_last_stream_valid) return 0;
      return gatt_read_bytes(ctxt, g_last_stream_sample_buf, kStreamSampleLen);
    }
    return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
  }

  return BLE_ATT_ERR_UNLIKELY;
}

// -----------------------------------------------------------------------------
// GATT DB objects (static storage)
// -----------------------------------------------------------------------------
// Must be stable storage + null-terminated.
static ble_gatt_chr_def g_cfg_chrs[5]; // 4 + terminator
static ble_gatt_svc_def g_cfg_svcs[2]; // 1 + terminator

static const struct ble_gatt_svc_def *build_cfg_svcs_once() {
  static bool built = false;
  if (built) return g_cfg_svcs;

  memset(g_cfg_chrs, 0, sizeof(g_cfg_chrs));
  memset(g_cfg_svcs, 0, sizeof(g_cfg_svcs));

  // CMD (Write)
  g_cfg_chrs[0].uuid = (ble_uuid_t *)&UUID_CHR_CMD;
  g_cfg_chrs[0].access_cb = gatt_access_cfg;
  g_cfg_chrs[0].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP;
  g_cfg_chrs[0].val_handle = &g_cmd_handle;

  // EVT (Notify)
  g_cfg_chrs[1].uuid = (ble_uuid_t *)&UUID_CHR_EVT;
    g_cfg_chrs[1].access_cb = gatt_access_evt_stream;
    g_cfg_chrs[1].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY;
  g_cfg_chrs[1].val_handle = &g_evt_handle;

  // STREAM (Notify)
  g_cfg_chrs[2].uuid = (ble_uuid_t *)&UUID_CHR_STREAM;
    g_cfg_chrs[2].access_cb = gatt_access_evt_stream;
    g_cfg_chrs[2].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY;
  g_cfg_chrs[2].val_handle = &g_stream_handle;

  // CFG (Read/Write)
  g_cfg_chrs[3].uuid = (ble_uuid_t *)&UUID_CHR_CFG;
  g_cfg_chrs[3].access_cb = gatt_access_cfg;
  g_cfg_chrs[3].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE;
  g_cfg_chrs[3].val_handle = &g_cfg_handle;

  // Terminator
  g_cfg_chrs[4] = {0};

  // Service
  g_cfg_svcs[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
  g_cfg_svcs[0].uuid = (ble_uuid_t *)&UUID_SVC_CFG;
  g_cfg_svcs[0].characteristics = g_cfg_chrs;

  // Terminator
  g_cfg_svcs[1] = {0};

  built = true;
  return g_cfg_svcs;
}

// -----------------------------------------------------------------------------
// STREAM payload (binary, <= 20 bytes at MTU=23)
// -----------------------------------------------------------------------------
struct __attribute__((packed)) StreamSample {
  uint8_t version;
  uint8_t flags;
  uint32_t device_id;
  uint32_t element_id;
  int32_t raw;
  int16_t norm_q15;
};

static int16_t clamp_q15(float v) {
  if (v > 1.0f) v = 1.0f;
  if (v < -1.0f) v = -1.0f;
  const int32_t q = (int32_t)lrintf(v * 32767.0f);
  if (q > 32767) return 32767;
  if (q < -32767) return -32767;
  return (int16_t)q;
}

// -----------------------------------------------------------------------------
// EXPORTED API
// -----------------------------------------------------------------------------
// These functions are part of the CONFIG-mode WebBLE service interface.
// They are declared with C linkage in ble_config_service.h to keep symbol names
// stable across compilation units and toolchains.

extern "C" {

const char *ble_config_service_uuid_str(void) {
  return "6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a1";
}

const struct ble_gatt_svc_def *ble_config_service_gatt_defs(void) {
  return build_cfg_svcs_once();
}

void ble_config_service_init(void) {
  g_conn_handle = BLE_HS_CONN_HANDLE_NONE;

  g_cmd_handle = 0;
  g_evt_handle = 0;
  g_stream_handle = 0;
  g_cfg_handle = 0;

  g_evt_notify_enabled = false;
  g_stream_notify_enabled = false;

  g_stream_active = false;
  g_stream_rr = 0;
  g_stream_elem_cursor = 0;

  g_msg_id = 0;

  g_last_stream_valid = false;

  if (g_config_json.empty()) g_config_json = "{}";

  ESP_LOGI(TAG, "Config service init (UUID=%s)", ble_config_service_uuid_str());
}

void ble_config_service_on_connect(uint16_t conn_handle) {
  g_conn_handle = conn_handle;
  g_evt_notify_enabled = false;
  g_stream_notify_enabled = false;
  if (!g_evt_tx_sem) {
    g_evt_tx_sem = xSemaphoreCreateBinary();
  }
  if (g_evt_tx_sem) {
    xSemaphoreGive(g_evt_tx_sem);
  }
  ESP_LOGI(TAG, "Connected (handle=%u)", (unsigned)conn_handle);
}

void ble_config_service_on_disconnect(void) {
  ESP_LOGI(TAG, "Disconnected");
  g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
  g_evt_notify_enabled = false;
  g_stream_notify_enabled = false;
  g_stream_active = false;
  g_last_stream_valid = false;
  if (g_evt_tx_sem) {
    xSemaphoreGive(g_evt_tx_sem);
  }
}

void ble_config_service_on_subscribe(uint16_t attr_handle, uint8_t cur_notify) {
  if (g_evt_handle != 0 && attr_handle == g_evt_handle) {
    g_evt_notify_enabled = (cur_notify != 0);
    ESP_LOGI(TAG, "EVT notifications %s", g_evt_notify_enabled ? "ENABLED" : "DISABLED");
  } else if (g_stream_handle != 0 && attr_handle == g_stream_handle) {
    g_stream_notify_enabled = (cur_notify != 0);
    ESP_LOGI(TAG, "STREAM notifications %s", g_stream_notify_enabled ? "ENABLED" : "DISABLED");
  }
}

void ble_config_service_on_notify_tx(uint16_t attr_handle, int status) {
  if (!g_evt_tx_sem) return;
  if (g_evt_handle != 0 && attr_handle == g_evt_handle) {
    xSemaphoreGive(g_evt_tx_sem);
    if (status != 0) {
      ESP_LOGW(TAG, "EVT notify-tx status=%d", status);
    }
  }
}

void ble_config_service_stream_tick(void) {
  if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
  if (!g_stream_active) return;
  if (!g_stream_notify_enabled) return;
  if (g_stream_handle == 0) return;

  HidDeviceInfo infos[8];
  const size_t n = hid_device_manager_list_devices(infos, 8);
  if (n == 0) return;

  const uint32_t device_id = infos[g_stream_rr % n].device_id;
  g_stream_rr++;

  static InputElement elems[MAX_INPUT_ELEMENTS];
  const size_t m = hid_device_manager_get_elements(device_id, elems, MAX_INPUT_ELEMENTS);
  if (m == 0) return;

  // Prefer recently-updated controls so the web detector can identify the moving element.
  size_t picked = SIZE_MAX;
  uint32_t best_update = 0;
  for (size_t i = 0; i < m; i++) {
    const auto k = elems[i].kind;
    if (k != IE_KIND_AXIS && k != IE_KIND_HAT && k != IE_KIND_BUTTON) continue;
    if (elems[i].last_update_ms >= best_update) {
      best_update = elems[i].last_update_ms;
      picked = i;
    }
  }

  // Fall back to round-robin over interesting controls if nothing changed recently.
  if (picked == SIZE_MAX || best_update == 0) {
    for (size_t tries = 0; tries < m; tries++) {
      const size_t idx = (size_t)(g_stream_elem_cursor++ % (uint16_t)m);
      const auto k = elems[idx].kind;
      if (k == IE_KIND_AXIS || k == IE_KIND_HAT || k == IE_KIND_BUTTON) {
        picked = idx;
        break;
      }
    }
  }
  if (picked == SIZE_MAX) picked = 0;

  StreamSample s;
  memset(&s, 0, sizeof(s));
  s.version = 1;
  s.flags = 0;
  s.device_id = device_id;
  s.element_id = elems[picked].element_id;
  s.raw = elems[picked].raw;
  s.norm_q15 = clamp_q15(elems[picked].norm_m1_1);

  // Cache for READs.
  memcpy(g_last_stream_sample_buf, &s, sizeof(s));
  g_last_stream_valid = true;

  struct os_mbuf *om = ble_hs_mbuf_from_flat(&s, sizeof(s));
  if (!om) return;

  (void)ble_gatts_notify_custom(g_conn_handle, g_stream_handle, om);
}

} // extern "C"