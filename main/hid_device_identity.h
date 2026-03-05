#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// A stable-ish identity for a USB HID device.
//
// Goals:
//  - Unique enough within a session to avoid mis-attributing reports
//  - As stable as possible across reconnects (where possible)
//  - Printable for logging/debugging
//
// Notes:
//  - USB address (dev_addr) can change on reconnect.
//  - VID/PID and string descriptors are best-effort; may be unavailable.
//  - report_desc_crc32 is derived from the HID report descriptor and is
//    typically stable across reconnects for the same device/interface.

typedef struct HidDeviceIdentity {
  // Session-only linkage (NOT stable across reconnects).
  uintptr_t session_handle_tag;  // (uintptr_t)hid_host_device_handle_t

  // USB/Interface routing
  uint8_t dev_addr;   // USB address assigned by the host
  uint8_t iface_num;  // USB interface number

  // Best-effort identifiers
  uint16_t vid;
  uint16_t pid;

  // Best-effort string descriptors (UTF-8, NUL terminated)
  char manufacturer[64];
  char product[64];

  // Derived fingerprint
  uint32_t report_desc_crc32;

  // Cached stable key/hash for quick matching/logging
  uint32_t stable_hash;
} HidDeviceIdentity;

void hid_identity_init(HidDeviceIdentity *id);

// Compute a stable-ish hash from available fields (NOT cryptographic).
uint32_t hid_identity_compute_hash(const HidDeviceIdentity *id);

// Update stable_hash after you set fields.
void hid_identity_refresh_hash(HidDeviceIdentity *id);

// Format identity into a human readable string.
// Returns out.
char *hid_identity_to_string(const HidDeviceIdentity *id, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif
