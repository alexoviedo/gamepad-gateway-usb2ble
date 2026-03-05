#include "hid_verbose.h"

#if HID_VERBOSE_HID_DEBUG

#include "input_decoder.h"
#include "shared_types.h"

#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <usb/usb_host.h>
#include <usb/usb_types_ch9.h>

static const char *TAG = "HID_VERBOSE";

// Debug-only helper to extract little-endian bits spanning across bytes.
// Mirrors input_decoder.cpp::extract_bits, but lives here so instrumentation can
// print raw field values alongside normalized axes without reaching into decoder internals.
static int32_t dbg_extract_bits(const uint8_t *report, size_t report_size,
                               uint32_t bit_offset, uint32_t bit_size, bool is_signed) {
  if (bit_size == 0) return 0;

  uint32_t value = 0;
  uint32_t current_bit = bit_offset;
  uint32_t remaining = bit_size;
  uint32_t val_shift = 0;

  while (remaining > 0) {
    uint32_t byte_idx = current_bit / 8;
    uint32_t bit_in_byte = current_bit % 8;
    if (byte_idx >= report_size) break;

    uint32_t bits_to_read = 8 - bit_in_byte;
    if (bits_to_read > remaining) bits_to_read = remaining;

    uint32_t mask = (1u << bits_to_read) - 1u;
    uint32_t chunk = (report[byte_idx] >> bit_in_byte) & mask;

    value |= (chunk << val_shift);

    val_shift += bits_to_read;
    current_bit += bits_to_read;
    remaining -= bits_to_read;
  }

  if (is_signed && (bit_size < 32)) {
    uint32_t sign_bit = 1u << (bit_size - 1);
    if (value & sign_bit) {
      uint32_t sext_mask = 0xFFFFFFFFu << bit_size;
      return (int32_t)(value | sext_mask);
    }
  }
  return (int32_t)value;
}


// --------------------------------------------------------------------------------------
// USB VID/PID cache (best effort)
// --------------------------------------------------------------------------------------
// Keyed by USB address (1..127). The USB Host library caches device descriptors after
// enumeration. We open the device briefly to read the cached device descriptor.

static usb_host_client_handle_t s_client = NULL;
static QueueHandle_t s_addr_q = NULL;

static uint16_t s_vid[128];
static uint16_t s_pid[128];
static bool s_has_vidpid[128];

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *) {
  if (!event_msg) return;

  if (event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    const uint8_t addr = event_msg->new_dev.address;
    if (addr > 0 && addr < 128 && s_addr_q) {
      // Non-blocking; if queue is full, we drop (cache will remain unknown).
      (void)xQueueSend(s_addr_q, &addr, 0);
    }
  }
}

static void vidpid_task(void *) {
  while (true) {
    // Drive this client's callbacks
    (void)usb_host_client_handle_events(s_client, 10);

    uint8_t addr = 0;
    while (s_addr_q && xQueueReceive(s_addr_q, &addr, 0) == pdTRUE) {
      if (addr == 0 || addr >= 128) continue;

      usb_device_handle_t dev_hdl = NULL;
      esp_err_t err = usb_host_device_open(s_client, addr, &dev_hdl);
      if (err != ESP_OK || !dev_hdl) {
        ESP_LOGW(TAG, "VID/PID cache: open failed for addr=%u (%s)", (unsigned)addr,
                 esp_err_to_name(err));
        continue;
      }

      const usb_device_desc_t *dev_desc = NULL;
      err = usb_host_get_device_descriptor(dev_hdl, &dev_desc);
      if (err == ESP_OK && dev_desc) {
        // USB descriptors are little-endian fields
        s_vid[addr] = dev_desc->idVendor;
        s_pid[addr] = dev_desc->idProduct;
        s_has_vidpid[addr] = true;
        ESP_LOGI(TAG, "VID/PID cache: addr=%u VID=0x%04X PID=0x%04X", (unsigned)addr,
                 (unsigned)s_vid[addr], (unsigned)s_pid[addr]);
      } else {
        ESP_LOGW(TAG, "VID/PID cache: get_device_descriptor failed for addr=%u (%s)",
                 (unsigned)addr, esp_err_to_name(err));
      }

      (void)usb_host_device_close(s_client, dev_hdl);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void hid_verbose_init(void) {
  static bool s_inited = false;
  if (s_inited) return;
  s_inited = true;

  memset(s_vid, 0, sizeof(s_vid));
  memset(s_pid, 0, sizeof(s_pid));
  memset(s_has_vidpid, 0, sizeof(s_has_vidpid));

  s_addr_q = xQueueCreate(16, sizeof(uint8_t));
  if (!s_addr_q) {
    ESP_LOGW(TAG, "Failed to create VID/PID queue; VID/PID will be unavailable");
    return;
  }

  usb_host_client_config_t cfg = {
      .is_synchronous = false,
      .max_num_event_msg = 5,
      .async = {
          .client_event_callback = client_event_cb,
          .callback_arg = NULL,
      },
  };

  esp_err_t err = usb_host_client_register(&cfg, &s_client);
  if (err != ESP_OK || !s_client) {
    ESP_LOGW(TAG, "usb_host_client_register failed (%s); VID/PID will be unavailable",
             esp_err_to_name(err));
    return;
  }

  xTaskCreate(vidpid_task, "usb_vidpid", 4096, NULL, 4, NULL);
  ESP_LOGI(TAG, "Verbose HID debug: USB VID/PID cache enabled");
}

bool hid_verbose_get_vidpid(uint8_t usb_addr, uint16_t *vid, uint16_t *pid) {
  if (usb_addr == 0 || usb_addr >= 128) return false;
  if (!s_has_vidpid[usb_addr]) return false;
  if (vid) *vid = s_vid[usb_addr];
  if (pid) *pid = s_pid[usb_addr];
  return true;
}

// --------------------------------------------------------------------------------------
// Per-device report stats + printing helpers
// --------------------------------------------------------------------------------------

static inline uint32_t now_ms(void) {
  return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void log_state_line(const char *prefix, const GamepadState &s) {
  ESP_LOGI(TAG,
           "%s x=%6d y=%6d z=%6d rx=%6d ry=%6d rz=%6d s1=%6d s2=%6d hat=%u buttons=0x%08X",
           prefix, (int)s.x, (int)s.y, (int)s.z, (int)s.rx, (int)s.ry, (int)s.rz,
           (int)s.slider1, (int)s.slider2, (unsigned)s.hat, (unsigned)s.buttons);
}

static void hex_snip(char *out, size_t out_sz, const uint8_t *buf, size_t len) {
  if (!out || out_sz == 0) return;
  out[0] = '\0';
  if (!buf || len == 0) return;

  // Format like: "01 02 0A FF" (no trailing space)
  size_t pos = 0;
  for (size_t i = 0; i < len; i++) {
    const int n = snprintf(out + pos, (pos < out_sz) ? (out_sz - pos) : 0,
                           "%02X%s", (unsigned)buf[i], (i + 1 < len) ? " " : "");
    if (n <= 0) break;
    pos += (size_t)n;
    if (pos + 1 >= out_sz) break;
  }
}

void hid_verbose_note_report(HidDeviceContext *ctx, uint8_t report_id,
                             size_t report_len, const uint8_t *report_data,
                             size_t report_data_len, bool uses_report_ids) {
  if (!ctx) return;
  const uint32_t t = now_ms();

  ctx->dbg_last_report_id = report_id;
  ctx->dbg_last_report_len = (uint16_t)report_len;
  ctx->dbg_uses_report_ids = uses_report_ids ? 1 : 0;
  ctx->dbg_report_count++;

  // If the descriptor indicates Report IDs are used, verify the incoming ID looks sane.
  // This is purely diagnostic (no behavior change).
  if (uses_report_ids) {
    bool known = false;
    for (size_t i = 0; i < ctx->caps.num_fields; i++) {
      if (ctx->caps.fields[i].report_id == report_id) {
        known = true;
        break;
      }
    }
    if (!known) {
      ESP_LOGW(TAG,
               "DEV[%u] unexpected ReportID=%u (descriptor fields expect different ID); decode may be skipping updates",
               (unsigned)ctx->slot_id, (unsigned)report_id);
    }
  }

  // Capture a small head of the raw report for later printing.
  const size_t cap = (report_data && report_data_len > 0) ? report_data_len : 0;
  const size_t n = (cap > sizeof(ctx->dbg_raw_report)) ? sizeof(ctx->dbg_raw_report) : cap;
  if (n > 0) {
    memcpy(ctx->dbg_raw_report, report_data, n);
    ctx->dbg_raw_report_cap_len = (uint8_t)n;
  } else {
    ctx->dbg_raw_report_cap_len = 0;
  }

  if (ctx->dbg_last_report_ms != 0) {
    const uint32_t dt = (t > ctx->dbg_last_report_ms) ? (t - ctx->dbg_last_report_ms) : 0;
    if (dt > 0) {
      const float inst_hz = 1000.0f / (float)dt;
      // EMA: smooth out jitter
      ctx->dbg_report_hz_ema = (ctx->dbg_report_hz_ema == 0.0f)
                                   ? inst_hz
                                   : (ctx->dbg_report_hz_ema * 0.90f + inst_hz * 0.10f);
    }
  }
  ctx->dbg_last_report_ms = t;
}

bool hid_verbose_maybe_dump_device_state(const HidDeviceContext *ctx) {
  if (!ctx || !ctx->active) return false;

  const uint32_t t = now_ms();
  // Per-device rate limit: max 10 Hz.
  // IMPORTANT: caller should update ctx->dbg_last_state_dump_ms only when this returns true.
  if (ctx->dbg_last_state_dump_ms != 0 && (t - ctx->dbg_last_state_dump_ms) < 100) {
    return false;
  }

  ESP_LOGI(TAG,
           "DEV[%u] addr=%u VID=0x%04X PID=0x%04X role=%u rpt_id=%u len=%u hz~%.1f "
           "x=%6d y=%6d z=%6d rx=%6d ry=%6d rz=%6d s1=%6d s2=%6d hat=%d buttons=0x%08X",
           (unsigned)ctx->slot_id, (unsigned)ctx->usb_addr, ctx->vid, ctx->pid,
           (unsigned)ctx->caps.role, (unsigned)ctx->dbg_last_report_id,
           (unsigned)ctx->dbg_last_report_len, (double)ctx->dbg_report_hz_ema,
           ctx->state.x, ctx->state.y, ctx->state.z, ctx->state.rx, ctx->state.ry, ctx->state.rz,
           ctx->state.slider1, ctx->state.slider2, ctx->state.hat, ctx->state.buttons);

  if (ctx->dbg_raw_report_cap_len > 0) {
    char buf[256];
    const size_t n = (ctx->dbg_raw_report_cap_len < 16) ? ctx->dbg_raw_report_cap_len : 16;
    hex_snip(buf, sizeof(buf), ctx->dbg_raw_report, n);
    ESP_LOGI(TAG, "DEV[%u] raw[%u]: %s...", (unsigned)ctx->slot_id, (unsigned)n, buf);
  }

  // Rudder heuristic for Thrustmaster throttle/pedals style reports:
  // In multiple field captures for VID:PID 044F:B687, Z often stays pegged while Slider1 moves with the rudder axis.
  if (ctx->dbg_last_report_id == 1) {
    // Track changes in slider1; if it is moving while Z is pegged, emit a hint.
    static int16_t s_prev_slider1[8] = {0};
    static bool s_has_prev[8] = {false};
    const unsigned idx = (ctx->slot_id < 8) ? (unsigned)ctx->slot_id : 7;

    const int16_t z = ctx->state.z;
    const int16_t s1 = ctx->state.slider1;
    if (s_has_prev[idx]) {
      const int16_t ds1 = (int16_t)(s1 - s_prev_slider1[idx]);
      if (abs(ds1) > 1000 && abs(z) > 32000) {
        ESP_LOGW(TAG,
                 "DEV[%u] NOTE: Z is near max while Slider1 is changing (ds1=%d); treat Slider1 as the likely rudder axis",
                 (unsigned)ctx->slot_id, (int)ds1);
      }
    }
    s_prev_slider1[idx] = s1;
    s_has_prev[idx] = true;
  }

  return true;
}

// Merge logging is global-rate-limited to keep output usable.
void hid_verbose_log_merge(const HidDeviceContext *contexts, size_t num_contexts,
                           const GamepadState *merged) {
  if (!contexts || !merged) return;

  static uint32_t s_last_merge_dump_ms = 0;
  static int8_t s_last_winner[8] = {-1, -1, -1, -1, -1, -1, -1, -1};

  const uint32_t t = now_ms();
  const bool periodic = (s_last_merge_dump_ms == 0) || ((t - s_last_merge_dump_ms) >= 200);

  // Compute winners using the same rule as hid_merge_states(): largest |deflection|
  // For hat: first non-center wins. For buttons: OR (no winner).
  int8_t win_x = -1, win_y = -1, win_z = -1, win_rx = -1, win_ry = -1, win_rz = -1,
         win_s1 = -1, win_s2 = -1;

  int16_t best_x = 0, best_y = 0, best_z = 0, best_rx = 0, best_ry = 0, best_rz = 0,
         best_s1 = 0, best_s2 = 0;

  // Collision counters (how many devices are materially contributing)
  const int16_t THRESH = 500; // ~1.5% full-scale
  int c_x = 0, c_y = 0, c_z = 0, c_rx = 0, c_ry = 0, c_rz = 0, c_s1 = 0, c_s2 = 0;

  for (size_t i = 0; i < num_contexts; i++) {
    if (!contexts[i].active) continue;
    const GamepadState &st = contexts[i].state;

    if (abs(st.x) > abs(best_x)) {
      best_x = st.x;
      win_x = (int8_t)i;
    }
    if (abs(st.y) > abs(best_y)) {
      best_y = st.y;
      win_y = (int8_t)i;
    }
    if (abs(st.z) > abs(best_z)) {
      best_z = st.z;
      win_z = (int8_t)i;
    }
    if (abs(st.rx) > abs(best_rx)) {
      best_rx = st.rx;
      win_rx = (int8_t)i;
    }
    if (abs(st.ry) > abs(best_ry)) {
      best_ry = st.ry;
      win_ry = (int8_t)i;
    }
    if (abs(st.rz) > abs(best_rz)) {
      best_rz = st.rz;
      win_rz = (int8_t)i;
    }
    if (abs(st.slider1) > abs(best_s1)) {
      best_s1 = st.slider1;
      win_s1 = (int8_t)i;
    }
    if (abs(st.slider2) > abs(best_s2)) {
      best_s2 = st.slider2;
      win_s2 = (int8_t)i;
    }

    if (abs(st.x) > THRESH) c_x++;
    if (abs(st.y) > THRESH) c_y++;
    if (abs(st.z) > THRESH) c_z++;
    if (abs(st.rx) > THRESH) c_rx++;
    if (abs(st.ry) > THRESH) c_ry++;
    if (abs(st.rz) > THRESH) c_rz++;
    if (abs(st.slider1) > THRESH) c_s1++;
    if (abs(st.slider2) > THRESH) c_s2++;
  }

  int8_t winners[8] = {win_x, win_y, win_z, win_rx, win_ry, win_rz, win_s1, win_s2};
  bool changed = false;
  for (int i = 0; i < 8; i++) {
    if (winners[i] != s_last_winner[i]) {
      changed = true;
      break;
    }
  }

  if (!periodic && !changed) return;
  s_last_merge_dump_ms = t;
  memcpy(s_last_winner, winners, sizeof(s_last_winner));

  ESP_LOGI(TAG,
           "MERGE winners: X=dev%d Y=dev%d Z=dev%d Rx=dev%d Ry=dev%d Rz=dev%d S1=dev%d S2=dev%d (thr=%d)",
           (int)win_x, (int)win_y, (int)win_z, (int)win_rx, (int)win_ry, (int)win_rz,
           (int)win_s1, (int)win_s2, (int)THRESH);
  log_state_line("MERGE out:", *merged);

  // Collision diagnostics (multiple devices actively moving same axis)
  if (c_z > 1) {
    ESP_LOGW(TAG, "MERGE collision: Z has %d active contributors", c_z);
  }
  if (c_rz > 1) {
    ESP_LOGW(TAG, "MERGE collision: Rz has %d active contributors", c_rz);
  }
  if (c_x > 1) {
    ESP_LOGW(TAG, "MERGE collision: X has %d active contributors", c_x);
  }
  if (c_y > 1) {
    ESP_LOGW(TAG, "MERGE collision: Y has %d active contributors", c_y);
  }
  if (c_rx > 1) {
    ESP_LOGW(TAG, "MERGE collision: Rx has %d active contributors", c_rx);
  }
  if (c_ry > 1) {
    ESP_LOGW(TAG, "MERGE collision: Ry has %d active contributors", c_ry);
  }
  if (c_s1 > 1) {
    ESP_LOGW(TAG, "MERGE collision: Slider1 has %d active contributors", c_s1);
  }
  if (c_s2 > 1) {
    ESP_LOGW(TAG, "MERGE collision: Slider2 has %d active contributors", c_s2);
  }
}

#endif // HID_VERBOSE_HID_DEBUG
