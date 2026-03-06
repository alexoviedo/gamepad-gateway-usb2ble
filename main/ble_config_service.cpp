// main/ble_config_service.cpp
#include "ble_config_service.h"

#include "app_mode.h"
#include "hid_device_manager.h"
#include "input_decoder.h"

#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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
    0x90, 0x2d, 0x1a, 0x6b, 0x1c, 0x9c, 0x3f, 0x4a,
    0xb1, 0xe0, 0x4f, 0xd8, 0xf5, 0xb2, 0xc1, 0xa1);

static const ble_uuid128_t UUID_CHR_CMD = BLE_UUID128_INIT(
    0x90, 0x2d, 0x1a, 0x6b, 0x1c, 0x9c, 0x3f, 0x4a,
    0xb1, 0xe0, 0x4f, 0xd8, 0xf5, 0xb2, 0xc1, 0xa2);

static const ble_uuid128_t UUID_CHR_EVT = BLE_UUID128_INIT(
    0x90, 0x2d, 0x1a, 0x6b, 0x1c, 0x9c, 0x3f, 0x4a,
    0xb1, 0xe0, 0x4f, 0xd8, 0xf5, 0xb2, 0xc1, 0xa3);

static const ble_uuid128_t UUID_CHR_STREAM = BLE_UUID128_INIT(
    0x90, 0x2d, 0x1a, 0x6b, 0x1c, 0x9c, 0x3f, 0x4a,
    0xb1, 0xe0, 0x4f, 0xd8, 0xf5, 0xb2, 0xc1, 0xa4);

static const ble_uuid128_t UUID_CHR_CFG = BLE_UUID128_INIT(
    0x90, 0x2d, 0x1a, 0x6b, 0x1c, 0x9c, 0x3f, 0x4a,
    0xb1, 0xe0, 0x4f, 0xd8, 0xf5, 0xb2, 0xc1, 0xa5);

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

static std::string g_config_json = "{}";
static constexpr size_t kMaxConfigJsonBytes = 4096;

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

  // Conservative payload sizing: assume MTU=23 -> 20 bytes value.
  // Header is 8 bytes -> payload <= 12.
  constexpr size_t kMaxValueLen = 20;
  constexpr size_t kHdrLen = 8;
  constexpr size_t kMaxPayload = kMaxValueLen - kHdrLen;

  uint16_t offset = 0;
  while (offset < total_len) {
    uint8_t buf[kMaxValueLen];
    const uint16_t remaining = (uint16_t)(total_len - offset);
    const uint16_t chunk_len = (remaining > kMaxPayload) ? (uint16_t)kMaxPayload : remaining;

    buf[0] = 1; // version
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
    if (!om) return;
    (void)ble_gatts_notify_custom(g_conn_handle, g_evt_handle, om);

    offset = (uint16_t)(offset + chunk_len);
    if (total_len > 128) vTaskDelay(pdMS_TO_TICKS(1));
  }
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

static void cmd_start_stream(cJSON *resp) {
  g_stream_active = true;
  cJSON_AddBoolToObject(resp, "streaming", true);
}

static void cmd_stop_stream(cJSON *resp) {
  g_stream_active = false;
  cJSON_AddBoolToObject(resp, "streaming", false);
}

static void cmd_get_config(cJSON *resp) {
  cJSON_AddStringToObject(resp, "config_json", g_config_json.c_str());
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

  g_config_json.assign(printed, len);
  cJSON_free(printed);

  cJSON_AddBoolToObject(resp, "ok", true);
}

static void cmd_save_profile_stub(cJSON *resp) {
  cJSON_AddStringToObject(resp, "note", "save_profile is a stub (persistence not implemented yet)");
  cJSON_AddBoolToObject(resp, "ok", true);
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
      } else if (strcmp(c, "start_stream") == 0) {
        cmd_start_stream(resp);
      } else if (strcmp(c, "stop_stream") == 0) {
        cmd_stop_stream(resp);
      } else if (strcmp(c, "get_config") == 0) {
        cmd_get_config(resp);
      } else if (strcmp(c, "set_config") == 0) {
        cmd_set_config(req, resp);
      } else if (strcmp(c, "save_profile") == 0) {
        cmd_save_profile_stub(resp);
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
  int rc = os_mbuf_append(ctxt->om, data, len);
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
      const uint16_t off = ctxt->offset;
      if (off > g_config_json.size()) return BLE_ATT_ERR_INVALID_OFFSET;

      const size_t remaining = g_config_json.size() - off;
      return gatt_read_bytes(ctxt, g_config_json.data() + off, remaining);
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
      return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
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
  g_cfg_chrs[1].access_cb = nullptr;
  g_cfg_chrs[1].flags = BLE_GATT_CHR_F_NOTIFY;
  g_cfg_chrs[1].val_handle = &g_evt_handle;

  // STREAM (Notify)
  g_cfg_chrs[2].uuid = (ble_uuid_t *)&UUID_CHR_STREAM;
  g_cfg_chrs[2].access_cb = nullptr;
  g_cfg_chrs[2].flags = BLE_GATT_CHR_F_NOTIFY;
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
// Your build logs showed the linker looking for UNMANGLED symbols like
// `ble_config_service_uuid_str` and `ble_config_service_gatt_defs`.
// So these exports MUST use C linkage to override any weak stubs and match calls.

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

  if (g_config_json.empty()) g_config_json = "{}";

  ESP_LOGI(TAG, "Config service init (UUID=%s)", ble_config_service_uuid_str());
}

void ble_config_service_on_connect(uint16_t conn_handle) {
  g_conn_handle = conn_handle;
  g_evt_notify_enabled = false;
  g_stream_notify_enabled = false;
  ESP_LOGI(TAG, "Connected (handle=%u)", (unsigned)conn_handle);
}

void ble_config_service_on_disconnect(void) {
  ESP_LOGI(TAG, "Disconnected");
  g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
  g_evt_notify_enabled = false;
  g_stream_notify_enabled = false;
  g_stream_active = false;
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

  // Pick next axis/hat element.
  size_t picked = SIZE_MAX;
  for (size_t tries = 0; tries < m; tries++) {
    const size_t idx = (size_t)(g_stream_elem_cursor++ % (uint16_t)m);
    const auto k = elems[idx].kind;
    if (k == IE_KIND_AXIS || k == IE_KIND_HAT) {
      picked = idx;
      break;
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

  struct os_mbuf *om = ble_hs_mbuf_from_flat(&s, sizeof(s));
  if (!om) return;

  (void)ble_gatts_notify_custom(g_conn_handle, g_stream_handle, om);
}

} // extern "C"