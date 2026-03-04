#include "input_decoder.h"
#include <stdlib.h>
#include <string.h>

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
        ctx->state.x = normalize_axis(raw_val, f.logical_min, f.logical_max);
      else if (f.usage == 0x31)
        ctx->state.y = normalize_axis(raw_val, f.logical_min, f.logical_max);
      else if (f.usage == 0x32)
        ctx->state.z = normalize_axis(raw_val, f.logical_min, f.logical_max);
      else if (f.usage == 0x33)
        ctx->state.rx = normalize_axis(raw_val, f.logical_min, f.logical_max);
      else if (f.usage == 0x34)
        ctx->state.ry = normalize_axis(raw_val, f.logical_min, f.logical_max);
      else if (f.usage == 0x35)
        ctx->state.rz = normalize_axis(raw_val, f.logical_min, f.logical_max);
      else if (f.usage == 0x36)
        ctx->state.slider1 =
            normalize_axis(raw_val, f.logical_min, f.logical_max);
      else if (f.usage == 0x39) { // Hat
        if (raw_val >= f.logical_min && raw_val <= f.logical_max) {
          // Usually 8-way hat starts at 0 = N, 1=NE, etc
          int normalized_hat = (raw_val - f.logical_min) + 1;
          ctx->state.hat = normalized_hat;
        } else {
          ctx->state.hat = 0; // centered
        }
      }
    } else if (f.usage_page == 0x02) { // Sim
      if (f.usage == 0xBA)
        ctx->state.z = normalize_axis(raw_val, f.logical_min,
                                      f.logical_max); // Rudder -> Z
      else if (f.usage == 0xBB)
        ctx->state.slider1 = normalize_axis(
            raw_val, f.logical_min, f.logical_max); // Throttle -> Slider
      else if (f.usage == 0xBF)
        ctx->state.slider2 =
            normalize_axis(raw_val, f.logical_min, f.logical_max); // ToeBrake
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
