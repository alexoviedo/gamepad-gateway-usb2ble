# Config Schema

## Purpose
Describe the JSON configuration schema applied by the firmware mapping engine and consumed by the web clients.

Related documents:
- [BLE_CONTRACTS.md](BLE_CONTRACTS.md)
- [HID_TO_GAMEPAD_MAPPING.md](HID_TO_GAMEPAD_MAPPING.md)
- [../firmware/CONFIG_SERVICE.md](../firmware/CONFIG_SERVICE.md)
- [../../AUDIT_REPORT.md](../../AUDIT_REPORT.md)

## Dependencies
- `main/mapping_engine.h`
- `main/mapping_engine.cpp`
- `webapp/app.js`
- `webapp/validation.js`

## Data Structures/Interfaces
### Current canonical shape emitted by firmware
```json
{
  "version": 2,
  "buttons_or_combine": true,
  "axes": {
    "z": {
      "configured": true,
      "device_id": 3,
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
| Field | Type | Required | Notes |
|---|---|---|---|
| `version` | number | emitted by firmware | current emitted value is `2` |
| `buttons_or_combine` | boolean | optional on wire | defaults to `true` |
| `replace_all` | boolean | optional in patches | parser-supported, used by web import/reset flows |
| `axes` | object | yes | sparse object keyed by output axis name |

### Axis keys
- `x`, `y`, `z`, `rx`, `ry`, `rz`, `slider1`, `slider2`, `hat`

### Axis mapping object
| Field | Type | Notes |
|---|---|---|
| `configured` | boolean | false or omitted can clear/unmap in patch logic |
| `device_id` | number | session-local device identifier |
| `element_id` | number | descriptor-derived or virtual element identifier |
| `invert` | boolean | default `false` |
| `deadzone.inner` | number | clamped to `[0, 0.99]` |
| `deadzone.outer` | number | clamped to `[0, 0.99]` |
| `smoothing_alpha` | number | clamped to `[0, 1]` |
| `curve.type` | string | current value is effectively `bezier` |
| `curve.p1.x`, `curve.p1.y`, `curve.p2.x`, `curve.p2.y` | number | clamped to `[0, 1]` |

### Patch semantics
- If `replace_all: true`, firmware clears the existing profile before applying provided axes.
- If an axis key is omitted, its existing mapping remains unchanged unless `replace_all: true` was applied first.
- If an axis object is `null`, firmware clears that mapping.
- Legacy scalar deadzone form is still tolerated (`deadzone: 0.05`).

## Expected State Changes
- Successful `set_config` updates the active in-memory profile.
- Successful `save_profile` persists the current profile.
- `mapping_engine_profile_to_json()` serializes only configured axes.

## Known Limitations
- `mapping_engine.h` comment examples still mention schema version `1`; implementation emits `2`.
- The schema is implemented in C++ and mirrored ad hoc in JS rather than generated from a single typed source.
