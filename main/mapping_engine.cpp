#include "mapping_engine.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"

namespace mapping {

static const char *TAG = "MAP_ENG";

// Reserved virtual element ids (not present in descriptor).
// These allow deterministic single-source mappings for simple composites.
static constexpr ElementId kVirtualToeBrakeMax = 0xFFFFFFFFu;

static MappingProfile g_profile;
static bool g_profile_dirty = true;
static uint32_t g_devices_signature = 0;
static bool g_profile_logged_for_sig = false;

// EMA state (normalized) per output axis.
static float g_axis_ema[(size_t)OutputAxis::COUNT];
static bool g_axis_ema_valid[(size_t)OutputAxis::COUNT];

static inline DeviceId make_device_id(const HidDeviceContext &d) {
  // While connected, dev_addr is stable (assigned by the HID manager).
  // Reserve 0 as "invalid".
  return (DeviceId)d.dev_addr + 1;
}

void MappingProfile::clear() {
  memset(this, 0, sizeof(*this));
  buttons_or_combine = true;
}

void mapping_engine_init() {
  g_profile.clear();
  g_profile_dirty = true;
  g_devices_signature = 0;
  g_profile_logged_for_sig = false;
  memset(g_axis_ema, 0, sizeof(g_axis_ema));
  memset(g_axis_ema_valid, 0, sizeof(g_axis_ema_valid));
}

void mapping_engine_notify_devices_changed() {
  g_profile_dirty = true;
  g_profile_logged_for_sig = false;
  memset(g_axis_ema_valid, 0, sizeof(g_axis_ema_valid));
}

static uint32_t hash_u32(uint32_t h, uint32_t v) {
  h ^= v;
  h *= 16777619u;
  return h;
}

static uint32_t compute_devices_signature(const HidDeviceContext *devices,
                                         size_t num_devices) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < num_devices; i++) {
    const auto &d = devices[i];
    if (!d.active) continue;
    h = hash_u32(h, make_device_id(d));
    h = hash_u32(h, (uint32_t)d.caps.role);
    h = hash_u32(h, (uint32_t)d.caps.num_elements);
  }
  return h;
}

static const char *axis_name(OutputAxis a) {
  switch (a) {
    case OutputAxis::X: return "x";
    case OutputAxis::Y: return "y";
    case OutputAxis::Z: return "z";
    case OutputAxis::RX: return "rx";
    case OutputAxis::RY: return "ry";
    case OutputAxis::RZ: return "rz";
    case OutputAxis::SLIDER1: return "slider1";
    case OutputAxis::SLIDER2: return "slider2";
    case OutputAxis::HAT: return "hat";
    default: return "?";
  }
}

static const HidDeviceContext *find_device_for_role(const HidDeviceContext *devices,
                                                    size_t num_devices,
                                                    DeviceRole role) {
  const HidDeviceContext *best = nullptr;
  for (size_t i = 0; i < num_devices; i++) {
    const auto &d = devices[i];
    if (!d.active) continue;
    if (d.caps.role != role) continue;
    if (!best || d.caps.num_elements > best->caps.num_elements) best = &d;
  }
  return best;
}

static const InputElement *find_element_by_usage(const HidDeviceContext &dev,
                                                 InputElementKind kind,
                                                 uint16_t usage_page,
                                                 uint16_t usage) {
  for (size_t i = 0; i < dev.caps.num_elements; i++) {
    const auto &e = dev.caps.elements[i];
    if (e.kind != kind) continue;
    if (e.usage_page == usage_page && e.usage == usage) return &e;
  }
  return nullptr;
}

static const InputElement *find_first_axis_like(const HidDeviceContext &dev) {
  for (size_t i = 0; i < dev.caps.num_elements; i++) {
    const auto &e = dev.caps.elements[i];
    if (e.kind == IE_KIND_AXIS) return &e;
  }
  return nullptr;
}

static int find_sim_brake_axes(const HidDeviceContext &dev,
                              const InputElement **out_a,
                              const InputElement **out_b) {
  const InputElement *a = nullptr;
  const InputElement *b = nullptr;

  for (size_t i = 0; i < dev.caps.num_elements; i++) {
    const auto &e = dev.caps.elements[i];
    if (e.kind != IE_KIND_AXIS) continue;
    if (e.usage_page == 0x0002 && e.usage == 0x00BF) {
      if (!a) a = &e;
      else if (!b && &e != a) { b = &e; break; }
    }
  }

  if (out_a) *out_a = a;
  if (out_b) *out_b = b;
  return (a ? 1 : 0) + (b ? 1 : 0);
}

static int count_toe_brake_elements(const HidDeviceContext &dev) {
  int count = 0;
  for (size_t i = 0; i < dev.caps.num_elements; i++) {
    const auto &e = dev.caps.elements[i];
    if (e.kind != IE_KIND_AXIS) continue;
    if (e.usage_page == 0x0002 && e.usage == 0x00BF) count++;
  }
  return count;
}

static float get_virtual_toe_brake_max_norm(const HidDeviceContext &dev) {
  float best = 0.0f;
  bool any = false;
  for (size_t i = 0; i < dev.caps.num_elements; i++) {
    const auto &e = dev.caps.elements[i];
    if (e.kind != IE_KIND_AXIS) continue;
    if (e.usage_page == 0x0002 && e.usage == 0x00BF) {
      float v01 = (e.logical_min < 0) ? (e.norm_m1_1 + 1.0f) * 0.5f : e.norm_0_1;
      if (v01 > best) best = v01;
      any = true;
    }
  }
  return any ? best : 0.0f;
}

static AxisSource src_from_element(const HidDeviceContext &dev, const InputElement *e) {
  AxisSource s;
  if (!e) return s;
  s.device_id = make_device_id(dev);
  s.element_id = e->element_id;
  return s;
}

static void set_axis_mapping(MappingProfile &p, OutputAxis axis, AxisSource src) {
  auto &m = p.axes[(size_t)axis];
  m.configured = src.is_valid();
  m.source = src;
  // Defaults: preserve legacy behavior until UI/config exists.
  m.mod.invert = false;
  m.mod.deadzone = 0.0f;
  m.mod.smoothing_alpha = 0.0f;
  m.mod.curve = 1.0f;
}

static void build_default_profile(const HidDeviceContext *devices, size_t num_devices,
                                  MappingProfile &out) {
  out.clear();

  const HidDeviceContext *stick = find_device_for_role(devices, num_devices, DeviceRole::STICK);
  const HidDeviceContext *thr   = find_device_for_role(devices, num_devices, DeviceRole::THROTTLE);
  const HidDeviceContext *ped   = find_device_for_role(devices, num_devices, DeviceRole::PEDALS);

  bool embedded_pedals = false;
  if (!ped && thr) {
    const InputElement *z = find_element_by_usage(*thr, IE_KIND_AXIS, 0x0001, 0x0032); // GenericDesktop Z
    if (z && z->bit_size >= 16 && z->logical_max >= 32767) {
      ped = thr;
      embedded_pedals = true;
      ESP_LOGI(TAG, "No separate pedals device; using throttle Z axis as embedded pedals.");
    }
  }

  ESP_LOGI(TAG,
           "Default mapping devices: stick=%s thr=%s ped=%s",
           stick ? "yes" : "no",
           thr   ? "yes" : "no",
           ped   ? "yes" : "no");
  ESP_LOGI(TAG,
           "  ids: stick=0x%08x thr=0x%08x ped=0x%08x",
           stick ? (uint32_t)make_device_id(*stick) : 0u,
           thr   ? (uint32_t)make_device_id(*thr)   : 0u,
           ped   ? (uint32_t)make_device_id(*ped)   : 0u);

  // Stick axes + hat
  if (stick) {
    set_axis_mapping(out, OutputAxis::X,  src_from_element(*stick, find_element_by_usage(*stick, IE_KIND_AXIS, 0x0001, 0x0030)));
    set_axis_mapping(out, OutputAxis::Y,  src_from_element(*stick, find_element_by_usage(*stick, IE_KIND_AXIS, 0x0001, 0x0031)));

    // Prefer the stick twist (Rz) as a standalone axis (often used as "rudder" when pedals are absent).
    // IMPORTANT: We treat output axis Z as the *primary rudder* axis for BLE compatibility.
    // Only map stick twist (Rz) to RZ when pedals are present (so Z can remain reserved for pedal rudder).
    if (ped) {
      set_axis_mapping(out, OutputAxis::RZ,
                       src_from_element(*stick, find_element_by_usage(*stick, IE_KIND_AXIS, 0x0001, 0x0035)));
    }

    // D-pad / POV hat
    set_axis_mapping(out, OutputAxis::HAT, src_from_element(*stick, find_element_by_usage(*stick, IE_KIND_HAT,  0x0001, 0x0039)));
  }

  // Throttle
  if (thr) {
    const InputElement *e = nullptr;
    e = find_element_by_usage(*thr, IE_KIND_AXIS, 0x0002, 0x00BB);  // Simulation Throttle
    if (!e) e = find_element_by_usage(*thr, IE_KIND_AXIS, 0x0001, 0x0036);  // Desktop Slider
    if (!e) e = find_element_by_usage(*thr, IE_KIND_AXIS, 0x0001, 0x0032);  // Desktop Z
    if (!e) e = find_first_axis_like(*thr);
    set_axis_mapping(out, OutputAxis::SLIDER1, src_from_element(*thr, e));

    // If pedals are present, we can expose extra twist/rocker as RZ. If pedals are absent,
    // leave RZ unmapped to avoid duplicating the primary rudder axis (Z).
    if (ped && !out.axes[(size_t)OutputAxis::RZ].configured) {
      const InputElement *rz = find_element_by_usage(*thr, IE_KIND_AXIS, 0x0001, 0x0035);
      if (rz) set_axis_mapping(out, OutputAxis::RZ, src_from_element(*thr, rz));
    }
  }

  // Pedals: rudder + toe brakes rudder + toe brakes
  if (ped) {
    // Rudder: prefer Simulation Controls Rudder; otherwise, many pedals expose it as Z.
    const InputElement *rud = nullptr;
    rud = find_element_by_usage(*ped, IE_KIND_AXIS, 0x0002, 0x00BA);  // Simulation Rudder
    if (!rud) rud = find_element_by_usage(*ped, IE_KIND_AXIS, 0x0001, 0x0032);  // Z
    if (!rud) rud = find_element_by_usage(*ped, IE_KIND_AXIS, 0x0001, 0x0035);  // Rz
    if (!rud) rud = find_element_by_usage(*ped, IE_KIND_AXIS, 0x0001, 0x0036);  // Slider
    if (!rud) rud = find_first_axis_like(*ped);
    set_axis_mapping(out, OutputAxis::Z, src_from_element(*ped, rud));

    // Toe brakes: try Rx/Ry first (common for pedals). If missing, try Simulation Brake usages.
    const InputElement *brk_l = find_element_by_usage(*ped, IE_KIND_AXIS, 0x0001, 0x0033);  // Rx
    const InputElement *brk_r = find_element_by_usage(*ped, IE_KIND_AXIS, 0x0001, 0x0034);  // Ry

    if (!brk_l || !brk_r) {
      const InputElement *a = nullptr;
      const InputElement *b = nullptr;
      int n = find_sim_brake_axes(*ped, &a, &b);
      if (!brk_l && n >= 1) brk_l = a;
      if (!brk_r && n >= 2) brk_r = b;
      // If only one brake axis exists, map it to RX and keep RY unmapped.
    }

    if (brk_l) set_axis_mapping(out, OutputAxis::RX, src_from_element(*ped, brk_l));
    if (brk_r) set_axis_mapping(out, OutputAxis::RY, src_from_element(*ped, brk_r));

    // Optional: also expose a combined brake axis on SLIDER2 (max of the two), if present.
    int toe_count = count_toe_brake_elements(*ped);
    if (toe_count == 1) {
      set_axis_mapping(out, OutputAxis::SLIDER2,
                       src_from_element(*ped, find_element_by_usage(*ped, IE_KIND_AXIS, 0x0002, 0x00BF)));
    } else if (toe_count > 1) {
      AxisSource s;
      s.device_id = make_device_id(*ped);
      s.element_id = kVirtualToeBrakeMax;
      set_axis_mapping(out, OutputAxis::SLIDER2, s);
    }
  }

  // If pedals are absent, try to map brake-like axes from throttle (common on HOTAS throttles).
  if (!out.axes[(size_t)OutputAxis::RX].configured && thr) {
    const InputElement *e = find_element_by_usage(*thr, IE_KIND_AXIS, 0x0001, 0x0033);  // Rx
    if (e) set_axis_mapping(out, OutputAxis::RX, src_from_element(*thr, e));
  }
  if (!out.axes[(size_t)OutputAxis::RY].configured && thr) {
    const InputElement *e = find_element_by_usage(*thr, IE_KIND_AXIS, 0x0001, 0x0034);  // Ry
    if (e) set_axis_mapping(out, OutputAxis::RY, src_from_element(*thr, e));
  }
  // If pedals absent, bind the primary rudder axis (Z) deterministically.
  // Prefer stick twist (Rz), otherwise throttle Rz, otherwise the first axis we can find.
  if (!out.axes[(size_t)OutputAxis::Z].configured) {
    const HidDeviceContext *src_dev = nullptr;
    const InputElement *e = nullptr;

    if (stick) {
      e = find_element_by_usage(*stick, IE_KIND_AXIS, 0x0001, 0x0035);  // stick twist
      if (e) src_dev = stick;
    }
    if (!e && thr) {
      e = find_element_by_usage(*thr, IE_KIND_AXIS, 0x0001, 0x0035);  // throttle rocker / twist
      if (e) src_dev = thr;
    }
    if (!e && stick) {
      e = find_first_axis_like(*stick);
      if (e) src_dev = stick;
    }
    if (!e && thr) {
      e = find_first_axis_like(*thr);
      if (e) src_dev = thr;
    }

    if (e && src_dev) set_axis_mapping(out, OutputAxis::Z, src_from_element(*src_dev, e));
  }
}


static float apply_deadzone(float v, float dz) {
  if (dz <= 0.0f) return v;
  float av = fabsf(v);
  if (av < dz) return 0.0f;
  float sign = (v < 0.0f) ? -1.0f : 1.0f;
  float scaled = (av - dz) / (1.0f - dz);
  if (scaled > 1.0f) scaled = 1.0f;
  return sign * scaled;
}

static float apply_smoothing(OutputAxis axis, float v, float alpha) {
  const size_t idx = (size_t)axis;
  if (alpha <= 0.0f) {
    g_axis_ema[idx] = v;
    g_axis_ema_valid[idx] = true;
    return v;
  }
  if (!g_axis_ema_valid[idx]) {
    g_axis_ema[idx] = v;
    g_axis_ema_valid[idx] = true;
    return v;
  }
  float out = g_axis_ema[idx] * (1.0f - alpha) + v * alpha;
  g_axis_ema[idx] = out;
  return out;
}

static int16_t norm_to_i16_bipolar(float v) {
  if (v > 1.0f) v = 1.0f;
  if (v < -1.0f) v = -1.0f;
  return (int16_t)lrintf(v * 32767.0f);
}

static int16_t norm01_to_i16(float v01) {
  if (v01 < 0.0f) v01 = 0.0f;
  if (v01 > 1.0f) v01 = 1.0f;
  return (int16_t)lrintf(v01 * 32767.0f);
}

static const HidDeviceContext *find_device_by_id(const HidDeviceContext *devices,
                                                 size_t num_devices,
                                                 DeviceId id) {
  for (size_t i = 0; i < num_devices; i++) {
    if (!devices[i].active) continue;
    if (make_device_id(devices[i]) == id) return &devices[i];
  }
  return nullptr;
}

static const InputElement *find_element_by_id(const HidDeviceContext &dev, ElementId id) {
  for (size_t i = 0; i < dev.caps.num_elements; i++) {
    if (dev.caps.elements[i].element_id == id) return &dev.caps.elements[i];
  }
  return nullptr;
}

static float read_mapped_norm(const HidDeviceContext &dev, OutputAxis out_axis, ElementId element_id) {
  if (element_id == kVirtualToeBrakeMax) {
    // virtual: 0..1
    return get_virtual_toe_brake_max_norm(dev);
  }
  const InputElement *e = find_element_by_id(dev, element_id);
  if (!e) return 0.0f;

  const bool out_is_slider = (out_axis == OutputAxis::SLIDER1 || out_axis == OutputAxis::SLIDER2);
  if (out_is_slider) {
    // Always interpret slider outputs as 0..1
    return (e->logical_min < 0) ? (e->norm_m1_1 + 1.0f) * 0.5f : e->norm_0_1;
  }

  // Bipolar outputs
  return (e->logical_min >= 0) ? (e->norm_0_1 * 2.0f - 1.0f) : e->norm_m1_1;
}

static uint8_t read_hat_value(const HidDeviceContext &dev, ElementId element_id) {
  const InputElement *e = find_element_by_id(dev, element_id);
  if (!e) return 0;
  int32_t raw = e->raw;
  if (raw < 0) return 0;
  if (raw >= 0 && raw <= 7) return (uint8_t)(raw + 1);
  return 0;
}

void mapping_engine_log_profile() {
  if (g_profile_logged_for_sig) return;

  ESP_LOGI(TAG, "Default mapping profile:");
  for (size_t i = 0; i < (size_t)OutputAxis::COUNT; i++) {
    OutputAxis a = (OutputAxis)i;
    const auto &m = g_profile.axes[i];
    if (!m.configured) {
      ESP_LOGI(TAG, "  %-7s -> (unmapped)", axis_name(a));
      continue;
    }
    if (m.source.element_id == kVirtualToeBrakeMax) {
      ESP_LOGI(TAG, "  %-7s -> dev=0x%08lx elem=VIRTUAL_TOE_BRAKE_MAX", axis_name(a),
               (unsigned long)m.source.device_id);
      continue;
    }
    ESP_LOGI(TAG, "  %-7s -> dev=0x%08lx elem=%u inv=%d dz=%.3f ema=%.3f", axis_name(a),
             (unsigned long)m.source.device_id, (unsigned)m.source.element_id,
             (int)m.mod.invert, (double)m.mod.deadzone, (double)m.mod.smoothing_alpha);
  }
  g_profile_logged_for_sig = true;
}

void mapping_engine_compute(const HidDeviceContext *devices, size_t num_devices, GamepadState *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));

  uint32_t sig = compute_devices_signature(devices, num_devices);
  if (sig != g_devices_signature) {
    g_devices_signature = sig;
    g_profile_dirty = true;
    g_profile_logged_for_sig = false;
    memset(g_axis_ema_valid, 0, sizeof(g_axis_ema_valid));
  }

  if (g_profile_dirty) {
    build_default_profile(devices, num_devices, g_profile);
    g_profile_dirty = false;
  }

  // Buttons OR-combine (default behavior)
  uint32_t buttons = 0;
  for (size_t i = 0; i < num_devices; i++) {
    if (!devices[i].active) continue;
    buttons |= devices[i].state.buttons;
  }
  out->buttons = buttons;

  auto compute_axis = [&](OutputAxis axis, int16_t *dst) {
    const auto &m = g_profile.axes[(size_t)axis];
    if (!m.configured || !m.source.is_valid()) {
      *dst = 0;
      return;
    }
    const HidDeviceContext *dev = find_device_by_id(devices, num_devices, m.source.device_id);
    if (!dev) {
      *dst = 0;
      return;
    }
    float v = read_mapped_norm(*dev, axis, m.source.element_id);
    if (axis != OutputAxis::SLIDER1 && axis != OutputAxis::SLIDER2) {
      if (m.mod.invert) v = -v;
      v = apply_deadzone(v, m.mod.deadzone);
      v = apply_smoothing(axis, v, m.mod.smoothing_alpha);
      *dst = norm_to_i16_bipolar(v);
    } else {
      // sliders: 0..1
      if (m.mod.invert) v = 1.0f - v;
      // No symmetric deadzone for sliders yet.
      v = apply_smoothing(axis, v, m.mod.smoothing_alpha);
      *dst = norm01_to_i16(v);
    }
  };

  compute_axis(OutputAxis::X, &out->x);
  compute_axis(OutputAxis::Y, &out->y);
  compute_axis(OutputAxis::Z, &out->z);
  compute_axis(OutputAxis::RX, &out->rx);
  compute_axis(OutputAxis::RY, &out->ry);
  compute_axis(OutputAxis::RZ, &out->rz);
  compute_axis(OutputAxis::SLIDER1, &out->slider1);
  compute_axis(OutputAxis::SLIDER2, &out->slider2);

  // Hat
  {
    const auto &m = g_profile.axes[(size_t)OutputAxis::HAT];
    if (m.configured && m.source.is_valid()) {
      const HidDeviceContext *dev = find_device_by_id(devices, num_devices, m.source.device_id);
      if (dev) out->hat = read_hat_value(*dev, m.source.element_id);
    }
  }

  mapping_engine_log_profile();
}

}  // namespace mapping
