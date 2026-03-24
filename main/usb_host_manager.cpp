#include "usb_host_manager.h"
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_private/usb_phy.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <usb/usb_host.h>

static const char *TAG = "USB_HOST_MGR";
static usb_phy_handle_t phy_hdl = nullptr;

#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
static constexpr BaseType_t kUsbTaskCore = 1;
#else
static constexpr BaseType_t kUsbTaskCore = tskNO_AFFINITY;
#endif

static constexpr UBaseType_t kUsbDaemonTaskPriority = 3;

static void usb_host_lib_daemon_task(void *arg) {
  bool has_clients = true;
  bool has_devices = true;
  while (has_clients || has_devices) {
    uint32_t event_flags;
    esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    if (err == ESP_OK) {
      if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
        has_clients = false;
        ESP_LOGI(TAG, "No more clients");
      }
      if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
        has_devices = false;
        ESP_LOGI(TAG, "All devices free");
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  ESP_LOGI(TAG, "USB Host Library task exiting");
  vTaskDelete(nullptr);
}

void usb_host_manager_init(void) {
  ESP_LOGI(TAG, "Initializing USB Host PHY...");

  usb_phy_config_t phy_config = {
      .controller = USB_PHY_CTRL_OTG,
      .target = USB_PHY_TARGET_INT,
      .otg_mode = USB_OTG_MODE_HOST,
      .otg_speed = USB_PHY_SPEED_UNDEFINED,
      .ext_io_conf = nullptr,
      .otg_io_conf = nullptr,
  };
  esp_err_t err = usb_new_phy(&phy_config, &phy_hdl);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize USB PHY: %s", esp_err_to_name(err));
    return;
  }

  ESP_LOGI(TAG, "Initializing USB Host...");
  usb_host_config_t host_config = {};
  host_config.skip_phy_setup = true;
  host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;
  err = usb_host_install(&host_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to install usb host: %s", esp_err_to_name(err));
    return;
  }

  xTaskCreatePinnedToCore(usb_host_lib_daemon_task, "usb_events", 4096, nullptr,
                          kUsbDaemonTaskPriority, nullptr, kUsbTaskCore);
  ESP_LOGI(TAG, "USB Host initialized and daemon started.");
}
