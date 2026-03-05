#include "hid_device_identity.h"

#include <string.h>
#include <stdio.h>

static uint32_t fnv1a32(const void *data, size_t len, uint32_t seed) {
  const uint8_t *p = (const uint8_t *)data;
  uint32_t h = seed;
  for (size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

void hid_identity_init(HidDeviceIdentity *id) {
  if (!id) return;
  memset(id, 0, sizeof(*id));
  id->stable_hash = 0;
}

uint32_t hid_identity_compute_hash(const HidDeviceIdentity *id) {
  if (!id) return 0;
  uint32_t h = 2166136261u;

  // Prefer stable-ish fields; session_handle_tag intentionally excluded.
  h = fnv1a32(&id->vid, sizeof(id->vid), h);
  h = fnv1a32(&id->pid, sizeof(id->pid), h);
  h = fnv1a32(&id->iface_num, sizeof(id->iface_num), h);
  h = fnv1a32(&id->report_desc_crc32, sizeof(id->report_desc_crc32), h);

  // Strings are best-effort. Include if present.
  if (id->manufacturer[0]) {
    h = fnv1a32(id->manufacturer, strnlen(id->manufacturer, sizeof(id->manufacturer)), h);
  }
  if (id->product[0]) {
    h = fnv1a32(id->product, strnlen(id->product, sizeof(id->product)), h);
  }

  // If we *still* don't have anything, include dev_addr as a last resort.
  if (h == 2166136261u) {
    h = fnv1a32(&id->dev_addr, sizeof(id->dev_addr), h);
  }

  return h;
}

void hid_identity_refresh_hash(HidDeviceIdentity *id) {
  if (!id) return;
  id->stable_hash = hid_identity_compute_hash(id);
}

char *hid_identity_to_string(const HidDeviceIdentity *id, char *out, size_t out_sz) {
  if (!out || out_sz == 0) return out;
  if (!id) {
    snprintf(out, out_sz, "<null>");
    return out;
  }

  // Compact, but include enough to distinguish devices.
  // Examples:
  //  addr=2 if=0 vid=044F pid=B687 crc=1A2B3C4D hash=9F12AB34 mfg='Thrustmaster' prod='TFRP'
  snprintf(out, out_sz,
           "addr=%u if=%u vid=%04X pid=%04X crc=%08X hash=%08X mfg='%s' prod='%s'",
           (unsigned)id->dev_addr, (unsigned)id->iface_num,
           (unsigned)id->vid, (unsigned)id->pid,
           (unsigned)id->report_desc_crc32, (unsigned)id->stable_hash,
           id->manufacturer[0] ? id->manufacturer : "?",
           id->product[0] ? id->product : "?");
  return out;
}
