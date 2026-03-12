#pragma once
#include "hid_parser.h"
#include "shared_types.h"
#include <stddef.h>
#include <stdint.h>

struct HidDeviceContext {
  HidDeviceCaps caps;
  // Current decoded state from this device alone
  GamepadState state;
  bool active;
  uint8_t dev_addr; // USB device address to match disconnects (legacy)

  // Cached raw HID report descriptor for WebBLE configuration / inspection.
  // NOTE: This is copied at enumeration time. If the descriptor is larger than
  // MAX_HID_REPORT_DESC_LEN, it is truncated.
  static constexpr size_t MAX_HID_REPORT_DESC_LEN = 1024;
  uint16_t report_desc_len;
  uint8_t report_desc[MAX_HID_REPORT_DESC_LEN];

  // Last time any input element changed (ms since boot)
  uint32_t last_report_ms;

  // For optional debug sampling logs (rate limit)
  uint32_t last_sample_log_ms;
};

// Decode a raw HID report from a device into its GamepadState
void hid_decode_report(const uint8_t *report, size_t report_size,
                       HidDeviceContext *ctx);

// State Merger: Merge all active device states into a single unified state
// Applies preference logic (e.g. throttle from throttle, stick from stick)
void hid_merge_states(const HidDeviceContext *contexts, size_t num_contexts,
                      GamepadState *out_merged);
