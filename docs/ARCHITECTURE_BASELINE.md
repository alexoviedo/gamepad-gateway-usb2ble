# Architecture Baseline (USB HID → BLE HID Gamepad)

> Repo snapshot: `gamepad-gateway-usb2ble-main` (ESP32-S3 / ESP-IDF)

This document is a **baseline** description of the current firmware architecture and data flow **as implemented today** (no new features).

---

## High-level runtime data flow

```
              +-------------------+
USB OTG Host  | ESP-IDF USB Host  |  (usb_host_install + internal PHY host mode)
port  ----->  | + HID Host Driver |  (usb_host_hid component)
              +---------+---------+
                        |
                        | HID_HOST_DRIVER_EVENT_CONNECTED
                        v
              +-------------------+
              | HID Device Manager|  (per-device init task)
              | - get descriptor  |
              | - parse descriptor|
              | - start HID dev   |
              +---------+---------+
                        |
                        | HID_HOST_INTERFACE_EVENT_INPUT_REPORT
                        v
              +-------------------+
              | Input Decode      |
              | - decode bits ->  |
              |   InputElement[]  |
              | - normalize       |
              | - (temp) adapt -> |
              |   legacy state    |
              +---------+---------+
                        |
                        | mapping_engine_compute()
                        v
              +-------------------+
              | Merged Gamepad    |
              |   GamepadState    |
              +---------+---------+
                        |
                        | 50 Hz loop (20ms)
                        v
              +-------------------+
              | BLE HID Gamepad   |
              | - format report   |
              | - notify if CCCD  |
              | - de-dupe reports |
              +-------------------+
```

Key concurrency model:

* USB Host library runs an **event daemon task**.
* HID host driver runs a **background task** (created by the driver).
* Each HID device is initialized in its own **init task**.
* BLE NimBLE host runs in its own **FreeRTOS task**.
* `app_main()` runs a simple **50 Hz loop** that sends the latest merged state over BLE.

---

## 1) USB Host / HID device enumeration

### What happens

1. USB Host stack is brought up with the ESP32-S3 internal PHY in **host mode**.
2. HID Host class driver is installed and begins monitoring for HID devices.
3. When a device connects (`HID_HOST_DRIVER_EVENT_CONNECTED`), a per-device init task:
   * opens the HID device interface,
   * delays 200ms (hub compatibility),
   * fetches the HID report descriptor,
   * parses it into `HidDeviceCaps` (element table + role classification),
   * starts the device so input reports begin flowing.

### Where it lives

* USB host bring-up:
  * `main/usb_host_manager.cpp`
    * `usb_host_manager_init()`
    * `usb_host_lib_daemon_task()`
* HID host driver installation + connect handling:
  * `main/hid_device_manager.cpp`
    * `hid_device_manager_init()` → `hid_host_install(...)`
    * `hid_host_driver_event_cb(..., HID_HOST_DRIVER_EVENT_CONNECTED, ...)`
    * `hid_init_device_task()`

---

## 2) HID report descriptor parsing

### What happens

* The firmware reads the device’s **HID report descriptor** and walks it item-by-item.
* It records **every non-constant INPUT field** into a flat list of `InputElement` records:
  * `usage_page`, `usage`
  * `report_id`
  * `bit_offset`, `bit_size`
  * logical min/max, sign, variable/relative flags
  * derived metadata: `kind` (axis/button/hat/other) and a stable-ish `element_id`
* After parsing the full descriptor, the device is assigned a **role** (`STICK`, `THROTTLE`, `PEDALS`, `UNKNOWN`) using a **score-based heuristic**.

### Where it lives

* Descriptor parsing & element table generation:
  * `main/hid_parser.cpp`
    * `hid_parse_report_descriptor(const uint8_t *desc, size_t len, HidDeviceCaps *caps)`
    * Uses `bit_offsets[coll_type][report_id]` to track per-report bit positions.
* Input element definitions & helpers:
  * `main/input_elements.h` / `main/input_elements.cpp`
    * `InputElement`, `InputElementKind`
    * `ie_guess_kind(...)`, `ie_compute_id(...)`, `ie_friendly_usage(...)`
* Capability container:
  * `main/hid_parser.h`
    * `struct HidDeviceCaps { InputElement elements[MAX_INPUT_ELEMENTS]; size_t num_elements; DeviceRole role; }`
* Role classification logic:
  * `main/hid_parser.cpp` (end of file)
    * score calculation → `caps->role`

---

## 3) Input report decoding / polling cadence

### How input reports arrive

* Input reports are delivered via the HID host driver callback:
  * `HID_HOST_INTERFACE_EVENT_INPUT_REPORT`
* The callback pulls the **raw report bytes** via:
  * `hid_host_device_get_raw_input_report_data(...)`

There is **no explicit polling loop** in this firmware; it is effectively **interrupt/event driven** by the HID host driver.

### How bytes become values

1. The raw report bytes are decoded into the descriptor-derived element table:
   * `input_elements_decode_report(...)`
   * Handles Report ID prefixing when any element uses `report_id > 0`.
   * Extracts arbitrary bitfields and sign-extends when needed.
   * Updates `InputElement.raw`, timestamps `last_update_ms`, and computes `norm_0_1` and `norm_m1_1`.
2. A temporary compatibility layer maps common usages into the legacy fixed-layout `GamepadState`:
   * axes normalize to `int16_t` in `[-32767..32767]`
   * hat normalizes to `0=center, 1..8` directions
   * buttons become a 32-bit OR mask

### Where it lives

* HID callback & raw report acquisition:
  * `main/hid_device_manager.cpp`
    * `hid_host_interface_callback(..., HID_HOST_INTERFACE_EVENT_INPUT_REPORT, ...)`
* Decode entry point:
  * `main/input_decoder.cpp`
    * `hid_decode_report(const uint8_t *report, size_t report_size, HidDeviceContext *ctx)`
* Bit extraction + normalization into `InputElement[]`:
  * `main/input_elements.cpp`
    * `input_elements_decode_report(...)`
* Legacy adapter into `GamepadState` (usage-based):
  * `main/input_decoder.cpp`
    * `adapt_elements_to_gamepad_state(...)`

---

## 4) State merge logic across multiple devices

### Current behavior (deterministic mapping engine)

The firmware currently uses a **deterministic mapping engine** rather than the older “max abs per axis” merge.

* Each output axis (X/Y/Z/Rx/Ry/Rz/Slider1/Slider2/Hat) is mapped to **exactly one** `(device_id, element_id)` source.
* A default mapping profile is generated at runtime using:
  * device roles (stick/throttle/pedals)
  * HID usage matching (Generic Desktop vs Simulation Controls)
  * fallbacks for common hardware patterns (e.g., throttle with embedded pedals)
* Buttons are still combined by OR by default.

The mapping engine is called on every input report from any device.

### Legacy behavior (still present, not used in the main path)

* `hid_merge_states(...)` still exists and implements the legacy “max abs deflection wins” strategy.
* The HID manager no longer uses it; it calls `mapping_engine_compute(...)` instead.

### Where it lives

* Merge compute (current):
  * `main/mapping_engine.h` / `main/mapping_engine.cpp`
    * `mapping_engine_init()`
    * `mapping_engine_notify_devices_changed()`
    * `mapping_engine_compute(const HidDeviceContext *devices, size_t num_devices, GamepadState *out)`
    * `build_default_profile(...)` (runtime default binding)
* Called from the HID input report callback:
  * `main/hid_device_manager.cpp`
    * `mapping::mapping_engine_compute(g_devices, MAX_DEVICES, &g_merged_state);`
* Legacy merge (not currently used by HID manager):
  * `main/input_decoder.cpp`
    * `hid_merge_states(...)`

---

## 5) BLE HID gamepad report formatting and send timing

### BLE stack and services

* BLE is implemented with **NimBLE** (ESP-IDF).
* The device advertises as `HOTAS_BRIDGE` with HID Gamepad appearance.
* GATT includes:
  * Device Information (0x180A)
  * Battery Service (0x180F)
  * HID Service (0x1812) with a single **Input Report** characteristic.

### Report format

The outgoing report payload (`GamepadInputReport`) is:

| Field | Type | Notes |
|---|---:|---|
| `buttons` | `uint32_t` | 32 buttons packed (Button1 → bit0) |
| `x,y,z,rx,ry,rz,slider1,slider2` | `uint16_t` | **Unsigned 0..32767**, matching ESP32-BLE-Gamepad style |
| `hat` | `uint8_t` | 0=center, 1..8 directions |

Internal `GamepadState` uses signed `int16_t` in `[-32767..32767]` and is converted to unsigned via `to_u16_axis()`.

### Send timing

* `app_main()` runs a fixed-period loop:
  * **20ms period** → **50 Hz**
* Each iteration:
  1. fetch merged USB state (`hid_device_manager_get_merged_state()`)
  2. translate USB→BLE (`translate_usb_to_ble()` currently identity mapping)
  3. `ble_gamepad_send_state()`

### Notification gating + de-duplication

* The firmware will **not notify** until:
  * the central is connected, and
  * the input report CCCD has been enabled (`g_input_notify_enabled`).
* Consecutive identical reports are suppressed (memcmp vs `g_last_report`) unless a “force once” path is set.

### Where it lives

* Main 50 Hz send loop:
  * `main/main.cpp`
    * `app_main()`
    * `translate_usb_to_ble(...)` (identity today)
* BLE implementation:
  * `main/ble_gamepad.h` / `main/ble_gamepad.cpp`
    * `ble_gamepad_init()`
    * `ble_gamepad_send_state(const GamepadState *s)`
    * `kHidReportMap[]` (HID report descriptor exposed over GATT)
    * `GamepadInputReport` (packed input report struct)

---

## 6) Debug portal / WiFi debug features

There is an ESP-IDF HTTP server based debug portal implementation, but it is **not currently initialized** by `app_main()`.

### What it provides (when enabled)

* Starts a WiFi SoftAP:
  * SSID: `HOTAS-Debug`
  * IP: `192.168.4.1`
  * Open auth (no password)
* HTTP endpoints:
  * `/` renders an HTML page with the current merged `GamepadState`
  * `/logs` returns a plaintext ring-buffer of captured log output
* Captures logs by overriding `esp_log_set_vprintf(...)`.

### Where it lives

* `main/debug_portal.cpp` / `main/debug_portal.h`
  * `debug_portal_init()`
  * `portal_get_handler()` → merged state HTML
  * `portal_logs_handler()` → ring buffer

---

## 7) Where things live (quick index)

| Area | Primary files | Key types / functions |
|---|---|---|
| App entrypoint + 50 Hz loop | `main/main.cpp` | `app_main()`, `translate_usb_to_ble()` |
| USB host bring-up | `main/usb_host_manager.*` | `usb_host_manager_init()`, `usb_host_lib_daemon_task()` |
| HID device lifecycle | `main/hid_device_manager.*` | `hid_device_manager_init()`, `hid_init_device_task()`, `hid_host_interface_callback()` |
| Descriptor parsing | `main/hid_parser.*` | `hid_parse_report_descriptor()`, role scoring |
| Element decode | `main/input_elements.*` | `input_elements_decode_report()`, `InputElement` |
| Report decode wrapper | `main/input_decoder.*` | `hid_decode_report()`, `adapt_elements_to_gamepad_state()` |
| Merge/mapping (current) | `main/mapping_engine.*` | `mapping_engine_compute()`, `build_default_profile()` |
| Merge (legacy, not used) | `main/input_decoder.cpp` | `hid_merge_states()` |
| BLE HID (NimBLE) | `main/ble_gamepad.*` | `ble_gamepad_init()`, `ble_gamepad_send_state()`, `kHidReportMap[]` |
| Debug portal (dormant) | `main/debug_portal.*` | `debug_portal_init()`, `/` + `/logs` handlers |

---

## 8) Build system + exact build/flash steps (as implemented)

### Build system

* **ESP-IDF CMake project**
  * Root: `CMakeLists.txt` includes `$ENV{IDF_PATH}/tools/cmake/project.cmake` and defines project `hotas_ble`.
  * Component registration: `main/CMakeLists.txt` via `idf_component_register(...)`.
* Managed components:
  * `idf_component.yml` (root) and `main/idf_component.yml` for ESP Component Manager.

### Scripts (repo-authoritative)

All scripts assume your ESP-IDF environment is already activated (i.e., `idf.py` is on PATH).

* Build:
  * `scripts/build.sh`
    * `idf.py set-target esp32s3`
    * `idf.py build`
* Flash + monitor:
  * `scripts/flash.sh <PORT>`
    * `idf.py -p "${PORT}" flash monitor`
* Clean:
  * `scripts/clean.sh`
    * removes `build/`, `sdkconfig*`, `managed_components/`, `.idf-component-manager/`

### Manual CLI (equivalent)

```bash
# from repo root
. "$IDF_PATH/export.sh"          # or your OS equivalent
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

---

## 9) Baseline validation checklist (PC repro of “rudder issue”)

This checklist is meant to validate **end-to-end** behavior on a PC BLE host (Windows is typical) and to specifically reproduce/observe the historical symptom: toe brakes update reliably while rudder (Z) appears intermittent.

> Note: The current firmware includes the deterministic mapping engine intended to eliminate the legacy “max abs merge” instability. This checklist is still useful to confirm whether the symptom is fully resolved and to capture evidence if it persists.

### A) Hardware + setup

* ESP32-S3 board flashed with this firmware.
* A **powered** USB hub connected to the ESP32-S3 USB OTG host port.
* Plug in:
  * Stick (e.g., T.16000M joystick)
  * Throttle (e.g., TWCS throttle)
  * Rudder pedals (e.g., TFRP)
* PC with Bluetooth and a BLE-capable input stack.

### B) Expected serial logs (USB + HID)

On boot:

* `--- HOTAS USB to BLE Gamepad Bridge ---`
* `Target: ESP32-S3 (USB OTG Host + BLE)`
* `USB_HOST_MGR: Initializing USB Host...`
* `HID_MGR: HID Class Driver installed`
* `BLE_GAMEPAD: BLE HID gamepad initialized ...`

Per device connect (repeat for each device):

* `HID_MGR: HID Device Connected`
* `HID_MGR: Got Report Descriptor of length ...`
* `HID_PARSER: Role scores: stick=... throttle=... pedals=... -> Role ...`
* `HID_MGR: Device registered. Role=... Elements=...`
  * followed by many `E[###]: ...` element lines (usages, report id, offsets)
* `MAP_ENG: Default mapping devices: stick=yes/no thr=yes/no ped=yes/no`
* `MAP_ENG: Default mapping profile:`
  * `x -> dev=... elem=...`
  * `y -> dev=... elem=...`
  * `z -> dev=... elem=...`  (**this should point at pedals when present**)
  * `rx -> ...` and `ry -> ...` (toe brakes when present)
  * `slider1 -> ...` (throttle)

While exercising controls, if sample logging is active in your build:

* periodic `HID_MGR: Sample: dev=... elem[...] ... raw=... n01=... n11=...`

### C) Expected BLE behavior (Windows “Game Controllers” calibration)

1. Pair the BLE device named **`HOTAS_BRIDGE`**.
2. Open Windows game controller calibration / properties for the new controller.
3. Verify:
   * Controller stays connected (no immediate disconnect after pairing).
   * Axes move smoothly when controls move.

### D) Axis expectations (baseline mapping)

The outgoing BLE report exposes the following axes:

| BLE axis | Expected physical source (default profile) | Notes |
|---|---|---|
| `X` | Stick roll (Generic Desktop X) | left/right on joystick |
| `Y` | Stick pitch (Generic Desktop Y) | forward/back on joystick (may appear inverted depending on host expectations) |
| `Z` | **Rudder pedals yaw** (Sim Rudder or Z) | **this is the problematic axis in the symptom** |
| `Rx` | Left toe brake (often pedals Rx or ToeBrake #1) | should move independently |
| `Ry` | Right toe brake (often pedals Ry or ToeBrake #2) | should move independently |
| `Rz` | Stick twist or throttle rocker (only if available and mapped) | secondary rudder-like axis; may be unmapped depending on device set |
| `Slider1` | Throttle main axis (Sim Throttle / Slider / Z fallback) | expect 0..max travel |
| `Slider2` | Optional combined toe brake (max of brakes) or single ToeBrake | depends on pedal descriptor |
| `Hat` | POV hat | 0=center, 1..8 directions |

### E) Rudder issue reproduction steps (what to look for)

1. In the Windows controller properties panel, leave the stick and throttle untouched.
2. Slowly sweep the **rudder pedals** full-left → center → full-right multiple times.
3. Observe:
   * **Expected (healthy):** `Z` responds continuously and proportionally.
   * **Symptom (bug):** `Z` updates only intermittently (jumps, drops out, or sticks), while `Rx/Ry` (toe brakes) remain responsive.
4. If the symptom appears:
   * Check serial logs for the printed mapping profile to confirm `Z` is bound to the pedals device.
   * Watch `Sample:` lines for pedal elements to see whether the pedal rudder element is updating consistently at the raw level.
   * If the raw element updates are consistent but BLE `Z` is not, the issue is likely in mapping/translation/BLE send gating.
   * If the raw element updates are inconsistent, the issue is likely in input report parsing, report ID handling, or host delivery cadence.

---

## Appendix: “What changed” context for the rudder symptom

For historical context on the rudder symptom and why deterministic mapping was introduced, see `docs/RUDDER_BUG_FINDINGS.md`.
