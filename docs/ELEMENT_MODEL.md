# Input Element Model

This project is transitioning from decoding USB HID reports directly into a fixed `GamepadState` to a **generic, descriptor‑derived element table**.

The new pipeline is:

```
USB HID input report bytes
  -> descriptor-derived InputElement[] per device/interface
      -> runtime element value updates (raw + normalized)
          -> (temporary) adapter to legacy GamepadState
              -> BLE gamepad output (unchanged for now)
```

## Why

Many HID devices (especially flight controls) expose controls using usages that don’t map cleanly into a gamepad schema, may use multiple report IDs, and may pack data in non‑obvious ways. Collapsing too early makes debugging hard and can hide issues (e.g., a rudder axis that is present in the descriptor but decoded incorrectly).

The element model provides:

* A **complete list of all non‑constant INPUT fields** from the report descriptor.
* A uniform way to decode and inspect values regardless of whether the usage is “known”.
* A stable foundation for a future mapping UI/service (not implemented yet).

## `InputElement` schema

Defined in `main/input_elements.h`.

### Identity

| Field | Type | Meaning |
|---|---:|---|
| `element_id` | `uint32_t` | Stable within a given descriptor. Computed as a hash of usage, report_id, bit offset/size, logical range, and relative/absolute flag. |

### Classification

| Field | Type | Meaning |
|---|---:|---|
| `kind` | enum | Heuristic classification: `axis`, `button`, `hat`, `other`. |
| `usage_page` | `uint16_t` | HID Usage Page (from descriptor). |
| `usage` | `uint16_t` | HID Usage (from descriptor). |

### Report location

All bit offsets are measured **relative to the report payload**, meaning **excluding** the report ID byte (if the device uses report IDs).

| Field | Type | Meaning |
|---|---:|---|
| `report_id` | `uint8_t` | Report ID this field belongs to. `0` means no report IDs are used. |
| `bit_offset` | `uint16_t` | Bit offset into the payload. |
| `bit_size` | `uint16_t` | Field width in bits. |

### Logical range and signedness

| Field | Type | Meaning |
|---|---:|---|
| `logical_min` | `int32_t` | HID logical minimum. |
| `logical_max` | `int32_t` | HID logical maximum. |
| `is_signed` | `uint8_t` | If set, decoded values are sign‑extended using `bit_size`. |

### Behavior flags

Derived from the HID **Input** item flags:

| Field | Type | Meaning |
|---|---:|---|
| `is_relative` | `uint8_t` | Relative vs absolute. |
| `is_absolute` | `uint8_t` | Absolute vs relative (inverse of `is_relative`). |
| `is_variable` | `uint8_t` | Variable vs Array. |

### Runtime value fields

These are updated when decoding reports:

| Field | Type | Meaning |
|---|---:|---|
| `raw` | `int32_t` | Extracted integer value (sign‑extended if `is_signed`). |
| `norm_0_1` | `float` | Normalized to `[0..1]` using logical min/max, with clamping. |
| `norm_m1_1` | `float` | Derived from `norm_0_1` and mapped to `[-1..1]`. |
| `last_update_ms` | `uint32_t` | Timestamp (ms since boot) when `raw` last changed. |

## Normalization rules

Normalization uses the logical range:

* If `logical_max == logical_min`, normalization returns `0.0`.
* `norm_0_1 = clamp((raw - logical_min) / (logical_max - logical_min), 0..1)`
* `norm_m1_1 = (norm_0_1 * 2) - 1`

For **buttons** (usage page `0x09`), `raw` will typically be `0` or `1`.

For **hat switches** (Generic Desktop `0x01`, usage `0x39`), the adapter treats values outside the logical range as “centered/neutral”.

For **relative** elements, `raw` represents a delta. Normalization still uses the descriptor range for display/debugging, but consumers should interpret the value as a delta.

## Descriptor dumping and sampling

On connect, the device manager logs a descriptor snapshot containing **all** Input elements (including unknown usages).

During runtime, the device manager periodically logs **sample updates** for elements whose values changed recently. This is intended to confirm that every physical control (including rudder) produces updates.

## Temporary adapter

The adapter (currently in `main/input_decoder.cpp`) maps a subset of common usages into the legacy `GamepadState`:

* Generic Desktop axes: X/Y/Z, Rx/Ry/Rz, Slider
* Generic Desktop hat: Hat switch
* Simulation controls: Rudder, Throttle, ToeBrake
* Buttons page: Button 1..32

This preserves BLE gamepad behavior while enabling the next phases to build an explicit mapping layer.
