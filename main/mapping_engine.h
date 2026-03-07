#pragma once

#include <stdint.h>
#include <stddef.h>

#include "shared_types.h"
#include "input_decoder.h"  // HidDeviceContext

// Deterministic mapping engine.
//
// Purpose:
//   - Stop relying on per-axis "max abs deflection wins" merges.
//   - Produce a final GamepadState deterministically from a MappingProfile.
//   - Default profile is generated at runtime using descriptor + role heuristics.
//
// NOTE: This is intentionally *in-memory only* for now (no persistence / WebBLE UI).

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
// Currently derived from HidDeviceContext::dev_addr (a per-connection handle tag).
using DeviceId = uint32_t;

// ElementId must match InputElement::element_id exactly.
// InputElement uses a 32-bit stable hash, so truncating to 16 bits breaks
// mapped axis/hat lookup while buttons still appear to work (because buttons are
// OR-combined separately from the mapping engine).
using ElementId = uint32_t;

struct AxisSource {
  DeviceId device_id = 0;
  ElementId element_id = 0;
  bool is_valid() const { return device_id != 0; }
};

struct AxisModifiers {
  bool invert = false;
  // deadzone in normalized [-1..1] units (0..1)
  float deadzone = 0.0f;
  // EMA alpha (0 disables).
  float smoothing_alpha = 0.0f;
  // Placeholder for future curve/sensitivity.
  float curve = 1.0f;
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
