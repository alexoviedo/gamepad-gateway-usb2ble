# Architecture Hardening Pass Assessment & Strategy

## 1. Codebase Assessment

The current ESP32-S3 firmware and frontend webapp are mostly functional but have several key weaknesses around safety, contracts, and type soundness:

- **Type Safety**:
  - `shared_types.h` uses plain `uint8_t` for `hat` and bitmask for `buttons` where strongly typed structures/enums could prevent invalid assignments.
  - Usage of `cJSON` leads to repetitive weakly typed parsing scattered through `mapping_engine.cpp` and `ble_config_service.cpp`.
  - Frontend (`webapp/app.js`) parses `JSON.parse()` without validating the shape, relying solely on defensive accessing.
- **Schema Validation**:
  - Missing strict schema boundary checks for BLE incoming configuration payloads and profile saves (NVS).
  - Webapp consumes BLE payloads without ensuring required fields exist.
- **Design by Contract**:
  - Missing precondition asserts in key math/conversion functions (e.g., `mapping_engine` curve applications, deadzones).
  - `input_elements_decode_report` has implicit assumptions about the report length matching the descriptor layout.
- **Interface Integrity**:
  - `hid_device_manager.h` exposes raw C arrays in global state or unprotected structures.
  - BLE Config Service acts on raw bytes and relies on deep string matching rather than a structured dispatch.
- **Type Soundness**:
  - The `HidDeviceContext` has optional/active fields but lacks explicit lifecycle states.
  - Unclear ownership of the JSON configuration buffer (`g_config_json`).
- **Composability**:
  - Tight coupling in the Webapp's `HotasConfigClient` between UI concerns (e.g., `log`, DOM events) and transport logic.
  - BLE and Mapping engine are deeply intertwined through global mutable state variables (`g_profile`, `g_config_json`).

## 2. Refactor Plan

The work is grouped into the following subsystems, starting with the highest-risk boundaries:

1. **Shared Types & Interfaces (Firmware Core)**
   - Introduce strictly typed boundaries in `shared_types.h`.
   - Upgrade C-style enums to `enum class` to prevent implicit integer coercion.
2. **Schema & Configuration Parsing (Mapping Engine & BLE)**
   - Centralize `cJSON` access behind a strongly typed configuration schema validator in C++.
   - Introduce strong bounds checking on all JSON payloads before writing to memory or NVS.
3. **Firmware Input/Output Decoders (USB & Math)**
   - Add explicit parameter guards in the `mapping_engine` logic.
   - Enforce report bounds checking inside `input_elements_decode_report`.
4. **Webapp Types & Boundary Validation (Frontend)**
   - Implement a lightweight schema validator for all inbound BLE data in `webapp/app.js`.
   - Standardize error handling and state transitions in the BLE client.
5. **BLE Command Router (Firmware Services)**
   - Replace unstructured string parsing with a clear router and typed command structs.

## 3. Contract Strategy

- **Firmware Layer (C++)**: Use `assert()` or equivalent lightweight ESP-IDF invariants (`ESP_ERROR_CHECK`, `assert`) for preconditions on internal module functions. Use explicit error-returning functions (e.g., `bool` or `esp_err_t`) for boundary input validations (BLE writes, NVS reads).
- **Webapp Layer (JS)**: Define schema objects that throw or return `false` on shape mismatch. All incoming events (e.g., `handleEvtNotification`, `getConfig`) will parse, then pass through schema validation, discarding/logging invalid payloads.

## 4. Validation Strategy

Runtime schema validation will be added:
1. **BLE Write Boundary**: Any chunk of configuration data from the Webapp will be completely validated against the `MappingProfile` rules before being applied.
2. **NVS Load Boundary**: Before adopting an NVS profile, the JSON will be parsed and structurally validated.
3. **Webapp BLE Read Boundary**: Any JSON or binary payload received from the `CMD`, `EVT`, or `STREAM` characteristic will be validated against expected domain types before driving UI state.

## 5. Risk Notes

- **Regression in Configuration Persistence**: Tightening NVS parsing could reject existing valid profiles if the schema is too strict or does not allow defaults. We must allow optional fields gracefully.
- **BLE Payload Limits**: Adding typed abstractions or wrappers could increase the overhead on the ESP32. We must remain careful about MTU sizes (e.g., `kMaxConfigJsonBytes`, `STREAM` payload packing).
- **Webapp Memory**: Deep schema validation can impact the tight loop of the `STREAM` telemetry updates. Stream processing needs to stay minimal and fast.
