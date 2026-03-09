# Config Schema

This document defines the canonical JSON structure used by the ESP32 Config Service for live mapping updates and persisted profiles.

## Root object

```json
{
  "version": 2,
  "buttons_or_combine": true,
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
      "smoothing_alpha": 0.18,
      "curve": {
        "type": "bezier",
        "p1": { "x": 0.24, "y": 0.10 },
        "p2": { "x": 0.78, "y": 0.96 }
      }
    }
  }
}
```

## Fields

### `version`
- Integer.
- Current schema version is `2`.

### `buttons_or_combine`
- Boolean.
- Preserves the current button-combine behavior in the mapping engine.

### `axes`
- Object keyed by output axis name.
- Supported keys:
  - `x`
  - `y`
  - `z`
  - `rx`
  - `ry`
  - `rz`
  - `slider1`
  - `slider2`
  - `hat`

Each axis entry may be omitted, set to `null`, or set to an object.

## Axis mapping object

```json
{
  "configured": true,
  "device_id": 57,
  "element_id": 17736,
  "invert": false,
  "deadzone": {
    "inner": 0.02,
    "outer": 0.00
  },
  "smoothing_alpha": 0.12,
  "curve": {
    "type": "bezier",
    "p1": { "x": 0.25, "y": 0.18 },
    "p2": { "x": 0.75, "y": 0.94 }
  }
}
```

### `configured`
- Boolean.
- If `false`, the output is treated as unmapped.

### `device_id`
- Integer.
- Matches the `device_id` value returned by `get_devices` and `descriptor` metadata.

### `element_id`
- Integer.
- Matches the `element_id` value returned in descriptor metadata.

### `invert`
- Boolean.
- Applies before deadzone and curve processing.

### `deadzone`
- Object.
- `inner`: `0.0..0.99`
- `outer`: `0.0..0.99`
- `inner + outer` should stay below `0.98`.

Behavior:
- `inner` suppresses small motion near center.
- `outer` clamps near-extreme motion to full scale early.

### `smoothing_alpha`
- Float `0.0..1.0`
- `0.0` disables EMA smoothing.
- Higher values follow the input more aggressively.

### `curve`
- Object.
- Current supported type is `bezier`.

### `curve.type`
- String.
- Canonical value: `"bezier"`

### `curve.p1`, `curve.p2`
- Objects with `x` and `y` fields.
- Each coordinate must be in `0.0..1.0`.
- Endpoints are implicitly fixed at `(0,0)` and `(1,1)`.

## Patch semantics

`set_config` accepts partial updates. You only need to send the axis you want to change.

Example: update Rudder modifiers only.

```json
{
  "version": 2,
  "axes": {
    "z": {
      "configured": true,
      "device_id": 101,
      "element_id": 54909,
      "invert": false,
      "deadzone": {
        "inner": 0.05,
        "outer": 0.01
      },
      "smoothing_alpha": 0.25,
      "curve": {
        "type": "bezier",
        "p1": { "x": 0.20, "y": 0.08 },
        "p2": { "x": 0.80, "y": 0.98 }
      }
    }
  }
}
```

## Clearing an axis mapping

```json
{
  "version": 2,
  "axes": {
    "ry": null
  }
}
```

Or:

```json
{
  "version": 2,
  "axes": {
    "ry": {
      "configured": false
    }
  }
}
```

## Persistence

- `set_config` updates the active in-memory mapping immediately.
- `save_profile` persists the current canonical config JSON to NVS.
- On boot, the firmware validates the stored version, length, and CRC before applying the profile.
- If validation fails, the firmware falls back cleanly to heuristic defaults.

## Legacy compatibility

The firmware still tolerates older payloads containing:
- scalar `deadzone`
- scalar `outer_clamp`

Those legacy fields are normalized into the canonical `deadzone` object internally.
