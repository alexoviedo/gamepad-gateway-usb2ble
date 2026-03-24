# Firmware: HID Pipeline

## Purpose
Document USB HID enumeration, descriptor parsing, runtime decoding, and deterministic output-state generation.

Related documents:
- [../interfaces/HID_TO_GAMEPAD_MAPPING.md](../interfaces/HID_TO_GAMEPAD_MAPPING.md)
- [../interfaces/BLE_CONTRACTS.md](../interfaces/BLE_CONTRACTS.md)
- [RUN_MODE_HID.md](RUN_MODE_HID.md)

## Dependencies
- `main/usb_host_manager.cpp`
- `main/hid_device_manager.cpp`
- `main/input_decoder.h`
- `main/input_elements.h`
- `main/mapping_engine.cpp`
- `main/shared_types.h`

## Data Structures/Interfaces
### Primary structs
#### `InputElement`
- metadata: `element_id`, `kind`, `usage_page`, `usage`, `report_id`, `bit_offset`, `bit_size`, logical ranges, flags
- runtime: `raw`, `norm_0_1`, `norm_m1_1`, `last_update_ms`

#### `HidDeviceContext`
- `caps: HidDeviceCaps`
- `state: GamepadState`
- `active: bool`
- `dev_addr: uint8`
- `report_desc_len: uint16`
- `report_desc[1024]`
- `last_report_ms`, `last_sample_log_ms`

### Runtime flow
1. USB host daemon handles library events.
2. HID host opens a device and fetches its report descriptor.
3. Descriptor parser populates `InputElement[]` and role heuristics.
4. Input reports update per-device runtime values.
5. Deterministic mapping engine computes merged `GamepadState`.

### Device identity
- `device_id = dev_addr + 1`
- `0` is reserved as invalid
- stable only while the device remains connected in the current session

## Expected State Changes
- On connect: descriptor cached, device marked active, mapping invalidated/regenerated when using defaults.
- On report: runtime values update and merged state recomputes.
- On disconnect: device context clears and merged state recomputes.

## Known Limitations
- `device_id` is session-local and not a persistent hardware identity.
- Descriptor cache is capped at 1024 bytes.
