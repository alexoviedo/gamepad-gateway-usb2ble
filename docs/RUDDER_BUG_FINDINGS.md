# Rudder bug findings

## Symptom

When multiple HID devices are connected (stick + throttle + pedals), the BLE gamepad calibration on PC shows that **toe brakes** change correctly, but **rudder deflection (Z)** is **intermittent/flaky**.

On the ESP32 serial logs, you can observe that a different device's “Z-like” axis sometimes appears to “win” even when the pedals rudder is moving.

## Root cause

The legacy merge strategy combined per-device `GamepadState` values using:

> **per axis:** choose the device with the **maximum absolute deflection**

This is not a safe merge for flight controls because:

* Many devices expose multiple axes with overlapping semantics (e.g., `Z`, `Rz`, `Slider`).
* Some axes may sit near an extreme at rest (or have different logical ranges), causing them to **dominate** the “max abs” heuristic.
* The “winning” device can change based on tiny fluctuations, which appears as intermittent control in the merged output.

In practice, this causes the pedals’ rudder axis to be overwritten by another device’s axis that happens to have a larger absolute value.

## Fix summary

**Phase 2 fix:** replace the merge heuristic with a **deterministic mapping engine**.

Key changes:

1. **One source per output axis**
   * Each output axis (`x y z rx ry rz slider1 slider2 hat`) is mapped to **exactly one** `(device_id, element_id)` source.
   * This removes cross-device axis collisions entirely.

2. **Runtime default profile + role-based heuristics**
   * A default `MappingProfile` is generated at runtime using:
     * `role_guess` (stick vs throttle vs pedals)
     * HID `usage_page/usage`
   * Importantly, **rudder (Z)** is selected from the **pedals** device when present, preventing collisions with stick/throttle axes.

3. **Optional processing pipeline**
   * Per-axis modifiers exist (invert, deadzone, EMA smoothing, curve placeholder).
   * Defaults are set to preserve existing behavior until you add a configuration UI.

4. **Logging for verification**
   * On profile generation, logs print a compact “default mapping” so you can confirm which `(device_id, element_id)` was chosen for each output axis.

## Before / after (conceptual)

**Before**

```
USB report -> (per-device decode) -> GamepadState per device
                         |
                         +--> MERGE (max abs per axis) -> final GamepadState
```

**After**

```
USB report -> InputElements[] per device
                         |
                         +--> MappingEngine(profile) -> final GamepadState
                                   (one source per axis)
```

## Notes / limitations

* The profile is **in-memory only** for now (no persistence, no WebBLE config service yet).
* Buttons are still **OR-combined by default**, but the profile model is ready for future overrides.

## Phase 2.1 refinements (recommended)

During real-world testing, we observed that some HOTAS throttles include a **mini-stick** that exposes **X/Y axes**. A naive role classifier ("if it has X/Y, it must be a stick") can therefore misclassify a throttle as a stick.

If the runtime default profile is regenerated after each device connects, that misclassification can cause the default mapping to bind primary outputs (X/Y/Z) to the throttle’s mini-stick instead of the joystick/pedals — which *looks* like intermittent axis “winning” again, even though the merge heuristic was removed.

**Phase 2.1 changes:**

1. **Score-based role classification**
   * Replace the old count-based `role_guess` with a score-based heuristic that accounts for:
     * total axis count (many axes strongly suggests throttle)
     * presence of `Slider`/`Dial`
     * buttons/hats
     * Simulation Controls usages (Throttle/Rudder/Brake)
   * This reliably classifies throttles with mini-sticks as `THROTTLE`, not `STICK`.

2. **Default mapping prioritization**
   * Keep primary stick axes (X/Y) coming from the real stick.
   * Keep rudder (Z) coming from pedals when present (otherwise fall back to stick twist / throttle rocker deterministically).

3. **Toe brake mapping**
   * Map differential brakes to output `Rx`/`Ry` when available (common for pedals).
   * Optionally expose a combined brake on `slider2` (max of the two) for convenience.

These changes preserve determinism and prevent the default profile from accidentally selecting the “wrong” X/Y/Z sources when multiple devices are present.
