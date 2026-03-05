#include "input_decoder.h"
#include "input_elements.h"

#include <esp_timer.h>

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

static uint8_t normalize_hat(int32_t raw, int32_t minVal, int32_t maxVal) {
  if (raw < minVal || raw > maxVal) return 0;
  int32_t v = (raw - minVal) + 1;
  if (v < 0) v = 0;
  if (v > 8) v = 8;
  return (uint8_t)v;
}

// Adapter layer (temporary): map common HID usages into the legacy fixed GamepadState.
static void adapt_elements_to_gamepad_state(const HidDeviceCaps *caps, GamepadState *out) {
  if (!caps || !out) return;
  GamepadState s = {};

  for (size_t i = 0; i < caps->num_elements; i++) {
    const InputElement &e = caps->elements[i];

    // Buttons
    if (e.usage_page == 0x09) {
      if (e.usage >= 1 && e.usage <= 32) {
        if (e.raw != 0) s.buttons |= (1u << (e.usage - 1));
      }
      continue;
    }

    // Hat
    if (e.usage_page == 0x01 && e.usage == 0x39) {
      s.hat = normalize_hat(e.raw, e.logical_min, e.logical_max);
      continue;
    }

    // Generic Desktop axes
    if (e.usage_page == 0x01) {
      switch (e.usage) {
      case 0x30:
        s.x = normalize_axis(e.raw, e.logical_min, e.logical_max);
        break;
      case 0x31:
        s.y = normalize_axis(e.raw, e.logical_min, e.logical_max);
        break;
      case 0x32:
        s.z = normalize_axis(e.raw, e.logical_min, e.logical_max);
        break;
      case 0x33:
        s.rx = normalize_axis(e.raw, e.logical_min, e.logical_max);
        break;
      case 0x34:
        s.ry = normalize_axis(e.raw, e.logical_min, e.logical_max);
        break;
      case 0x35:
        s.rz = normalize_axis(e.raw, e.logical_min, e.logical_max);
        break;
      case 0x36:
        s.slider1 = normalize_axis(e.raw, e.logical_min, e.logical_max);
        break;
      default:
        break;
      }
      continue;
    }

    // Simulation Controls axes
    if (e.usage_page == 0x02) {
      switch (e.usage) {
      case 0xBA: // Rudder
        s.z = normalize_axis(e.raw, e.logical_min, e.logical_max);
        break;
      case 0xBB: // Throttle
        s.slider1 = normalize_axis(e.raw, e.logical_min, e.logical_max);
        break;
      case 0xBF: // ToeBrake
        s.slider2 = normalize_axis(e.raw, e.logical_min, e.logical_max);
        break;
      default:
        break;
      }
      continue;
    }
  }

  *out = s;
}

void hid_decode_report(const uint8_t *report, size_t report_size,
                       HidDeviceContext *ctx) {
  if (!ctx || !ctx->active || !report || report_size == 0) return;

  uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
  ctx->last_report_ms = now_ms;

  // 1) Decode into descriptor-derived generic element table
  input_elements_decode_report(ctx->caps.elements, ctx->caps.num_elements, report,
                               report_size, now_ms);

  // 2) Temporary adapter to legacy GamepadState
  adapt_elements_to_gamepad_state(&ctx->caps, &ctx->state);
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
