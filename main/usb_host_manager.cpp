#include "usb_host_manager.h"
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_private/usb_phy.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <usb/usb_host.h>

static const char *TAG = "USB_HOST_MGR";
static usb_phy_handle_t phy_hdl = NULL;

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
  vTaskDelete(NULL);
}

void usb_host_manager_init(void) {
  ESP_LOGI(TAG, "Initializing USB Host PHY...");

  // Configure USB PHY interface for internal routing (Pins 19 and 20 usually)
  usb_phy_config_t phy_config = {
      .controller = USB_PHY_CTRL_OTG,
      .target = USB_PHY_TARGET_INT,
      .otg_mode = USB_OTG_MODE_HOST,
      .otg_speed = USB_PHY_SPEED_UNDEFINED, // auto
      .ext_io_conf = NULL,
      .otg_io_conf = NULL,
  };
  esp_err_t err = usb_new_phy(&phy_config, &phy_hdl);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize USB PHY: %s", esp_err_to_name(err));
    return;
  }

  ESP_LOGI(TAG, "Initializing USB Host...");
  usb_host_config_t host_config = {};
  host_config.skip_phy_setup = true; // We did it manually above
  host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;
  err = usb_host_install(&host_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to install usb host: %s", esp_err_to_name(err));
    return;
  }

  // Start daemon task
  xTaskCreate(usb_host_lib_daemon_task, "usb_events", 4096, NULL, 5, NULL);
  ESP_LOGI(TAG, "USB Host initialized and daemon started.");
}
