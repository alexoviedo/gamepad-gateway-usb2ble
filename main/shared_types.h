#pragma once
#include <stdint.h>
#include <stdbool.h>

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
    uint8_t hat; // 0=center, 1=N, 2=NE, 3=E, 4=SE, 5=S, 6=SW, 7=W, 8=NW
    uint32_t buttons;
};

// Device Role Classification
enum class DeviceRole {
    UNKNOWN = 0,
    STICK,
    THROTTLE,
    PEDALS
};
