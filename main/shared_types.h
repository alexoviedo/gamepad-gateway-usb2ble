#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus

// Strongly typed Hat direction. Must fit in 8 bits to align with BLE payload schema.
enum class HatDirection : uint8_t {
    CENTER = 0,
    N  = 1,
    NE = 2,
    E  = 3,
    SE = 4,
    S  = 5,
    SW = 6,
    W  = 7,
    NW = 8,
};

// Unified Output State for BLE Gamepad
struct GamepadState {
    int16_t x;
    int16_t y;
    int16_t z;
    int16_t rx;
    int16_t ry;
    int16_t rz;
    int16_t slider1;
    int16_t slider2;
    HatDirection hat;
    uint32_t buttons;
};

// Device Role Classification
enum class DeviceRole : uint8_t {
    UNKNOWN = 0,
    STICK,
    THROTTLE,
    PEDALS
};

#else
// C fallback
typedef uint8_t HatDirection;

struct GamepadState {
    int16_t x;
    int16_t y;
    int16_t z;
    int16_t rx;
    int16_t ry;
    int16_t rz;
    int16_t slider1;
    int16_t slider2;
    HatDirection hat; // 0=center, 1=N, 2=NE, 3=E, 4=SE, 5=S, 6=SW, 7=W, 8=NW
    uint32_t buttons;
};
#endif
