#pragma once

#include <stddef.h>
#include <string>

namespace nvs_profile_store {

// Persist the canonical mapping profile JSON together with metadata.
bool save_json(const char *json, size_t len, std::string *error_out);

// Load and validate the persisted mapping profile JSON.
// Returns false if missing or invalid.
bool load_json(std::string *json_out, std::string *error_out);

}  // namespace nvs_profile_store
