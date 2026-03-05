#pragma once
#include "shared_types.h"
#include "input_elements.h"
#include <stddef.h>
#include <stdint.h>

// Represents the capabilities of a single connected HID device
struct HidDeviceCaps {
  // Descriptor-derived generic INPUT elements (all non-constant INPUT fields)
  InputElement elements[MAX_INPUT_ELEMENTS];
  size_t num_elements;
  DeviceRole role;
};

// Parse a raw descriptor, return the classified capabilities
void hid_parse_report_descriptor(const uint8_t *desc, size_t len,
                                 HidDeviceCaps *caps);
