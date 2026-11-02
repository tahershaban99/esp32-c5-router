#include "web_server.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/etharp.h"
#include <sys/socket.h>
#include <netinet/in.h>

#include "router_globals.h"
#include "router_config.h"
#include "meter.h"
#include "wifi_manager.h"
#include "telegram_bot.h"
#include "ota_updater.h"
#include "web_page_gz.h"

httpd_handle_t web_server = NULL;

static char active_session_token[65] = {0};
static bool session_valid = false;
static int64_t session_expiry_us = 0;
static uint32_t failed_login_attempts = 0;
static int64_t lockout_until_us = 0;

#define SESSION_TTL_US (3600ULL * 24ULL * 1000000ULL)
#define LOCKOUT_DURATION_US (60ULL * 1000000ULL)
#define MAX_FAILED_ATTEMPTS 5

static void url_decode(char *s) {
  char *out = s;
  while (*s) {
    if (*s == '+') { *out++ = ' '; ++s; }
    else if (*s == '%' && s[1] && s[2]) {
      char hex[3] = {s[1], s[2], 0};
      *out++ = (char)strtol(hex, NULL, 16); s += 3;
    } else *out++ = *s++;
  }
  *out = 0;
}

static void form_value(char *body, const char *key, char *dest, size_t capacity) {
  char search[48];
  snprintf(search, sizeof(search), "%s=", key);
  char *p = strstr(body, search);
  if (!p) { dest[0] = 0; return; }
  p += strlen(search);
  char *end = strchr(p, '&');
  size_t n = end ? (size_t)(end - p) : strlen(p);
  if (n >= capacity) n = capacity - 1;
  memcpy(dest, p, n);
  dest[n] = 0;
  url_decode(dest);
}

static void format_mac(const uint8_t mac[6], char out[18]) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool parse_mac_address(const char *text, uint8_t mac[6]) {
  if (strlen(text) != 17) return false;
  for (size_t i = 0; i < 6; ++i) {
    char pair[3] = {text[i * 3], text[i * 3 + 1], 0};
    char *end = NULL;
    unsigned long value = strtoul(pair, &end, 16);
    if (end != pair + 2 || value > 255 || (i < 5 && text[i * 3 + 2] != ':')) return false;
    mac[i] = (uint8_t)value;
  }
  return valid_device_mac(mac);
}

static size_t append_json_string(char *dst, size_t capacity, size_t used, const char *src) {
  if (used + 2 >= capacity) return used;
  dst[used++] = '"';
  for (const unsigned char *p = (const unsigned char *)src; *p && used + 3 < capacity; ++p) {
    if (*p == '"' || *p == '\\') dst[used++] = '\\';
    dst[used++] = (*p < 0x20) ? '_' : (char)*p;
  }
  dst[used++] = '"';
  dst[used] = 0;
  return used;
}

static esp_err_t read_small_form(httpd_req_t *req, char *body, size_t capacity) {
  if (req->content_len <= 0 || req->content_len >= capacity) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid form");
    return ESP_FAIL;
  }
  int received = 0;
  while (received < req->content_len) {
    int n = httpd_req_recv(req, body + received, req->content_len - received);
    if (n <= 0) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "form read failed");
      return ESP_FAIL;
    }
    received += n;
  }
  body[received] = 0;
  return ESP_OK;
}

static bool constant_time_eq(const char *a, const char *b) {
  if (!a || !b) return false;
  size_t len_a = strlen(a);
  size_t len_b = strlen(b);
  if (len_a != len_b) return false;
  unsigned char result = 0;
  for (size_t i = 0; i < len_a; ++i) {
    result |= (unsigned char)(a[i] ^ b[i]);
  }
  return result == 0;
}

static bool is_admin_authenticated(httpd_req_t *req) {
  if (!session_valid || active_session_token[0] == 0) return false;
  int64_t now_us = esp_timer_get_time();
  if (now_us > session_expiry_us) {
    session_valid = false;
    return false;
  }
  char cookie_hdr[160];
  size_t buf_len = httpd_req_get_hdr_value_len(req, "Cookie");
  if (buf_len == 0 || buf_len >= sizeof(cookie_hdr)) return false;
  if (httpd_req_get_hdr_value_str(req, "Cookie", cookie_hdr, sizeof(cookie_hdr)) != ESP_OK) {
    return false;
  }
  char cookie_val[68] = {0};
  char *p = strstr(cookie_hdr, "session_id=");
  if (!p) return false;
  p += strlen("session_id=");
  char *end = strchr(p, ';');
  size_t n = end ? (size_t)(end - p) : strlen(p);
  if (n >= sizeof(cookie_val)) n = sizeof(cookie_val) - 1;
  memcpy(cookie_val, p, n);
  cookie_val[n] = 0;

  return constant_time_eq(cookie_val, active_session_token);
}

static bool require_admin(httpd_req_t *req) {
  if (!is_admin_authenticated(req)) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"unauthorized\",\"auth_required\":true}");
    return false;
  }
  return true;
}

// -----------------------------------------------------------------------------
// ZERO-RAM GZIP WEB UI SERVING
// Serves pre-compressed HTML frontend directly from Flash .rodata with Content-Encoding: gzip
// -----------------------------------------------------------------------------
static esp_err_t root_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");

  const char *ptr = (const char *)PAGE_GZ;
  size_t remaining = PAGE_GZ_LEN;
  while (remaining > 0) {
    size_t chunk_size = (remaining > 1460) ? 1460 : remaining;
    esp_err_t err = httpd_resp_send_chunk(req, ptr, chunk_size);
    if (err != ESP_OK) return err;
    ptr += chunk_size;
    remaining -= chunk_size;
  }
  return httpd_resp_send_chunk(req, NULL, 0);
}

// Captive Portal Redirect: Redirects connectivity checks and unknown routes to http://192.168.4.1/
static esp_err_t captive_redirect_handler(httpd_req_t *req) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

static esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

// -----------------------------------------------------------------------------
// CHUNKED JSON STREAMING: /api/devices
// Uses static stack buffer (0 bytes heap allocated)
// -----------------------------------------------------------------------------
static esp_err_t devices_handler(httpd_req_t *req) {
  wifi_manager_refresh_clients();

  active_client_t active[MAX_CLIENTS];
  size_t count;
  uint16_t period_days;
  uint32_t elapsed_s;
  uint64_t total_up = 0, total_down = 0;
  uint32_t live_total_up = 0, live_total_down = 0;
  uint64_t m_quota = 0;
  char q_start[12] = {0}, q_end[12] = {0};

  portENTER_CRITICAL(&data_mux);
  memcpy(active, active_clients, sizeof(active));
  count = usage_count;
  if (count > MAX_RECORDS) count = MAX_RECORDS;
  period_days = meter_period_days;
  elapsed_s = meter_period_elapsed_s;
  live_total_up = total_cur_up_bps;
  live_total_down = total_cur_down_bps;
  m_quota = main_quota_bytes;
  snprintf(q_start, sizeof(q_start), "%s", quota_start_date);
  snprintf(q_end, sizeof(q_end), "%s", quota_end_date);
  for (size_t i = 0; i < count; ++i) {
    total_up += usage_records[i].upload_bytes;
    total_down += usage_records[i].download_bytes;
  }
  portEXIT_CRITICAL(&data_mux);

  wifi_ap_record_t upstream_record;
  bool wan_connected = (has_saved_config && esp_wifi_sta_get_ap_info(&upstream_record) == ESP_OK);
  uint32_t free_heap = esp_get_free_heap_size();
  uint32_t min_free_heap = esp_get_minimum_free_heap_size();
  uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
  int8_t sta_rssi = wan_connected ? upstream_record.rssi : 0;
  int sta_channel = wan_connected ? upstream_record.primary : 0;
  char sta_ssid_safe[33] = {0};
  if (wan_connected) {
    size_t slen = strlen((char *)upstream_record.ssid);
    if (slen > 32) slen = 32;
    memcpy(sta_ssid_safe, upstream_record.ssid, slen);
  }
  uint16_t active_clients_count = 0;
  for (size_t j = 0; j < MAX_CLIENTS; ++j) {
    if (valid_device_mac(active[j].mac) && active[j].aid != 0) {
      active_clients_count++;
    }
  }

  time_t now = time(NULL);
  bool time_synced = (now > 1700000000);
  int32_t rem_days = -1;
  int32_t rem_hours = -1;
  if (time_synced && q_end[0]) {
    time_t end_ts = meter_parse_date(q_end, true);
    if (end_ts > now) {
      int64_t diff = (int64_t)(end_ts - now);
      rem_days = (int32_t)(diff / 86400);
      rem_hours = (int32_t)((diff % 86400) / 3600);
    } else {
      rem_days = 0;
      rem_hours = 0;
    }
  }
  double gb_div = (router_gb_standard == 1000) ? 1000000000.0 : (1024.0 * 1024.0 * 1024.0);
  double main_quota_gb = (double)m_quota / gb_div;
  uint64_t grand_total_bytes = total_up + total_down;
  double grand_total_gb = (double)grand_total_bytes / gb_div;
  double rem_main_gb = 0.0;
  if (main_quota_gb > 0.0) {
    rem_main_gb = main_quota_gb > grand_total_gb ? (main_quota_gb - grand_total_gb) : 0.0;
  }

  bool is_admin = is_admin_authenticated(req);

  ota_info_t ota_inf;
  ota_updater_get_info(&ota_inf);

  char router_time_str[16] = "--:--";
  time_t cur_epoch = time(NULL);
  if (cur_epoch > 1700000000) {
    time_t local_cur = cur_epoch + ((int64_t)auto_reboot_tz_offset * 3600LL);
    struct tm cur_tm;
    gmtime_r(&local_cur, &cur_tm);
    snprintf(router_time_str, sizeof(router_time_str), "%02d:%02d", cur_tm.tm_hour, cur_tm.tm_min);
  }
  char reboot_time_str[8];
  snprintf(reboot_time_str, sizeof(reboot_time_str), "%02d:%02d", auto_reboot_hour, auto_reboot_min);

  // Stack buffer (0 bytes heap allocated!)
  char buf[896];
  httpd_resp_set_type(req, "application/json; charset=utf-8");

  size_t used = (size_t)snprintf(buf, sizeof(buf),
                      "{\"wan\":%s,\"is_admin\":%s,\"period_days\":%u,\"elapsed_s\":%" PRIu32 ","
                      "\"total_up\":%" PRIu64 ",\"total_down\":%" PRIu64 ","
                      "\"cur_up_bps\":%" PRIu32 ",\"cur_down_bps\":%" PRIu32 ","
                      "\"free_heap\":%" PRIu32 ",\"min_free_heap\":%" PRIu32 ",\"uptime_s\":%" PRIu32 ","
                      "\"sta_rssi\":%d,\"sta_channel\":%d,\"active_clients\":%u,"
                      "\"cpu_freq_mhz\":%u,\"wifi_power_save\":%s,\"meter_scale_pct\":%u,\"gb_standard\":%u,"
                      "\"main_quota_gb\":%.2f,\"q_start\":\"%s\",\"q_end\":\"%s\","
                      "\"time_synced\":%s,\"rem_days\":%ld,\"rem_hours\":%ld,\"rem_main_gb\":%.3f,"
                      "\"soc_arch\":\"ESP32-C5 (RISC-V 240MHz)\",\"wifi_band\":\"5GHz\",\"wifi_phy\":\"Wi-Fi 6 (802.11ax)\",\"firmware_version\":\"" ROUTER_FIRMWARE_VERSION "\",\"running_partition\":\"%s\",\"sta_ssid\":",
                      wan_connected ? "true" : "false", is_admin ? "true" : "false", period_days, elapsed_s, total_up, total_down,
                      live_total_up, live_total_down,
                      free_heap, min_free_heap, uptime_s,
                      sta_rssi, sta_channel, active_clients_count,
                      (unsigned)esp_cpu_freq_mhz, esp_wifi_power_save ? "true" : "false", (unsigned)router_meter_scale_pct, (unsigned)router_gb_standard,
                      main_quota_gb, q_start, q_end,
                      time_synced ? "true" : "false", (long)rem_days, (long)rem_hours, rem_main_gb,
                      ota_inf.running_partition);
  used = append_json_string(buf, sizeof(buf), used, sta_ssid_safe);
  used += (size_t)snprintf(buf + used, sizeof(buf) - used,
      ",\"sta_static_enabled\":%s,\"sta_ip\":\"%s\",\"sta_mask\":\"%s\",\"sta_gw\":\"%s\",\"sta_dns\":\"%s\","
      "\"wifi_proto\":%u,\"router_ssid\":",
      sta_static_enabled ? "true" : "false", sta_ip_text, sta_mask_text, sta_gateway_text, sta_dns_text,
      (unsigned)wifi_protocol_mode);
  used = append_json_string(buf, sizeof(buf), used, router_ssid);
  used += (size_t)snprintf(buf + used, sizeof(buf) - used, ",\"ap_ssid\":");
  used = append_json_string(buf, sizeof(buf), used, ap_ssid);
  used += (size_t)snprintf(buf + used, sizeof(buf) - used,
      ",\"tg_enable\":%s,\"tg_interactive\":%s,\"tg_chat_id\":\"%s\",\"tg_thresholds\":\"%s\",\"rb_enable\":%s,\"rb_time\":\"%s\",\"rb_tz\":%d,\"router_clock\":\"%s\"",
      telegram_enabled ? "true" : "false", telegram_interactive ? "true" : "false",
      telegram_chat_id, telegram_thresholds,
      auto_reboot_enabled ? "true" : "false", reboot_time_str, auto_reboot_tz_offset, router_time_str);
  used += (size_t)snprintf(buf + used, sizeof(buf) - used, ",\"devices\":[");

  esp_err_t err = httpd_resp_send_chunk(req, buf, used);
  if (err != ESP_OK) return err;

  bool first = true;
  for (size_t i = 0; i < count; ++i) {
    usage_record_t rec;
    device_policy_t pol = {0};
    char name[DEVICE_NAME_BYTES] = {0};
    device_speed_t spd = {0};
    device_live_speed_t live = {0};
    bool valid = false;

    portENTER_CRITICAL(&data_mux);
    if (i < usage_count && valid_device_mac(usage_records[i].mac)) {
      valid = true;
      rec = usage_records[i];
      live = device_live_speeds[i];
      int p_idx = meter_find_policy_locked(rec.mac, false);
      if (p_idx >= 0) pol = device_policies[p_idx];
      int n_idx = meter_find_name_locked(rec.mac, false);
      if (n_idx >= 0) {
        snprintf(name, sizeof(name), "%s", device_names[n_idx].name);
      }
      int s_idx = meter_find_speed_locked(rec.mac, false);
      if (s_idx >= 0) spd = device_speeds[s_idx];
    }
    portEXIT_CRITICAL(&data_mux);

    if (!valid) continue;

    uint32_t rec_ip = 0;
    if (rec.mac[0] == 0x02 && rec.mac[1] == 0x00) {
      memcpy(&rec_ip, rec.mac + 2, 4);
    }
    int active_index = -1;
    for (size_t j = 0; j < MAX_CLIENTS; ++j) {
      if ((rec_ip != 0 && active[j].ip == rec_ip) ||
          memcmp(active[j].mac, rec.mac, 6) == 0) {
        active_index = (int)j;
        break;
      }
    }

    char mac[18];
    if (active_index >= 0 && (active[active_index].mac[0] != 0x02 || active[active_index].mac[1] != 0x00)) {
      format_mac(active[active_index].mac, mac);
    } else {
      format_mac(rec.mac, mac);
    }

    char canon_mac[18];
    format_mac(rec.mac, canon_mac);

    char ip_text[16] = "";
    if (rec_ip != 0) {
      ip4_addr_t bip = {.addr = rec_ip};
      snprintf(ip_text, sizeof(ip_text), IPSTR, IP2STR(&bip));
    } else if (active_index >= 0 && active[active_index].ip != 0) {
      ip4_addr_t bip = {.addr = active[active_index].ip};
      snprintf(ip_text, sizeof(ip_text), IPSTR, IP2STR(&bip));
    }

    int8_t rssi = (active_index >= 0) ? active[active_index].rssi : 0;
    bool connected = (active_index >= 0 && active[active_index].aid != 0);
    uint64_t quota = pol.quota_bytes;
    uint32_t upload_limit_bps = spd.upload_bps;
    uint32_t download_limit_bps = spd.download_bps;
    uint64_t dev_tot = rec.upload_bytes + rec.download_bytes;
    bool quota_exceeded = (quota > 0 && dev_tot >= quota);
    bool blocked = pol.blocked || quota_exceeded;
    bool gaming = pol.gaming_mode;
    double dev_share_pct = (m_quota > 0) ? (((double)dev_tot / (double)m_quota) * 100.0) : 0.0;
    double dev_quota_pct = (quota > 0) ? (((double)dev_tot / (double)quota) * 100.0) : 0.0;
    double gb_div = (router_gb_standard == 1000) ? 1000000000.0 : (1024.0 * 1024.0 * 1024.0);
    double dev_quota_gb = (double)quota / gb_div;

    used = 0;
    if (!first) {
      buf[used++] = ',';
    }
    first = false;

    used += (size_t)snprintf(buf + used, sizeof(buf) - used,
      "{\"ip\":\"%s\",\"mac\":\"%s\",\"canon_mac\":\"%s\",\"rssi\":%d,\"up\":%" PRIu64 ",\"down\":%" PRIu64 ","
      "\"quota\":%" PRIu64 ",\"quota_gb\":%.2f,\"quota_pct\":%.1f,\"quota_exceeded\":%s,"
      "\"upload_mbps\":%.2f,\"download_mbps\":%.2f,"
      "\"cur_up_bps\":%" PRIu32 ",\"cur_down_bps\":%" PRIu32 ",\"share_pct\":%.2f,\"name\":",
      ip_text, mac, canon_mac, rssi, rec.upload_bytes, rec.download_bytes,
      quota, dev_quota_gb, dev_quota_pct, quota_exceeded ? "true" : "false",
      upload_limit_bps / 1000000.0, download_limit_bps / 1000000.0,
      live.cur_up_bps, live.cur_down_bps, dev_share_pct);

    used = append_json_string(buf, sizeof(buf), used, name);
    used += (size_t)snprintf(buf + used, sizeof(buf) - used,
      ",\"connected\":%s,\"blocked\":%s,\"gaming\":%s,\"telegram_chat_id\":",
      connected ? "true" : "false", blocked ? "true" : "false", gaming ? "true" : "false");
    used = append_json_string(buf, sizeof(buf), used, pol.telegram_chat_id);

    portENTER_CRITICAL(&data_mux);
    int b_idx = meter_find_binding_locked(rec.mac, false);
    uint32_t bound_ip = (b_idx >= 0) ? device_bindings[b_idx].bound_ip : 0;
    portEXIT_CRITICAL(&data_mux);
    char bound_ip_str[16] = "";
    if (bound_ip != 0) {
      ip4_addr_t bip = {.addr = bound_ip};
      snprintf(bound_ip_str, sizeof(bound_ip_str), "%d.%d.%d.%d", IP2STR(&bip));
    }
    used += (size_t)snprintf(buf + used, sizeof(buf) - used,
      ",\"is_static_ip\":%s,\"bound_ip\":\"%s\"",
      (bound_ip != 0) ? "true" : "false", bound_ip_str);
    if (used < sizeof(buf) - 1) buf[used++] = '}';

    err = httpd_resp_send_chunk(req, buf, used);
    if (err != ESP_OK) return err;
  }

  err = httpd_resp_send_chunk(req, "]}", 2);
  if (err != ESP_OK) return err;
  return httpd_resp_send_chunk(req, NULL, 0);
}

// -----------------------------------------------------------------------------
// AUTHENTICATION: LOGIN, LOGOUT, PASSWORD
// -----------------------------------------------------------------------------
static esp_err_t login_handler(httpd_req_t *req) {
  int64_t now_us = esp_timer_get_time();
  if (lockout_until_us > now_us) {
    uint32_t rem_sec = (uint32_t)((lockout_until_us - now_us) / 1000000ULL);
    httpd_resp_set_status(req, "429 Too Many Requests");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    char res[90];
    snprintf(res, sizeof(res), "{\"ok\":false,\"error\":\"locked_out\",\"remaining\":%lu}", (unsigned long)rem_sec);
    return httpd_resp_sendstr(req, res);
  }

  char body[160];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;

  char user[48] = {0};
  char pass[80] = {0};
  form_value(body, "username", user, sizeof(user));
  form_value(body, "password", pass, sizeof(pass));

  bool ok = constant_time_eq(user, admin_username) && constant_time_eq(pass, admin_password);

  if (ok) {
    failed_login_attempts = 0;
    lockout_until_us = 0;
    uint32_t r1 = esp_random(), r2 = esp_random();
    snprintf(active_session_token, sizeof(active_session_token), "%08lx%08lx%08lx%08lx",
             (unsigned long)r1, (unsigned long)r2, (unsigned long)esp_random(), (unsigned long)esp_random());
    session_valid = true;
    session_expiry_us = now_us + SESSION_TTL_US;

    char set_cookie[160];
    snprintf(set_cookie, sizeof(set_cookie), "session_id=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=86400", active_session_token);
    httpd_resp_set_hdr(req, "Set-Cookie", set_cookie);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
  } else {
    failed_login_attempts++;
    if (failed_login_attempts >= MAX_FAILED_ATTEMPTS) {
      lockout_until_us = now_us + LOCKOUT_DURATION_US;
      httpd_resp_set_status(req, "429 Too Many Requests");
      httpd_resp_set_type(req, "application/json; charset=utf-8");
      return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"locked_out\",\"remaining\":60}");
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    char res[90];
    snprintf(res, sizeof(res), "{\"ok\":false,\"error\":\"invalid_credentials\",\"attempts_left\":%u}",
             (unsigned)(MAX_FAILED_ATTEMPTS - failed_login_attempts));
    return httpd_resp_sendstr(req, res);
  }
}

static esp_err_t logout_handler(httpd_req_t *req) {
  session_valid = false;
  active_session_token[0] = 0;
  httpd_resp_set_hdr(req, "Set-Cookie", "session_id=; Path=/; Max-Age=0");
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t password_handler(httpd_req_t *req) {
  if (!require_admin(req)) return ESP_OK;
  char body[256];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;

  char old_p[80] = {0}, new_u[48] = {0}, new_p[80] = {0};
  form_value(body, "old_pass", old_p, sizeof(old_p));
  form_value(body, "new_user", new_u, sizeof(new_u));
  form_value(body, "new_pass", new_p, sizeof(new_p));

  if (!constant_time_eq(old_p, admin_password)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"wrong_password\"}");
  }

  if (strlen(new_u) < 2 || strlen(new_p) < 4) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"too_short\"}");
  }

  config_save_admin(new_u, new_p);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// -----------------------------------------------------------------------------
// DEVICE MANAGEMENT API
// -----------------------------------------------------------------------------
static esp_err_t device_save_handler(httpd_req_t *req) {
  if (!require_admin(req)) return ESP_OK;
  char body[320];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;

  char mac_text[24] = {0};
  form_value(body, "mac", mac_text, sizeof(mac_text));
  uint8_t mac[6];
  if (!parse_mac_address(mac_text, mac)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mac");
    return ESP_FAIL;
  }

  char name[DEVICE_NAME_BYTES] = {0};
  form_value(body, "name", name, sizeof(name));
  if (name[0]) {
    meter_set_device_name(mac, name);
  }

  char tg_chat[32] = {0};
  form_value(body, "telegram_chat_id", tg_chat, sizeof(tg_chat));
  meter_set_device_telegram(mac, tg_chat);

  char bound_ip_s[24] = {0};
  form_value(body, "bound_ip", bound_ip_s, sizeof(bound_ip_s));
  char is_static_s[8] = {0};
  form_value(body, "is_static_ip", is_static_s, sizeof(is_static_s));

  if (is_static_s[0]) {
    bool is_static = (strcmp(is_static_s, "true") == 0 || strcmp(is_static_s, "1") == 0);
    if (!is_static) {
      meter_set_device_binding(mac, 0);
    } else if (bound_ip_s[0]) {
      ip4_addr_t b_ip = {0};
      if (ip4addr_aton(bound_ip_s, &b_ip)) {
        meter_set_device_binding(mac, b_ip.addr);
      }
    }
  } else if (bound_ip_s[0]) {
    if (strcmp(bound_ip_s, "0") == 0 || strcmp(bound_ip_s, "none") == 0) {
      meter_set_device_binding(mac, 0);
    } else {
      ip4_addr_t b_ip = {0};
      if (ip4addr_aton(bound_ip_s, &b_ip)) {
        meter_set_device_binding(mac, b_ip.addr);
      }
    }
  }

  char up_s[20] = {0}, down_s[20] = {0};
  form_value(body, "upload_mbps", up_s, sizeof(up_s));
  form_value(body, "download_mbps", down_s, sizeof(down_s));
  if (up_s[0] || down_s[0]) {
    double up_m = atof(up_s);
    double down_m = atof(down_s);
    uint32_t up_bps = (uint32_t)(up_m * 1000000.0);
    uint32_t down_bps = (uint32_t)(down_m * 1000000.0);
    meter_set_device_speed(mac, up_bps, down_bps);
  }

  char quota_s[24] = {0};
  form_value(body, "quota_gb", quota_s, sizeof(quota_s));
  if (quota_s[0]) {
    double quota_gb = atof(quota_s);
    double gb_div = (router_gb_standard == 1000) ? 1000000000.0 : (1024.0 * 1024.0 * 1024.0);
  uint64_t quota_bytes = (uint64_t)(quota_gb * gb_div);
    int p_idx = meter_find_policy_locked(mac, true);
    bool blk = (p_idx >= 0) ? device_policies[p_idx].blocked : false;
    bool gam = (p_idx >= 0) ? device_policies[p_idx].gaming_mode : false;

    // Auto-unblock device if newly assigned quota > current usage or set to 0 (unlimited)
    int u_idx = meter_find_usage_locked(mac, false);
    if (u_idx >= 0) {
      uint64_t consumed = usage_records[u_idx].upload_bytes + usage_records[u_idx].download_bytes;
      if (quota_bytes == 0 || consumed < quota_bytes) {
        blk = false;
      }
    }
    meter_set_device_policy(mac, quota_bytes, blk, gam);
  }

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t device_block_handler(httpd_req_t *req) {
  if (!require_admin(req)) return ESP_OK;
  char body[160];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;

  char mac_text[24] = {0}, val_s[8] = {0};
  form_value(body, "mac", mac_text, sizeof(mac_text));
  form_value(body, "blocked", val_s, sizeof(val_s));

  uint8_t mac[6];
  if (!parse_mac_address(mac_text, mac)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mac");
    return ESP_FAIL;
  }
  meter_set_device_blocked(mac, atoi(val_s) != 0);

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t device_gaming_handler(httpd_req_t *req) {
  if (!require_admin(req)) return ESP_OK;
  char body[160];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;

  char mac_text[24] = {0}, val_s[8] = {0};
  form_value(body, "mac", mac_text, sizeof(mac_text));
  form_value(body, "gaming", val_s, sizeof(val_s));

  uint8_t mac[6];
  if (!parse_mac_address(mac_text, mac)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mac");
    return ESP_FAIL;
  }
  meter_set_device_gaming(mac, atoi(val_s) != 0);

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t device_reset_handler(httpd_req_t *req) {
  if (!require_admin(req)) return ESP_OK;
  char body[160];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;

  char mac_text[24] = {0};
  form_value(body, "mac", mac_text, sizeof(mac_text));
  uint8_t mac[6];
  if (!parse_mac_address(mac_text, mac)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mac");
    return ESP_FAIL;
  }
  meter_reset_device_usage(mac);

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t device_delete_handler(httpd_req_t *req) {
  if (!require_admin(req)) return ESP_OK;
  char body[160];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;

  char mac_text[24] = {0};
  form_value(body, "mac", mac_text, sizeof(mac_text));
  uint8_t mac[6];
  if (!parse_mac_address(mac_text, mac)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mac");
    return ESP_FAIL;
  }
  meter_delete_device(mac);

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// -----------------------------------------------------------------------------
// MAIN ISP QUOTA & PERIOD
// -----------------------------------------------------------------------------
static esp_err_t main_quota_handler(httpd_req_t *req) {
  if (!require_admin(req)) return ESP_OK;
  char body[200];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;

  char q_gb_s[20] = {0}, s_date[16] = {0}, e_date[16] = {0};
  form_value(body, "quota_gb", q_gb_s, sizeof(q_gb_s));
  form_value(body, "start_date", s_date, sizeof(s_date));
  form_value(body, "end_date", e_date, sizeof(e_date));

  double q_gb = atof(q_gb_s);
  double gb_div = (router_gb_standard == 1000) ? 1000000000.0 : (1024.0 * 1024.0 * 1024.0);
  uint64_t q_bytes = (uint64_t)(q_gb * gb_div);
  meter_set_main_quota(q_bytes, s_date[0] ? s_date : NULL, e_date[0] ? e_date : NULL);

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t meter_period_handler(httpd_req_t *req) {
  if (!require_admin(req)) return ESP_OK;
  char body[64];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;

  char days_s[12] = {0};
  form_value(body, "period_days", days_s, sizeof(days_s));
  int d = atoi(days_s);
  if (d >= 1 && d <= 365) {
    meter_set_period_days((uint16_t)d);
  }
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t meter_reset_handler(httpd_req_t *req) {
  if (!require_admin(req)) return ESP_OK;
  meter_reset_all_usage(); // Also clears Telegram alert flags!
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// -----------------------------------------------------------------------------
// PERFORMANCE & METER SCALE
// -----------------------------------------------------------------------------
static esp_err_t performance_handler(httpd_req_t *req) {
  if (!require_admin(req)) return ESP_OK;
  char body[96];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;

  char freq_s[12] = {0}, ps_s[8] = {0};
  form_value(body, "cpu_freq", freq_s, sizeof(freq_s));
  form_value(body, "wifi_ps", ps_s, sizeof(ps_s));

  int f = atoi(freq_s);
  if (f == 80 || f == 160 || f == 240) {
    bool ps = (atoi(ps_s) != 0);
    config_apply_performance((uint16_t)f, ps);
    nvs_handle_t nvs;
    if (nvs_open("router", NVS_READWRITE, &nvs) == ESP_OK) {
      nvs_set_u16(nvs, "cpu_mhz", (uint16_t)f);
      nvs_set_u8(nvs, "wifi_ps", ps ? 1 : 0);
      nvs_commit(nvs);
      nvs_close(nvs);
    }
  }

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t meter_scale_handler(httpd_req_t *req) {
  if (!require_admin(req)) return ESP_OK;
  char body[96];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;

  char scale_s[12] = {0}, gb_std_s[12] = {0};
  form_value(body, "scale", scale_s, sizeof(scale_s));
  form_value(body, "gb_std", gb_std_s, sizeof(gb_std_s));
  int sc = atoi(scale_s);
  int gb_std = atoi(gb_std_s);
  if (sc >= 10 && sc <= 200) {
    if (gb_std != 1000 && gb_std != 1024) gb_std = 1024;
    config_save_meter_scale((uint8_t)sc, (uint16_t)gb_std);
  }
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// -----------------------------------------------------------------------------
// WI-FI SCANNING
// -----------------------------------------------------------------------------
static esp_err_t scan_start_handler(httpd_req_t *req) {
  if (!require_admin(req)) return ESP_OK;
  bool ok = wifi_manager_start_scan();
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static esp_err_t scan_results_handler(httpd_req_t *req) {
  char buf[896];
  httpd_resp_set_type(req, "application/json; charset=utf-8");

  size_t used = (size_t)snprintf(buf, sizeof(buf),
    "{\"ready\":%s,\"running\":%s,\"results\":[",
    scan_ready ? "true" : "false", scan_running ? "true" : "false");

  esp_err_t err = httpd_resp_send_chunk(req, buf, used);
  if (err != ESP_OK) return err;

  portENTER_CRITICAL(&data_mux);
  uint16_t count = scan_result_count;
  scan_entry_t entries[MAX_SCAN_RESULTS];
  memcpy(entries, scan_results, sizeof(entries));
  portEXIT_CRITICAL(&data_mux);

  bool first = true;
  for (uint16_t i = 0; i < count; ++i) {
    used = 0;
    if (!first) buf[used++] = ',';
    first = false;
    used += (size_t)snprintf(buf + used, sizeof(buf) - used, "{\"ssid\":");
    used = append_json_string(buf, sizeof(buf), used, entries[i].ssid);
    used += (size_t)snprintf(buf + used, sizeof(buf) - used,
      ",\"rssi\":%d,\"secured\":%s,\"is_5g\":%s,\"is_ax\":%s}",
      entries[i].rssi, entries[i].secured ? "true" : "false", entries[i].is_5g ? "true" : "false", entries[i].is_ax ? "true" : "false");

    err = httpd_resp_send_chunk(req, buf, used);
    if (err != ESP_OK) return err;
  }

  err = httpd_resp_send_chunk(req, "]}", 2);
  if (err != ESP_OK) return err;
  return httpd_resp_send_chunk(req, NULL, 0);
}

// -----------------------------------------------------------------------------
// SAVE WI-FI SETTINGS & REBOOT
// -----------------------------------------------------------------------------
static esp_err_t save_handler(httpd_req_t *req) {
  if (!require_admin(req)) return ESP_OK;
  char body[512];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;

  char r_ssid[33] = {0}, r_pass[65] = {0}, clr_r_pass[8] = {0};
  char a_ssid[33] = {0}, a_pass[65] = {0}, proto_s[8] = {0};
  char static_s[8] = {0}, ip[16] = {0}, mask[16] = {0}, gw[16] = {0}, dns[16] = {0};

  form_value(body, "router_ssid", r_ssid, sizeof(r_ssid));
  form_value(body, "router_password", r_pass, sizeof(r_pass));
  form_value(body, "clear_router_password", clr_r_pass, sizeof(clr_r_pass));
  form_value(body, "ap_ssid", a_ssid, sizeof(a_ssid));
  form_value(body, "ap_password", a_pass, sizeof(a_pass));
  form_value(body, "wifi_protocol", proto_s, sizeof(proto_s));
  form_value(body, "static_enabled", static_s, sizeof(static_s));
  form_value(body, "sta_ip", ip, sizeof(ip));
  form_value(body, "sta_mask", mask, sizeof(mask));
  form_value(body, "sta_gateway", gw, sizeof(gw));
  form_value(body, "sta_dns", dns, sizeof(dns));

  uint8_t proto = (uint8_t)atoi(proto_s);
  bool static_en = (atoi(static_s) != 0);
  bool clear_pass = (atoi(clr_r_pass) != 0);

  config_save_wifi(r_ssid, r_pass, clear_pass, a_ssid, a_pass, proto, static_en, ip, mask, gw, dns);

  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_sendstr(req, "<!doctype html><html lang='ar' dir='rtl'><body style='background:#0f172a;color:#fff;font-family:sans-serif;text-align:center;padding:50px'><h2>تم حفظ الإعدادات بنجاح!</h2><p>جاري إعادة تشغيل الراوتر... انتظر 15 ثانية ثم اتصل بالشبكة الجديدة.</p></body></html>");

  vTaskDelay(pdMS_TO_TICKS(1500));
  meter_save_usage();
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
  return ESP_OK;
}

static esp_err_t reboot_handler(httpd_req_t *req) {
  if (!require_admin(req)) return ESP_OK;
  char body[96];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;

  char cd_min_s[12] = {0}, imm_s[8] = {0};
  form_value(body, "countdown_min", cd_min_s, sizeof(cd_min_s));
  form_value(body, "immediate", imm_s, sizeof(imm_s));

  if (atoi(imm_s) != 0) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, "{\"ok\":true,\"action\":\"rebooting\"}");
    vTaskDelay(pdMS_TO_TICKS(1000));
    meter_save_usage();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
  }

  int cd = atoi(cd_min_s);
  if (cd > 0 && cd <= 1440) {
    reboot_countdown_s = cd * 60;
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"countdown_s\":true}");
  }

  httpd_resp_set_status(req, "400 Bad Request");
  return httpd_resp_sendstr(req, "{\"ok\":false}");
}

// -----------------------------------------------------------------------------
// DAILY SCHEDULED AUTO-REBOOT
// -----------------------------------------------------------------------------
static esp_err_t auto_reboot_handler(httpd_req_t *req) {
  if (!require_admin(req)) return ESP_OK;
  char body[128];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;

  char en_s[8] = {0}, time_s[12] = {0}, tz_s[8] = {0};
  form_value(body, "enable", en_s, sizeof(en_s));
  form_value(body, "time", time_s, sizeof(time_s));
  form_value(body, "tz", tz_s, sizeof(tz_s));

  bool en = (atoi(en_s) != 0);
  int h = 3, m = 0;
  sscanf(time_s, "%d:%d", &h, &m);
  int tz = atoi(tz_s);

  config_save_auto_reboot(en, (uint8_t)h, (uint8_t)m, (int8_t)tz);

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// -----------------------------------------------------------------------------
// TELEGRAM BOT & MULTI-THRESHOLD QUOTA ALERTS
// -----------------------------------------------------------------------------
static esp_err_t telegram_handler(httpd_req_t *req) {
  if (!require_admin(req)) return ESP_OK;
  char body[320];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;

  char test_s[8] = {0};
  form_value(body, "test", test_s, sizeof(test_s));
  if (atoi(test_s) != 0) {
    bool ok = telegram_send_test_alert();
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
  }

  char en_s[8] = {0}, inter_s[8] = {0};
  char token[80] = {0}, chat_id[32] = {0}, thresholds[48] = {0};

  form_value(body, "enable", en_s, sizeof(en_s));
  form_value(body, "interactive", inter_s, sizeof(inter_s));
  form_value(body, "token", token, sizeof(token));
  form_value(body, "chat_id", chat_id, sizeof(chat_id));
  form_value(body, "thresholds", thresholds, sizeof(thresholds));

  bool en = (atoi(en_s) != 0);
  bool inter = (atoi(inter_s) != 0);

  config_save_telegram(en, inter, token[0] ? token : NULL, chat_id[0] ? chat_id : NULL, thresholds[0] ? thresholds : NULL);

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// -----------------------------------------------------------------------------
// SECURE OVER-THE-AIR (OTA) AUTOMATED UPDATE HANDLERS
// -----------------------------------------------------------------------------
static esp_err_t ota_status_handler(httpd_req_t *req) {
  if (!require_admin(req)) return ESP_OK;

  ota_info_t info;
  ota_updater_get_info(&info);

  char res[340];
  const char *state_str = "idle";
  switch (info.state) {
    case OTA_STATE_IDLE: state_str = "idle"; break;
    case OTA_STATE_PREPARING: state_str = "preparing"; break;
    case OTA_STATE_DOWNLOADING: state_str = "downloading"; break;
    case OTA_STATE_FLASHING: state_str = "flashing"; break;
    case OTA_STATE_SUCCESS: state_str = "success"; break;
    case OTA_STATE_FAILED: state_str = "failed"; break;
  }

  snprintf(res, sizeof(res),
    "{\"version\":\"%s\",\"build_timestamp\":\"%s\","
    "\"running_partition\":\"%s\",\"target_partition\":\"%s\","
    "\"state\":\"%s\",\"progress\":%u,\"written\":%" PRIu32 ",\"total\":%" PRIu32 ",\"error\":\"%s\"}",
    info.current_version, info.build_timestamp,
    info.running_partition, info.target_partition,
    state_str, (unsigned)info.progress_pct, info.written_bytes, info.total_bytes, info.error_msg);

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, res);
}

static esp_err_t ota_update_handler(httpd_req_t *req) {
  if (!require_admin(req)) return ESP_OK;

  // Check if this is a direct binary file upload (.bin)
  // If Content-Type contains octet-stream or content length is substantial without form urlencoded
  char content_type[64] = {0};
  httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type));

  if (strstr(content_type, "application/octet-stream") != NULL ||
      (req->content_len > 1024 && strstr(content_type, "application/x-www-form-urlencoded") == NULL)) {
    return ota_updater_handle_stream(req);
  }

  // Otherwise, handle form with URL for automated remote OTA
  char body[280];
  if (read_small_form(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;

  char url[220] = {0};
  form_value(body, "url", url, sizeof(url));
  if (url[0] == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing URL parameter or binary file");
    return ESP_FAIL;
  }

  esp_err_t err = ota_updater_start_from_url(url);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  if (err == ESP_OK) {
    return httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"ota_started\"}");
  } else {
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"ota_start_failed\"}");
  }
}

static esp_err_t ota_check_handler(httpd_req_t *req) {
  if (!require_admin(req)) return ESP_OK;

  char ver[24] = {0};
  char url[256] = {0};
  char log[256] = {0};
  bool update_avail = false;

  esp_err_t err = ota_check_remote_version(ver, sizeof(ver), url, sizeof(url), log, sizeof(log), &update_avail);

  char res[700];
  if (err == ESP_OK) {
    snprintf(res, sizeof(res),
      "{\"ok\":true,\"current_version\":\"%s\",\"latest_version\":\"%s\","
      "\"update_available\":%s,\"firmware_url\":\"%s\",\"changelog\":\"%s\"}",
      ROUTER_FIRMWARE_VERSION, ver, update_avail ? "true" : "false", url, log);
  } else {
    snprintf(res, sizeof(res),
      "{\"ok\":false,\"error\":\"check_failed\",\"current_version\":\"%s\"}",
      ROUTER_FIRMWARE_VERSION);
  }
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_sendstr(req, res);
}

// -----------------------------------------------------------------------------
// SERVER INITIALIZATION & ROUTE REGISTRATION
// -----------------------------------------------------------------------------
void web_server_start(void) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.lru_purge_enable = true;
  config.max_open_sockets = 7;
  config.stack_size = 4096;
  config.core_id = 0; // ESP32-C5 RISC-V Single Core
  config.max_uri_handlers = 40;

  httpd_uri_t uris[] = {
    { .uri = "/", .method = HTTP_GET, .handler = root_handler },
    { .uri = "/index.html", .method = HTTP_GET, .handler = root_handler },
    { .uri = "/generate_204", .method = HTTP_GET, .handler = captive_redirect_handler },
    { .uri = "/gen_204", .method = HTTP_GET, .handler = captive_redirect_handler },
    { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_redirect_handler },
    { .uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_redirect_handler },
    { .uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_redirect_handler },
    { .uri = "/api/devices", .method = HTTP_GET, .handler = devices_handler },
    { .uri = "/api/login", .method = HTTP_POST, .handler = login_handler },
    { .uri = "/api/logout", .method = HTTP_POST, .handler = logout_handler },
    { .uri = "/api/password", .method = HTTP_POST, .handler = password_handler },
    { .uri = "/api/device/save", .method = HTTP_POST, .handler = device_save_handler },
    { .uri = "/api/device/block", .method = HTTP_POST, .handler = device_block_handler },
    { .uri = "/api/device/gaming", .method = HTTP_POST, .handler = device_gaming_handler },
    { .uri = "/api/device/reset", .method = HTTP_POST, .handler = device_reset_handler },
    { .uri = "/api/device/delete", .method = HTTP_POST, .handler = device_delete_handler },
    { .uri = "/api/main_quota", .method = HTTP_POST, .handler = main_quota_handler },
    { .uri = "/meter/period", .method = HTTP_POST, .handler = meter_period_handler },
    { .uri = "/meter/reset", .method = HTTP_POST, .handler = meter_reset_handler },
    { .uri = "/api/performance", .method = HTTP_POST, .handler = performance_handler },
    { .uri = "/api/meter_scale", .method = HTTP_POST, .handler = meter_scale_handler },
    { .uri = "/api/scan/start", .method = HTTP_POST, .handler = scan_start_handler },
    { .uri = "/api/scan", .method = HTTP_GET, .handler = scan_results_handler },
    { .uri = "/save", .method = HTTP_POST, .handler = save_handler },
    { .uri = "/api/system/reboot", .method = HTTP_POST, .handler = reboot_handler },
    { .uri = "/api/auto_reboot", .method = HTTP_POST, .handler = auto_reboot_handler },
    { .uri = "/api/telegram", .method = HTTP_POST, .handler = telegram_handler },
    { .uri = "/api/ota/update", .method = HTTP_POST, .handler = ota_update_handler },
    { .uri = "/api/ota/status", .method = HTTP_GET, .handler = ota_status_handler },
    { .uri = "/api/ota/check", .method = HTTP_GET, .handler = ota_check_handler },
  };

  if (httpd_start(&web_server, &config) == ESP_OK) {
    size_t count = sizeof(uris) / sizeof(uris[0]);
    for (size_t i = 0; i < count; ++i) {
      httpd_register_uri_handler(web_server, &uris[i]);
    }
    httpd_register_err_handler(web_server, HTTPD_404_NOT_FOUND, http_404_error_handler);
    ESP_LOGI(ROUTER_TAG, "High-throughput HTTP Web Server running on Core %d", config.core_id);
  } else {
    ESP_LOGE(ROUTER_TAG, "Failed to start HTTP Web Server");
  }
}

void web_server_stop(void) {
  if (web_server) {
    httpd_stop(web_server);
    web_server = NULL;
  }
}

