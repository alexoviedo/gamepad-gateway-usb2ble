#pragma once
#include "shared_types.h"
#include <stddef.h>
#include <stdint.h>

// Represents a mapped input field from the HID Report Descriptor
struct HidMappedField {
  uint8_t report_id;
  uint32_t bit_offset;
  uint32_t bit_size;
  int32_t logical_min;
  int32_t logical_max;
  bool is_signed;
  uint16_t usage_page;
  uint16_t usage;
};

// Represents the capabilities of a single connected HID device
struct HidDeviceCaps {
  HidMappedField fields[128];
  size_t num_fields;
  DeviceRole role;
};

// Parse a raw descriptor, return the classified capabilities
void hid_parse_report_descriptor(const uint8_t *desc, size_t len,
                                 HidDeviceCaps *caps);
