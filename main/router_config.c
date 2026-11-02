#include "router_config.h"
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "router_globals.h"
#include "meter.h"
#include "soc/rtc.h"
#include "esp_wifi.h"

bool has_saved_config = false;
bool sta_static_enabled = false;
char router_ssid[33] = {0};
char router_password[65] = {0};
char ap_ssid[33] = {0};
char ap_password[65] = {0};
char sta_ip_text[16] = {0};
char sta_mask_text[16] = {0};
char sta_gateway_text[16] = {0};
char sta_dns_text[16] = {0};
char admin_username[33] = "admin";
char admin_password[65] = "admin";

uint16_t esp_cpu_freq_mhz = 240;
bool esp_wifi_power_save = false;
uint8_t wifi_protocol_mode = 1;
uint8_t router_meter_scale_pct = 100;
uint16_t router_gb_standard = 1024;

// Daily Scheduled Auto-Reboot
bool auto_reboot_enabled = false;
uint8_t auto_reboot_hour = 3;  // Default 03:00 AM
uint8_t auto_reboot_min = 0;
int8_t auto_reboot_tz_offset = 2; // Default UTC+2 (Cairo/Jerusalem)

// Telegram Settings
bool telegram_enabled = false;
bool telegram_interactive = false;
char telegram_token[80] = {0};
char telegram_chat_id[32] = {0};
char telegram_thresholds[48] = "50,75,90,95"; // Default multi-thresholds
uint32_t telegram_alert_flags = 0;

void config_load(void) {
  nvs_handle_t nvs;
  if (nvs_open("router", NVS_READONLY, &nvs) != ESP_OK) return;

  size_t len = sizeof(router_ssid);
  nvs_get_str(nvs, "router_ssid", router_ssid, &len);

  len = sizeof(router_password);
  nvs_get_str(nvs, "router_pass", router_password, &len);

  len = sizeof(ap_ssid);
  nvs_get_str(nvs, "ap_ssid", ap_ssid, &len);

  len = sizeof(ap_password);
  nvs_get_str(nvs, "ap_pass", ap_password, &len);

  uint8_t static_enabled = 0;
  nvs_get_u8(nvs, "sta_static", &static_enabled);
  sta_static_enabled = (static_enabled != 0);

  len = sizeof(sta_ip_text);
  nvs_get_str(nvs, "sta_ip", sta_ip_text, &len);

  len = sizeof(sta_mask_text);
  nvs_get_str(nvs, "sta_mask", sta_mask_text, &len);

  len = sizeof(sta_gateway_text);
  nvs_get_str(nvs, "sta_gw", sta_gateway_text, &len);

  len = sizeof(sta_dns_text);
  nvs_get_str(nvs, "sta_dns", sta_dns_text, &len);

  len = sizeof(admin_username);
  if (nvs_get_str(nvs, "admin_user", admin_username, &len) != ESP_OK || !admin_username[0]) {
    strncpy(admin_username, "admin", sizeof(admin_username) - 1);
  }

  len = sizeof(admin_password);
  if (nvs_get_str(nvs, "admin_pass", admin_password, &len) != ESP_OK || !admin_password[0]) {
    strncpy(admin_password, "admin", sizeof(admin_password) - 1);
  }

  has_saved_config = router_ssid[0] && ap_ssid[0] && strlen(ap_password) >= 8;

  uint16_t loaded_freq = 0;
  if (nvs_get_u16(nvs, "cpu_mhz", &loaded_freq) == ESP_OK &&
      (loaded_freq == 80 || loaded_freq == 160 || loaded_freq == 240)) {
    esp_cpu_freq_mhz = loaded_freq;
  } else {
    esp_cpu_freq_mhz = 240;
  }

  uint8_t loaded_ps = 0;
  if (nvs_get_u8(nvs, "wifi_ps", &loaded_ps) == ESP_OK) {
    esp_wifi_power_save = (loaded_ps != 0);
  } else {
    esp_wifi_power_save = false;
  }

  uint8_t loaded_scale = 100;
  if (nvs_get_u8(nvs, "meter_scale", &loaded_scale) == ESP_OK && loaded_scale >= 10 && loaded_scale <= 200) {
    router_meter_scale_pct = loaded_scale;
  } else {
    router_meter_scale_pct = 100;
  }
  uint16_t loaded_gb_std = 1024;
  if (nvs_get_u16(nvs, "gb_std", &loaded_gb_std) == ESP_OK && (loaded_gb_std == 1000 || loaded_gb_std == 1024)) {
    router_gb_standard = loaded_gb_std;
  } else {
    router_gb_standard = 1024;
  }

  uint8_t loaded_proto = 1;
  if (nvs_get_u8(nvs, "wifi_proto", &loaded_proto) == ESP_OK && loaded_proto <= 5) {
    wifi_protocol_mode = loaded_proto;
  }

  // Auto Reboot
  uint8_t loaded_rb_en = 0;
  if (nvs_get_u8(nvs, "rb_en", &loaded_rb_en) == ESP_OK) {
    auto_reboot_enabled = (loaded_rb_en != 0);
  }
  uint8_t loaded_rb_h = 3;
  if (nvs_get_u8(nvs, "rb_h", &loaded_rb_h) == ESP_OK && loaded_rb_h < 24) {
    auto_reboot_hour = loaded_rb_h;
  }
  uint8_t loaded_rb_m = 0;
  if (nvs_get_u8(nvs, "rb_m", &loaded_rb_m) == ESP_OK && loaded_rb_m < 60) {
    auto_reboot_min = loaded_rb_m;
  }
  int8_t loaded_rb_tz = 2;
  if (nvs_get_i8(nvs, "rb_tz", &loaded_rb_tz) == ESP_OK && loaded_rb_tz >= -12 && loaded_rb_tz <= 14) {
    auto_reboot_tz_offset = loaded_rb_tz;
  }

  // Telegram Settings
  uint8_t loaded_tg_en = 0;
  if (nvs_get_u8(nvs, "tg_en", &loaded_tg_en) == ESP_OK) {
    telegram_enabled = (loaded_tg_en != 0);
  }
  uint8_t loaded_tg_inter = 0;
  if (nvs_get_u8(nvs, "tg_inter", &loaded_tg_inter) == ESP_OK) {
    telegram_interactive = (loaded_tg_inter != 0);
  }
  len = sizeof(telegram_token);
  nvs_get_str(nvs, "tg_token", telegram_token, &len);

  len = sizeof(telegram_chat_id);
  nvs_get_str(nvs, "tg_chat_id", telegram_chat_id, &len);

  len = sizeof(telegram_thresholds);
  if (nvs_get_str(nvs, "tg_thresh", telegram_thresholds, &len) != ESP_OK || !telegram_thresholds[0]) {
    strncpy(telegram_thresholds, "50,75,90,95", sizeof(telegram_thresholds) - 1);
  }
  nvs_get_u32(nvs, "tg_flags", &telegram_alert_flags);

  nvs_close(nvs);
}

void config_save_admin(const char *user, const char *pass) {
  if (!user || !pass || strlen(user) == 0 || strlen(user) > 32 || strlen(pass) == 0 || strlen(pass) > 64) {
    return;
  }
  nvs_handle_t nvs;
  if (nvs_open("router", NVS_READWRITE, &nvs) != ESP_OK) return;
  nvs_set_str(nvs, "admin_user", user);
  nvs_set_str(nvs, "admin_pass", pass);
  nvs_commit(nvs);
  nvs_close(nvs);

  portENTER_CRITICAL(&data_mux);
  strncpy(admin_username, user, sizeof(admin_username) - 1);
  admin_username[sizeof(admin_username) - 1] = 0;
  strncpy(admin_password, pass, sizeof(admin_password) - 1);
  admin_password[sizeof(admin_password) - 1] = 0;
  portEXIT_CRITICAL(&data_mux);
}

void config_reset_admin_default(void) {
  config_save_admin("admin", "admin");
}

void config_apply_performance(uint16_t cpu_freq, bool wifi_ps) {
  if (cpu_freq == 80 || cpu_freq == 160 || cpu_freq == 240) {
    rtc_cpu_freq_config_t conf;
    if (rtc_clk_cpu_freq_mhz_to_config(cpu_freq, &conf)) {
      rtc_clk_cpu_freq_set_config_fast(&conf);
      esp_cpu_freq_mhz = cpu_freq;
      ESP_LOGI(ROUTER_TAG, "CPU frequency switched to %u MHz", (unsigned)cpu_freq);
    }
  }
  esp_wifi_set_ps(wifi_ps ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
  esp_wifi_power_save = wifi_ps;
  ESP_LOGI(ROUTER_TAG, "Wi-Fi Power Save mode set to: %s", wifi_ps ? "ENABLED (Min Modem)" : "DISABLED (Full Speed)");
}

void config_save_meter_scale(uint8_t scale_pct, uint16_t gb_std) {
  if (scale_pct < 10 || scale_pct > 200) return;
  router_meter_scale_pct = scale_pct;
  if (gb_std == 1000 || gb_std == 1024) {
    router_gb_standard = gb_std;
  }
  nvs_handle_t nvs;
  if (nvs_open("router", NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_set_u8(nvs, "meter_scale", scale_pct);
    nvs_set_u16(nvs, "gb_std", router_gb_standard);
    nvs_commit(nvs);
    nvs_close(nvs);
  }
}

void config_save_auto_reboot(bool enabled, uint8_t hour, uint8_t min, int8_t tz) {
  auto_reboot_enabled = enabled;
  auto_reboot_hour = (hour < 24) ? hour : 3;
  auto_reboot_min = (min < 60) ? min : 0;
  auto_reboot_tz_offset = (tz >= -12 && tz <= 14) ? tz : 2;

  nvs_handle_t nvs;
  if (nvs_open("router", NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_set_u8(nvs, "rb_en", auto_reboot_enabled ? 1 : 0);
    nvs_set_u8(nvs, "rb_h", auto_reboot_hour);
    nvs_set_u8(nvs, "rb_m", auto_reboot_min);
    nvs_set_i8(nvs, "rb_tz", auto_reboot_tz_offset);
    nvs_commit(nvs);
    nvs_close(nvs);
  }
}

void config_save_telegram(bool enabled, bool interactive, const char *token, const char *chat_id, const char *thresholds) {
  telegram_enabled = enabled;
  telegram_interactive = interactive;
  if (token) {
    strncpy(telegram_token, token, sizeof(telegram_token) - 1);
    telegram_token[sizeof(telegram_token) - 1] = 0;
  }
  if (chat_id) {
    strncpy(telegram_chat_id, chat_id, sizeof(telegram_chat_id) - 1);
    telegram_chat_id[sizeof(telegram_chat_id) - 1] = 0;
  }
  if (thresholds && thresholds[0]) {
    strncpy(telegram_thresholds, thresholds, sizeof(telegram_thresholds) - 1);
    telegram_thresholds[sizeof(telegram_thresholds) - 1] = 0;
  }

  nvs_handle_t nvs;
  if (nvs_open("router", NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_set_u8(nvs, "tg_en", telegram_enabled ? 1 : 0);
    nvs_set_u8(nvs, "tg_inter", telegram_interactive ? 1 : 0);
    nvs_set_str(nvs, "tg_token", telegram_token);
    nvs_set_str(nvs, "tg_chat_id", telegram_chat_id);
    nvs_set_str(nvs, "tg_thresh", telegram_thresholds);
    nvs_commit(nvs);
    nvs_close(nvs);
  }
}

void config_save_telegram_flags(uint32_t flags) {
  telegram_alert_flags = flags;
  nvs_handle_t nvs;
  if (nvs_open("router", NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_set_u32(nvs, "tg_flags", telegram_alert_flags);
    nvs_commit(nvs);
    nvs_close(nvs);
  }
}

void config_save_wifi(const char *r_ssid, const char *r_pass, bool clear_r_pass,
                      const char *a_ssid, const char *a_pass, uint8_t proto,
                      bool static_en, const char *ip, const char *mask,
                      const char *gw, const char *dns) {
  nvs_handle_t nvs;
  if (nvs_open("router", NVS_READWRITE, &nvs) != ESP_OK) return;

  if (r_ssid && r_ssid[0]) {
    nvs_set_str(nvs, "router_ssid", r_ssid);
    strncpy(router_ssid, r_ssid, sizeof(router_ssid) - 1);
  }
  if (clear_r_pass) {
    nvs_set_str(nvs, "router_pass", "");
    router_password[0] = 0;
  } else if (r_pass && r_pass[0]) {
    nvs_set_str(nvs, "router_pass", r_pass);
    strncpy(router_password, r_pass, sizeof(router_password) - 1);
  }

  if (a_ssid && a_ssid[0]) {
    nvs_set_str(nvs, "ap_ssid", a_ssid);
    strncpy(ap_ssid, a_ssid, sizeof(ap_ssid) - 1);
  }
  if (a_pass && strlen(a_pass) >= 8) {
    nvs_set_str(nvs, "ap_pass", a_pass);
    strncpy(ap_password, a_pass, sizeof(ap_password) - 1);
  }

  nvs_set_u8(nvs, "wifi_proto", proto);
  wifi_protocol_mode = proto;

  nvs_set_u8(nvs, "sta_static", static_en ? 1 : 0);
  sta_static_enabled = static_en;

  if (ip) {
    nvs_set_str(nvs, "sta_ip", ip);
    strncpy(sta_ip_text, ip, sizeof(sta_ip_text) - 1);
  }
  if (mask) {
    nvs_set_str(nvs, "sta_mask", mask);
    strncpy(sta_mask_text, mask, sizeof(sta_mask_text) - 1);
  }
  if (gw) {
    nvs_set_str(nvs, "sta_gw", gw);
    strncpy(sta_gateway_text, gw, sizeof(sta_gateway_text) - 1);
  }
  if (dns) {
    nvs_set_str(nvs, "sta_dns", dns);
    strncpy(sta_dns_text, dns, sizeof(sta_dns_text) - 1);
  }

  nvs_commit(nvs);
  nvs_close(nvs);
}


void config_set_scheduled_reboot_flag(bool flag) {
  nvs_handle_t nvs;
  if (nvs_open("router", NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_set_u8(nvs, "rb_fired", flag ? 1 : 0);
    nvs_commit(nvs);
    nvs_close(nvs);
  }
}

bool config_get_scheduled_reboot_flag(void) {
  nvs_handle_t nvs;
  uint8_t flag = 0;
  if (nvs_open("router", NVS_READONLY, &nvs) == ESP_OK) {
    nvs_get_u8(nvs, "rb_fired", &flag);
    nvs_close(nvs);
  }
  return (flag != 0);
}
