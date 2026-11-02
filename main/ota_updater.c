#include "ota_updater.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

#define TAG "ota_updater"
#define OTA_BUF_SIZE 2048

static ota_info_t s_ota_info = {
  .state = OTA_STATE_IDLE,
  .progress_pct = 0,
  .current_version = ROUTER_FIRMWARE_VERSION,
  .build_timestamp = ROUTER_BUILD_TIMESTAMP,
  .running_partition = "unknown",
  .target_partition = "unknown",
  .error_msg = "",
  .total_bytes = 0,
  .written_bytes = 0,
};

static portMUX_TYPE s_ota_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_target_url[256] = {0};

static void reboot_timer_task(void *pvParameter) {
  (void)pvParameter;
  vTaskDelay(pdMS_TO_TICKS(2000));
  ESP_LOGI(TAG, "Rebooting into newly flashed firmware partition...");
  esp_restart();
}

static void schedule_reboot(void) {
  xTaskCreate(reboot_timer_task, "ota_reboot", 2048, NULL, 5, NULL);
}

void ota_updater_init(void) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (running != NULL) {
    snprintf(s_ota_info.running_partition, sizeof(s_ota_info.running_partition), "%s", running->label);
    ESP_LOGI(TAG, "Running firmware partition: %s (address 0x%08" PRIx32 ", size 0x%" PRIx32 ")",
             running->label, running->address, running->size);
  }

  const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
  if (next != NULL) {
    snprintf(s_ota_info.target_partition, sizeof(s_ota_info.target_partition), "%s", next->label);
    ESP_LOGI(TAG, "Next update target partition: %s (address 0x%08" PRIx32 ")",
             next->label, next->address);
  }

  // Cancel pending rollback if new image booted cleanly
  esp_ota_img_states_t ota_state;
  if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
      ESP_LOGI(TAG, "First boot of newly flashed firmware. Validating and cancelling rollback!");
      esp_ota_mark_app_valid_cancel_rollback();
    }
  }
}

void ota_updater_get_info(ota_info_t *out_info) {
  if (out_info == NULL) return;
  portENTER_CRITICAL(&s_ota_mux);
  memcpy(out_info, &s_ota_info, sizeof(ota_info_t));
  portEXIT_CRITICAL(&s_ota_mux);
}

bool ota_updater_is_busy(void) {
  portENTER_CRITICAL(&s_ota_mux);
  bool busy = (s_ota_info.state == OTA_STATE_PREPARING ||
               s_ota_info.state == OTA_STATE_DOWNLOADING ||
               s_ota_info.state == OTA_STATE_FLASHING);
  portEXIT_CRITICAL(&s_ota_mux);
  return busy;
}

static void set_state(ota_state_t state, uint8_t progress, const char *err) {
  portENTER_CRITICAL(&s_ota_mux);
  s_ota_info.state = state;
  s_ota_info.progress_pct = progress;
  if (err != NULL) {
    snprintf(s_ota_info.error_msg, sizeof(s_ota_info.error_msg), "%s", err);
  } else if (state == OTA_STATE_IDLE || state == OTA_STATE_SUCCESS) {
    s_ota_info.error_msg[0] = 0;
  }
  portEXIT_CRITICAL(&s_ota_mux);
}

// -----------------------------------------------------------------------------
// Method 1: Web UI Direct Binary Upload Stream Handler
// -----------------------------------------------------------------------------
esp_err_t ota_updater_handle_stream(httpd_req_t *req) {
  if (ota_updater_is_busy()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA update already in progress");
    return ESP_FAIL;
  }

  const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
  if (update_partition == NULL) {
    ESP_LOGE(TAG, "No valid OTA update partition found in partition table!");
    set_state(OTA_STATE_FAILED, 0, "No OTA partition found");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition found");
    return ESP_FAIL;
  }

  set_state(OTA_STATE_PREPARING, 0, NULL);
  ESP_LOGI(TAG, "Starting direct stream OTA flash into partition: %s (size %" PRIu32 " bytes)",
           update_partition->label, update_partition->size);

  esp_ota_handle_t ota_handle = 0;
  esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
    set_state(OTA_STATE_FAILED, 0, "Flash erase failed");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
    return ESP_FAIL;
  }

  set_state(OTA_STATE_FLASHING, 0, NULL);

  char buf[OTA_BUF_SIZE];
  int remaining = req->content_len;
  int total_len = req->content_len;
  uint32_t written = 0;
  bool first_chunk = true;

  while (remaining > 0 || total_len <= 0) {
    int to_read = (remaining > (int)sizeof(buf) || total_len <= 0) ? (int)sizeof(buf) : remaining;
    int received = httpd_req_recv(req, buf, to_read);
    if (received <= 0) {
      if (received == HTTPD_SOCK_ERR_TIMEOUT) {
        continue;
      }
      break;
    }

    if (first_chunk) {
      first_chunk = false;
      // ESP32 binary header verification (first byte must be 0xE7 magic number)
      if ((uint8_t)buf[0] != 0xE7) {
        ESP_LOGE(TAG, "Invalid firmware magic byte: 0x%02X (expected 0xE7)", (uint8_t)buf[0]);
        esp_ota_abort(ota_handle);
        set_state(OTA_STATE_FAILED, 0, "Invalid binary magic byte (not ESP32 image)");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid firmware binary header");
        return ESP_FAIL;
      }
    }

    err = esp_ota_write(ota_handle, buf, received);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_ota_write failed at offset %" PRIu32 ": %s", written, esp_err_to_name(err));
      esp_ota_abort(ota_handle);
      set_state(OTA_STATE_FAILED, 0, "Flash write error");
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Flash write failed");
      return ESP_FAIL;
    }

    written += received;
    if (total_len > 0) {
      remaining -= received;
      uint8_t pct = (uint8_t)(((uint64_t)written * 100ULL) / (uint64_t)total_len);
      portENTER_CRITICAL(&s_ota_mux);
      s_ota_info.written_bytes = written;
      s_ota_info.total_bytes = total_len;
      s_ota_info.progress_pct = pct;
      portEXIT_CRITICAL(&s_ota_mux);
    }
  }

  err = esp_ota_end(ota_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
    set_state(OTA_STATE_FAILED, 0, "Firmware verification failed");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA validation failed");
    return ESP_FAIL;
  }

  err = esp_ota_set_boot_partition(update_partition);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
    set_state(OTA_STATE_FAILED, 0, "Boot partition switch failed");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Boot partition switch failed");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "OTA update successfully flashed %" PRIu32 " bytes! New boot partition: %s",
           written, update_partition->label);
  set_state(OTA_STATE_SUCCESS, 100, NULL);

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"firmware_updated\",\"reboot_seconds\":2}");

  schedule_reboot();
  return ESP_OK;
}

// -----------------------------------------------------------------------------
// Method 2: Automated Background Remote OTA Task (URL Download)
// -----------------------------------------------------------------------------
static void ota_download_task(void *pvParameter) {
  (void)pvParameter;
  ESP_LOGI(TAG, "Starting automated remote OTA download from: %s", s_target_url);
  set_state(OTA_STATE_DOWNLOADING, 0, NULL);

  const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
  if (update_partition == NULL) {
    ESP_LOGE(TAG, "No OTA partition available");
    set_state(OTA_STATE_FAILED, 0, "No OTA partition available");
    vTaskDelete(NULL);
    return;
  }

  esp_http_client_config_t config = {
    .url = s_target_url,
    .timeout_ms = 15000,
    .buffer_size = OTA_BUF_SIZE,
    .skip_cert_common_name_check = true,
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == NULL) {
    ESP_LOGE(TAG, "Failed to initialize HTTP client");
    set_state(OTA_STATE_FAILED, 0, "HTTP client initialization failed");
    vTaskDelete(NULL);
    return;
  }

  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to connect to remote OTA URL: %s", esp_err_to_name(err));
    set_state(OTA_STATE_FAILED, 0, "Cannot connect to server");
    esp_http_client_cleanup(client);
    vTaskDelete(NULL);
    return;
  }

  int content_length = esp_http_client_fetch_headers(client);
  int status_code = esp_http_client_get_status_code(client);
  if (status_code != 200) {
    ESP_LOGE(TAG, "Remote OTA server returned HTTP status: %d", status_code);
    char err_buf[48];
    snprintf(err_buf, sizeof(err_buf), "Server returned HTTP %d", status_code);
    set_state(OTA_STATE_FAILED, 0, err_buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    vTaskDelete(NULL);
    return;
  }

  esp_ota_handle_t ota_handle = 0;
  err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
    set_state(OTA_STATE_FAILED, 0, "Flash erase failed");
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    vTaskDelete(NULL);
    return;
  }

  set_state(OTA_STATE_FLASHING, 0, NULL);
  char buf[OTA_BUF_SIZE];
  uint32_t written = 0;
  bool first = true;

  while (1) {
    int read_bytes = esp_http_client_read(client, buf, sizeof(buf));
    if (read_bytes < 0) {
      ESP_LOGE(TAG, "Error reading HTTP stream");
      esp_ota_abort(ota_handle);
      set_state(OTA_STATE_FAILED, 0, "Network read error");
      break;
    }
    if (read_bytes == 0) {
      // Finished stream
      break;
    }

    if (first) {
      first = false;
      if ((uint8_t)buf[0] != 0xE7) {
        ESP_LOGE(TAG, "Downloaded file is not a valid ESP32 firmware binary!");
        esp_ota_abort(ota_handle);
        set_state(OTA_STATE_FAILED, 0, "Not a valid ESP32 firmware binary");
        break;
      }
    }

    err = esp_ota_write(ota_handle, buf, read_bytes);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
      esp_ota_abort(ota_handle);
      set_state(OTA_STATE_FAILED, 0, "Flash write failed");
      break;
    }

    written += read_bytes;
    if (content_length > 0) {
      uint8_t pct = (uint8_t)(((uint64_t)written * 100ULL) / (uint64_t)content_length);
      portENTER_CRITICAL(&s_ota_mux);
      s_ota_info.written_bytes = written;
      s_ota_info.total_bytes = content_length;
      s_ota_info.progress_pct = pct;
      portEXIT_CRITICAL(&s_ota_mux);
    }
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  if (s_ota_info.state == OTA_STATE_FAILED) {
    vTaskDelete(NULL);
    return;
  }

  err = esp_ota_end(ota_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_end verification failed: %s", esp_err_to_name(err));
    set_state(OTA_STATE_FAILED, 0, "Firmware verification failed");
    vTaskDelete(NULL);
    return;
  }

  err = esp_ota_set_boot_partition(update_partition);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
    set_state(OTA_STATE_FAILED, 0, "Boot partition switch failed");
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "Remote OTA update completed successfully (%" PRIu32 " bytes). Boot partition set to %s",
           written, update_partition->label);
  set_state(OTA_STATE_SUCCESS, 100, NULL);

  schedule_reboot();
  vTaskDelete(NULL);
}

esp_err_t ota_updater_start_from_url(const char *url) {
  if (url == NULL || strlen(url) < 8) return ESP_ERR_INVALID_ARG;
  if (ota_updater_is_busy()) return ESP_ERR_INVALID_STATE;

  strncpy(s_target_url, url, sizeof(s_target_url) - 1);
  s_target_url[sizeof(s_target_url) - 1] = 0;

  // Pin OTA task strictly to Core 1 so Core 0 stays dedicated to Wi-Fi & lwIP hot-paths
  BaseType_t res = xTaskCreate(ota_download_task, "ota_url_task", 8192, NULL, 5, NULL);
  return (res == pdPASS) ? ESP_OK : ESP_FAIL;
}

static void extract_json_field(const char *json, const char *key, char *out, size_t max_len) {
  out[0] = 0;
  if (!json || !key) return;
  char needle[40];
  snprintf(needle, sizeof(needle), "\"%s\":", key);
  const char *p = strstr(json, needle);
  if (!p) return;
  p += strlen(needle);
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  if (*p == '"') {
    p++;
    size_t k = 0;
    while (*p && *p != '"' && k + 1 < max_len) {
      if (*p == '\\' && *(p + 1)) p++;
      out[k++] = *p++;
    }
    out[k] = 0;
  }
}

esp_err_t ota_check_remote_version(char *out_version, size_t ver_len,
                                   char *out_url, size_t url_len,
                                   char *out_changelog, size_t log_len,
                                   bool *update_available) {
  if (update_available) *update_available = false;
  if (out_version) out_version[0] = 0;
  if (out_url) out_url[0] = 0;
  if (out_changelog) out_changelog[0] = 0;

  const char *check_url = "https://raw.githubusercontent.com/tahershaban99/ESP32-Pro-Router-NAPT/main/version.json";

  esp_http_client_config_t config = {
    .url = check_url,
    .timeout_ms = 8000,
    .buffer_size = 1024,
    .skip_cert_common_name_check = true,
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) return ESP_FAIL;

  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    esp_http_client_cleanup(client);
    return err;
  }

  esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  if (status != 200) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_FAIL;
  }

  char buf[1024];
  int read_bytes = esp_http_client_read(client, buf, sizeof(buf) - 1);
  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  if (read_bytes <= 0) return ESP_FAIL;
  buf[read_bytes] = 0;

  if (out_version) extract_json_field(buf, "version", out_version, ver_len);
  if (out_url) extract_json_field(buf, "firmware_url", out_url, url_len);
  if (out_changelog) extract_json_field(buf, "changelog", out_changelog, log_len);

  if (update_available && out_version && out_version[0]) {
    // If version is different from current version, update is available!
    if (strcmp(out_version, ROUTER_FIRMWARE_VERSION) != 0) {
      *update_available = true;
    }
  }

  return ESP_OK;
}


