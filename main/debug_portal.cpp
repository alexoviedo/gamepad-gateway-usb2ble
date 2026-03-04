#include "debug_portal.h"
#include "hid_device_manager.h"
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

static const char *TAG = "DEBUG_PORTAL";

#include <freertos/semphr.h>
#include <stdarg.h>

#define LOG_BUFFER_SIZE 16384
static char s_log_buffer[LOG_BUFFER_SIZE];
static size_t s_log_head = 0;
static size_t s_log_tail = 0;
static SemaphoreHandle_t s_log_mutex = NULL;
static vprintf_like_t s_original_vprintf = NULL;

static int custom_vprintf(const char *fmt, va_list ap) {
  va_list ap_copy;
  va_copy(ap_copy, ap);

  char buf[512];
  int len = vsnprintf(buf, sizeof(buf), fmt, ap_copy);
  va_end(ap_copy);

  if (len > 0) {
    if (s_log_mutex) {
      xSemaphoreTake(s_log_mutex, portMAX_DELAY);
      for (int i = 0; i < len; i++) {
        // Ignore nulls, handle truncation from snprintf gracefully if needed
        if (buf[i] == '\0')
          break;
        s_log_buffer[s_log_head] = buf[i];
        s_log_head = (s_log_head + 1) % LOG_BUFFER_SIZE;
        if (s_log_head == s_log_tail) {
          s_log_tail = (s_log_tail + 1) % LOG_BUFFER_SIZE;
        }
      }
      xSemaphoreGive(s_log_mutex);
    }
  }

  if (s_original_vprintf) {
    return s_original_vprintf(fmt, ap);
  }
  return len;
}
static esp_err_t portal_get_handler(httpd_req_t *req) {
  char buf[1024];
  GamepadState state;
  hid_device_manager_get_merged_state(&state);

  snprintf(buf, sizeof(buf),
           "<html><head><meta http-equiv=\"refresh\" "
           "content=\"1\"><style>body{font-family:monospace;background:#111;"
           "color:#0f0;padding:20px;}h1{color:#fff;}</style></"
           "head><body><h1>HOTAS Debug Portal</h1><pre>"
           "Merged State:\n"
           "X: %d\n"
           "Y: %d\n"
           "Z: %d\n"
           "Rx: %d\n"
           "Ry: %d\n"
           "Rz: %d\n"
           "Slider1: %d\n"
           "Slider2: %d\n"
           "Hat: %d\n"
           "Buttons: 0x%08X\n"
           "</pre></body></html>",
           state.x, state.y, state.z, state.rx, state.ry, state.rz,
           state.slider1, state.slider2, state.hat,
           (unsigned int)state.buttons);

  httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

static esp_err_t portal_logs_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/plain");

  if (s_log_mutex)
    xSemaphoreTake(s_log_mutex, portMAX_DELAY);

  size_t curr_tail = s_log_tail;
  size_t curr_head = s_log_head;

  if (curr_head >= curr_tail) {
    if (curr_head > curr_tail) {
      httpd_resp_send_chunk(req, &s_log_buffer[curr_tail],
                            curr_head - curr_tail);
    }
  } else {
    httpd_resp_send_chunk(req, &s_log_buffer[curr_tail],
                          LOG_BUFFER_SIZE - curr_tail);
    if (curr_head > 0) {
      httpd_resp_send_chunk(req, s_log_buffer, curr_head);
    }
  }

  if (s_log_mutex)
    xSemaphoreGive(s_log_mutex);

  httpd_resp_send_chunk(req, NULL, 0); // End chunk
  return ESP_OK;
}

static const httpd_uri_t uri_get = {.uri = "/",
                                    .method = HTTP_GET,
                                    .handler = portal_get_handler,
                                    .user_ctx = NULL};

static const httpd_uri_t uri_logs = {.uri = "/logs",
                                     .method = HTTP_GET,
                                     .handler = portal_logs_handler,
                                     .user_ctx = NULL};

static void wifi_init_softap(void) {
  esp_netif_init();
  esp_event_loop_create_default();
  esp_netif_create_default_wifi_ap();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);

  wifi_config_t wifi_config = {};
  strcpy((char *)wifi_config.ap.ssid, "HOTAS-Debug");
  wifi_config.ap.ssid_len = strlen("HOTAS-Debug");
  wifi_config.ap.channel = 1;
  strcpy((char *)wifi_config.ap.password, "");
  wifi_config.ap.max_connection = 4;
  wifi_config.ap.authmode = WIFI_AUTH_OPEN;

  esp_wifi_set_mode(WIFI_MODE_AP);
  esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
  esp_wifi_start();

  ESP_LOGI(TAG,
           "WiFi AP started. Connect to HOTAS-Debug and go to 192.168.4.1");
}

void debug_portal_init(void) {
  s_log_mutex = xSemaphoreCreateMutex();
  s_original_vprintf = esp_log_set_vprintf(custom_vprintf);

  // NVS required for WiFi
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  wifi_init_softap();

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  httpd_handle_t server = NULL;
  if (httpd_start(&server, &config) == ESP_OK) {
    httpd_register_uri_handler(server, &uri_get);
    httpd_register_uri_handler(server, &uri_logs);
  }
}
