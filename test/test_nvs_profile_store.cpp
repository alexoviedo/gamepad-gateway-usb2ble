#include "unity.h"
#include "nvs_profile_store.h"
#include <string>
#include <vector>

TEST_CASE("save_json bounds check - null and empty string", "[nvs_profile_store]") {
    std::string error;

    // Test null JSON string
    TEST_ASSERT_FALSE(nvs_profile_store::save_json(nullptr, 10, &error));
    TEST_ASSERT_EQUAL_STRING("empty profile", error.c_str());

    // Test zero length string
    TEST_ASSERT_FALSE(nvs_profile_store::save_json("{}", 0, &error));
    TEST_ASSERT_EQUAL_STRING("empty profile", error.c_str());
}

TEST_CASE("save_json bounds check - too large profile", "[nvs_profile_store]") {
    std::string error;

    // Create a string larger than kMaxProfileBytes (8192)
    std::string large_json(8193, 'a');

    TEST_ASSERT_FALSE(nvs_profile_store::save_json(large_json.c_str(), large_json.length(), &error));
    TEST_ASSERT_EQUAL_STRING("profile too large", error.c_str());
}

TEST_CASE("save_json valid json fails due to uninitialized NVS", "[nvs_profile_store]") {
    std::string error;

    const char * valid_json = "{\"test\": 123}";
    size_t len = std::string(valid_json).length();

    // Since this is a unit test, NVS won't be initialized by default unless we specifically
    // call nvs_flash_init(). Thus, nvs_open should fail and return false.
    // We just verify it returns false and error_out is populated with something.
    TEST_ASSERT_FALSE(nvs_profile_store::save_json(valid_json, len, &error));
    TEST_ASSERT_NOT_EQUAL(0, error.length());
}
