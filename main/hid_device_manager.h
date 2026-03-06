#pragma once
#include "shared_types.h"
#include "input_elements.h"

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------
// WebBLE / Config Introspection
// ------------------------------
// These APIs expose a *read-only* snapshot of connected device metadata and
// descriptor-derived element tables so a configuration UI can inspect and map
// controls.

typedef struct {
  // Stable while connected. Matches mapping_engine's DeviceId convention.
  // (dev_addr + 1), where 0 is reserved as invalid.
  uint32_t device_id;
  uint8_t dev_addr;
  uint8_t role;         // DeviceRole numeric value
  uint16_t num_elements;
  uint16_t report_desc_len;  // cached report descriptor length (may be truncated)
} HidDeviceInfo;

// Enumerate active devices into out_infos. Returns count written.
size_t hid_device_manager_list_devices(HidDeviceInfo *out_infos, size_t max_infos);

// Copy cached report descriptor for a device_id. Returns bytes copied.
size_t hid_device_manager_get_report_descriptor(uint32_t device_id, uint8_t *out_buf, size_t max_len);

// Copy the descriptor-derived InputElement table (includes runtime values).
// Returns elements copied.
size_t hid_device_manager_get_elements(uint32_t device_id, InputElement *out_elems, size_t max_elems);

// Copy the latest per-device GamepadState (decoded from that device alone).
bool hid_device_manager_get_device_state(uint32_t device_id, struct GamepadState *out_state);

// Initialize the HID Host Driver and connection callbacks
void hid_device_manager_init(void);

// Get the latest merged state (thread-safe, lock-free or protected)
void hid_device_manager_get_merged_state(struct GamepadState *out_state);

#ifdef __cplusplus
}
#endif
