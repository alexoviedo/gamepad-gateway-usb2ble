#pragma once
#include "shared_types.h"
#include "input_elements.h"
#include <freertos/FreeRTOS.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t device_id;
  uint8_t dev_addr;
  uint8_t role;
  uint16_t num_elements;
  uint16_t report_desc_len;
} HidDeviceInfo;

size_t hid_device_manager_list_devices(HidDeviceInfo *out_infos, size_t max_infos);
size_t hid_device_manager_get_report_descriptor(uint32_t device_id, uint8_t *out_buf, size_t max_len);
size_t hid_device_manager_get_elements(uint32_t device_id, InputElement *out_elems, size_t max_elems);
bool hid_device_manager_get_device_state(uint32_t device_id, struct GamepadState *out_state);
void hid_device_manager_init(void);
void hid_device_manager_get_merged_state(struct GamepadState *out_state);
bool hid_device_manager_wait_for_next_state_ticks(struct GamepadState *out_state, TickType_t timeout_ticks);
void hid_device_manager_recompute_mapping(void);

#ifdef __cplusplus
}
#endif
