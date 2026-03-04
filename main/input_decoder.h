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
  uint8_t dev_addr; // USB device address to match disconnects
};

// Decode a raw HID report from a device into its GamepadState
void hid_decode_report(const uint8_t *report, size_t report_size,
                       HidDeviceContext *ctx);

// State Merger: Merge all active device states into a single unified state
// Applies preference logic (e.g. throttle from throttle, stick from stick)
void hid_merge_states(const HidDeviceContext *contexts, size_t num_contexts,
                      GamepadState *out_merged);
