#pragma once
#include "hid_parser.h"
#include "shared_types.h"
#include "hid_device_identity.h"
#include <stddef.h>
#include <stdint.h>

struct HidDeviceContext {
  HidDeviceCaps caps;
  // Current decoded state from this device alone
  GamepadState state;
  bool active;

  // Legacy matching field used by existing code paths.
  // (Kept to avoid behavior changes outside Phase 0 instrumentation.)
  uint8_t dev_addr;

  // --- Identification (best effort) ---
  uint8_t slot_id;   // Stable enough for this phase (slot index)
  uint8_t usb_addr;  // USB address (if available)
  uint8_t iface_num; // HID interface number (if available)

  uint16_t vid; // USB VID (best effort; 0 if unknown)
  uint16_t pid; // USB PID (best effort; 0 if unknown)

  HidDeviceIdentity identity; // Canonical device identity

  // Stored handle tag (do not dereference; used only for matching)
  uintptr_t hid_handle_tag;

  // --- Debug report stats (only meaningful when verbose debug enabled) ---
  uint32_t dbg_report_count;
  uint8_t dbg_last_report_id;
  uint16_t dbg_last_report_len;
  uint32_t dbg_last_report_ms;
  float dbg_report_hz_ema;

  // Snapshot of the most recent raw input report bytes (debug-only usage).
  // We store only the first N bytes to keep logs manageable.
  uint8_t dbg_raw_report[32];
  uint8_t dbg_raw_report_cap_len; // number of bytes stored in dbg_raw_report
  uint8_t dbg_uses_report_ids;    // 0/1

  // Caller-managed last per-device dump timestamp (ms)
  uint32_t dbg_last_state_dump_ms;
};

// Decode a raw HID report from a device into its GamepadState
void hid_decode_report(const uint8_t *report, size_t report_size,
                       HidDeviceContext *ctx);

// State Merger: Merge all active device states into a single unified state
// Applies preference logic (e.g. throttle from throttle, stick from stick)
void hid_merge_states(const HidDeviceContext *contexts, size_t num_contexts,
                      GamepadState *out_merged);
