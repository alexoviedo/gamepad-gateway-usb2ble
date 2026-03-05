# Architecture Baseline (USB HID → BLE HID Gamepad)

This document summarizes the **current** codebase architecture and dataflow as of the extracted ZIP snapshot. It is intended as a baseline for debugging (e.g., the intermittent rudder axis issue) **without adding new features**.

---

## High-level overview

The firmware is an **ESP-IDF** project targeting **ESP32-S3**.

At runtime:

1. **USB Host** is initialized in OTG **host mode**.
2. The **HID Host class driver** detects USB HID device connections.
3. For each HID device:
   - Fetches its **HID report descriptor**.
   - Parses the descriptor into a list of **mapped input fields** (axes / hat / buttons).
   - Starts the HID device to receive **input reports**.
4. Each incoming HID input report is **decoded** into a per-device `GamepadState`.
5. All active device states are **merged** into a single global `GamepadState`.
6. A NimBLE **BLE HID Gamepad (HOGP)** is advertised; when connected and subscribed, the merged state is sent as BLE HID **input reports** at a fixed cadence (50 Hz), with de-duplication.

---

## Task / thread model (FreeRTOS)

The project uses multiple background tasks plus a simple foreground loop:

- **USB host event daemon task** (created by `usb_host_manager_init()`)
  - Runs `usb_host_lib_handle_events()` in a loop.
- **HID host background task** (created by `hid_host_install()` with `create_background_task=true`)
  - Manages class-driver work.
- **Per-device init task** (created on each HID connection)
  - Fetches and parses the report descriptor, then starts the device.
- **NimBLE host task** (created by `nimble_port_freertos_init()`)
  - Runs the NimBLE host loop.
- **Main loop** (inside `app_main()`)
  - Reads merged USB state → optional translation → calls BLE send at **50 Hz**.

---

## USB Host / HID device enumeration flow

### USB Host bring-up

**Where:** `main/usb_host_manager.cpp`

- `usb_host_manager_init()`:
  - Creates and configures the ESP32-S3 **internal USB PHY** for OTG host mode via `usb_new_phy()`.
  - Installs the ESP-IDF USB Host library via `usb_host_install()`.
  - Spawns `usb_host_lib_daemon_task()` to service host events.

### HID class driver and device lifecycle

**Where:** `main/hid_device_manager.cpp`

- `hid_device_manager_init()`:
  - Installs the HID host driver via `hid_host_install()`.
  - Registers `hid_host_driver_event_cb()` for connect events.

- `hid_host_driver_event_cb()` (on `HID_HOST_DRIVER_EVENT_CONNECTED`):
  - Logs the connection.
  - Spawns `hid_init_device_task()` to perform descriptor fetch + parsing.

- `hid_init_device_task()`:
  - Opens the device (`hid_host_device_open()`), then waits `200ms` (hub compatibility delay).
  - Fetches HID report descriptor (`hid_host_get_report_descriptor()`).
  - Allocates an available slot in `g_devices[]`, parses descriptor into `HidDeviceCaps`.
  - Marks the device active, stores a derived `dev_addr`.
  - Starts the device (`hid_host_device_start()`) so input reports begin flowing.

- `hid_host_interface_callback()`:
  - Receives `HID_HOST_INTERFACE_EVENT_INPUT_REPORT` and pulls the raw report bytes via `hid_host_device_get_raw_input_report_data()`.
  - Receives `HID_HOST_INTERFACE_EVENT_DISCONNECTED` and clears the device slot.

**Important implementation detail:** device → context matching uses a canonical `HidDeviceIdentity` (see below). Callbacks match devices by the full HID device handle pointer tag (`session_handle_tag`) and fall back to `(usb_addr, iface_num)` when available. The legacy `dev_addr` field now tracks the USB address (not bits of the handle).

---

## Device Identity

The project now uses a dedicated **device identity** object to make device tracking safe and consistent across multiple connected HID devices.

**Where:** `main/hid_device_identity.h`, `main/hid_device_identity.cpp`, used by `main/hid_device_manager.cpp` and stored per-slot in `HidDeviceContext.identity`.

### Fields (best-effort)

`HidDeviceIdentity` includes, as available:

- **USB routing:** `dev_addr` (USB address assigned by host), `iface_num` (USB interface)
- **Session linkage:** `session_handle_tag` = `(uintptr_t)hid_host_device_handle_t` (stable for the lifetime of the connection, not across reconnects)
- **Device identifiers:** `vid`, `pid`, `manufacturer`, `product` (best-effort)
- **Descriptor fingerprint:** `report_desc_crc32` (a 32-bit fingerprint derived from the HID report descriptor; typically stable across reconnects for the same device/interface)
- **Stable hash:** `stable_hash` computed from the above (used for logging/profiling keys)

### Matching rules (callbacks)

For both **input reports** and **disconnects**, the manager resolves the correct device slot using:

1. **Primary:** exact match on `session_handle_tag` (full handle pointer tag)
2. **Fallback:** match on `(usb_addr, iface_num)` when `hid_host_device_get_params()` succeeds

This eliminates the prior unsafe approach of using only the low 8 bits of the handle pointer.

### Reconnect behavior

On reconnect, the USB address may change. When VID/PID and/or strings are unavailable, the **report descriptor fingerprint** helps keep `stable_hash` consistent across reconnects *where possible* (e.g., for the same model/interface).

---

## HID report descriptor parsing

**Where:** `main/hid_parser.h`, `main/hid_parser.cpp`

### What the parser produces

- Output struct: `HidDeviceCaps`
  - `HidMappedField fields[128]`
  - `size_t num_fields`
  - `DeviceRole role` (classified as STICK/THROTTLE/PEDALS/UNKNOWN)

Each `HidMappedField` includes:

- `report_id` (from HID global item `Report ID`)
- `bit_offset` / `bit_size` (bit-level position within the report payload)
- logical min/max + signedness
- `usage_page` + `usage`

### What gets mapped (filter)

The parser only maps **Input** items (not Output/Feature) and only if they are not marked **Constant**.

Supported usages are filtered to:

- **Generic Desktop (0x01):** usages `0x30..0x39` (X, Y, Z, Rx, Ry, Rz, Slider, Dial, Wheel, Hat)
- **Simulation Controls (0x02):** `0xBA` (Rudder), `0xBB` (Throttle), `0xBF` (Toe Brake)
- **Buttons (0x09):** any button usage (later limited to 1..32 in decoding)

### Bit offset accounting

The parser tracks bit offsets **per report ID** with `bit_offsets[coll_type][report_id]`.

- For each `Input` main item, it iterates `report_count` times and assigns bit offsets sequentially by `report_size`.
- Local state (`Usage`, `Usage Min/Max`) is consumed to determine which usage applies to each field.

### Role classification

After mapping fields, the parser assigns a `DeviceRole` by counting presence of certain fields:

- Stick: presence of X/Y
- Throttle: presence of Simulation Throttle or Desktop Slider
- Pedals: presence of Simulation Rudder or Toe Brake
- Hat and Buttons affect heuristics

**Note:** role is currently logged but not used in merge logic.

---

## Input report decoding and polling cadence

**Where:**

- Event ingestion: `main/hid_device_manager.cpp` (`hid_host_interface_callback()`)
- Report decode: `main/input_decoder.cpp` (`hid_decode_report()`)

### Polling / cadence

There is **no explicit polling loop** implemented in this codebase.

- The HID class driver delivers `HID_HOST_INTERFACE_EVENT_INPUT_REPORT` events whenever an input report is available (driven by the HID interrupt IN endpoint scheduling and device report rate).
- Each event triggers a pull of the latest raw report bytes into a local buffer (`uint8_t report_data[128]`).

### Report ID handling

`hid_decode_report()` determines whether the device uses report IDs:

- If any parsed `HidMappedField.report_id > 0`, it assumes **reports include a leading report ID byte**.
- If so, it reads `report_id = report[0]` and sets `payload = report + 1`.

Only fields matching that report ID are decoded.

### Field extraction and normalization

- `extract_bits()` reads bitfields little-endian from `payload` using `bit_offset` and `bit_size`.
- Axes are normalized to `int16_t` in approximately `[-32767, 32767]` via `normalize_axis()`.
- Hat is normalized to `0=center` or `1..8` directions.
- Buttons are packed into `uint32_t buttons`.

### Current mapping to `GamepadState`

From decoding (intent as implemented):

- Desktop page (0x01):
  - `X → state.x`, `Y → state.y`, `Z → state.z`
  - `Rx → state.rx`, `Ry → state.ry`, `Rz → state.rz`
  - `Slider (0x36) → state.slider1`
  - `Hat (0x39) → state.hat`
- Simulation page (0x02):
  - `Rudder (0xBA) → state.z`
  - `Throttle (0xBB) → state.slider1`
  - `Toe Brake (0xBF) → state.slider2`
- Buttons page (0x09):
  - Button usage N sets bit `N-1` (limited to 32)

**Note:** `hid_decode_report()` clears `ctx->state.buttons` each report, but does **not** clear axes/hat/slider fields. Those values persist unless overwritten by a newly-decoded field.

---

## State merge logic across multiple devices

**Where:** `main/input_decoder.cpp` (`hid_merge_states()`)

The system tracks up to `MAX_DEVICES=8` contexts (`g_devices[]`). On each report arrival, it:

1. Decodes the report into the corresponding device's `ctx->state`.
2. Recomputes `g_merged_state` via `hid_merge_states()`.

Merge rules (as implemented):

- **Buttons:** bitwise OR across all active devices.
- **Hat:** first non-zero hat wins.
- **Axes/Sliders:** for each axis, the value with the **largest absolute deflection** wins (|v| maximum).

There is currently **no role-aware merge preference** (despite comments suggesting stick/throttle/pedals preference). The merge is purely based on magnitude.

Threading:

- Merge operations and per-device state updates are guarded by `g_state_mutex`.
- The main loop reads `g_merged_state` via `hid_device_manager_get_merged_state()` under the same mutex.

---

## BLE HID gamepad report formatting and send timing

**Where:** `main/ble_gamepad.h`, `main/ble_gamepad.cpp`

### GATT + HID report map

- Device name: `HOTAS_BRIDGE`
- Appearance: Gamepad (0x03C4)
- Services:
  - Device Information (0x180A)
  - Battery (0x180F)
  - HID (0x1812)

The HID Report Map (`kHidReportMap`) defines:

- Report ID: `0x03` (declared in the report map, but **not** prepended to notifications)
- **32 buttons** (bitfield)
- **8 axes**, each `uint16_t` with logical range `0..32767`:
  - X, Y, Z, Rx, Ry, Rz, Slider, Slider
- **Hat** as 8-bit with Null State:
  - `0=center`, `1..8=direction`

### State conversion

Internal `GamepadState` uses signed centered `int16_t` (~-32767..32767). Before sending:

- `to_u16_axis()` maps signed → unsigned `0..32767`.
- `clamp_hat()` ensures hat is `0..8`.

### Send timing and gating

- `app_main()` calls `ble_gamepad_send_state()` every **20ms** (50 Hz).
- `ble_gamepad_send_state()` returns early unless:
  - BLE is connected, and
  - the central has enabled notifications for the HID input report characteristic (`g_input_notify_enabled`).

De-duplication:

- If the newly constructed report matches the previous report byte-for-byte, it is not re-notified.
- After subscription is enabled, `g_force_send_once` forces one immediate report to be sent (even if unchanged).

---

## Debug portal / WiFi debug features

**Where:** `main/debug_portal.h`, `main/debug_portal.cpp`

The repository contains a simple **SoftAP + HTTP server** debug portal that:

- Starts an AP named `HOTAS-Debug` (open auth) and serves:
  - `/` : auto-refreshing HTML page of the merged `GamepadState`
  - `/logs` : ring-buffered serial logs captured by a custom `vprintf`

**Current status:** this module is **not compiled** (not listed in `main/CMakeLists.txt`) and is **not invoked** from `app_main()`.

---

## Where things live (paths + key entry points)

| Concern | Primary files | Key structs / functions |
|---|---|---|
| USB Host init + host event loop | `main/usb_host_manager.cpp`, `main/usb_host_manager.h` | `usb_host_manager_init()`, `usb_host_lib_daemon_task()` |
| HID device enumeration / lifecycle | `main/hid_device_manager.cpp`, `main/hid_device_manager.h` | `hid_device_manager_init()`, `hid_host_driver_event_cb()`, `hid_init_device_task()`, `hid_host_interface_callback()` |
| HID report descriptor parsing | `main/hid_parser.cpp`, `main/hid_parser.h` | `hid_parse_report_descriptor()`, `HidMappedField`, `HidDeviceCaps` |
| Input report decode | `main/input_decoder.cpp`, `main/input_decoder.h` | `hid_decode_report()`, `extract_bits()`, `normalize_axis()`, `HidDeviceContext` |
| Multi-device state merge | `main/input_decoder.cpp`, `main/hid_device_manager.cpp` | `hid_merge_states()`, `g_devices[]`, `g_merged_state` |
| BLE HID gamepad (HOGP) | `main/ble_gamepad.cpp`, `main/ble_gamepad.h` | `ble_gamepad_init()`, `ble_gamepad_send_state()`, `kHidReportMap`, `GamepadInputReport` |
| Main loop + translation layer | `main/main.cpp` | `app_main()`, `translate_usb_to_ble()` |
| Debug portal (not enabled) | `main/debug_portal.cpp`, `main/debug_portal.h` | `debug_portal_init()` |
| Shared state types | `main/shared_types.h` | `GamepadState`, `DeviceRole` |

---

## Build system and exact build/flash steps

### Build system

This is an **ESP-IDF** project using the standard CMake-based layout:

- Root `CMakeLists.txt` includes `$IDF_PATH/tools/cmake/project.cmake`.
- `main/CMakeLists.txt` registers sources via `idf_component_register()`.
- Dependencies are managed via **ESP Component Manager** (`idf_component.yml`, `dependencies.lock`).

### Repo scripts (as implemented)

All scripts require `idf.py` to already be on `PATH` (ESP-IDF environment activated).

- Build:
  - `./scripts/build.sh` runs:
    - `idf.py set-target esp32s3`
    - `idf.py build`

- Flash + monitor:
  - `./scripts/flash.sh <PORT>` runs:
    - `idf.py -p <PORT> flash monitor`

- Clean:
  - `./scripts/clean.sh` removes:
    - `build/`, `sdkconfig*`, `managed_components/`, `.idf-component-manager/`

### README build/flash flow (current doc)

The repo `README.md` describes the expected manual flow:

1. Install ESP-IDF (>= 5.5.0; lock file pins 5.5.3).
2. Activate the ESP-IDF environment so `idf.py` is available.
3. `idf.py set-target esp32s3`
4. `idf.py reconfigure` (to fetch managed components)
5. `idf.py build`
6. `idf.py -p <PORT> flash monitor`

---

## Baseline validation checklist (PC rudder issue reproduction)

This checklist is designed to reproduce and observe the *current* rudder behavior on a PC BLE host, and to correlate it with expected firmware logs.

### A) Firmware bring-up (serial)

1. Flash and open monitor (example):
   - `./scripts/flash.sh <PORT>`
2. Confirm boot banner:
   - `--- HOTAS USB to BLE Gamepad Bridge ---`
   - `Target: ESP32-S3 (USB OTG Host + BLE)`
3. Confirm BLE is initialized and advertising:
   - `BLE_GAMEPAD: BLE HID gamepad initialized ...`
   - `BLE_GAMEPAD: Advertising as 'HOTAS_BRIDGE' ...`
4. Confirm USB host + HID driver installed:
   - `USB_HOST_MGR: Initializing USB Host...`
   - `USB_HOST_MGR: USB Host initialized and daemon started.`
   - `HID_MGR: HID Class Driver installed`

### B) USB device enumeration (serial)

1. Plug a **powered USB hub** into the ESP32-S3 OTG port.
2. Plug in devices (stick, throttle, pedals).
3. For each device, confirm:
   - `HID_MGR: HID Device Connected`
   - `HID_MGR: Got Report Descriptor of length ...`
   - `HID_PARSER: Classified device as Role ...`
   - `HID_MGR: Device registered with Role: ..., Fields: ...`

**Pedals-specific expectation:** in the printed fields, look for at least one field with:

- `usage_page=0002` and `usage=00BA` (Simulation Rudder), and
- one or more fields with `usage_page=0002` and `usage=00BF` (Toe Brake)

If those are absent, the pedals may be exposing rudder/brakes using Generic Desktop usages instead (still decodable, but mapped differently).

### C) BLE pairing and notification gating (serial)

1. On the PC, scan for BLE devices and connect/pair to:
   - **Name:** `HOTAS_BRIDGE`
2. On successful connection, firmware logs should show:
   - `BLE_GAMEPAD: Connected (handle=...)`
3. When the PC enables notifications for the input report characteristic, firmware logs should show:
   - `BLE_GAMEPAD: Input report notifications ENABLED`

**If notifications are not enabled**, the firmware will not send input reports (by design).

### D) PC calibration expectations (BLE HID axes/buttons)

The outgoing BLE HID report always contains:

- Buttons: 32
- Hat: 0..8
- Axes (8): X, Y, Z, Rx, Ry, Rz, Slider1, Slider2

Expected physical control → axis movement (based on current decoding intent):

#### Stick (T.16000M)

- Move stick left/right: **X** should move.
- Move stick forward/back: **Y** should move.
- Twist stick (if used): **Rz** should move (if the stick reports Rz usage).
- Stick buttons: corresponding button bits should toggle.
- POV hat (if present): **Hat** should move (1..8).

#### Throttle (TWCS)

- Main throttle lever: **Slider1** should move (either Simulation Throttle or Desktop Slider usage).
- Any mini-stick/extra axes (if exposed as Rx/Ry/Z/etc.): the corresponding axis should move.

#### Pedals (TFRP)

- Rudder sweep left/right: **Z** should move (Simulation Rudder is mapped to `state.z`).
- Toe brakes: **Slider2** should move (Toe Brake usage is mapped to `state.slider2`).

### E) Reproducing the rudder issue

1. With the PC calibration UI open, repeatedly sweep the pedals rudder axis.
2. Observe whether the **Z** axis moves smoothly and continuously.
3. If the reported issue is present, you may observe:
   - Z axis remains stationary or updates only intermittently while toe brake (Slider2) continues to respond.

### F) Correlating with firmware side symptoms (baseline)

When the issue occurs, check for these baseline indicators:

- Does the device still show as connected (no disconnect logs)?
- Are input report notifications still enabled?
- Did the descriptor print include a Simulation Rudder field (usage 0xBA)?
- If you temporarily rely on any merged-state observation (e.g., via added logging or the disabled portal), does `GamepadState.z` change when pedals move?

---

## Notes for future debugging (non-invasive observations)

This baseline is intentionally descriptive only. A few hotspots to focus on later (without changing behavior yet):

- Device-to-context matching in `hid_host_interface_callback()` uses a low-byte mask of the HID device handle; context mismatches would cause reports to be ignored.
- The report buffer is fixed at 128 bytes; unusually large reports could truncate.
- Multiple Toe Brake fields (if present) will all map to the same `slider2` target (last decoded wins).
