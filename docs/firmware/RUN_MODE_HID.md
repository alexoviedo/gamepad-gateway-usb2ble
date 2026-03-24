# Firmware: RUN-Mode HID Output

## Purpose
Describe the BLE HID gamepad services and the exact 21-byte input report format used in RUN mode.

Related documents:
- [../interfaces/BLE_CONTRACTS.md](../interfaces/BLE_CONTRACTS.md)
- [../interfaces/HID_TO_GAMEPAD_MAPPING.md](../interfaces/HID_TO_GAMEPAD_MAPPING.md)
- [CONFIG_SERVICE.md](CONFIG_SERVICE.md)

## Dependencies
- `main/ble_gamepad.cpp`
- `main/shared_types.h`
- `main/mapping_engine.cpp`

## Data Structures/Interfaces
### GATT services in RUN mode
- Device Information Service (`0x180A`)
- Battery Service (`0x180F`)
- HID Service (`0x1812`)

### HID wire contract
- Input report ID: `0x03`
- Input report size: `21 bytes`
- Buttons: 32 bits
- Axes: 8 × unsigned 16-bit values serialized from the canonical `GamepadState`
- Hat: 1 byte

### Wire order
1. buttons[0..31] little-endian (`4 bytes`)
2. `x`
3. `y`
4. `z`
5. `rz`
6. `rx`
7. `ry`
8. `slider1`
9. `slider2`
10. `hat`

### Axis encoding
- Source domain in firmware: signed `int16` nominally in `[-32767, 32767]`
- Serializer transform:
  - `t = v + 32768`
  - clamp to `[0, 65535]`
  - right shift by 1
  - final wire value in `[0, 32767]`

## Expected State Changes
- On state change, cached report updates and `ble_gatts_chr_updated()` is called.

## Known Limitations
- HID wire order is not the same textual order as the canonical semantics list; this is deliberate.
- Battery level is currently static at `100`.
