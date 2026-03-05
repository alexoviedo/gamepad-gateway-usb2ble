#include "hid_parser.h"

#include <esp_log.h>
#include <string.h>

static const char *TAG = "HID_PARSER";

struct GlobalState {
  uint16_t usage_page;
  int32_t logical_min;
  int32_t logical_max;
  uint32_t report_size;
  uint32_t report_count;
  uint8_t report_id;
};

#define MAX_USAGES 64
struct LocalState {
  uint16_t usages[MAX_USAGES];
  uint32_t usage_count;
  uint16_t usage_min;
  uint16_t usage_max;
};

static int32_t sign_extend(uint32_t val, int bits) {
  if (bits == 0 || bits > 32)
    return val;
  uint32_t sign_bit = 1ul << (bits - 1);
  if (val & sign_bit) {
    uint32_t mask = 0xFFFFFFFFul << bits;
    return (int32_t)(val | mask);
  }
  return (int32_t)val;
}

void hid_parse_report_descriptor(const uint8_t *desc, size_t len,
                                 HidDeviceCaps *caps) {
  memset(caps, 0, sizeof(HidDeviceCaps));

  GlobalState gstate = {0, 0, 0, 0, 0, 0};
  LocalState lstate = {{0}, 0, 0, 0};

  GlobalState stack[4];
  int stack_ptr = 0;

  uint32_t bit_offsets[3][256]; // 0=Input, 1=Output, 2=Feature
  memset(bit_offsets, 0, sizeof(bit_offsets));

  size_t pos = 0;
  while (pos < len) {
    uint8_t header = desc[pos++];
    int size = header & 0x03;
    if (size == 3)
      size = 4; // size code 3 means 4 bytes

    int type = (header >> 2) & 0x03;
    int tag = (header >> 4) & 0x0F;

    uint32_t data = 0;
    if (size > 0 && pos + size <= len) {
      for (int i = 0; i < size; i++) {
        data |= ((uint32_t)desc[pos + i]) << (i * 8);
      }
    }
    pos += size;

    if (type == 1) { // Global
      switch (tag) {
      case 0:
        gstate.usage_page = data;
        break;
      case 1:
        gstate.logical_min = sign_extend(data, size * 8);
        break;
      case 2:
        gstate.logical_max = sign_extend(data, size * 8);
        break;
      case 7:
        gstate.report_size = data;
        break;
      case 8:
        gstate.report_id = data;
        break;
      case 9:
        gstate.report_count = data;
        break;
      case 10: // Push
        if (stack_ptr < 4)
          stack[stack_ptr++] = gstate;
        break;
      case 11: // Pop
        if (stack_ptr > 0)
          gstate = stack[--stack_ptr];
        break;
      }
    } else if (type == 2) { // Local
      switch (tag) {
      case 0: // Usage
        if (lstate.usage_count < MAX_USAGES) {
          lstate.usages[lstate.usage_count++] = data;
        }
        break;
      case 1:
        lstate.usage_min = data;
        break;
      case 2:
        lstate.usage_max = data;
        break;
      }
    } else if (type == 0) {                    // Main
      if (tag == 8 || tag == 9 || tag == 11) { // Input, Output, Feature
        int coll_type = (tag == 8) ? 0 : (tag == 9 ? 1 : 2);

        // HID Input item flags: bit0=Constant, bit1=Variable, bit2=Relative
        bool is_constant = (data & 0x01);
        bool is_variable = (data & 0x02);
        bool is_relative = (data & 0x04);

        // If it's not constant, record it (we mainly care about Input)
        if (!is_constant && coll_type == 0) {
          uint32_t current_usage_idx = 0;
          uint16_t sequential_usage = lstate.usage_min;

          for (uint32_t i = 0; i < gstate.report_count; i++) {
            uint16_t usage = 0;
            if (lstate.usage_count > 0) {
              usage = lstate.usages[current_usage_idx];
              if (current_usage_idx + 1 < lstate.usage_count)
                current_usage_idx++;
            } else if (lstate.usage_max > 0) {
              usage = sequential_usage;
              if (sequential_usage < lstate.usage_max)
                sequential_usage++;
            }

            if (caps->num_elements < MAX_INPUT_ELEMENTS) {
              InputElement &e = caps->elements[caps->num_elements++];
              memset(&e, 0, sizeof(e));
              e.report_id = gstate.report_id;
              e.bit_offset = (uint16_t)bit_offsets[coll_type][gstate.report_id];
              e.bit_size = (uint16_t)gstate.report_size;
              e.logical_min = gstate.logical_min;
              e.logical_max = gstate.logical_max;
              e.is_signed = (gstate.logical_min < 0);
              e.usage_page = gstate.usage_page;
              e.usage = usage;
              e.is_relative = is_relative ? 1 : 0;
              e.is_absolute = is_relative ? 0 : 1;
              e.is_variable = is_variable ? 1 : 0;
              e.kind = ie_guess_kind(e.usage_page, e.usage);
              e.element_id = ie_compute_id(e.usage_page, e.usage, e.report_id,
                                           e.bit_offset, e.bit_size,
                                           e.logical_min, e.logical_max,
                                           e.is_relative);
            }

            bit_offsets[coll_type][gstate.report_id] += gstate.report_size;
          }
        } else {
          // Constant / Padding: just advance the offset
          bit_offsets[coll_type][gstate.report_id] +=
              (gstate.report_size * gstate.report_count);
        }

        // Clear Local state after Main item
        lstate.usage_count = 0;
        lstate.usage_min = 0;
        lstate.usage_max = 0;
      } else if (tag == 10) { // Collection
        lstate.usage_count = 0;
        lstate.usage_min = 0;
        lstate.usage_max = 0;
      } else if (tag == 12) { // End Collection
        lstate.usage_count = 0;
        lstate.usage_min = 0;
        lstate.usage_max = 0;
      }
    }
  }

    // Role classification (score-based).
  //
  // Background: Many throttle quadrants include a "mini-stick" that exposes X/Y axes.
  // A naive rule like "has X/Y => stick" misclassifies throttles and causes the
  // runtime default mapping to bind primary axes to the wrong device (and can
  // rebind them again as devices connect/disconnect).
  struct RoleMetrics {
    int axes = 0;
    int buttons = 0;
    int hats = 0;
    bool has_x = false;
    bool has_y = false;
    bool has_z = false;
    bool has_rx = false;
    bool has_ry = false;
    bool has_rz = false;
    bool has_slider = false;
    bool has_dial = false;
    int sim_throttle = 0;  // Usage Page 0x02, Usage 0x00BB
    int sim_rudder = 0;    // Usage Page 0x02, Usage 0x00BA
    int sim_brake = 0;     // Usage Page 0x02, Usage 0x00BF
  } rm;

  for (uint32_t i = 0; i < caps->num_elements; i++) {
    const InputElement &e = caps->elements[i];

    if (e.kind == IE_KIND_AXIS) {
      rm.axes++;
      if (e.usage_page == 0x0001) {
        if (e.usage == 0x0030) rm.has_x = true;
        if (e.usage == 0x0031) rm.has_y = true;
        if (e.usage == 0x0032) rm.has_z = true;
        if (e.usage == 0x0033) rm.has_rx = true;
        if (e.usage == 0x0034) rm.has_ry = true;
        if (e.usage == 0x0035) rm.has_rz = true;
        if (e.usage == 0x0036) rm.has_slider = true;
        if (e.usage == 0x0037) rm.has_dial = true;
      }
      if (e.usage_page == 0x0002) {
        if (e.usage == 0x00BB) rm.sim_throttle++;
        if (e.usage == 0x00BA) rm.sim_rudder++;
        if (e.usage == 0x00BF) rm.sim_brake++;
      }
    } else if (e.kind == IE_KIND_BUTTON) {
      rm.buttons++;
    } else if (e.kind == IE_KIND_HAT) {
      rm.hats++;
    }
  }

  int stick_score = 0;
  int throttle_score = 0;
  int pedals_score = 0;

  // Stick score: X/Y + hat + lots of buttons; penalize "too many axes" which is common on throttles.
  if (rm.has_x && rm.has_y) stick_score += 30;
  else if (rm.has_x || rm.has_y) stick_score += 10;
  if (rm.hats > 0) stick_score += 6;
  if (rm.buttons >= 12) stick_score += 8;
  else if (rm.buttons >= 8) stick_score += 6;
  else if (rm.buttons >= 4) stick_score += 2;
  if (rm.has_rz) stick_score += 4;
  if (rm.axes > 5) stick_score -= (rm.axes - 5) * 10;  // big penalty: throttle-like
  if (rm.sim_throttle > 0) stick_score -= 10;
  if (rm.sim_rudder > 0 || rm.sim_brake > 0) stick_score -= 15;

  // Throttle score: throttle/slider/dial + many axes/buttons; tolerate X/Y (mini-stick).
  if (rm.sim_throttle > 0) throttle_score += 35;
  if (rm.has_slider) throttle_score += 12;
  if (rm.has_dial) throttle_score += 6;
  if (rm.axes >= 6) throttle_score += 8;
  if (rm.buttons >= 8) throttle_score += 6;
  if (rm.hats > 0) throttle_score += 2;
  if (rm.has_x && rm.has_y) throttle_score += 2;  // mini-stick present
  if (rm.sim_rudder > 0) throttle_score -= 10;
  if (rm.sim_brake > 0) throttle_score -= 5;

  // Pedals score: rudder/brake usages, ~3 axes, few/no buttons/hats, often Z + Rx/Ry.
  if (rm.sim_rudder > 0) pedals_score += 35;
  if (rm.sim_brake > 0) pedals_score += 20;
  if (rm.axes == 3) pedals_score += 10;
  if (rm.hats == 0) pedals_score += 5;
  if (rm.buttons <= 4) pedals_score += 5;
  if (!rm.has_x && !rm.has_y && (rm.has_z || rm.has_rz) && (rm.has_rx || rm.has_ry)) pedals_score += 10;
  if (rm.buttons >= 8) pedals_score -= 15;
  if (rm.hats > 0) pedals_score -= 10;
  if (rm.sim_throttle > 0) pedals_score -= 10;

  // Pick the best-scoring role.
  caps->role = DeviceRole::UNKNOWN;
  int best = stick_score;
  caps->role = DeviceRole::STICK;
  if (throttle_score > best) {
    best = throttle_score;
    caps->role = DeviceRole::THROTTLE;
  }
  if (pedals_score > best) {
    best = pedals_score;
    caps->role = DeviceRole::PEDALS;
  }

  ESP_LOGI(TAG,
           "Role scores: stick=%d throttle=%d pedals=%d (axes=%d btn=%d hat=%d) -> Role %d",
           stick_score, throttle_score, pedals_score, rm.axes, rm.buttons, rm.hats, (int)caps->role);
}
