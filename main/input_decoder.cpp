#include "input_decoder.h"
#include "hid_verbose.h"
#include <stdlib.h>
#include <string.h>

#if HID_VERBOSE_HID_DEBUG
#include <esp_log.h>
static const char *TAG_DECODE = "HID_DECODE";
#endif

// helper to extract little-endian bits spanning across bytes
static int32_t extract_bits(const uint8_t *report, size_t report_size,
                            uint32_t bit_offset, uint32_t bit_size,
                            bool is_signed) {
  if (bit_size == 0)
    return 0;

  uint32_t value = 0;
  uint32_t current_bit = bit_offset;
  uint32_t remaining = bit_size;
  uint32_t val_shift = 0;

  while (remaining > 0) {
    uint32_t byte_idx = current_bit / 8;
    uint32_t bit_in_byte = current_bit % 8;
    if (byte_idx >= report_size)
      break;

    uint32_t bits_to_read = 8 - bit_in_byte;
    if (bits_to_read > remaining)
      bits_to_read = remaining;

    uint32_t mask = (1 << bits_to_read) - 1;
    uint32_t chunk = (report[byte_idx] >> bit_in_byte) & mask;

    value |= (chunk << val_shift);

    val_shift += bits_to_read;
    current_bit += bits_to_read;
    remaining -= bits_to_read;
  }

  if (is_signed && (bit_size < 32)) {
    uint32_t sign_bit = 1UL << (bit_size - 1);
    if (value & sign_bit) {
      uint32_t sext_mask = 0xFFFFFFFFUL << bit_size;
      return (int32_t)(value | sext_mask);
    }
  }
  return (int32_t)value;
}

static int16_t normalize_axis(int32_t val, int32_t min, int32_t max) {
  if (min >= max)
    return 0;
  if (val <= min)
    return -32767;
  if (val >= max)
    return 32767;

  int64_t range = (int64_t)max - (int64_t)min;
  int64_t v = (int64_t)val - min;
  int64_t mapped = (v * 65534LL) / range;
  mapped -= 32767;
  return (int16_t)mapped;
}

void hid_decode_report(const uint8_t *report, size_t report_size,
                       HidDeviceContext *ctx) {
  uint8_t report_id = 0;
  const uint8_t *payload = report;
  size_t payload_len = report_size;

  // If the device has multiple report IDs, the first byte is the report ID
  // We check if the parsed fields have report_id > 0 to know if we expect one
  bool uses_report_ids = false;
  for (size_t i = 0; i < ctx->caps.num_fields; i++) {
    if (ctx->caps.fields[i].report_id > 0) {
      uses_report_ids = true;
      break;
    }
  }

  if (uses_report_ids && report_size > 0) {
    report_id = report[0];
    payload = report + 1;
    payload_len = report_size - 1;
  }

#if HID_VERBOSE_HID_DEBUG
  // Track "multiple usages mapped to the same internal axis" within ONE decoded report.
  // This is critical for diagnosing cases like Generic Desktop Z (0x01/0x32) and
  // Simulation Rudder (0x02/0xBA) both writing into ctx->state.z.
  struct AxisWriteInfo {
    bool written;
    uint16_t usage_page;
    uint16_t usage;
  };
  AxisWriteInfo w_x{false, 0, 0}, w_y{false, 0, 0}, w_z{false, 0, 0}, w_rx{false, 0, 0},
      w_ry{false, 0, 0}, w_rz{false, 0, 0}, w_s1{false, 0, 0}, w_s2{false, 0, 0},
      w_hat{false, 0, 0};

  auto note_axis_write = [&](const char *axis, AxisWriteInfo &w, uint16_t up,
                             uint16_t u) {
    if (!w.written) {
      w.written = true;
      w.usage_page = up;
      w.usage = u;
      return;
    }
    // Same usage writing again is noise; only log when a different usage overwrites.
    if (w.usage_page != up || w.usage != u) {
      ESP_LOGW(TAG_DECODE,
               "DEV[%u] addr=%u report_id=%u: AXIS COLLISION %s overwritten (%04X/%04X -> %04X/%04X)",
               (unsigned)ctx->slot_id, (unsigned)ctx->usb_addr, (unsigned)report_id,
               axis, (unsigned)w.usage_page, (unsigned)w.usage, (unsigned)up,
               (unsigned)u);
      w.usage_page = up;
      w.usage = u;
    }
  };
#endif

  // Clear transient button state
  ctx->state.buttons = 0;

  for (size_t i = 0; i < ctx->caps.num_fields; i++) {
    const auto &f = ctx->caps.fields[i];
    if (f.report_id != report_id)
      continue;

    int32_t raw_val = extract_bits(payload, payload_len, f.bit_offset,
                                   f.bit_size, f.is_signed);

    if (f.usage_page == 0x01) { // Desktop
      if (f.usage == 0x30)
      {
#if HID_VERBOSE_HID_DEBUG
        note_axis_write("X", w_x, f.usage_page, f.usage);
#endif
        ctx->state.x = normalize_axis(raw_val, f.logical_min, f.logical_max);
      }
      else if (f.usage == 0x31)
      {
#if HID_VERBOSE_HID_DEBUG
        note_axis_write("Y", w_y, f.usage_page, f.usage);
#endif
        ctx->state.y = normalize_axis(raw_val, f.logical_min, f.logical_max);
      }
      else if (f.usage == 0x32)
      {
#if HID_VERBOSE_HID_DEBUG
        note_axis_write("Z", w_z, f.usage_page, f.usage);
#endif
        ctx->state.z = normalize_axis(raw_val, f.logical_min, f.logical_max);
      }
      else if (f.usage == 0x33)
      {
#if HID_VERBOSE_HID_DEBUG
        note_axis_write("Rx", w_rx, f.usage_page, f.usage);
#endif
        ctx->state.rx = normalize_axis(raw_val, f.logical_min, f.logical_max);
      }
      else if (f.usage == 0x34)
      {
#if HID_VERBOSE_HID_DEBUG
        note_axis_write("Ry", w_ry, f.usage_page, f.usage);
#endif
        ctx->state.ry = normalize_axis(raw_val, f.logical_min, f.logical_max);
      }
      else if (f.usage == 0x35)
      {
#if HID_VERBOSE_HID_DEBUG
        note_axis_write("Rz", w_rz, f.usage_page, f.usage);
#endif
        ctx->state.rz = normalize_axis(raw_val, f.logical_min, f.logical_max);
      }
      else if (f.usage == 0x36)
      {
#if HID_VERBOSE_HID_DEBUG
        note_axis_write("Slider1", w_s1, f.usage_page, f.usage);
#endif
	      const int16_t v = normalize_axis(raw_val, f.logical_min, f.logical_max);
	      ctx->state.slider1 = v;

	      // Quirk: For Thrustmaster composite throttle/pedals (VID:PID 044F:B687),
	      // we've observed the physical rudder deflection come in on Slider1 (0x36)
	      // while Generic Desktop Z (0x32) may appear stuck at max.
	      // Mirror Slider1 into Rz so the BLE gamepad exposes a usable "Z Rotation" axis.
	      if (ctx->vid == 0x044F && ctx->pid == 0xB687 && report_id == 1) {
	        ctx->state.rz = v;
	      }
      }
      else if (f.usage == 0x39) { // Hat
        if (raw_val >= f.logical_min && raw_val <= f.logical_max) {
          // Usually 8-way hat starts at 0 = N, 1=NE, etc
          int normalized_hat = (raw_val - f.logical_min) + 1;
#if HID_VERBOSE_HID_DEBUG
          note_axis_write("Hat", w_hat, f.usage_page, f.usage);
#endif
          ctx->state.hat = normalized_hat;
        } else {
          ctx->state.hat = 0; // centered
        }
      }
    } else if (f.usage_page == 0x02) { // Sim
      if (f.usage == 0xBA)
      {
#if HID_VERBOSE_HID_DEBUG
        note_axis_write("Z", w_z, f.usage_page, f.usage); // Rudder -> Z
#endif
        ctx->state.z = normalize_axis(raw_val, f.logical_min, f.logical_max);
      }
      else if (f.usage == 0xBB)
      {
#if HID_VERBOSE_HID_DEBUG
        note_axis_write("Slider1", w_s1, f.usage_page, f.usage);
#endif
        ctx->state.slider1 = normalize_axis(raw_val, f.logical_min, f.logical_max);
      }
      else if (f.usage == 0xBF)
      {
#if HID_VERBOSE_HID_DEBUG
        note_axis_write("Slider2", w_s2, f.usage_page, f.usage);
#endif
        ctx->state.slider2 = normalize_axis(raw_val, f.logical_min, f.logical_max);
      }
    } else if (f.usage_page == 0x09) {                             // Buttons
      if (raw_val) {
        int btn_idx = f.usage - 1;
        if (btn_idx >= 0 && btn_idx < 32) {
          ctx->state.buttons |= (1UL << btn_idx);
        }
      }
    }
  }
}

void hid_merge_states(const HidDeviceContext *contexts, size_t num_contexts,
                      GamepadState *out_merged) {
  memset(out_merged, 0, sizeof(GamepadState));

  for (size_t i = 0; i < num_contexts; i++) {
    if (!contexts[i].active)
      continue;

    const GamepadState &st = contexts[i].state;

    // Buttons are bitwise OR'd
    out_merged->buttons |= st.buttons;

    // Hat is prioritized ( erste non-center hat wins )
    if (out_merged->hat == 0 && st.hat != 0) {
      out_merged->hat = st.hat;
    }

    // For axes, we take the one with the largest absolute deflection from
    // center (0) This allows a joystick and a mini-stick on a throttle to share
    // X/Y without overriding each other completely, or pedals to map cleanly.

    if (abs(st.x) > abs(out_merged->x))
      out_merged->x = st.x;
    if (abs(st.y) > abs(out_merged->y))
      out_merged->y = st.y;
    if (abs(st.z) > abs(out_merged->z))
      out_merged->z = st.z;

    if (abs(st.rx) > abs(out_merged->rx))
      out_merged->rx = st.rx;
    if (abs(st.ry) > abs(out_merged->ry))
      out_merged->ry = st.ry;
    if (abs(st.rz) > abs(out_merged->rz))
      out_merged->rz = st.rz;

    if (abs(st.slider1) > abs(out_merged->slider1))
      out_merged->slider1 = st.slider1;
    if (abs(st.slider2) > abs(out_merged->slider2))
      out_merged->slider2 = st.slider2;
  }
}
