#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_http_server.h"

#define ROUTER_FIRMWARE_VERSION "v3.2.0-PRO"
#define ROUTER_BUILD_TIMESTAMP __DATE__ " " __TIME__

typedef enum {
  OTA_STATE_IDLE = 0,
  OTA_STATE_PREPARING,
  OTA_STATE_DOWNLOADING,
  OTA_STATE_FLASHING,
  OTA_STATE_SUCCESS,
  OTA_STATE_FAILED
} ota_state_t;

typedef struct {
  ota_state_t state;
  uint8_t progress_pct;      // 0 - 100
  char current_version[24];  // e.g. "v3.2.0-PRO"
  char build_timestamp[32];  // compile time
  char running_partition[24];// e.g. "ota_0"
  char target_partition[24]; // e.g. "ota_1"
  char error_msg[64];
  uint32_t total_bytes;
  uint32_t written_bytes;
} ota_info_t;

void ota_updater_init(void);
void ota_updater_get_info(ota_info_t *out_info);
bool ota_updater_is_busy(void);
esp_err_t ota_updater_handle_stream(httpd_req_t *req);
esp_err_t ota_updater_start_from_url(const char *url);
esp_err_t ota_check_remote_version(char *out_version, size_t ver_len,
                                   char *out_url, size_t url_len,
                                   char *out_changelog, size_t log_len,
                                   bool *update_available);


