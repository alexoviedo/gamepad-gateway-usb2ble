#include "nvs_profile_store.h"

#include <nvs.h>
#include <esp_err.h>
#include <esp_log.h>

namespace nvs_profile_store {

static const char *TAG = "PROFILE_NVS";
static constexpr const char *kNamespace = "profile";
static constexpr const char *kKeyVersion = "ver";
static constexpr const char *kKeyLength = "len";
static constexpr const char *kKeyCrc = "crc";
static constexpr const char *kKeyJson = "json";
static constexpr uint32_t kStoredVersion = 2;
static constexpr size_t kMaxProfileBytes = 8192;

static uint32_t crc32_bytes(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) {
      const uint32_t mask = -(crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

bool save_json(const char *json, size_t len, std::string *error_out) {
  if (!json || len == 0) {
    if (error_out) *error_out = "empty profile";
    return false;
  }
  if (len > kMaxProfileBytes) {
    if (error_out) *error_out = "profile too large";
    return false;
  }

  const uint32_t crc = crc32_bytes(reinterpret_cast<const uint8_t *>(json), len);

  nvs_handle_t h = 0;
  esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    if (error_out) *error_out = esp_err_to_name(err);
    return false;
  }

  err = nvs_set_u32(h, kKeyVersion, kStoredVersion);
  if (err == ESP_OK) err = nvs_set_u32(h, kKeyLength, static_cast<uint32_t>(len));
  if (err == ESP_OK) err = nvs_set_u32(h, kKeyCrc, crc);
  if (err == ESP_OK) err = nvs_set_blob(h, kKeyJson, json, len);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);

  if (err != ESP_OK) {
    if (error_out) *error_out = esp_err_to_name(err);
    return false;
  }

  ESP_LOGI(TAG, "Saved profile (%u bytes, crc=0x%08lx)", (unsigned)len, (unsigned long)crc);
  return true;
}

bool load_json(std::string *json_out, std::string *error_out) {
  if (json_out) json_out->clear();

  nvs_handle_t h = 0;
  esp_err_t err = nvs_open(kNamespace, NVS_READONLY, &h);
  if (err != ESP_OK) {
    if (error_out) *error_out = (err == ESP_ERR_NVS_NOT_FOUND) ? "profile not found" : esp_err_to_name(err);
    return false;
  }

  uint32_t version = 0;
  uint32_t length = 0;
  uint32_t stored_crc = 0;
  err = nvs_get_u32(h, kKeyVersion, &version);
  if (err == ESP_OK) err = nvs_get_u32(h, kKeyLength, &length);
  if (err == ESP_OK) err = nvs_get_u32(h, kKeyCrc, &stored_crc);
  if (err != ESP_OK) {
    nvs_close(h);
    if (error_out) *error_out = esp_err_to_name(err);
    return false;
  }

  if (version != kStoredVersion) {
    nvs_close(h);
    if (error_out) *error_out = "unsupported profile version";
    return false;
  }
  if (length == 0 || length > kMaxProfileBytes) {
    nvs_close(h);
    if (error_out) *error_out = "invalid profile length";
    return false;
  }

  size_t actual_len = 0;
  err = nvs_get_blob(h, kKeyJson, nullptr, &actual_len);
  if (err != ESP_OK) {
    nvs_close(h);
    if (error_out) *error_out = esp_err_to_name(err);
    return false;
  }
  if (actual_len != length) {
    nvs_close(h);
    if (error_out) *error_out = "stored profile length mismatch";
    return false;
  }

  std::string json;
  json.resize(actual_len);
  err = nvs_get_blob(h, kKeyJson, json.data(), &actual_len);
  nvs_close(h);
  if (err != ESP_OK) {
    if (error_out) *error_out = esp_err_to_name(err);
    return false;
  }

  const uint32_t calc_crc = crc32_bytes(reinterpret_cast<const uint8_t *>(json.data()), json.size());
  if (calc_crc != stored_crc) {
    if (error_out) *error_out = "profile CRC mismatch";
    return false;
  }

  if (json_out) *json_out = std::move(json);
  ESP_LOGI(TAG, "Loaded profile (%u bytes, crc=0x%08lx)", (unsigned)length, (unsigned long)stored_crc);
  return true;
}

}  // namespace nvs_profile_store
