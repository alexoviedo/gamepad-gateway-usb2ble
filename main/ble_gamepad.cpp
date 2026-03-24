#include "ble_gamepad.h"
#include "ble_config_service.h"

#include <string.h>

#include <esp_err.h>
#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>

extern "C" {
#include <esp_nimble_hci.h>
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
}

extern "C" void ble_store_config_init(void);
extern "C" esp_err_t esp_nimble_hci_and_controller_init(void) __attribute__((weak));
extern "C" esp_err_t esp_nimble_hci_and_controller_deinit(void) __attribute__((weak));

static const char *TAG = "BLE_GAMEPAD";
static bool g_is_config_mode = false;
static constexpr uint16_t kAppearanceHidGamepad = 0x03C4;

static constexpr uint8_t kReportIdGamepad = 0x03;
static constexpr uint8_t kReportTypeInput = 0x01;
static constexpr size_t kInputReportSize = 21;

struct __attribute__((packed)) BleHidInputReport {
  uint32_t buttons;
  uint16_t x;
  uint16_t y;
  uint16_t z;
  uint16_t rz;
  uint16_t rx;
  uint16_t ry;
  uint16_t slider1;
  uint16_t slider2;
  uint8_t hat;
};
static_assert(sizeof(BleHidInputReport) == kInputReportSize, "BLE HID report size drift");

static uint8_t g_last_report[kInputReportSize] = {0};

static inline uint16_t to_u16_axis(int16_t v) {
  int32_t t = (int32_t)v + 32768;
  if (t < 0) t = 0;
  if (t > 65535) t = 65535;
  return (uint16_t)(t >> 1);
}

static inline uint8_t clamp_hat(HatDirection hat) {
  uint8_t raw = static_cast<uint8_t>(hat);
  return (raw <= 8) ? raw : 0;
}

static const uint8_t kHidReportMap[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, kReportIdGamepad,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x20, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x20, 0x81, 0x02,
    0x15, 0x00, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x08, 0xA1, 0x00,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x09, 0x33, 0x09, 0x34, 0x09, 0x36, 0x09, 0x36, 0x81, 0x02, 0xC0,
    0x05, 0x01, 0x09, 0x39, 0x15, 0x01, 0x25, 0x08, 0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x75, 0x08, 0x95, 0x01, 0x81, 0x42, 0x65, 0x00,
    0xC0
};

static uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool g_ble_connected = false;
static uint8_t g_own_addr_type = BLE_OWN_ADDR_PUBLIC;

static constexpr const char *kIdentityNs = "ble_ident";
static constexpr const char *kCfgAddrKey = "cfg_addr";
static constexpr const char *kRunAddrKey = "run_addr";

static uint16_t g_hid_report_handle = 0;
static uint16_t g_battery_level_handle = 0;

static bool g_input_notify_enabled = false;
static bool g_battery_notify_enabled = false;
static bool g_force_send_once = false;

static uint8_t g_protocol_mode = 1;
static uint8_t g_hid_ctrl_point = 0;
static uint8_t g_battery_level = 100;

static const uint8_t kHidInfoValue[4] = {0x11, 0x01, 0x00, 0x02};
static const uint8_t kReportRefValue[2] = {kReportIdGamepad, kReportTypeInput};
static const uint8_t kExtReportRefValue[2] = {0x0F, 0x18};
static const uint8_t kPnpIdValue[7] = {0x02, 0x5E, 0x04, 0xAD, 0xDE, 0x00, 0x01};

static int gatt_read_bytes(struct ble_gatt_access_ctxt *ctxt, const void *data, size_t len) {
  if (ctxt->offset > len) return BLE_ATT_ERR_INVALID_OFFSET;
  const uint8_t *ptr = static_cast<const uint8_t *>(data);
  const size_t remaining = len - ctxt->offset;
  int rc = os_mbuf_append(ctxt->om, ptr + ctxt->offset, remaining);
  return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int gatt_access_dis(uint16_t, uint16_t, struct ble_gatt_access_ctxt *ctxt, void *) {
  const uint16_t u16 = ble_uuid_u16(ctxt->chr->uuid);
  if (u16 == 0x2A29) { static const char kMfg[] = "Thrustmaster-Bridge"; return gatt_read_bytes(ctxt, kMfg, sizeof(kMfg) - 1); }
  if (u16 == 0x2A24) { static const char kModel[] = "HOTAS-BLE"; return gatt_read_bytes(ctxt, kModel, sizeof(kModel) - 1); }
  if (u16 == 0x2A50) { return gatt_read_bytes(ctxt, kPnpIdValue, sizeof(kPnpIdValue)); }
  return BLE_ATT_ERR_UNLIKELY;
}

static int gatt_access_battery(uint16_t, uint16_t, struct ble_gatt_access_ctxt *ctxt, void *) {
  if (ble_uuid_u16(ctxt->chr->uuid) == 0x2A19) return gatt_read_bytes(ctxt, &g_battery_level, sizeof(g_battery_level));
  return BLE_ATT_ERR_UNLIKELY;
}

static int gatt_access_hid(uint16_t, uint16_t, struct ble_gatt_access_ctxt *ctxt, void *) {
  const uint16_t u16 = ble_uuid_u16(ctxt->chr->uuid);
  switch (u16) {
  case 0x2A4B: return gatt_read_bytes(ctxt, kHidReportMap, sizeof(kHidReportMap));
  case 0x2A4E:
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) return gatt_read_bytes(ctxt, &g_protocol_mode, sizeof(g_protocol_mode));
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
      uint8_t v = 0; int rc = ble_hs_mbuf_to_flat(ctxt->om, &v, sizeof(v), nullptr); if (rc != 0) return BLE_ATT_ERR_UNLIKELY; if (v > 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN; g_protocol_mode = v; return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
  case 0x2A4C:
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
      uint8_t v = 0; int rc = ble_hs_mbuf_to_flat(ctxt->om, &v, sizeof(v), nullptr); if (rc != 0) return BLE_ATT_ERR_UNLIKELY; g_hid_ctrl_point = v; return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
  case 0x2A4A: return gatt_read_bytes(ctxt, kHidInfoValue, sizeof(kHidInfoValue));
  case 0x2A4D: return gatt_read_bytes(ctxt, g_last_report, sizeof(g_last_report));
  default: return BLE_ATT_ERR_UNLIKELY;
  }
}

static int gatt_access_report_ref(uint16_t, uint16_t, struct ble_gatt_access_ctxt *ctxt, void *) {
  return gatt_read_bytes(ctxt, kReportRefValue, sizeof(kReportRefValue));
}

static int gatt_access_ext_report_ref(uint16_t, uint16_t, struct ble_gatt_access_ctxt *ctxt, void *) {
  return gatt_read_bytes(ctxt, kExtReportRefValue, sizeof(kExtReportRefValue));
}

static const ble_uuid16_t UUID_SVC_DEVINFO = BLE_UUID16_INIT(0x180A);
static const ble_uuid16_t UUID_CHR_MFG_NAME = BLE_UUID16_INIT(0x2A29);
static const ble_uuid16_t UUID_CHR_MODEL_NUM = BLE_UUID16_INIT(0x2A24);
static const ble_uuid16_t UUID_CHR_PNP_ID = BLE_UUID16_INIT(0x2A50);
static const ble_uuid16_t UUID_SVC_BAS = BLE_UUID16_INIT(0x180F);
static const ble_uuid16_t UUID_CHR_BATTERY_LEVEL = BLE_UUID16_INIT(0x2A19);
static const ble_uuid16_t UUID_SVC_HID = BLE_UUID16_INIT(0x1812);
static const ble_uuid16_t UUID_CHR_PROTOCOL_MODE = BLE_UUID16_INIT(0x2A4E);
static const ble_uuid16_t UUID_CHR_REPORT_MAP = BLE_UUID16_INIT(0x2A4B);
static const ble_uuid16_t UUID_CHR_HID_INFO = BLE_UUID16_INIT(0x2A4A);
static const ble_uuid16_t UUID_CHR_HID_CTRL_PT = BLE_UUID16_INIT(0x2A4C);
static const ble_uuid16_t UUID_CHR_REPORT = BLE_UUID16_INIT(0x2A4D);
static const ble_uuid16_t UUID_DSC_EXT_REPORT_REF = BLE_UUID16_INIT(0x2907);
static const ble_uuid16_t UUID_DSC_REPORT_REF = BLE_UUID16_INIT(0x2908);

static ble_gatt_dsc_def g_report_map_descs[2];
static ble_gatt_dsc_def g_hid_report_descs[2];
static ble_gatt_chr_def g_devinfo_chrs[4];
static ble_gatt_chr_def g_bas_chrs[2];
static ble_gatt_chr_def g_hid_chrs[6];
static ble_gatt_svc_def gatt_svr_svcs[4];

static void build_gatt_db(void) {
  memset(g_report_map_descs, 0, sizeof(g_report_map_descs));
  memset(g_hid_report_descs, 0, sizeof(g_hid_report_descs));
  memset(g_devinfo_chrs, 0, sizeof(g_devinfo_chrs));
  memset(g_bas_chrs, 0, sizeof(g_bas_chrs));
  memset(g_hid_chrs, 0, sizeof(g_hid_chrs));
  memset(gatt_svr_svcs, 0, sizeof(gatt_svr_svcs));

  g_devinfo_chrs[0].uuid = (ble_uuid_t *)&UUID_CHR_MFG_NAME; g_devinfo_chrs[0].access_cb = gatt_access_dis; g_devinfo_chrs[0].flags = BLE_GATT_CHR_F_READ;
  g_devinfo_chrs[1].uuid = (ble_uuid_t *)&UUID_CHR_MODEL_NUM; g_devinfo_chrs[1].access_cb = gatt_access_dis; g_devinfo_chrs[1].flags = BLE_GATT_CHR_F_READ;
  g_devinfo_chrs[2].uuid = (ble_uuid_t *)&UUID_CHR_PNP_ID; g_devinfo_chrs[2].access_cb = gatt_access_dis; g_devinfo_chrs[2].flags = BLE_GATT_CHR_F_READ;
  gatt_svr_svcs[0].type = BLE_GATT_SVC_TYPE_PRIMARY; gatt_svr_svcs[0].uuid = (ble_uuid_t *)&UUID_SVC_DEVINFO; gatt_svr_svcs[0].characteristics = g_devinfo_chrs;

  if (g_is_config_mode) {
    const struct ble_gatt_svc_def *cfg = ble_config_service_gatt_defs();
    if (cfg && cfg[0].uuid) gatt_svr_svcs[1] = cfg[0]; else ESP_LOGE(TAG, "CONFIG mode requested but ble_config_service_gatt_defs() missing/empty.");
  } else {
    g_bas_chrs[0].uuid = (ble_uuid_t *)&UUID_CHR_BATTERY_LEVEL; g_bas_chrs[0].access_cb = gatt_access_battery; g_bas_chrs[0].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY; g_bas_chrs[0].val_handle = &g_battery_level_handle;
    gatt_svr_svcs[1].type = BLE_GATT_SVC_TYPE_PRIMARY; gatt_svr_svcs[1].uuid = (ble_uuid_t *)&UUID_SVC_BAS; gatt_svr_svcs[1].characteristics = g_bas_chrs;

    g_hid_chrs[0].uuid = (ble_uuid_t *)&UUID_CHR_PROTOCOL_MODE; g_hid_chrs[0].access_cb = gatt_access_hid; g_hid_chrs[0].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP;
    g_report_map_descs[0].uuid = (ble_uuid_t *)&UUID_DSC_EXT_REPORT_REF; g_report_map_descs[0].att_flags = BLE_ATT_F_READ; g_report_map_descs[0].access_cb = gatt_access_ext_report_ref;
    g_hid_chrs[1].uuid = (ble_uuid_t *)&UUID_CHR_REPORT_MAP; g_hid_chrs[1].access_cb = gatt_access_hid; g_hid_chrs[1].flags = BLE_GATT_CHR_F_READ; g_hid_chrs[1].descriptors = g_report_map_descs;
    g_hid_chrs[2].uuid = (ble_uuid_t *)&UUID_CHR_HID_INFO; g_hid_chrs[2].access_cb = gatt_access_hid; g_hid_chrs[2].flags = BLE_GATT_CHR_F_READ;
    g_hid_chrs[3].uuid = (ble_uuid_t *)&UUID_CHR_HID_CTRL_PT; g_hid_chrs[3].access_cb = gatt_access_hid; g_hid_chrs[3].flags = BLE_GATT_CHR_F_WRITE_NO_RSP;
    g_hid_report_descs[0].uuid = (ble_uuid_t *)&UUID_DSC_REPORT_REF; g_hid_report_descs[0].att_flags = BLE_ATT_F_READ; g_hid_report_descs[0].access_cb = gatt_access_report_ref;
    g_hid_chrs[4].uuid = (ble_uuid_t *)&UUID_CHR_REPORT; g_hid_chrs[4].access_cb = gatt_access_hid; g_hid_chrs[4].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY; g_hid_chrs[4].val_handle = &g_hid_report_handle; g_hid_chrs[4].descriptors = g_hid_report_descs;
    gatt_svr_svcs[2].type = BLE_GATT_SVC_TYPE_PRIMARY; gatt_svr_svcs[2].uuid = (ble_uuid_t *)&UUID_SVC_HID; gatt_svr_svcs[2].characteristics = g_hid_chrs;
  }
}

static void ble_advertise(void);

static void gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *) {
  switch (ctxt->op) {
  case BLE_GATT_REGISTER_OP_SVC: {
    uint16_t u16 = 0; if (ctxt->svc.svc_def && ctxt->svc.svc_def->uuid) u16 = ble_uuid_u16(ctxt->svc.svc_def->uuid); ESP_LOGI(TAG, "GATT REG SVC 0x%04x -> handle=%d", u16, (int)ctxt->svc.handle); break; }
  case BLE_GATT_REGISTER_OP_CHR: {
    uint16_t u16 = 0; if (ctxt->chr.chr_def && ctxt->chr.chr_def->uuid) u16 = ble_uuid_u16(ctxt->chr.chr_def->uuid); ESP_LOGI(TAG, "GATT REG CHR 0x%04x -> def_handle=%d val_handle=%d", u16, (int)ctxt->chr.def_handle, (int)ctxt->chr.val_handle); break; }
  case BLE_GATT_REGISTER_OP_DSC: {
    uint16_t u16 = 0; if (ctxt->dsc.dsc_def && ctxt->dsc.dsc_def->uuid) u16 = ble_uuid_u16(ctxt->dsc.dsc_def->uuid); ESP_LOGI(TAG, "GATT REG DSC 0x%04x -> handle=%d", u16, (int)ctxt->dsc.handle); break; }
  default: break;
  }
}

static int gap_event_cb(struct ble_gap_event *event, void *) {
  switch (event->type) {
  case BLE_GAP_EVENT_CONNECT:
    if (event->connect.status == 0) {
      g_conn_handle = event->connect.conn_handle; g_ble_connected = true; g_input_notify_enabled = false; g_battery_notify_enabled = false; g_force_send_once = true; ESP_LOGI(TAG, "Connected (handle=%d)", (int)g_conn_handle);
      if (g_is_config_mode) ble_config_service_on_connect(g_conn_handle);
    } else {
      ESP_LOGW(TAG, "Connect failed; status=%d", event->connect.status); ble_advertise();
    }
    return 0;
  case BLE_GAP_EVENT_DISCONNECT:
    ESP_LOGI(TAG, "Disconnected; reason=%d", event->disconnect.reason);
    g_ble_connected = false; g_conn_handle = BLE_HS_CONN_HANDLE_NONE; g_input_notify_enabled = false; g_battery_notify_enabled = false;
    if (g_is_config_mode) ble_config_service_on_disconnect();
    ble_advertise(); return 0;
  case BLE_GAP_EVENT_SUBSCRIBE:
    if (g_is_config_mode) { ble_config_service_on_subscribe(event->subscribe.attr_handle, event->subscribe.cur_notify); return 0; }
    if (event->subscribe.attr_handle == g_hid_report_handle) {
      g_input_notify_enabled = event->subscribe.cur_notify; ESP_LOGI(TAG, "Input report notifications %s", g_input_notify_enabled ? "ENABLED" : "DISABLED"); if (g_input_notify_enabled) g_force_send_once = true;
    } else if (event->subscribe.attr_handle == g_battery_level_handle) {
      g_battery_notify_enabled = event->subscribe.cur_notify; ESP_LOGI(TAG, "Battery notifications %s", g_battery_notify_enabled ? "ENABLED" : "DISABLED");
    }
    return 0;
  case BLE_GAP_EVENT_NOTIFY_TX:
    if (g_is_config_mode) ble_config_service_on_notify_tx(event->notify_tx.attr_handle, event->notify_tx.status);
    return 0;
  case BLE_GAP_EVENT_ENC_CHANGE: {
    struct ble_gap_conn_desc desc; memset(&desc, 0, sizeof(desc)); int rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc); if (rc == 0) {
      ESP_LOGI(TAG, "Encryption changed; status=%d encrypted=%d bonded=%d key_size=%d", event->enc_change.status, desc.sec_state.encrypted, desc.sec_state.bonded, desc.sec_state.key_size);
    } else { ESP_LOGI(TAG, "Encryption changed; status=%d", event->enc_change.status); }
    return 0;
  }
  default: return 0;
  }
}

static void ble_advertise(void) {
  (void)ble_gap_adv_stop();
  static const ble_uuid128_t kCfgSvcUuid = BLE_UUID128_INIT(0x90, 0x2d, 0x1a, 0x6b, 0x1c, 0x9c, 0x3f, 0x4a, 0xb1, 0xe0, 0x4f, 0xd8, 0xf5, 0xb2, 0xc1, 0xa1);
  static ble_uuid128_t kCfgSvcUuids128[] = {kCfgSvcUuid};

  struct ble_hs_adv_fields adv_fields; memset(&adv_fields, 0, sizeof(adv_fields));
  adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  const char *name = ble_svc_gap_device_name(); adv_fields.name = (uint8_t *)name; adv_fields.name_len = strlen(name); adv_fields.name_is_complete = 1;

  if (g_is_config_mode) {
    adv_fields.uuids128 = kCfgSvcUuids128; adv_fields.num_uuids128 = (uint8_t)(sizeof(kCfgSvcUuids128) / sizeof(kCfgSvcUuids128[0])); adv_fields.uuids128_is_complete = 1;
  } else {
    adv_fields.appearance = kAppearanceHidGamepad; static ble_uuid16_t uuids16[] = {BLE_UUID16_INIT(0x1812), BLE_UUID16_INIT(0x180F)}; adv_fields.uuids16 = uuids16; adv_fields.num_uuids16 = (uint8_t)(sizeof(uuids16) / sizeof(uuids16[0])); adv_fields.uuids16_is_complete = 1;
  }

  bool cfg_uuid_in_adv = g_is_config_mode;
  int rc = ble_gap_adv_set_fields(&adv_fields);
  if (rc != 0 && g_is_config_mode) {
    cfg_uuid_in_adv = false; adv_fields.uuids128 = nullptr; adv_fields.num_uuids128 = 0; adv_fields.uuids128_is_complete = 0; rc = ble_gap_adv_set_fields(&adv_fields);
  }
  if (rc != 0) { ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc); return; }

  if (g_is_config_mode) {
    struct ble_hs_adv_fields rsp_fields; memset(&rsp_fields, 0, sizeof(rsp_fields));
    if (!cfg_uuid_in_adv) { rsp_fields.uuids128 = kCfgSvcUuids128; rsp_fields.num_uuids128 = (uint8_t)(sizeof(kCfgSvcUuids128) / sizeof(kCfgSvcUuids128[0])); rsp_fields.uuids128_is_complete = 1; }
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields); if (rc != 0) { ESP_LOGE(TAG, "ble_gap_adv_rsp_set_fields failed: %d", rc); return; }
  } else {
    struct ble_hs_adv_fields empty_rsp; memset(&empty_rsp, 0, sizeof(empty_rsp)); (void)ble_gap_adv_rsp_set_fields(&empty_rsp);
  }

  struct ble_gap_adv_params adv_params; memset(&adv_params, 0, sizeof(adv_params)); adv_params.conn_mode = BLE_GAP_CONN_MODE_UND; adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  rc = ble_gap_adv_start(g_own_addr_type, nullptr, BLE_HS_FOREVER, &adv_params, gap_event_cb, nullptr);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
  } else {
    if (g_is_config_mode) ESP_LOGI(TAG, "Advertising as '%s' (CFG svc %s)", name, ble_config_service_uuid_str());
    else ESP_LOGI(TAG, "Advertising as '%s' (HID=0x1812, BAS=0x180F)", name);
  }
}

static int load_or_create_mode_random_addr(uint8_t out_addr[6]) {
  const char *key = g_is_config_mode ? kCfgAddrKey : kRunAddrKey;
  nvs_handle_t h; esp_err_t err = nvs_open(kIdentityNs, NVS_READWRITE, &h); if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_open(%s) failed: %s", kIdentityNs, esp_err_to_name(err)); return BLE_HS_ESTORE_FAIL; }
  size_t len = 6; err = nvs_get_blob(h, key, out_addr, &len); if (err == ESP_OK && len == 6) { nvs_close(h); return 0; }
  ble_addr_t addr; int rc = ble_hs_id_gen_rnd(0, &addr); if (rc != 0) { nvs_close(h); ESP_LOGE(TAG, "ble_hs_id_gen_rnd failed: %d", rc); return rc; }
  memcpy(out_addr, addr.val, 6); err = nvs_set_blob(h, key, out_addr, 6); if (err == ESP_OK) err = nvs_commit(h); nvs_close(h); if (err != ESP_OK) { ESP_LOGE(TAG, "Persisting %s identity failed: %s", key, esp_err_to_name(err)); return BLE_HS_ESTORE_FAIL; }
  ESP_LOGI(TAG, "Created persistent %s BLE identity", g_is_config_mode ? "CONFIG" : "RUN");
  return 0;
}

static int configure_mode_identity(void) {
  uint8_t rnd_addr[6] = {0}; int rc = load_or_create_mode_random_addr(rnd_addr); if (rc != 0) return rc;
  rc = ble_hs_id_set_rnd(rnd_addr); if (rc != 0) { ESP_LOGE(TAG, "ble_hs_id_set_rnd failed: %d", rc); return rc; }
  rc = ble_hs_id_infer_auto(0, &g_own_addr_type); if (rc != 0) { ESP_LOGE(TAG, "ble_hs_id_infer_auto failed after rnd addr set: %d", rc); return rc; }
  uint8_t id_addr[6] = {0}; int is_nrpa = 0; rc = ble_hs_id_copy_addr(BLE_ADDR_RANDOM, id_addr, &is_nrpa); if (rc == 0) { ESP_LOGI(TAG, "%s identity addr=%02x:%02x:%02x:%02x:%02x:%02x (nrpa=%d own_addr_type=%u)", g_is_config_mode ? "CONFIG" : "RUN", id_addr[5], id_addr[4], id_addr[3], id_addr[2], id_addr[1], id_addr[0], is_nrpa, (unsigned)g_own_addr_type); }
  return 0;
}

static void on_sync(void) {
  int rc = configure_mode_identity(); if (rc != 0) { ESP_LOGE(TAG, "configure_mode_identity failed: %d", rc); return; }
  ble_advertise();
}

static void host_task(void *) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

static void ble_common_init(bool config_mode) {
  g_is_config_mode = config_mode;

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) { ESP_ERROR_CHECK(nvs_flash_erase()); ret = nvs_flash_init(); }
  ESP_ERROR_CHECK(ret);

  if (esp_nimble_hci_and_controller_init) {
    esp_err_t rc = esp_nimble_hci_and_controller_init();
    if (rc != ESP_OK) ESP_LOGE(TAG, "esp_nimble_hci_and_controller_init failed: %s", esp_err_to_name(rc));
  } else {
    ESP_LOGW(TAG, "esp_nimble_hci_and_controller_init not linked; continuing without explicit HCI init");
  }

  nimble_port_init();
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
  ble_store_config_init();
  ble_svc_gap_init();
  ble_svc_gatt_init();

  if (g_is_config_mode) {
    ble_svc_gap_device_name_set("HOTAS_CFG"); ble_svc_gap_device_appearance_set(0); ble_config_service_init();
  } else {
    ble_svc_gap_device_name_set("HOTAS_BRIDGE"); ble_svc_gap_device_appearance_set(kAppearanceHidGamepad);
  }

  build_gatt_db();

  int rc = ble_gatts_count_cfg(gatt_svr_svcs); if (rc != 0) { ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc); return; }
  rc = ble_gatts_add_svcs(gatt_svr_svcs); if (rc != 0) { ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc); return; }

  ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
  ble_hs_cfg.sm_bonding = g_is_config_mode ? 0 : 1;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 0;
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

  ble_hs_cfg.sync_cb = on_sync;
  ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;

  nimble_port_freertos_init(host_task);

  if (g_is_config_mode) ESP_LOGI(TAG, "BLE CONFIG initialized (service UUID=%s)", ble_config_service_uuid_str());
  else ESP_LOGI(TAG, "BLE HID gamepad initialized (ReportID=%u, report=%u bytes, map=%u bytes)", (unsigned)kReportIdGamepad, (unsigned)kInputReportSize, (unsigned)sizeof(kHidReportMap));
}

void ble_gamepad_init(void) { ble_common_init(false); }
void ble_config_init(void) { ble_common_init(true); }

void ble_gamepad_send_state(const GamepadState *s) {
  if (!s) return;
  if (g_is_config_mode) return;
  if (!g_ble_connected || g_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
  if (g_hid_report_handle == 0) return;

  BleHidInputReport report = {
      .buttons = s->buttons,
      .x = to_u16_axis(s->x),
      .y = to_u16_axis(s->y),
      .z = to_u16_axis(s->z),
      .rz = to_u16_axis(s->rz),
      .rx = to_u16_axis(s->rx),
      .ry = to_u16_axis(s->ry),
      .slider1 = to_u16_axis(s->slider1),
      .slider2 = to_u16_axis(s->slider2),
      .hat = clamp_hat(s->hat),
  };

  uint8_t next[kInputReportSize];
  memcpy(next, &report, sizeof(report));

  if (!g_force_send_once && memcmp(next, g_last_report, sizeof(next)) == 0) return;

  g_force_send_once = false;
  memcpy(g_last_report, next, sizeof(g_last_report));
  ble_gatts_chr_updated(g_hid_report_handle);
}
