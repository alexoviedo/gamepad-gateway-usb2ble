# BLE_CONTRACTS

## Purpose
Absolute source of truth for BLE communications between ESP32-S3 firmware and the browser WebApp.

This file maps firmware-produced payloads directly to their corresponding JavaScript handling in `webapp/shared/ble_client.js`.

## Dependencies
### Firmware sources
- `main/ble_config_service.h`
- `main/mapping_engine.h`
- `main/main.cpp`

### WebApp source
- `webapp/shared/ble_client.js`

## Data Structures/Interfaces
## 1. GATT services and characteristics
### CONFIG service UUIDs
| Name | UUID | Firmware meaning | JavaScript usage |
|---|---|---|---|
| Service | `6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a1` | Primary custom config service | `BLE_UUIDS.service` |
| CMD | `6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a2` | JSON command write characteristic | `BLE_UUIDS.cmd`, `writeCommandText()` |
| EVT | `6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a3` | Notification channel for chunked responses | `BLE_UUIDS.evt`, `ensureEvtSubscription()`, `handleEvtNotification()` |
| STREAM | `6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a4` | Notification channel for 16-byte telemetry samples | `BLE_UUIDS.stream`, `ensureStreamSubscription()`, `handleStreamNotification()` |
| CFG | `6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a5` | Config blob read/write characteristic | `BLE_UUIDS.cfg`, `ensureConfigCharacteristic()`, `readConfigCharacteristic()`, `getConfig()` |

## 2. EVT frame contract
### Frame purpose
`EVT` carries chunked payloads emitted by firmware during CONFIG-mode responses.

### Frame types
| Type | Meaning | Firmware producer | JavaScript consumer |
|---|---|---|---|
| `1` | JSON payload | `notify_evt_json()` / `notify_evt_chunked(1, ...)` | `ChunkAssembler.push()` + `handleEvtNotification()` JSON branch |
| `2` | Descriptor payload | `notify_evt_chunked(2, ...)` | `ChunkAssembler.push()` + `handleEvtNotification()` descriptor branch |

### EVT header layout
| Byte offset | Size | Type | Meaning |
|---|---:|---|---|
| 0 | 1 | `u8` | `version` |
| 1 | 1 | `u8` | `type` |
| 2 | 2 | `u16 LE` | `msg_id` |
| 4 | 2 | `u16 LE` | `offset` |
| 6 | 2 | `u16 LE` | `total_len` |
| 8.. | `N` | bytes | chunk payload |

### Firmware ↔ JavaScript symmetry
| Firmware concept | JavaScript handler |
|---|---|
| `version` | `ChunkAssembler.push()` reads `getUint8(0)` |
| `type` | `ChunkAssembler.push()` reads `getUint8(1)` |
| `msg_id` | `ChunkAssembler.push()` reads `getUint16(2, true)` |
| `offset` | `ChunkAssembler.push()` reads `getUint16(4, true)` |
| `total_len` | `ChunkAssembler.push()` reads `getUint16(6, true)` |
| chunk payload | `frameBytes.slice(8)` |
| completed JSON | `handleEvtNotification()` → `JSON.parse(text)` |
| completed descriptor bytes | `handleEvtNotification()` → `descriptorCache.set(...)` |

## 3. STREAM binary payload contract
### Payload purpose
`STREAM` carries live input samples during CONFIG mode.

### Exact 16-byte layout
| Byte offset | Size | Type | Meaning |
|---|---:|---|---|
| 0 | 1 | `u8` | `version` |
| 1 | 1 | `u8` | `flags` |
| 2 | 4 | `u32 LE` | `device_id` |
| 6 | 4 | `u32 LE` | `element_id` |
| 10 | 4 | `i32 LE` | `raw` |
| 14 | 2 | `i16 LE` | `norm_q15` |

### Firmware notes
- Payload is emitted by `ble_config_service_stream_tick()`.
- `version` is currently `1`.
- `flags` is currently `0`.
- `norm_q15` is derived from normalized input and clamped to `[-32767, 32767]`.

### Firmware ↔ JavaScript symmetry
| Firmware field | JavaScript field in `handleStreamNotification()` |
|---|---|
| `version` | `view.getUint8(0)` → `sample.version` |
| `flags` | `view.getUint8(1)` → `sample.flags` |
| `device_id` | `view.getUint32(2, true)` → `sample.deviceId` |
| `element_id` | `view.getUint32(6, true)` → `sample.elementId` |
| `raw` | `view.getInt32(10, true)` → `sample.raw` |
| `norm_q15` | `view.getInt16(14, true)` → `sample.normQ15` |
| derived normalized float | `sample.norm = normQ15 / 32767` |

## 4. JSON configuration schema
### Source of truth
The firmware mapping header documents the accepted/emitted JSON schema for mapping profiles.

### Canonical shape
```json
{
  "version": 2,
  "buttons_or_combine": true,
  "replace_all": true,
  "axes": {
    "z": {
      "configured": true,
      "device_id": 101,
      "element_id": 54909,
      "invert": false,
      "deadzone": {
        "inner": 0.03,
        "outer": 0.02
      },
      "smoothing_alpha": 0.2,
      "curve": {
        "type": "bezier",
        "p1": { "x": 0.25, "y": 0.15 },
        "p2": { "x": 0.75, "y": 0.95 }
      }
    }
  }
}
```

### Top-level fields
| Field | Type | Meaning |
|---|---|---|
| `version` | number | Current schema version. Current value is `2`. |
| `buttons_or_combine` | boolean | Button merge behavior flag. |
| `replace_all` | boolean | Optional parser control. `true` clears the existing active profile before applying the provided patch. |
| `axes` | object | Sparse mapping object keyed by canonical output axis name. |

### JavaScript config handling symmetry
| Firmware concept | JavaScript handler |
|---|---|
| `version: 2` default config | `BleClient` constructor initializes `currentConfig = normalizeConfig({ version: 2, axes: {} })` |
| CFG-first config retrieval | `getConfig()` → `readConfigCharacteristic()` |
| Inline config object fallback | `getConfig()` → `response.config` branch |
| Inline config JSON string fallback | `getConfig()` → `response.config_json` branch |
| Firmware metadata fallback (`transport: "cfg_read"`) | `getConfig()` → `response.transport === 'cfg_read'` branch |
| Axis patch send | `applyAxisConfig()` |
| Candidate-based patch send | `applyAxisPatch()` |
| Save profile | `saveProfile()` |

## 5. Command transport
### Command envelope
Commands are written as JSON to `CMD`.

Example:
```json
{
  "rid": 1,
  "cmd": "get_devices"
}
```

### Shared timeout policy in JavaScript
| Command | Timeout |
|---|---:|
| default | `10000 ms` |
| `get_devices` | `15000 ms` |
| `get_descriptor` | `30000 ms` |
| `get_config` | `15000 ms` |

### Command-related JavaScript entry points
- `sendCommand()`
- `resolveCommandTimeout()`
- `writeCommandText()`
- `getDevices()`
- `getDescriptor()`
- `getElements()`
- `getConfig()`
- `startStream()`
- `stopStream()`
- `saveProfile()`
- `rebootToRun()`
- `rebootToConfig()`

## Expected State Changes
### On connect
- Browser requests the CONFIG service and its characteristics.
- JavaScript subscribes to `EVT` immediately.
- `STREAM` is subscribed on demand.
- `CFG` is requested when available.

### On config retrieval
1. JavaScript attempts `CFG` read first.
2. If unavailable or failing, JavaScript falls back to `get_config` command response.
3. If firmware advertises `transport: "cfg_read"`, JavaScript retries via `CFG`.

### On descriptor retrieval
- Firmware emits type-2 `EVT` chunks.
- JavaScript assembles the descriptor and caches it by `deviceId`.

### On stream sample receipt
- Firmware emits a 16-byte binary packet.
- JavaScript parses it into a sample object and updates live telemetry state.

## Known Limitations
- The canonical contract currently lives in C++ comments plus shared JavaScript parsing code; it is not schema-generated.
- Any future firmware change to EVT framing, STREAM layout, or config schema must be mirrored in `webapp/shared/ble_client.js` immediately.
