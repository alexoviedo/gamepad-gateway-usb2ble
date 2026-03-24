#pragma once
#include "hid_parser.h"
#include "shared_types.h"
#include <stddef.h>
#include <stdint.h>

struct HidDeviceContext {
  HidDeviceCaps caps;
  // Current decoded state from this device alone.
  // This remains available for introspection and debugging, while the canonical
  // merged output state is produced by mapping_engine_compute().
  GamepadState state;
  bool active;
  uint8_t dev_addr; // USB device address to match disconnects (legacy)

  // Cached raw HID report descriptor for WebBLE configuration / inspection.
  //
  // Current hardening policy:
  // - descriptor storage is dynamically allocated at enumeration time
  // - minimum allocation is 1024 bytes
  // - allocation prefers DMA-capable memory so the cache is safe to use with
  //   USB-host-adjacent flows if needed
  // - report_desc_len may be smaller than report_desc_capacity
  static constexpr size_t MIN_HID_REPORT_DESC_CAPACITY = 1024;
  uint16_t report_desc_len;
  uint16_t report_desc_capacity;
  uint8_t *report_desc;

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
