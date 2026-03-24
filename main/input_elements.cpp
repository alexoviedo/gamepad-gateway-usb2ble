#include "input_elements.h"

#include <string.h>

static uint32_t fnv1a32(const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

const char *ie_kind_str(InputElementKind k) {
  switch (k) {
  case InputElementKind::AXIS:
    return "axis";
  case InputElementKind::BUTTON:
    return "button";
  case InputElementKind::HAT:
    return "hat";
  default:
    return "other";
  }
}

InputElementKind ie_guess_kind(uint16_t usage_page, uint16_t usage) {
  if (usage_page == 0x09) return InputElementKind::BUTTON;
  if (usage_page == 0x01 && usage == 0x39) return InputElementKind::HAT;
  if (usage_page == 0x01 && usage >= 0x30 && usage <= 0x38) return InputElementKind::AXIS;
  if (usage_page == 0x02) return InputElementKind::AXIS;
  return InputElementKind::OTHER;
}

const char *ie_friendly_usage(uint16_t usage_page, uint16_t usage) {
  if (usage_page == 0x01) {
    switch (usage) {
    case 0x30: return "X";
    case 0x31: return "Y";
    case 0x32: return "Z";
    case 0x33: return "Rx";
    case 0x34: return "Ry";
    case 0x35: return "Rz";
    case 0x36: return "Slider";
    case 0x37: return "Dial";
    case 0x38: return "Wheel";
    case 0x39: return "Hat";
    default: break;
    }
  }

  if (usage_page == 0x02) {
    switch (usage) {
    case 0xBA: return "Rudder";
    case 0xBB: return "Throttle";
    case 0xBF: return "ToeBrake";
    default: break;
    }
  }

  if (usage_page == 0x09) return "Button";
  return nullptr;
}

uint32_t ie_compute_id(uint16_t usage_page, uint16_t usage, uint8_t report_id,
                       uint16_t bit_offset, uint16_t bit_size,
                       int32_t logical_min, int32_t logical_max,
                       uint8_t is_relative) {
  struct {
    uint16_t up;
    uint16_t u;
    uint8_t rid;
    uint16_t bo;
    uint16_t bs;
    int32_t lmin;
    int32_t lmax;
    uint8_t rel;
  } k;
  memset(&k, 0, sizeof(k));
  k.up = usage_page;
  k.u = usage;
  k.rid = report_id;
  k.bo = bit_offset;
  k.bs = bit_size;
  k.lmin = logical_min;
  k.lmax = logical_max;
  k.rel = is_relative;
  return fnv1a32(&k, sizeof(k));
}

static uint32_t extract_bits_le(const uint8_t *data, uint32_t bit_offset, uint32_t bit_size) {
  if (!data || bit_size == 0) return 0;
  if (bit_size > 32) bit_size = 32;

  uint64_t value = 0;
  for (uint32_t i = 0; i < bit_size; i++) {
    const uint32_t byte_index = (bit_offset + i) / 8;
    const uint32_t bit_index = (bit_offset + i) % 8;
    const uint8_t bit = (data[byte_index] >> bit_index) & 0x01;
    value |= (uint64_t)bit << i;
  }
  return (uint32_t)value;
}

static int32_t sign_extend(uint32_t v, uint32_t bits) {
  if (bits == 0 || bits >= 32) return (int32_t)v;
  const uint32_t m = 1u << (bits - 1);
  const uint32_t mask = (1u << bits) - 1u;
  v &= mask;
  return (int32_t)((v ^ m) - m);
}

static void normalize_element(InputElement *e) {
  e->norm_0_1 = 0.0f;
  e->norm_m1_1 = 0.0f;

  int32_t minv = e->logical_min;
  int32_t maxv = e->logical_max;
  if (maxv == minv) return;

  int32_t rv = e->raw;
  if (rv < minv) rv = minv;
  if (rv > maxv) rv = maxv;

  float denom = (float)(maxv - minv);
  if (denom == 0.0f) return;
  e->norm_0_1 = (float)(rv - minv) / denom;
  if (e->norm_0_1 < 0.0f) e->norm_0_1 = 0.0f;
  if (e->norm_0_1 > 1.0f) e->norm_0_1 = 1.0f;
  e->norm_m1_1 = (e->norm_0_1 * 2.0f) - 1.0f;
}

void input_elements_decode_report(InputElement *elements, size_t n,
                                  const uint8_t *report, size_t len,
                                  uint32_t now_ms) {
  if (!elements || n == 0 || !report || len == 0) return;

  bool uses_report_ids = false;
  for (size_t i = 0; i < n; i++) {
    if (elements[i].report_id > 0) {
      uses_report_ids = true;
      break;
    }
  }

  uint8_t report_id = 0;
  const uint8_t *payload = report;
  size_t payload_len = len;
  if (uses_report_ids) {
    report_id = report[0];
    payload = report + 1;
    payload_len = (len > 0) ? (len - 1) : 0;
  }
  if (payload_len == 0) return;

  for (size_t i = 0; i < n; i++) {
    InputElement *e = &elements[i];
    if (uses_report_ids && e->report_id != report_id) continue;
    if (e->bit_size == 0) continue;

    uint32_t last_bit = (uint32_t)e->bit_offset + (uint32_t)e->bit_size;
    uint32_t payload_bits = (uint32_t)payload_len * 8u;
    if (last_bit > payload_bits) continue;

    uint32_t raw_u = extract_bits_le(payload, e->bit_offset, e->bit_size);
    int32_t raw_s = e->is_signed ? sign_extend(raw_u, e->bit_size) : (int32_t)raw_u;

    if (raw_s != e->raw) {
      e->raw = raw_s;
      e->last_update_ms = now_ms;
      normalize_element(e);
    }
  }
}
