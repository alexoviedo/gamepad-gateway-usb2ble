#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Compile-time gate:
// - Preferred: CONFIG_VERBOSE_HID_DEBUG (set via menuconfig / Kconfig.projbuild)
// - Alternate: -DGPG_VERBOSE_HID_DEBUG=1 passed to CMake
#if ((defined(CONFIG_VERBOSE_HID_DEBUG) && CONFIG_VERBOSE_HID_DEBUG) || (defined(GPG_VERBOSE_HID_DEBUG) && GPG_VERBOSE_HID_DEBUG))
#define HID_VERBOSE_HID_DEBUG 1
#else
#define HID_VERBOSE_HID_DEBUG 0
#endif

struct GamepadState;
struct HidDeviceContext;

#if HID_VERBOSE_HID_DEBUG

// Start any debug-only background helpers (e.g., USB VID/PID cache).
void hid_verbose_init(void);

// Best-effort VID/PID lookup by USB address.
// Returns true if known.
bool hid_verbose_get_vidpid(uint8_t usb_addr, uint16_t *vid, uint16_t *pid);

// Update per-device report stats (cadence, last report metadata)
void hid_verbose_note_report(struct HidDeviceContext *ctx, uint8_t report_id,
                             size_t report_len, const uint8_t *report_data,
                             size_t report_data_len, bool uses_report_ids);

// Per-device decoded state dump (rate-limited)
bool hid_verbose_maybe_dump_device_state(const struct HidDeviceContext *ctx);

// Merge diagnostics (winner per axis + collisions) (rate-limited)
void hid_verbose_log_merge(const struct HidDeviceContext *contexts,
                           size_t num_contexts, const struct GamepadState *merged);

#endif // HID_VERBOSE_HID_DEBUG
