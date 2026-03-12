#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string>

#include "shared_types.h"
#include "input_decoder.h"  // HidDeviceContext

// Deterministic mapping engine.
//
// Purpose:
//   - Stop relying on per-axis "max abs deflection wins" merges.
//   - Produce a final GamepadState deterministically from a MappingProfile.
//   - Default profile is generated at runtime using descriptor + role heuristics.
//
// NOTE: Active mappings are applied in-memory immediately and can be serialized for NVS persistence.

namespace mapping {

// The BLE report supports these outputs today (see ble_gamepad.cpp report map).
enum class OutputAxis : uint8_t {
  X = 0,
  Y,
  Z,
  RX,
  RY,
  RZ,
  SLIDER1,
  SLIDER2,
  HAT,
  COUNT,
};

// Device key for this connection. Best-effort and deterministic while connected.
// Stable for the lifetime of a connected HID interface.
using DeviceId = uint32_t;

// ElementId is stable within a device (assigned by the report descriptor parser).
using ElementId = uint32_t;

struct AxisSource {
  DeviceId device_id = 0;
  ElementId element_id = 0;
  bool is_valid() const { return device_id != 0; }
};

struct AxisModifiers {
  bool invert = false;
  // Inner deadzone in normalized units (0..0.99).
  float deadzone_inner = 0.0f;
  // Optional outer clamp region near the extremes (0..0.99).
  // Example: 0.05 means values beyond 95% are treated as full scale.
  float outer_clamp = 0.0f;
  // EMA alpha (0 disables).
  float smoothing_alpha = 0.0f;
  // Cubic-bezier response curve control points.
  // Endpoints are fixed at (0,0) and (1,1).
  float bezier_p1x = 0.25f;
  float bezier_p1y = 0.25f;
  float bezier_p2x = 0.75f;
  float bezier_p2y = 0.75f;
};

struct AxisMapping {
  bool configured = false;
  AxisSource source;
  AxisModifiers mod;
};

struct MappingProfile {
  AxisMapping axes[(size_t)OutputAxis::COUNT];
  bool buttons_or_combine = true;  // default behavior today

  void clear();
};

// Public API
// Human-readable canonical axis key names used by the WebBLE config schema.
const char *axis_name(OutputAxis axis);

// Parse / serialize the active in-memory profile.
// Schema (current phase):
// {
//   "version": 1,
//   "buttons_or_combine": true,
//   "axes": {
//     "z": {
//       "configured": true,
//       "device_id": 101,
//       "element_id": 54909,
//       "invert": false,
//       "deadzone": { "inner": 0.03, "outer": 0.02 },
//       "smoothing_alpha": 0.2,
//       "curve": {
//         "type": "bezier",
//         "p1": { "x": 0.25, "y": 0.15 },
//         "p2": { "x": 0.75, "y": 0.95 }
//       }
//     }
//   }
// }
std::string mapping_engine_profile_to_json();
bool mapping_engine_apply_profile_json(const char *json, size_t len, std::string *error_out);

// Re-apply the current profile on the next compute without regenerating defaults.
void mapping_engine_mark_profile_dirty();

void mapping_engine_init();

// Mark that the set of connected devices changed (connect/disconnect).
void mapping_engine_notify_devices_changed();

// Compute final output state deterministically.
// Call this after decoding reports into per-device InputElements.
void mapping_engine_compute(const HidDeviceContext *devices, size_t num_devices,
                            GamepadState *out);

// For debugging/validation: print the active mapping (once per profile generation).
void mapping_engine_log_profile();

}  // namespace mapping
