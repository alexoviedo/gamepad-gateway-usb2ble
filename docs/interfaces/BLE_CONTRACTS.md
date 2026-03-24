# BLE Contracts

## Purpose
Absolute source of truth for BLE-facing data contracts on the current `main` branch.

This file maps firmware payloads directly to their corresponding JavaScript handling logic in `webapp/shared/ble_client.js`.

## Dependencies
### Firmware sources
- `main/ble_config_service.h`
- `main/ble_config_service.cpp`
- `main/mapping_engine.h`
- `main/ble_gamepad.cpp`

### WebApp sources
- `webapp/shared/ble_client.js`

## Data Structures/Interfaces
## 1. CONFIG service UUIDs
### Primary service
| Name | UUID | Firmware source | JavaScript source |
|---|---|---|---|
| CONFIG service | `6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a1` | `ble_config_service.cpp` / `ble_config_service.h` | `BLE_UUIDS.service` |

### Characteristics
| Name | UUID | Direction | Firmware behavior | JavaScript behavior |
|---|---|---|---|---|
| `CMD` | `6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a2` | WebApp → firmware | JSON command write | `BLE_UUIDS.cmd`, `writeCommandText()`, `sendCommand()` |
| `EVT` | `6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a3` | firmware → WebApp | chunked framed notifications | `BLE_UUIDS.evt`, `ensureEvtSubscription()`, `ChunkAssembler.push()`, `handleEvtNotification()` |
| `STREAM` | `6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a4` | firmware → WebApp | 16-byte binary telemetry sample | `BLE_UUIDS.stream`, `ensureStreamSubscription()`, `handleStreamNotification()` |
| `CFG` | `6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a5` | bidirectional | full config JSON blob read/write | `BLE_UUIDS.cfg`, `ensureConfigCharacteristic()`, `readConfigCharacteristic()`, `getConfig()` |

## 2. EVT framing contract
### Frame scope
`EVT` is a chunked notification transport. A logical payload may arrive as one or more chunks.

### Header layout
| Byte offset | Size | Type | Meaning |
|---|---:|---|---|
| 0 | 1 | `uint8` | protocol version |
| 1 | 1 | `uint8` | frame type |
| 2 | 2 | `uint16 LE` | `msg_id` |
| 4 | 2 | `uint16 LE` | `offset` |
| 6 | 2 | `uint16 LE` | `total_len` |
| 8.. | variable | bytes | chunk payload |

### Current frame types
| Type | Meaning | Firmware emitter | JavaScript consumer |
|---|---|---|---|
| `1` | JSON response payload | `notify_evt_json()` / `notify_evt_chunked(1, ...)` | `ChunkAssembler.push()` → `handleEvtNotification()` JSON branch |
| `2` | raw HID report descriptor bytes | `notify_evt_chunked(2, ...)` | `ChunkAssembler.push()` → `handleEvtNotification()` descriptor branch |

### Current non-types
- Current `main` does **not** emit an active EVT type `3` config payload.
- Configuration retrieval is performed via:
  - `CFG` characteristic read
  - or inline JSON response fallback to `get_config`

### JavaScript symmetry
| Firmware concept | JavaScript mirror |
|---|---|
| chunked notify with `msg_id`, `offset`, `total_len` | `ChunkAssembler` class |
| JSON `EVT` completion | `handleEvtNotification()` → `JSON.parse()` |
| descriptor payload completion | `handleEvtNotification()` → `descriptorCache.set(...)` |

## 3. STREAM binary contract
### Wire format
`STREAM` is a packed 16-byte little-endian payload.

| Byte offset | Size | Type | Firmware field | Meaning | JavaScript parse field |
|---|---:|---|---|---|---|
| 0 | 1 | `uint8` | `version` | sample version, currently `1` | `sample.version = view.getUint8(0)` |
| 1 | 1 | `uint8` | `flags` | flags byte, currently `0` on `main` | `sample.flags = view.getUint8(1)` |
| 2 | 4 | `uint32 LE` | `device_id` | session-local device identifier | `sample.deviceId = view.getUint32(2, true)` |
| 6 | 4 | `uint32 LE` | `element_id` | descriptor-derived element identifier | `sample.elementId = view.getUint32(6, true)` |
| 10 | 4 | `int32 LE` | `raw` | raw control value | `sample.raw = view.getInt32(10, true)` |
| 14 | 2 | `int16 LE` | `norm_q15` | normalized control value in signed Q15-style units | `sample.normQ15 = view.getInt16(14, true)` |

### Derived JavaScript values
`handleStreamNotification()` also derives:
- `sample.norm = sample.normQ15 / 32767`
- `sample.receivedAt = new Date()`
- `sample.meta = getElementMeta(sample.deviceId, sample.elementId)` when available

### Firmware source of values
- `device_id` selected in `ble_config_service_stream_tick()` from current active device list
- `element_id` selected from `InputElement.element_id`
- `raw` sourced from `InputElement.raw`
- `norm_q15` produced by `clamp_q15(InputElement.norm_m1_1)`

## 4. JSON command contract
### Request envelope
All commands written to `CMD` are JSON objects with this base shape:

```json
{
  "rid": 1,
  "cmd": "get_devices"
}
```

### Response envelope
Firmware wraps command responses as JSON with this base shape:

```json
{
  "evt": "resp",
  "rid": 1,
  "cmd": "get_devices"
}
```

### JavaScript correlation
| Firmware concept | JavaScript mirror |
|---|---|
| `rid` request identifier | `this.requestId` in `sendCommand()` |
| pending response table | `this.pendingJson` |
| timeout and cleanup | `resolveCommandTimeout()` and `sendCommand()` promise timeout |
| response dispatch | `handleEvtNotification()` |

## 5. Command matrix
| Command | Request shape | Firmware result | JavaScript entrypoint | JavaScript result handling |
|---|---|---|---|---|
| `get_devices` | `{ "rid", "cmd": "get_devices" }` | JSON `devices[]` | `getDevices()` | updates `devices[]`, prunes caches |
| `get_elements` | `{ "rid", "cmd": "get_elements", "device_id" }` | JSON `device_id`, `elements[]` | `getElements(deviceId)` | updates `elementMetaByDevice` |
| `get_descriptor` | `{ "rid", "cmd": "get_descriptor", "device_id" }` | JSON metadata + EVT type-2 binary payload | `getDescriptor(deviceId)` | caches descriptor in `descriptorCache` |
| `start_stream` | `{ "rid", "cmd": "start_stream" }` | JSON `{ "streaming": true }` | `startStream()` | sets `streamActive = true` |
| `stop_stream` | `{ "rid", "cmd": "stop_stream" }` | JSON `{ "streaming": false }` | `stopStream()` | sets `streamActive = false` |
| `get_config` | `{ "rid", "cmd": "get_config" }` | current `main`: JSON `{ "ok": true, "config_len": N, "transport": "cfg_read" }` | `getConfig()` fallback path | if CFG-first failed, may re-read `CFG` when `transport === "cfg_read"` |
| `set_config` | `{ "rid", "cmd": "set_config", "config": { ... } }` | JSON `{ "ok": true, "config": normalizedProfile }` on success | `applyAxisConfig()`, `sendCommand()` | updates `currentConfig` and `pendingChanges` |
| `save_profile` | `{ "rid", "cmd": "save_profile" }` | JSON `{ "ok": true, "note": "profile saved to NVS", "bytes": N, "config": ... }` | `saveProfile()` | updates `savedConfigString`, clears `pendingChanges` |
| `reboot_to_run` | `{ "rid", "cmd": "reboot_to_run" }` | JSON `{ "ok": true, "rebooting_to": "run" }` | `rebootToRun()` | caller handles UX only |
| `reboot_to_config` | `{ "rid", "cmd": "reboot_to_config" }` | JSON `{ "ok": true, "rebooting_to": "config" }` | `rebootToConfig()` | caller handles UX only |

## 6. Configuration JSON schema
### Source of truth
The schema is documented in `main/mapping_engine.h` and normalized/consumed by `webapp/shared/ble_client.js` via `normalizeConfig` callbacks supplied by the page clients.

### Current canonical shape
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
| `version` | number | current schema version, `2` |
| `buttons_or_combine` | boolean | current combined-button behavior flag |
| `replace_all` | boolean | optional patch-mode control; if `true`, firmware clears existing active profile before applying the supplied patch |
| `axes` | object | sparse object keyed by canonical output axis name |

### Axis keys
- `x`
- `y`
- `z`
- `rx`
- `ry`
- `rz`
- `slider1`
- `slider2`
- `hat`

### Axis mapping object
| Field | Type | Meaning |
|---|---|---|
| `configured` | boolean | whether the axis is actively mapped |
| `device_id` | number | session-local device identifier |
| `element_id` | number | element identifier within that device |
| `invert` | boolean | invert mapped value |
| `deadzone.inner` | number | inner deadzone |
| `deadzone.outer` | number | outer clamp |
| `smoothing_alpha` | number | EMA alpha |
| `curve.type` | string | currently `bezier` |
| `curve.p1.x`, `curve.p1.y`, `curve.p2.x`, `curve.p2.y` | number | cubic Bezier control points |

### JavaScript symmetry
| Firmware schema concept | JavaScript mirror in `ble_client.js` |
|---|---|
| `version: 2` | `BleClient` initializes `currentConfig` with `{ version: 2, axes: {} }` |
| `replace_all` accepted by firmware parser | passes through via `sendCommand({ cmd: 'set_config', config })` |
| `device_id`, `element_id` mapping fields | `applyAxisPatch()` and `applyAxisConfig()` |
| normalized config state | `getConfig()` and `applyAxisConfig()` update `currentConfig` |
| saved-vs-pending config state | `savedConfigString`, `pendingChanges` |

## 7. CFG-first configuration retrieval contract
### Current behavior
`BleClient.getConfig()` uses this order:
1. `readConfigCharacteristic()`
2. fallback to `sendCommand({ cmd: 'get_config' })`
3. accept one of:
   - `response.config`
   - `response.config_json`
   - `response.transport === "cfg_read"`, then re-read `CFG`

### Why this matters
This is the current symmetry point between firmware and WebApp for configuration retrieval. Future assistants should preserve this ordering unless firmware contracts are intentionally changed.

## 8. RUN-mode BLE HID summary
### Purpose
CONFIG service contracts above are for WebApp communication only.

RUN mode is a separate BLE HID surface and is not parsed by `webapp/shared/ble_client.js`.

### Current wire summary
- HID input report size: `21 bytes`
- payload structure:
  - `buttons`: 4 bytes
  - `axes`: 16 bytes (`8 × uint16 LE`)
  - `hat`: 1 byte

## Expected State Changes
- successful `connect()` resolves service + characteristics and subscribes to `EVT`
- successful `getDevices()` refreshes `devices[]`
- successful `getElements()` refreshes `elementMetaByDevice`
- successful `getDescriptor()` refreshes `descriptorCache`
- successful `getConfig()` refreshes `currentConfig`
- successful `startStream()` enables `STREAM` notifications and sample parsing

## Known Limitations
- `device_id` is session-local, not a stable hardware UUID
- `CFG` may be unavailable on older firmware builds, so `getConfig()` still preserves inline fallback behavior
- `EVT` framing and `STREAM` parsing are implemented in JavaScript directly rather than generated from a shared schema file
