# Firmware: CONFIG Service

## Purpose
Describe the custom BLE GATT service exposed in `APP_MODE_CONFIG` and the firmware-side command/router behavior.

Related documents:
- [../interfaces/BLE_CONTRACTS.md](../interfaces/BLE_CONTRACTS.md)
- [../interfaces/CONFIG_SCHEMA.md](../interfaces/CONFIG_SCHEMA.md)
- [../webapp/CONFIG_CONSOLE.md](../webapp/CONFIG_CONSOLE.md)
- [../../AUDIT_REPORT.md](../../AUDIT_REPORT.md)

## Dependencies
- `main/ble_config_service.cpp`
- `main/ble_config_service.h`
- `main/hid_device_manager.cpp`
- `main/mapping_engine.cpp`
- `main/nvs_profile_store.*`

## Data Structures/Interfaces
### Service UUIDs
- Service: `6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a1`
- CMD: `...c1a2`
- EVT: `...c1a3`
- STREAM: `...c1a4`
- CFG: `...c1a5`

### Characteristics
| Characteristic | Direction | Semantics |
|---|---|---|
| `CMD` | Web → firmware | JSON command write (`WRITE` / `WRITE_NO_RSP`) |
| `EVT` | Firmware → web | Chunked framed notifications for JSON responses and descriptor bytes |
| `STREAM` | Firmware → web | 16-byte binary telemetry sample notifications |
| `CFG` | Bidirectional | Full config blob read/write path |

### Command router
Supported commands on current `main`:
- `get_devices`
- `get_descriptor`
- `get_elements`
- `start_stream`
- `stop_stream`
- `get_config`
- `set_config`
- `save_profile`
- `reboot_to_run`
- `reboot_to_config`

### Response shaping
- All command responses are wrapped as JSON with at least `evt`, `rid`, and `cmd` when available.
- `get_descriptor` also causes a type-2 EVT binary transfer.
- `get_config` on current `main` attempts to inline the config as either `config` or `config_json`.

## Expected State Changes
- `start_stream` enables periodic binary sample emission.
- `stop_stream` stops periodic sample emission.
- `set_config` mutates the active in-memory profile and recomputes mapping.
- `save_profile` persists current profile JSON to NVS.
- `reboot_to_*` schedules reboot into the requested boot mode.

## Known Limitations
- Header comments still imply notify-tx pacing even though current pacing is retry/delay based in `notify_evt_chunked()`.
- Current `get_config` behavior is fragile because it couples configuration retrieval to the EVT response path instead of always using `CFG`. See [../../AUDIT_REPORT.md](../../AUDIT_REPORT.md).
- The frame type comment mentions a config type-3 payload that is not actively used on current `main`.
