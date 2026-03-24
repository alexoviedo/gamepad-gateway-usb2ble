// main/ble_config_service.h
#pragma once

#include <stdint.h>
#include <stddef.h>

// Forward declaration to avoid pulling NimBLE headers into all compilation units.
struct ble_gatt_svc_def;

#ifdef __cplusplus
extern "C" {
#endif

// Called by the BLE stack init when booting in CONFIG mode.
void ble_config_service_init(void);

// Called on GAP connection lifecycle events.
void ble_config_service_on_connect(uint16_t conn_handle);
void ble_config_service_on_disconnect(void);

// Called on GAP subscribe events.
void ble_config_service_on_subscribe(uint16_t attr_handle, uint8_t cur_notify);

// Called on GAP notify-tx completion events.
//
// Current implementation note:
//   - This callback is intentionally empty on current main.
//   - Chunked EVT pacing is handled inside notify_evt_chunked() in
//     ble_config_service.cpp via retry/backoff delays and connection-state checks.
//   - Future maintainers should not assume notify completion callbacks are part of
//     the active pacing algorithm unless the implementation changes.
void ble_config_service_on_notify_tx(uint16_t attr_handle, int status);

// 60Hz tick (called from an app task) to emit STREAM notifications when enabled.
//
// STREAM binary payload contract (16 bytes, little-endian):
//   byte  0 : u8   version
//   byte  1 : u8   flags
//   byte  2 : u32  device_id
//   byte  6 : u32  element_id
//   byte 10 : i32  raw
//   byte 14 : i16  norm_q15
//
// Notes:
//   - version is currently 1
//   - flags is currently 0 on current main
//   - norm_q15 is the normalized control value in signed Q15-style units
//     derived from InputElement::norm_m1_1 and clamped to [-32767, 32767]
void ble_config_service_stream_tick(void);

// Returns a pointer to a NimBLE service definition array.
// The array is terminated by {0}. In this project we return exactly one primary
// service plus a terminator.
const struct ble_gatt_svc_def *ble_config_service_gatt_defs(void);

// Expose the service UUID string values for logging/docs.
const char *ble_config_service_uuid_str(void);

#ifdef __cplusplus
} // extern "C"
#endif
