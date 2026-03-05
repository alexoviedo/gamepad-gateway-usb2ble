#pragma once

#include <stddef.h>
#include <stdint.h>

// Maximum number of descriptor-derived INPUT elements per HID interface.
// Keep modest to avoid excessive RAM use on ESP32-S3.
#ifndef MAX_INPUT_ELEMENTS
#define MAX_INPUT_ELEMENTS 256
#endif

typedef enum {
  IE_KIND_AXIS = 0,
  IE_KIND_BUTTON = 1,
  IE_KIND_HAT = 2,
  IE_KIND_OTHER = 3,
} InputElementKind;

// A generic descriptor-derived INPUT element.
//
// This is the central transition schema for "USB report -> elements -> adapter -> GamepadState".
//
// Metadata comes from the HID report descriptor.
// Runtime values are updated by decoding input reports.
typedef struct {
  // Stable within a device/interface descriptor. (Hash of stable metadata)
  uint32_t element_id;

  // What kind of control this element most closely resembles.
  InputElementKind kind;

  // HID usage information
  uint16_t usage_page;
  uint16_t usage;

  // Report location
  uint8_t report_id;      // 0 if no report IDs are used
  uint16_t bit_offset;    // offset into the report payload (excluding report ID byte)
  uint16_t bit_size;      // size in bits

  // Logical range (descriptor)
  int32_t logical_min;
  int32_t logical_max;
  uint8_t is_signed;      // 1 if sign-extended extraction is required

  // Behavior flags (descriptor Input item)
  uint8_t is_relative;    // 1 if Relative
  uint8_t is_absolute;    // 1 if Absolute
  uint8_t is_variable;    // 1 if Variable (vs Array)

  // Runtime decoded values
  int32_t raw;            // extracted, sign-extended as needed
  float norm_0_1;         // normalized to [0..1] using logical min/max (when possible)
  float norm_m1_1;        // normalized to [-1..1] derived from norm_0_1
  uint32_t last_update_ms;
} InputElement;

// Friendly label for a subset of common usages. Returns NULL for unknown usages.
const char *ie_friendly_usage(uint16_t usage_page, uint16_t usage);

// Convert kind to short string.
const char *ie_kind_str(InputElementKind k);

// Heuristic classification for kind.
InputElementKind ie_guess_kind(uint16_t usage_page, uint16_t usage);

// Stable-ish 32-bit element id derived from metadata.
uint32_t ie_compute_id(uint16_t usage_page, uint16_t usage, uint8_t report_id,
                       uint16_t bit_offset, uint16_t bit_size,
                       int32_t logical_min, int32_t logical_max,
                       uint8_t is_relative);

// Decode a single input report into the element table.
// - elements/n: descriptor-derived elements
// - report/len: raw report bytes from HID host
// - now_ms: monotonic timestamp in milliseconds
void input_elements_decode_report(InputElement *elements, size_t n,
                                  const uint8_t *report, size_t len,
                                  uint32_t now_ms);
