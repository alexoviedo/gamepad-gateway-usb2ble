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

        bool is_constant = (data & 0x01);

        // If it's not constant, map it!
        if (!is_constant && coll_type == 0) { // We mainly care about Input
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

            bool map_it = false;
            if (gstate.usage_page == 0x01) { // Generic Desktop
              if (usage >= 0x30 && usage <= 0x39)
                map_it = true;                      // X,Y,Z,Rx,Ry,Rz,Slider,Hat
            } else if (gstate.usage_page == 0x02) { // Simulation
              if (usage == 0xBA || usage == 0xBB || usage == 0xBF)
                map_it = true; // Rudder, Throttle, ToeBrake
            } else if (gstate.usage_page == 0x09) { // Buttons
              map_it = true;
            }

            if (map_it && caps->num_fields < 128) {
              HidMappedField &f = caps->fields[caps->num_fields++];
              f.report_id = gstate.report_id;
              f.bit_offset = bit_offsets[coll_type][gstate.report_id];
              f.bit_size = gstate.report_size;
              f.logical_min = gstate.logical_min;
              f.logical_max = gstate.logical_max;
              f.is_signed = (gstate.logical_min < 0);
              f.usage_page = gstate.usage_page;
              f.usage = usage;
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

  // Simple classification logic
  int sticks = 0, throttles = 0, pedals = 0, hats = 0, buttons = 0;
  for (size_t i = 0; i < caps->num_fields; i++) {
    const auto &f = caps->fields[i];
    if (f.usage_page == 0x01 && (f.usage == 0x30 || f.usage == 0x31))
      sticks++;
    if (f.usage_page == 0x02 && f.usage == 0xBB)
      throttles++; // Throttle
    if (f.usage_page == 0x01 && f.usage == 0x36)
      throttles++; // Slider often acts as throttle
    if (f.usage_page == 0x02 && (f.usage == 0xBA || f.usage == 0xBF))
      pedals++; // Rudder/Brake
    if (f.usage_page == 0x09)
      buttons++;
    if (f.usage_page == 0x01 && f.usage == 0x39)
      hats++;
  }

  if (pedals > 0 && sticks == 0)
    caps->role = DeviceRole::PEDALS;
  else if (throttles > 0 && sticks == 0 && hats == 0)
    caps->role = DeviceRole::THROTTLE;
  else if (sticks > 0)
    caps->role = DeviceRole::STICK;
  else if (buttons > 0)
    caps->role = DeviceRole::STICK; // fallback
  else
    caps->role = DeviceRole::UNKNOWN;

  ESP_LOGI(TAG, "Classified device as Role %d", (int)caps->role);
}
