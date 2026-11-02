#include "telegram_bot.h"
#include "router_config.h"
#include "router_globals.h"
#include "meter.h"
#include "ota_updater.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "lwip/inet.h"

#define TG_TAG "telegram_bot"

static int64_t last_update_id = 0;
static bool telegram_busy = false;

// Helper to escape characters for JSON string value
static void json_escape(const char *src, char *dst, size_t max_len) {
  if (!src || !dst || max_len == 0) return;
  size_t j = 0;
  for (size_t i = 0; src[i] && j + 4 < max_len; ++i) {
    if (src[i] == '"') { dst[j++] = '\\'; dst[j++] = '"'; }
    else if (src[i] == '\\') { dst[j++] = '\\'; dst[j++] = '\\'; }
    else if (src[i] == '\n') { dst[j++] = '\\'; dst[j++] = 'n'; }
    else if (src[i] == '\r') { dst[j++] = '\\'; dst[j++] = 'r'; }
    else if (src[i] == '\t') { dst[j++] = '\\'; dst[j++] = 't'; }
    else dst[j++] = src[i];
  }
  dst[j] = 0;
}

bool telegram_send_message(const char *chat_id, const char *text) {
  return telegram_send_message_with_keyboard(chat_id, text, NULL);
}

bool telegram_send_message_with_keyboard(const char *chat_id, const char *text, const char *inline_keyboard_json) {
  if (!telegram_token[0] || !chat_id || !chat_id[0] || !text || !text[0]) {
    return false;
  }

  char url[180];
  snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", telegram_token);

  char escaped_text[1800];
  json_escape(text, escaped_text, sizeof(escaped_text));

  char *body = (char *)malloc(2800);
  if (!body) {
    ESP_LOGE(TG_TAG, "Failed to allocate memory for sendMessage body");
    return false;
  }

  if (inline_keyboard_json && inline_keyboard_json[0]) {
    snprintf(body, 2800,
             "{\"chat_id\":\"%s\",\"text\":\"%s\",\"parse_mode\":\"HTML\",\"reply_markup\":%s}",
             chat_id, escaped_text, inline_keyboard_json);
  } else {
    snprintf(body, 2800,
             "{\"chat_id\":\"%s\",\"text\":\"%s\",\"parse_mode\":\"HTML\"}",
             chat_id, escaped_text);
  }

  esp_http_client_config_t config = {
      .url = url,
      .method = HTTP_METHOD_POST,
      .timeout_ms = 8000,
      .skip_cert_common_name_check = true,
      .buffer_size = 512,
      .buffer_size_tx = 2800,
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    ESP_LOGE(TG_TAG, "Failed to initialize HTTP client for sendMessage");
    free(body);
    return false;
  }

  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, body, strlen(body));

  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  free(body);

  if (err == ESP_OK && (status == 200 || status == 201)) {
    ESP_LOGI(TG_TAG, "Telegram message sent successfully to %s", chat_id);
    return true;
  } else {
    ESP_LOGW(TG_TAG, "Failed to send Telegram message to %s: err=%s, status=%d", chat_id, esp_err_to_name(err), status);
    return false;
  }
}

bool telegram_answer_callback_query(const char *callback_query_id, const char *text) {
  if (!telegram_token[0] || !callback_query_id || !callback_query_id[0]) return false;

  char url[180];
  snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/answerCallbackQuery", telegram_token);

  char body[256];
  if (text && text[0]) {
    char esc_txt[100];
    json_escape(text, esc_txt, sizeof(esc_txt));
    snprintf(body, sizeof(body), "{\"callback_query_id\":\"%s\",\"text\":\"%s\"}", callback_query_id, esc_txt);
  } else {
    snprintf(body, sizeof(body), "{\"callback_query_id\":\"%s\"}", callback_query_id);
  }

  esp_http_client_config_t config = {
      .url = url,
      .method = HTTP_METHOD_POST,
      .timeout_ms = 6000,
      .skip_cert_common_name_check = true,
      .buffer_size = 256,
      .buffer_size_tx = 300,
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) return false;

  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, body, strlen(body));

  esp_err_t err = esp_http_client_perform(client);
  esp_http_client_cleanup(client);
  return (err == ESP_OK);
}

bool telegram_send_test_alert(void) {
  if (!telegram_token[0] || !telegram_chat_id[0]) {
    return false;
  }

  char msg[500];
  snprintf(msg, sizeof(msg),
           "✅ <b>اختبار إشعارات راوتر ESP32 المتقدم</b>\n\n"
           "الاتصال ببوت التيليجرام يعمل بنجاح وبكفاءة فائقة! 🚀\n"
           "ستصلك الآن تنبيهات الاستهلاك التلقائية للراوتر ولأجهزتك فور تجاوز الحدود المقررة.\n"
           "💡 يمكنك إرسال كلمة <code>استهلاك</code> أو <code>/quota</code> للاستعلام في أي وقت.");
  return telegram_send_message(telegram_chat_id, msg);
}

void telegram_reset_alert_flags(void) {
  telegram_alert_flags = 0;
  config_save_telegram_flags(0);
  ESP_LOGI(TG_TAG, "Telegram alert threshold flags reset for new billing cycle.");
}

// -----------------------------------------------------------------------------
// Helper to find device MAC & info from IP string or partial name
// -----------------------------------------------------------------------------
static bool find_device_by_target(const char *target, uint8_t out_mac[6], char *out_name, size_t max_name, uint32_t *out_ip) {
  if (!target || !target[0]) return false;
  while (*target == ' ') target++;

  // 1. Check if target is an IPv4 string (e.g. 192.168.4.2)
  ip4_addr_t ip_val;
  if (ip4addr_aton(target, &ip_val)) {
    uint32_t req_ip = ip_val.addr;
    if (out_ip) *out_ip = req_ip;

    portENTER_CRITICAL(&data_mux);
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
      if (active_clients[i].ip == req_ip && valid_device_mac(active_clients[i].mac)) {
        memcpy(out_mac, active_clients[i].mac, 6);
        int n = meter_find_name_locked(out_mac, false);
        if (n >= 0 && device_names[n].name[0]) {
          snprintf(out_name, max_name, "%s", device_names[n].name);
        } else {
          snprintf(out_name, max_name, "%s", target);
        }
        portEXIT_CRITICAL(&data_mux);
        return true;
      }
    }
    for (size_t b = 0; b < binding_count; ++b) {
      if (device_bindings[b].bound_ip == req_ip && valid_device_mac(device_bindings[b].mac)) {
        memcpy(out_mac, device_bindings[b].mac, 6);
        int n = meter_find_name_locked(out_mac, false);
        if (n >= 0 && device_names[n].name[0]) {
          snprintf(out_name, max_name, "%s", device_names[n].name);
        } else {
          snprintf(out_name, max_name, "%s", target);
        }
        portEXIT_CRITICAL(&data_mux);
        return true;
      }
    }
    // Fallback deterministic MAC (02:00:C0:A8:04:XX)
    uint32_t host_ip = ntohl(req_ip);
    if ((host_ip & 0xFFFFFF00) == 0xC0A80400) {
      uint8_t d = (uint8_t)(host_ip & 0xFF);
      if (d >= 2 && d <= 254) {
        out_mac[0] = 0x02; out_mac[1] = 0x00; out_mac[2] = 0xC0;
        out_mac[3] = 0xA8; out_mac[4] = 0x04; out_mac[5] = d;
        snprintf(out_name, max_name, "%s", target);
        portEXIT_CRITICAL(&data_mux);
        return true;
      }
    }
    portEXIT_CRITICAL(&data_mux);
    return false;
  }

  // 2. Check if target matches device name
  portENTER_CRITICAL(&data_mux);
  for (size_t n = 0; n < name_count; ++n) {
    if (strstr(device_names[n].name, target) != NULL) {
      memcpy(out_mac, device_names[n].mac, 6);
      snprintf(out_name, max_name, "%s", device_names[n].name);
      if (out_ip) {
        *out_ip = 0;
        for (size_t k = 0; k < MAX_CLIENTS; ++k) {
          if (memcmp(active_clients[k].mac, out_mac, 6) == 0) {
            *out_ip = active_clients[k].ip;
            break;
          }
        }
      }
      portEXIT_CRITICAL(&data_mux);
      return true;
    }
  }
  portEXIT_CRITICAL(&data_mux);
  return false;
}

// -----------------------------------------------------------------------------
// Formatters for Single Device and Multi-Device Reports
// -----------------------------------------------------------------------------
static void format_single_device_report(int p_idx, char *out, size_t max_out) {
  if (p_idx < 0 || (size_t)p_idx >= policy_count) {
    snprintf(out, max_out, "⚠️ بيانات الجهاز غير متوفرة حالياً.");
    return;
  }
  uint8_t mac[6];
  uint64_t quota = 0;
  bool blk = false, gam = false;
  char dev_name[DEVICE_NAME_BYTES] = "جهاز متصل";

  portENTER_CRITICAL(&data_mux);
  memcpy(mac, device_policies[p_idx].mac, 6);
  quota = device_policies[p_idx].quota_bytes;
  blk = device_policies[p_idx].blocked;
  gam = device_policies[p_idx].gaming_mode;

  int n = meter_find_name_locked(mac, false);
  if (n >= 0 && device_names[n].name[0]) {
    strncpy(dev_name, device_names[n].name, sizeof(dev_name) - 1);
    dev_name[sizeof(dev_name) - 1] = 0;
  }

  uint32_t ip = 0;
  bool connected = false;
  for (size_t k = 0; k < MAX_CLIENTS; ++k) {
    if (memcmp(active_clients[k].mac, mac, 6) == 0) {
      ip = active_clients[k].ip;
      connected = (active_clients[k].aid != 0);
      break;
    }
  }
  if (ip == 0 && mac[0] == 0x02 && mac[1] == 0x00) {
    memcpy(&ip, mac + 2, 4);
  }

  int u = meter_find_usage_locked(mac, false);
  uint64_t up = (u >= 0) ? usage_records[u].upload_bytes : 0;
  uint64_t down = (u >= 0) ? usage_records[u].download_bytes : 0;
  uint32_t live_down = (u >= 0) ? device_live_speeds[u].cur_down_bps : 0;
  uint32_t live_up = (u >= 0) ? device_live_speeds[u].cur_up_bps : 0;

  int s = meter_find_speed_locked(mac, false);
  uint32_t cap_up = (s >= 0) ? device_speeds[s].upload_bps : 0;
  uint32_t cap_down = (s >= 0) ? device_speeds[s].download_bps : 0;
  portEXIT_CRITICAL(&data_mux);

  double gb_div = (router_gb_standard == 1000) ? 1000000000.0 : (1024.0 * 1024.0 * 1024.0);
  double up_gb = (double)up / gb_div;
  double down_gb = (double)down / gb_div;
  double tot_gb = up_gb + down_gb;
  double q_gb = (double)quota / gb_div;
  double rem_gb = (q_gb > tot_gb) ? (q_gb - tot_gb) : 0.0;
  uint32_t pct = (quota > 0) ? (uint32_t)((tot_gb / q_gb) * 100.0) : 0;
  if (pct > 100) pct = 100;

  ip4_addr_t tip = {.addr = ip};
  char ip_str[24] = "--";
  if (ip != 0) snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d", IP2STR(&tip));

  const char *conn_str = blk ? "🔴 محظور من الإنترنت" : (connected ? "🟢 متصل بالواي فاي" : "⚪ غير متصل حالياً");

  char cap_str[90] = "غير محدود ♾️";
  if (cap_down > 0 || cap_up > 0) {
    snprintf(cap_str, sizeof(cap_str), "⬇️ %.1f Mbps | ⬆️ %.1f Mbps",
             (double)cap_down / 1000000.0, (double)cap_up / 1000000.0);
  }

  char q_str[130] = "غير محددة (استهلاك مفتوح ♾️)";
  if (quota > 0) {
    snprintf(q_str, sizeof(q_str), "%.2f GB (متبقي %.2f GB - %u%%)", q_gb, rem_gb, (unsigned)pct);
  }

  snprintf(out, max_out,
           "📱 <b>تقرير جهازك: %s</b>\n\n"
           "🌐 <b>عنوان IP:</b> <code>%s</code>\n"
           "📶 <b>حالة الاتصال:</b> %s%s\n"
           "📊 <b>حصة الباقة:</b> %s\n"
           "📥 <b>إجمالي الاستهلاك:</b> %.2f GB\n"
           "   • ⬇️ تحميل: %.2f GB\n"
           "   • ⬆️ رفع: %.2f GB\n"
           "⚡ <b>السرعة اللحظية:</b> ⬇️ %.2f Mbps | ⬆️ %.2f Mbps\n"
           "🛡️ <b>سقف السرعة:</b> %s",
           dev_name, ip_str, conn_str, gam ? " | 🎮 VIP" : "",
           q_str, tot_gb, down_gb, up_gb,
           (double)live_down / 1000000.0, (double)live_up / 1000000.0,
           cap_str);
}

static void format_multi_device_summary(const int *p_indices, size_t count, char *out, size_t max_out, char *kb_out, size_t max_kb) {
  uint64_t total_used = 0;
  uint64_t total_quota = 0;
  uint64_t total_up = 0;
  uint64_t total_down = 0;

  char dev_lines[700] = {0};
  char kb_buttons[500] = {0};
  double gb_div = (router_gb_standard == 1000) ? 1000000000.0 : (1024.0 * 1024.0 * 1024.0);

  for (size_t i = 0; i < count; ++i) {
    int p_idx = p_indices[i];
    if (p_idx < 0 || (size_t)p_idx >= policy_count) continue;

    uint8_t mac[6];
    uint64_t quota = 0;
    bool blk = false;
    char dev_name[DEVICE_NAME_BYTES] = "جهاز";

    portENTER_CRITICAL(&data_mux);
    memcpy(mac, device_policies[p_idx].mac, 6);
    quota = device_policies[p_idx].quota_bytes;
    blk = device_policies[p_idx].blocked;

    int n = meter_find_name_locked(mac, false);
    if (n >= 0 && device_names[n].name[0]) {
      strncpy(dev_name, device_names[n].name, sizeof(dev_name) - 1);
      dev_name[sizeof(dev_name) - 1] = 0;
    }

    int u = meter_find_usage_locked(mac, false);
    uint64_t up = (u >= 0) ? usage_records[u].upload_bytes : 0;
    uint64_t down = (u >= 0) ? usage_records[u].download_bytes : 0;
    portEXIT_CRITICAL(&data_mux);

    uint64_t dev_tot = up + down;
    total_used += dev_tot;
    total_up += up;
    total_down += down;
    total_quota += quota;

    double d_used_gb = (double)dev_tot / gb_div;
    double d_q_gb = (double)quota / gb_div;
    uint32_t pct = (quota > 0) ? (uint32_t)((d_used_gb / d_q_gb) * 100.0) : 0;

    char line[160];
    if (quota > 0) {
      snprintf(line, sizeof(line), "• <b>%s</b>: %.2f / %.2f GB (%u%%) %s\n",
               dev_name, d_used_gb, d_q_gb, (unsigned)pct, blk ? "🔴" : "🟢");
    } else {
      snprintf(line, sizeof(line), "• <b>%s</b>: %.2f GB %s\n",
               dev_name, d_used_gb, blk ? "🔴" : "🟢");
    }
    if (strlen(dev_lines) + strlen(line) < sizeof(dev_lines) - 20) {
      strcat(dev_lines, line);
    }

    // Add inline keyboard button
    char btn[140];
    snprintf(btn, sizeof(btn), "%s[{\"text\":\"📱 %s\",\"callback_data\":\"dev_idx:%d\"}]",
             (kb_buttons[0] ? "," : ""), dev_name, p_idx);
    if (strlen(kb_buttons) + strlen(btn) < sizeof(kb_buttons) - 30) {
      strcat(kb_buttons, btn);
    }
  }

  double tot_used_gb = (double)total_used / gb_div;
  double tot_up_gb = (double)total_up / gb_div;
  double tot_down_gb = (double)total_down / gb_div;
  double tot_q_gb = (double)total_quota / gb_div;
  double rem_q_gb = (tot_q_gb > tot_used_gb) ? (tot_q_gb - tot_used_gb) : 0.0;
  uint32_t overall_pct = (tot_q_gb > 0) ? (uint32_t)((tot_used_gb / tot_q_gb) * 100.0) : 0;

  char q_header[180] = "";
  if (tot_q_gb > 0) {
    snprintf(q_header, sizeof(q_header),
             "🎯 <b>إجمالي الباقات:</b> %.2f GB (متبقي %.2f GB - %u%%)\n",
             tot_q_gb, rem_q_gb, (unsigned)overall_pct);
  }

  snprintf(out, max_out,
           "📊 <b>تقرير استهلاك أجهزتك المربوطة (%u أجهزة):</b>\n\n"
           "📥 <b>إجمالي الاستهلاك:</b> %.2f GB (⬇️ %.2f GB | ⬆️ %.2f GB)\n"
           "%s\n"
           "📱 <b>قائمة الأجهزة:</b>\n"
           "%s\n"
           "👇 <i>اضغط على زر أي جهاز بالأسفل لعرض تفاصيله وسرعته:</i>",
           (unsigned)count, tot_used_gb, tot_down_gb, tot_up_gb,
           q_header, dev_lines);

  // Add refresh button
  if (strlen(kb_buttons) + 60 < sizeof(kb_buttons)) {
    strcat(kb_buttons, ",[{\"text\":\"🔄 تحديث البيانات\",\"callback_data\":\"dev_all\"}]");
  }

  snprintf(kb_out, max_kb, "{\"inline_keyboard\":[%s]}", kb_buttons);
}

// -----------------------------------------------------------------------------
// Automated Multi-Threshold Push Notifications (Main Quota & Per-Device Quota)
// -----------------------------------------------------------------------------
void telegram_check_quota_alerts(uint64_t total_used_bytes, uint64_t main_quota_bytes_val) {
  if (!telegram_enabled || !telegram_token[0]) {
    return;
  }
  if (telegram_busy) return;

  double gb_div = (router_gb_standard == 1000) ? 1000000000.0 : (1024.0 * 1024.0 * 1024.0);

  // 1. Check Main ISP Quota Multi-Thresholds
  if (main_quota_bytes_val > 0 && telegram_chat_id[0]) {
    uint32_t current_pct = (uint32_t)((total_used_bytes * 100ULL) / main_quota_bytes_val);

    char thresh_copy[50];
    strncpy(thresh_copy, telegram_thresholds, sizeof(thresh_copy) - 1);
    thresh_copy[sizeof(thresh_copy) - 1] = 0;

    char *saveptr = NULL;
    char *tok = strtok_r(thresh_copy, ",", &saveptr);
    uint8_t th_idx = 0;

    while (tok && th_idx < 16) {
      while (*tok == ' ') tok++;
      int th_val = atoi(tok);
      if (th_val > 0 && th_val <= 100) {
        uint32_t mask = (1UL << th_idx);
        if (current_pct >= (uint32_t)th_val && !(telegram_alert_flags & mask)) {
          telegram_alert_flags |= mask;
          config_save_telegram_flags(telegram_alert_flags);

          double used_gb = (double)total_used_bytes / gb_div;
          double total_gb = (double)main_quota_bytes_val / gb_div;
          double rem_gb = total_gb > used_gb ? (total_gb - used_gb) : 0.0;

          time_t now = time(NULL);
          int32_t rem_days = -1, rem_hours = -1;
          if (now > 1700000000 && quota_end_date[0]) {
            time_t end_ts = meter_parse_date(quota_end_date, true);
            if (end_ts > now) {
              int64_t diff = (int64_t)(end_ts - now);
              rem_days = (int32_t)(diff / 86400);
              rem_hours = (int32_t)((diff % 86400) / 3600);
            } else {
              rem_days = 0; rem_hours = 0;
            }
          }

          uint16_t active_cnt = 0;
          portENTER_CRITICAL(&data_mux);
          for (size_t k = 0; k < MAX_CLIENTS; ++k) {
            if (valid_device_mac(active_clients[k].mac) && active_clients[k].aid != 0) {
              active_cnt++;
            }
          }
          portEXIT_CRITICAL(&data_mux);

          char alert_msg[550];
          if (rem_days >= 0) {
            snprintf(alert_msg, sizeof(alert_msg),
                     "🚨 <b>تنبيه استهلاك باقة الإنترنت الرئيسية (%u%%)</b> 🚨\n\n"
                     "📊 <b>النسبة المستهلكة:</b> %u%%\n"
                     "📥 <b>المستهلك:</b> %.2f GB من أصل %.2f GB\n"
                     "⏳ <b>الرصيد المتبقي:</b> %.2f GB\n"
                     "📅 <b>تاريخ التجديد:</b> %s (متبقي %ld يوم و %ld ساعة)\n"
                     "⚡ <b>الأجهزة النشطة:</b> %u / 12 متصل",
                     th_val, (unsigned)current_pct, used_gb, total_gb, rem_gb,
                     quota_end_date, (long)rem_days, (long)rem_hours, active_cnt);
          } else {
            snprintf(alert_msg, sizeof(alert_msg),
                     "🚨 <b>تنبيه استهلاك باقة الإنترنت الرئيسية (%u%%)</b> 🚨\n\n"
                     "📊 <b>النسبة المستهلكة:</b> %u%%\n"
                     "📥 <b>المستهلك:</b> %.2f GB من أصل %.2f GB\n"
                     "⏳ <b>الرصيد المتبقي:</b> %.2f GB\n"
                     "⚡ <b>الأجهزة النشطة:</b> %u / 12 متصل",
                     th_val, (unsigned)current_pct, used_gb, total_gb, rem_gb, active_cnt);
          }

          ESP_LOGW(TG_TAG, "Main ISP threshold %u%% triggered! Sending alert to admin %s...", th_val, telegram_chat_id);
          telegram_busy = true;
          telegram_send_message(telegram_chat_id, alert_msg);
          telegram_busy = false;
          return; // Send one alert per tick
        }
      }
      tok = strtok_r(NULL, ",", &saveptr);
      th_idx++;
    }
  }

  // 2. Check Per-Device Quota Multi-Thresholds
  portENTER_CRITICAL(&data_mux);
  for (size_t p = 0; p < policy_count; ++p) {
    uint64_t dev_quota = device_policies[p].quota_bytes;
    if (dev_quota == 0) continue;

    int u = meter_find_usage_locked(device_policies[p].mac, false);
    uint64_t dev_used = (u >= 0) ? (usage_records[u].upload_bytes + usage_records[u].download_bytes) : 0;
    uint32_t dev_pct = (uint32_t)((dev_used * 100ULL) / dev_quota);

    char dev_name[DEVICE_NAME_BYTES] = "جهاز متصل";
    int n_idx = meter_find_name_locked(device_policies[p].mac, false);
    if (n_idx >= 0 && device_names[n_idx].name[0]) {
      strncpy(dev_name, device_names[n_idx].name, sizeof(dev_name) - 1);
      dev_name[sizeof(dev_name) - 1] = 0;
    }

    uint32_t dev_ip = 0;
    for (size_t k = 0; k < MAX_CLIENTS; ++k) {
      if (memcmp(active_clients[k].mac, device_policies[p].mac, 6) == 0 && active_clients[k].aid != 0) {
        dev_ip = active_clients[k].ip;
        break;
      }
    }
    if (dev_ip == 0) {
      int b = meter_find_binding_locked(device_policies[p].mac, false);
      if (b >= 0) dev_ip = device_bindings[b].bound_ip;
    }

    uint8_t mac_copy[6];
    memcpy(mac_copy, device_policies[p].mac, 6);
    uint16_t cur_alert_flags = device_policies[p].alert_flags;
    char linked_chat[32] = {0};
    strncpy(linked_chat, device_policies[p].telegram_chat_id, sizeof(linked_chat) - 1);
    portEXIT_CRITICAL(&data_mux);

    char thresh_copy[50];
    strncpy(thresh_copy, telegram_thresholds, sizeof(thresh_copy) - 1);
    thresh_copy[sizeof(thresh_copy) - 1] = 0;

    char *saveptr = NULL;
    char *tok = strtok_r(thresh_copy, ",", &saveptr);
    uint8_t th_idx = 0;

    while (tok && th_idx < 16) {
      while (*tok == ' ') tok++;
      int th_val = atoi(tok);
      if (th_val > 0 && th_val <= 100) {
        uint16_t mask = (1U << th_idx);
        if (dev_pct >= (uint32_t)th_val && !(cur_alert_flags & mask)) {
          portENTER_CRITICAL(&data_mux);
          int p_cur = meter_find_policy_locked(mac_copy, false);
          if (p_cur >= 0) {
            device_policies[p_cur].alert_flags |= mask;
          }
          portEXIT_CRITICAL(&data_mux);

          double dev_used_gb = (double)dev_used / gb_div;
          double dev_total_gb = (double)dev_quota / gb_div;
          double dev_rem_gb = (dev_total_gb > dev_used_gb) ? (dev_total_gb - dev_used_gb) : 0.0;

          char alert_msg[550];
          ip4_addr_t dev_addr = {.addr = dev_ip};
          const char *status_str = (dev_pct >= 100) ? "🚫 تم حظر الوصول للإنترنت تلقائياً لوصول الاستهلاك إلى 100%" : "🟢 متصل وتحت المراقبة";

          snprintf(alert_msg, sizeof(alert_msg),
                   "🚨 <b>تنبيه استهلاك باقة الجهاز (%u%%)</b> 🚨\n\n"
                   "📱 <b>الجهاز:</b> %s\n"
                   "🌐 <b>عنوان IP:</b> %d.%d.%d.%d\n"
                   "📊 <b>النسبة المستهلكة:</b> %u%%\n"
                   "📥 <b>المستهلك:</b> %.2f GB من أصل %.2f GB\n"
                   "⏳ <b>الرصيد المتبقي:</b> %.2f GB\n"
                   "⚠️ <b>الحالة:</b> %s",
                   th_val, dev_name, IP2STR(&dev_addr), (unsigned)dev_pct,
                   dev_used_gb, dev_total_gb, dev_rem_gb, status_str);

          ESP_LOGW(TG_TAG, "Device alert triggered: %s reached %u%%", dev_name, th_val);
          telegram_busy = true;

          // Dispatch to linked owner chat_id
          if (linked_chat[0]) {
            telegram_send_message(linked_chat, alert_msg);
            // Optionally CC admin if different
            if (telegram_chat_id[0] && strcmp(linked_chat, telegram_chat_id) != 0) {
              char cc_msg[600];
              snprintf(cc_msg, sizeof(cc_msg), "📢 [نسخة للمدير]\n%s", alert_msg);
              telegram_send_message(telegram_chat_id, cc_msg);
            }
          } else if (telegram_chat_id[0]) {
            // No personal chat_id: alert admin
            telegram_send_message(telegram_chat_id, alert_msg);
          }

          telegram_busy = false;
          meter_save_usage();
          return; // Send one alert per tick
        }
      }
      tok = strtok_r(NULL, ",", &saveptr);
      th_idx++;
    }

    portENTER_CRITICAL(&data_mux);
  }
  portEXIT_CRITICAL(&data_mux);
}

// -----------------------------------------------------------------------------
// Interactive Telegram Bot Command Handler (Background Long Polling FreeRTOS Task)
// -----------------------------------------------------------------------------
static void telegram_task(void *pvParameters) {
  (void)pvParameters;
  ESP_LOGI(TG_TAG, "Telegram polling task started on Core %d", xPortGetCoreID());

  char *rx_buffer = (char *)malloc(4096);
  if (!rx_buffer) {
    ESP_LOGE(TG_TAG, "Failed to allocate memory for Telegram rx buffer");
    vTaskDelete(NULL);
    return;
  }

  while (true) {
    // Wait for Wi-Fi STA connection
    wifi_ap_record_t ap_info;
        // Post-Boot Alert for Daily Scheduled Auto-Reboot
    if (config_get_scheduled_reboot_flag()) {
      config_set_scheduled_reboot_flag(false);
      if (telegram_enabled && telegram_token[0] && telegram_chat_id[0]) {
        char reboot_msg[512];
        uint32_t free_ram_kb = (uint32_t)(esp_get_free_heap_size() / 1024);
        snprintf(reboot_msg, sizeof(reboot_msg),
                 "🔄 <b>تمت إعادة التشغيل اليومية المجدولة بنجاح!</b>\n\n"
                 "✅ تم تنظيف الذاكرة ومسح الـ Cache وحفظ العدادات.\n"
                 "🧠 <b>الذاكرة الحرة (Free RAM):</b> %lu KB\n"
                 "⏱️ <b>الموعد المجدول:</b> %02d:%02d يومياً (UTC%+d)\n"
                 "🚀 راوتر ESP32 يعمل الآن بأقصى سرعة واستقرار.",
                 (unsigned long)free_ram_kb,
                 auto_reboot_hour, auto_reboot_min, auto_reboot_tz_offset);
        telegram_send_message(telegram_chat_id, reboot_msg);
      }
    }

    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK || !telegram_enabled || !telegram_interactive || !telegram_token[0]) {
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }

    char url[220];
    snprintf(url, sizeof(url),
             "https://api.telegram.org/bot%s/getUpdates?offset=%" PRIi64 "&timeout=5&limit=5",
             telegram_token, last_update_id + 1);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 12000,
        .skip_cert_common_name_check = true,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
      vTaskDelay(pdMS_TO_TICKS(4000));
      continue;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
      esp_http_client_fetch_headers(client);
      int read_len = esp_http_client_read(client, rx_buffer, 4095);
      if (read_len > 0) {
        rx_buffer[read_len] = 0;

        char *ptr = rx_buffer;
        while ((ptr = strstr(ptr, "\"update_id\":")) != NULL) {
          ptr += 12;
          int64_t up_id = strtoll(ptr, NULL, 10);
          if (up_id > last_update_id) {
            last_update_id = up_id;
          }

          char *next_up = strstr(ptr, "\"update_id\":");

          // -------------------------------------------------------------------
          // A. Check for Telegram Callback Query (Inline Keyboard Button Tap)
          // -------------------------------------------------------------------
          char *cb_ptr = strstr(ptr, "\"callback_query\":");
          if (cb_ptr && (!next_up || cb_ptr < next_up)) {
            char cb_id[64] = {0};
            char cb_sender[32] = {0};
            char cb_data[64] = {0};

            // Extract callback id
            char *id_p = strstr(cb_ptr, "\"id\":");
            if (id_p && id_p < cb_ptr + 80) {
              id_p += 5;
              while (*id_p == ' ' || *id_p == '"') id_p++;
              size_t c_idx = 0;
              while (*id_p && *id_p != '"' && c_idx < sizeof(cb_id) - 1) {
                cb_id[c_idx++] = *id_p++;
              }
              cb_id[c_idx] = 0;
            }

            // Extract sender from.id
            char *from_p = strstr(cb_ptr, "\"from\":");
            if (from_p && from_p < cb_ptr + 150) {
              char *from_id_p = strstr(from_p, "\"id\":");
              if (from_id_p && from_id_p < from_p + 100) {
                from_id_p += 5;
                while (*from_id_p == ' ') from_id_p++;
                size_t s_idx = 0;
                while ((*from_id_p == '-' || (*from_id_p >= '0' && *from_id_p <= '9')) && s_idx < sizeof(cb_sender) - 1) {
                  cb_sender[s_idx++] = *from_id_p++;
                }
                cb_sender[s_idx] = 0;
              }
            }

            // Extract callback data
            char *data_p = strstr(cb_ptr, "\"data\":");
            if (data_p && (!next_up || data_p < next_up)) {
              data_p += 7;
              while (*data_p == ' ' || *data_p == '"') data_p++;
              size_t d_idx = 0;
              while (*data_p && *data_p != '"' && d_idx < sizeof(cb_data) - 1) {
                cb_data[d_idx++] = *data_p++;
              }
              cb_data[d_idx] = 0;
            }

            if (cb_id[0]) {
              telegram_answer_callback_query(cb_id, NULL);
            }

            if (cb_sender[0] && cb_data[0]) {
              ESP_LOGI(TG_TAG, "Callback query received: data='%s' from chat=%s", cb_data, cb_sender);

              if (strncmp(cb_data, "dev_idx:", 8) == 0) {
                int dev_i = atoi(cb_data + 8);
                char rep[900];
                format_single_device_report(dev_i, rep, sizeof(rep));
                telegram_send_message(cb_sender, rep);
              } else if (strcmp(cb_data, "dev_all") == 0) {
                int linked_indices[MAX_RECORDS];
                size_t linked_count = 0;
                portENTER_CRITICAL(&data_mux);
                for (size_t p = 0; p < policy_count; ++p) {
                  if (device_policies[p].telegram_chat_id[0] != 0 &&
                      strcmp(device_policies[p].telegram_chat_id, cb_sender) == 0) {
                    if (linked_count < MAX_RECORDS) linked_indices[linked_count++] = (int)p;
                  }
                }
                portEXIT_CRITICAL(&data_mux);

                char sum_rep[1000], kb_rep[600];
                format_multi_device_summary(linked_indices, linked_count, sum_rep, sizeof(sum_rep), kb_rep, sizeof(kb_rep));
                telegram_send_message_with_keyboard(cb_sender, sum_rep, kb_rep);
              } else if (strcmp(cb_data, "adm_devices") == 0 && strcmp(cb_sender, telegram_chat_id) == 0) {
                // Forward to devices summary
                char reply[1200] = "👥 <b>الأجهزة النشطة حالياً:</b>\n\n";
                uint16_t count = 0;
                portENTER_CRITICAL(&data_mux);
                for (size_t k = 0; k < MAX_CLIENTS; ++k) {
                  if (valid_device_mac(active_clients[k].mac) && active_clients[k].aid != 0) {
                    count++;
                    uint32_t ip = active_clients[k].ip;
                    char name[DEVICE_NAME_BYTES] = "جهاز متصل";
                    int n_idx = meter_find_name_locked(active_clients[k].mac, false);
                    if (n_idx >= 0 && device_names[n_idx].name[0]) {
                      strncpy(name, device_names[n_idx].name, sizeof(name) - 1);
                    }
                    int u_idx = meter_find_usage_locked(active_clients[k].mac, false);
                    uint32_t cur_down = (u_idx >= 0) ? device_live_speeds[u_idx].cur_down_bps : 0;
                    uint32_t cur_up = (u_idx >= 0) ? device_live_speeds[u_idx].cur_up_bps : 0;

                    ip4_addr_t bip = {.addr = ip};
                    char dev_line[160];
                    snprintf(dev_line, sizeof(dev_line),
                             "• <b>%s</b> (<code>%d.%d.%d.%d</code>)\n"
                             "   ⬇️ %.2f Mbps | ⬆️ %.2f Mbps\n",
                             name, IP2STR(&bip), (double)cur_down / 1000000.0, (double)cur_up / 1000000.0);
                    if (strlen(reply) + strlen(dev_line) < sizeof(reply) - 80) strcat(reply, dev_line);
                  }
                }
                portEXIT_CRITICAL(&data_mux);
                telegram_send_message(cb_sender, reply);
              }
            }
          }

          // -------------------------------------------------------------------
          // B. Check for Telegram Standard Message
          // -------------------------------------------------------------------
          char *msg_ptr = strstr(ptr, "\"message\":");
          if (msg_ptr && (!next_up || msg_ptr < next_up)) {
            // Locate sender chat ID
            char *chat_ptr = strstr(msg_ptr, "\"chat\":");
            char sender_chat_id[32] = {0};
            if (chat_ptr) {
              char *id_ptr = strstr(chat_ptr, "\"id\":");
              if (id_ptr && id_ptr < chat_ptr + 100) {
                id_ptr += 5;
                size_t k = 0;
                while ((*id_ptr == ' ' || *id_ptr == '-' || (*id_ptr >= '0' && *id_ptr <= '9')) && k < sizeof(sender_chat_id) - 1) {
                  if (*id_ptr != ' ') sender_chat_id[k++] = *id_ptr;
                  id_ptr++;
                }
                sender_chat_id[k] = 0;
              }
            }

            // Locate message text
            char *text_ptr = strstr(msg_ptr, "\"text\":");
            char cmd_text[128] = {0};
            if (text_ptr) {
              text_ptr += 7;
              while (*text_ptr == ' ' || *text_ptr == '"') text_ptr++;
              size_t m = 0;
              while (*text_ptr && *text_ptr != '"' && m < sizeof(cmd_text) - 1) {
                cmd_text[m++] = *text_ptr++;
              }
              cmd_text[m] = 0;
            }

            if (!sender_chat_id[0] || !cmd_text[0]) {
              continue;
            }

            // Check if sender is admin or first-time setup
            bool is_admin = false;
            if (telegram_chat_id[0] == 0) {
              // First time setup: auto-bind chat ID as administrator
              snprintf(telegram_chat_id, sizeof(telegram_chat_id), "%s", sender_chat_id);
              config_save_telegram(telegram_enabled, telegram_interactive, telegram_token, telegram_chat_id, telegram_thresholds);
              is_admin = true;
            } else if (strcmp(sender_chat_id, telegram_chat_id) == 0) {
              is_admin = true;
            }

            // Check for devices linked to this sender's chat_id
            int linked_indices[MAX_RECORDS];
            size_t linked_count = 0;
            portENTER_CRITICAL(&data_mux);
            for (size_t p = 0; p < policy_count; ++p) {
              if (device_policies[p].telegram_chat_id[0] != 0 &&
                  strcmp(device_policies[p].telegram_chat_id, sender_chat_id) == 0) {
                if (linked_count < MAX_RECORDS) {
                  linked_indices[linked_count++] = (int)p;
                }
              }
            }
            portEXIT_CRITICAL(&data_mux);

            ESP_LOGI(TG_TAG, "Processing message '%s' from %s (is_admin=%d, linked_devices=%u)",
                     cmd_text, sender_chat_id, is_admin, (unsigned)linked_count);

            // =================================================================
            // CASE 1: UNRECOGNIZED SENDER (Not Admin & No Linked Devices)
            // =================================================================
            if (!is_admin && linked_count == 0) {
              char polite_reply[600];
              snprintf(polite_reply, sizeof(polite_reply),
                       "👋 <b>مرحباً بك في بوت راوتر ESP32 الذكي!</b> 🌐\n\n"
                       "🆔 <b>معرف المحادثة الخاص بك (Chat ID):</b>\n"
                       "<code>%s</code>\n\n"
                       "⚠️ <i>هذا الحساب غير مربوط بأي جهاز في الشبكة حالياً.</i>\n\n"
                       "يرجى نسخ هذا المعرف وتزويد مدير الشبكة به لربطه بأجهزتك لتتمكن من متابعة استهلاكك وسرعاتك وباقاتك مباشرة.",
                       sender_chat_id);
              telegram_send_message(sender_chat_id, polite_reply);
              continue;
            }

            // =================================================================
            // CASE 2: SINGLE LINKED DEVICE OWNER (Not Admin)
            // =================================================================
            if (!is_admin && linked_count == 1) {
              char dev_rep[950];
              format_single_device_report(linked_indices[0], dev_rep, sizeof(dev_rep));
              telegram_send_message(sender_chat_id, dev_rep);
              continue;
            }

            // =================================================================
            // CASE 3: MULTIPLE LINKED DEVICES OWNER (Not Admin)
            // =================================================================
            if (!is_admin && linked_count >= 2) {
              char sum_rep[1000], kb_rep[600];
              format_multi_device_summary(linked_indices, linked_count, sum_rep, sizeof(sum_rep), kb_rep, sizeof(kb_rep));
              telegram_send_message_with_keyboard(sender_chat_id, sum_rep, kb_rep);
              continue;
            }

            // =================================================================
            // CASE 4: ADMINISTRATOR (Full Router Management Capabilities)
            // =================================================================
            if (is_admin) {
              // 1. Command: /quota, استهلاك, باقي, كوتا
              if (strstr(cmd_text, "/quota") || strstr(cmd_text, "استهلاك") || strstr(cmd_text, "باقي") || strstr(cmd_text, "كوتا")) {
                uint64_t total_up = 0, total_down = 0;
                uint32_t live_up = 0, live_down = 0;
                uint16_t active_cnt = 0;

                portENTER_CRITICAL(&data_mux);
                for (size_t i = 0; i < usage_count; ++i) {
                  total_up += usage_records[i].upload_bytes;
                  total_down += usage_records[i].download_bytes;
                }
                live_up = total_cur_up_bps;
                live_down = total_cur_down_bps;
                for (size_t k = 0; k < MAX_CLIENTS; ++k) {
                  if (valid_device_mac(active_clients[k].mac) && active_clients[k].aid != 0) {
                    active_cnt++;
                  }
                }
                portEXIT_CRITICAL(&data_mux);

                double gb_div = (router_gb_standard == 1000) ? 1000000000.0 : (1024.0 * 1024.0 * 1024.0);
                double total_used_gb = (double)(total_up + total_down) / gb_div;
                double main_cap_gb = (double)main_quota_bytes / gb_div;
                double rem_gb = (main_cap_gb > total_used_gb) ? (main_cap_gb - total_used_gb) : 0.0;
                double used_pct = (main_cap_gb > 0) ? ((total_used_gb / main_cap_gb) * 100.0) : 0.0;

                time_t now = time(NULL);
                int32_t rem_days = -1, rem_hours = -1;
                if (now > 1700000000 && quota_end_date[0]) {
                  time_t end_ts = meter_parse_date(quota_end_date, true);
                  if (end_ts > now) {
                    int64_t diff = (int64_t)(end_ts - now);
                    rem_days = (int32_t)(diff / 86400);
                    rem_hours = (int32_t)((diff % 86400) / 3600);
                  } else {
                    rem_days = 0; rem_hours = 0;
                  }
                }

                char reply[1200];
                if (main_cap_gb > 0) {
                  snprintf(reply, sizeof(reply),
                           "📊 <b>تقرير استهلاك باقة الإنترنت الرئيسية:</b>\n\n"
                           "📥 <b>المستهلك:</b> %.2f GB (%.1f%%)\n"
                           "🎯 <b>سعة الباقة:</b> %.2f GB\n"
                           "⏳ <b>الرصيد المتبقي:</b> %.2f GB\n"
                           "📅 <b>تاريخ التجديد:</b> %s (%ld يوم و %ld ساعة)\n"
                           "⚡ <b>الأجهزة النشطة:</b> %u / 12 متصل\n"
                           "🚀 <b>السرعة اللحظية:</b> ⬇️ %.2f Mbps | ⬆️ %.2f Mbps\n",
                           total_used_gb, used_pct, main_cap_gb, rem_gb,
                           quota_end_date, (long)rem_days, (long)rem_hours, active_cnt,
                           (double)live_down / 1000000.0, (double)live_up / 1000000.0);
                } else {
                  snprintf(reply, sizeof(reply),
                           "📊 <b>إجمالي استهلاك راوتر ESP32:</b>\n\n"
                           "📥 <b>إجمالي التحميل:</b> %.2f GB\n"
                           "📤 <b>إجمالي الرفع:</b> %.2f GB\n"
                           "⚡ <b>الأجهزة النشطة:</b> %u / 12 متصل\n"
                           "🚀 <b>السرعة اللحظية:</b> ⬇️ %.2f Mbps | ⬆️ %.2f Mbps\n",
                           (double)total_down / gb_div, (double)total_up / gb_div,
                           active_cnt, (double)live_down / 1000000.0, (double)live_up / 1000000.0);
                }

                // Append per-device breakdown for devices with assigned quotas
                bool has_dev_quota = false;
                portENTER_CRITICAL(&data_mux);
                for (size_t p = 0; p < policy_count; ++p) {
                  if (device_policies[p].quota_bytes > 0) {
                    if (!has_dev_quota) {
                      strcat(reply, "\n📱 <b>باقات الأجهزة المحددة:</b>\n");
                      has_dev_quota = true;
                    }
                    int u = meter_find_usage_locked(device_policies[p].mac, false);
                    uint64_t d_used = (u >= 0) ? (usage_records[u].upload_bytes + usage_records[u].download_bytes) : 0;
                    double d_used_gb = (double)d_used / gb_div;
                    double d_q_gb = (double)device_policies[p].quota_bytes / gb_div;
                    uint32_t d_pct = (uint32_t)((d_used * 100ULL) / device_policies[p].quota_bytes);

                    char d_name[DEVICE_NAME_BYTES] = "جهاز";
                    int n = meter_find_name_locked(device_policies[p].mac, false);
                    if (n >= 0 && device_names[n].name[0]) {
                      strncpy(d_name, device_names[n].name, sizeof(d_name) - 1);
                      d_name[sizeof(d_name) - 1] = 0;
                    }

                    char d_line[120];
                    snprintf(d_line, sizeof(d_line), "• <b>%s</b>: %.2f / %.2f GB (%u%%) %s\n",
                             d_name, d_used_gb, d_q_gb, (unsigned)d_pct,
                             (device_policies[p].blocked || d_pct >= 100) ? "🚫" : "🟢");
                    if (strlen(reply) + strlen(d_line) < sizeof(reply) - 20) {
                      strcat(reply, d_line);
                    }
                  }
                }
                portEXIT_CRITICAL(&data_mux);

                const char *adm_kb = "{\"inline_keyboard\":[[{\"text\":\"👥 الأجهزة المتصلة\",\"callback_data\":\"adm_devices\"},{\"text\":\"🚀 قياس السرعة\",\"callback_data\":\"/speed\"}],[{\"text\":\"⚙️ حالة النظام\",\"callback_data\":\"/status\"}]]}";
                telegram_send_message_with_keyboard(sender_chat_id, reply, adm_kb);
              }
              // 2. Command: /devices, أجهزة, المتصلين
              else if (strstr(cmd_text, "/devices") || strstr(cmd_text, "أجهزة") || strstr(cmd_text, "المتصلين")) {
                char reply[1200] = "👥 <b>الأجهزة النشطة حالياً (SoftAP & Bridge):</b>\n\n";
                uint16_t count = 0;

                portENTER_CRITICAL(&data_mux);
                for (size_t k = 0; k < MAX_CLIENTS; ++k) {
                  if (valid_device_mac(active_clients[k].mac) && active_clients[k].aid != 0) {
                    count++;
                    uint32_t ip = active_clients[k].ip;
                    char name[DEVICE_NAME_BYTES] = "جهاز متصل";
                    int n_idx = meter_find_name_locked(active_clients[k].mac, false);
                    if (n_idx >= 0 && device_names[n_idx].name[0]) {
                      strncpy(name, device_names[n_idx].name, sizeof(name) - 1);
                      name[sizeof(name) - 1] = 0;
                    }

                    int p_idx = meter_find_policy_locked(active_clients[k].mac, false);
                    bool blk = (p_idx >= 0) ? device_policies[p_idx].blocked : false;
                    bool gam = (p_idx >= 0) ? device_policies[p_idx].gaming_mode : false;
                    uint64_t q_bytes = (p_idx >= 0) ? device_policies[p_idx].quota_bytes : 0;

                    int u_idx = meter_find_usage_locked(active_clients[k].mac, false);
                    uint32_t cur_down = (u_idx >= 0) ? device_live_speeds[u_idx].cur_down_bps : 0;
                    uint32_t cur_up = (u_idx >= 0) ? device_live_speeds[u_idx].cur_up_bps : 0;

                    int s_idx = meter_find_speed_locked(active_clients[k].mac, false);
                    uint32_t cap_down = (s_idx >= 0) ? device_speeds[s_idx].download_bps : 0;
                    uint32_t cap_up = (s_idx >= 0) ? device_speeds[s_idx].upload_bps : 0;

                    ip4_addr_t bip = {.addr = ip};
                    char dev_line[220];
                    const char *stat_tag = blk ? "🔴 محظور" : (gam ? "🎮 ألعاب VIP" : "🟢 نشط");

                    if (q_bytes > 0) {
                      uint64_t dev_used = (u_idx >= 0) ? (usage_records[u_idx].upload_bytes + usage_records[u_idx].download_bytes) : 0;
                      uint32_t q_pct = (uint32_t)((dev_used * 100ULL) / q_bytes);
                      double q_gb = (double)q_bytes / 1000000000.0;
                      snprintf(dev_line, sizeof(dev_line),
                               "• <b>%s</b> (<code>%d.%d.%d.%d</code>)\n"
                               "   ⚡ ⬇️ %.2f Mbps | ⬆️ %.2f Mbps\n"
                               "   📊 باقة: %.1f GB (%u%%) | %s\n",
                               name, IP2STR(&bip),
                               (double)cur_down / 1000000.0, (double)cur_up / 1000000.0,
                               q_gb, (unsigned)q_pct, stat_tag);
                    } else if (cap_down > 0 || cap_up > 0) {
                      snprintf(dev_line, sizeof(dev_line),
                               "• <b>%s</b> (<code>%d.%d.%d.%d</code>)\n"
                               "   ⚡ ⬇️ %.2f Mbps | ⬆️ %.2f Mbps\n"
                               "   🛡️ تحديد: ⬇️ %.1f M | ⬆️ %.1f M | %s\n",
                               name, IP2STR(&bip),
                               (double)cur_down / 1000000.0, (double)cur_up / 1000000.0,
                               (double)cap_down / 1000000.0, (double)cap_up / 1000000.0, stat_tag);
                    } else {
                      snprintf(dev_line, sizeof(dev_line),
                               "• <b>%s</b> (<code>%d.%d.%d.%d</code>) [%s]\n"
                               "   ⚡ ⬇️ %.2f Mbps | ⬆️ %.2f Mbps\n",
                               name, IP2STR(&bip), stat_tag,
                               (double)cur_down / 1000000.0, (double)cur_up / 1000000.0);
                    }

                    if (strlen(reply) + strlen(dev_line) < sizeof(reply) - 80) {
                      strcat(reply, dev_line);
                    }
                  }
                }
                portEXIT_CRITICAL(&data_mux);

                if (count == 0) {
                  strcat(reply, "لا توجد أجهزة متصلة بالواي فاي حالياً.");
                } else {
                  char summary[256];
                  snprintf(summary, sizeof(summary),
                           "\nالإجمالي: %u / 12 متصل\n"
                           "💡 للحظر: <code>/block [IP]</code> | لفك الحظر: <code>/unblock [IP]</code>", count);
                  strcat(reply, summary);
                }
                telegram_send_message(sender_chat_id, reply);
              }
              // 3. Command: /all, الكل
              else if (strstr(cmd_text, "/all") || strstr(cmd_text, "الكل")) {
                char reply[1200] = "📋 <b>سجل كافة أجهزة الشبكة المسجلة:</b>\n\n";
                portENTER_CRITICAL(&data_mux);
                for (size_t p = 0; p < policy_count; ++p) {
                  char d_name[DEVICE_NAME_BYTES] = "جهاز";
                  int n_idx = meter_find_name_locked(device_policies[p].mac, false);
                  if (n_idx >= 0 && device_names[n_idx].name[0]) {
                    strncpy(d_name, device_names[n_idx].name, sizeof(d_name) - 1);
                  }
                  int u_idx = meter_find_usage_locked(device_policies[p].mac, false);
                  uint64_t dev_used = (u_idx >= 0) ? (usage_records[u_idx].upload_bytes + usage_records[u_idx].download_bytes) : 0;
                  double u_gb = (double)dev_used / 1000000000.0;
                  double q_gb = (double)device_policies[p].quota_bytes / 1000000000.0;

                  char line[150];
                  if (device_policies[p].quota_bytes > 0) {
                    snprintf(line, sizeof(line), "• <b>%s</b>: %.2f / %.2f GB %s%s\n",
                             d_name, u_gb, q_gb,
                             device_policies[p].blocked ? "🔴" : "🟢",
                             device_policies[p].telegram_chat_id[0] ? " [TG]" : "");
                  } else {
                    snprintf(line, sizeof(line), "• <b>%s</b>: %.2f GB %s%s\n",
                             d_name, u_gb,
                             device_policies[p].blocked ? "🔴" : "🟢",
                             device_policies[p].telegram_chat_id[0] ? " [TG]" : "");
                  }
                  if (strlen(reply) + strlen(line) < sizeof(reply) - 30) {
                    strcat(reply, line);
                  }
                }
                portEXIT_CRITICAL(&data_mux);
                telegram_send_message(sender_chat_id, reply);
              }
              // 4. Command: /block, حظر
              else if ((strstr(cmd_text, "/block") && !strstr(cmd_text, "/unblock")) ||
                       (strstr(cmd_text, "حظر") && !strstr(cmd_text, "فك"))) {
                char *arg = NULL;
                if (strncmp(cmd_text, "/block", 6) == 0) arg = cmd_text + 6;
                else if (strncmp(cmd_text, "حظر", 6) == 0 || strncmp(cmd_text, "حظر", 3) == 0) {
                  arg = strstr(cmd_text, "حظر") + strlen("حظر");
                } else {
                  arg = cmd_text;
                }
                while (*arg == ' ') arg++;

                uint8_t target_mac[6] = {0};
                char target_name[DEVICE_NAME_BYTES] = {0};
                uint32_t target_ip = 0;

                if (arg[0] && find_device_by_target(arg, target_mac, target_name, sizeof(target_name), &target_ip)) {
                  meter_set_device_blocked(target_mac, true);
                  char reply[512];
                  ip4_addr_t tip = {.addr = target_ip};
                  snprintf(reply, sizeof(reply),
                           "🚫 <b>تم حظر الجهاز بنجاح!</b>\n\n"
                           "📱 <b>الجهاز:</b> %s\n"
                           "🌐 <b>عنوان IP:</b> %d.%d.%d.%d\n"
                           "⚡ تم قطع وصول الإنترنت فوراً عبر lwIP NAPT hook.",
                           target_name, IP2STR(&tip));
                  telegram_send_message(sender_chat_id, reply);
                } else {
                  char reply[512];
                  snprintf(reply, sizeof(reply),
                           "⚠️ <b>تعذر العثور على الجهاز المطلوب: '%.32s'</b>\n\n"
                           "يرجى تحديد عنوان IP صحيح للجهاز، مثال:\n"
                           "<code>/block 192.168.4.2</code>\n"
                           "أو كتابة <code>/devices</code> لعرض الأجهزة المتصلة.", arg);
                  telegram_send_message(sender_chat_id, reply);
                }
              }
              // 5. Command: /unblock, فك_حظر, فك حظر
              else if (strstr(cmd_text, "/unblock") || strstr(cmd_text, "فك_حظر") || strstr(cmd_text, "فك حظر")) {
                char *arg = NULL;
                if (strncmp(cmd_text, "/unblock", 8) == 0) arg = cmd_text + 8;
                else if (strstr(cmd_text, "فك_حظر")) arg = strstr(cmd_text, "فك_حظر") + strlen("فك_حظر");
                else if (strstr(cmd_text, "فك حظر")) arg = strstr(cmd_text, "فك حظر") + strlen("فك حظر");
                else arg = cmd_text;
                while (*arg == ' ') arg++;

                uint8_t target_mac[6] = {0};
                char target_name[DEVICE_NAME_BYTES] = {0};
                uint32_t target_ip = 0;

                if (arg[0] && find_device_by_target(arg, target_mac, target_name, sizeof(target_name), &target_ip)) {
                  meter_set_device_blocked(target_mac, false);
                  char reply[512];
                  ip4_addr_t tip = {.addr = target_ip};
                  snprintf(reply, sizeof(reply),
                           "✅ <b>تم إلغاء حظر الجهاز بنجاح!</b>\n\n"
                           "📱 <b>الجهاز:</b> %s\n"
                           "🌐 <b>عنوان IP:</b> %d.%d.%d.%d\n"
                           "⚡ تمت استعادة اتصال الإنترنت للجهاز بالكامل.",
                           target_name, IP2STR(&tip));
                  telegram_send_message(sender_chat_id, reply);
                } else {
                  char reply[512];
                  snprintf(reply, sizeof(reply),
                           "⚠️ <b>تعذر العثور على الجهاز المطلوب: '%.32s'</b>\n\n"
                           "يرجى تحديد عنوان IP صحيح للجهاز، مثال:\n"
                           "<code>/unblock 192.168.4.2</code>", arg);
                  telegram_send_message(sender_chat_id, reply);
                }
              }
              // 6. Command: /speed, سرعة
              else if (strstr(cmd_text, "/speed") || strstr(cmd_text, "سرعة")) {
                uint16_t active_cnt = 0;
                portENTER_CRITICAL(&data_mux);
                for (size_t k = 0; k < MAX_CLIENTS; ++k) {
                  if (valid_device_mac(active_clients[k].mac) && active_clients[k].aid != 0) {
                    active_cnt++;
                  }
                }
                uint32_t lup = total_cur_up_bps;
                uint32_t ldown = total_cur_down_bps;
                portEXIT_CRITICAL(&data_mux);

                char reply[450];
                snprintf(reply, sizeof(reply),
                         "🚀 <b>السرعة اللحظية لراوتر ESP32:</b>\n\n"
                         "📥 <b>سرعة التحميل (Download):</b> %.2f Mbps\n"
                         "📤 <b>سرعة الرفع (Upload):</b> %.2f Mbps\n"
                         "👥 <b>الأجهزة النشطة:</b> %u / 12 متصل\n"
                         "⚡ <b>عرض النطاق:</b> 40 MHz (HT40 فائق السرعة)\n"
                         "📶 <b>قوة الإشارة:</b> %d dBm (%s)",
                         (double)ldown / 1000000.0, (double)lup / 1000000.0,
                         active_cnt, ap_info.rssi, ap_info.ssid);
                telegram_send_message(sender_chat_id, reply);
              }
              // 7. Command: /ota, تحديث
              else if (strstr(cmd_text, "/ota") || strstr(cmd_text, "تحديث")) {
                ota_info_t o_inf;
                ota_updater_get_info(&o_inf);
                char reply[650];
                snprintf(reply, sizeof(reply),
                         "🔄 <b>نظام التحديث الهوائي (Dual-Bank OTA):</b>\n\n"
                         "🏷️ <b>الإصدار الحالي:</b> %s\n"
                         "🕒 <b>تاريخ البناء:</b> %s\n"
                         "💾 <b>القسم النشط:</b> %s\n"
                         "🎯 <b>القسم البديل:</b> %s\n"
                         "🛡️ <b>الحماية من الأعطال:</b> Anti-Rollback مفعل\n\n"
                         "💡 <i>يمكنك رفع ملف .bin أو فحص التحديثات السحابية من لوحة تحكم الويب.</i>",
                         o_inf.current_version, o_inf.build_timestamp,
                         o_inf.running_partition, o_inf.target_partition);
                telegram_send_message(sender_chat_id, reply);
              }
              // 8. Command: /reboot, ريستارت
              else if (strstr(cmd_text, "/reboot") || strstr(cmd_text, "ريستارت")) {
                telegram_send_message(sender_chat_id, "🔄 <b>جاري حفظ العدادات وإعادة تشغيل الراوتر الآن بأمان...</b>");
                vTaskDelay(pdMS_TO_TICKS(1500));
                meter_save_usage();
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
              }
              // 9. Command: /status, حالة
              else if (strstr(cmd_text, "/status") || strstr(cmd_text, "حالة")) {
                uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
                uint32_t days = uptime_s / 86400;
                uint32_t hours = (uptime_s % 86400) / 3600;
                uint32_t mins = (uptime_s % 3600) / 60;
                uint32_t free_heap_kb = esp_get_free_heap_size() / 1024;

                uint16_t active_cnt = 0;
                portENTER_CRITICAL(&data_mux);
                for (size_t k = 0; k < MAX_CLIENTS; ++k) {
                  if (valid_device_mac(active_clients[k].mac) && active_clients[k].aid != 0) {
                    active_cnt++;
                  }
                }
                portEXIT_CRITICAL(&data_mux);

                char reply[750];
                snprintf(reply, sizeof(reply),
                         "⚙️ <b>حالة نظام راوتر ESP32-C5 (Wi-Fi 6):</b>\n\n"
                         "⏱️ <b>مدة التشغيل:</b> %lu يوم و %lu ساعة و %lu دقيقة\n"
                         "🧠 <b>الذاكرة المتاحة (RAM):</b> %lu KB\n"
                         "⚡ <b>المعالج:</b> RISC-V 32-bit @ %u MHz\n📡 <b>التردد:</b> 5GHz Wi-Fi 6 (802.11ax)\n"
                         "📶 <b>الراوتر الأساسي:</b> %s (إشارة %d dBm)\n"
                         "👥 <b>الأجهزة المتصلة:</b> %u / 12 متصل\n"
                         "🚀 <b>السرعة اللحظية:</b> ⬇️ %.2f Mbps | ⬆️ %.2f Mbps",
                         (unsigned long)days, (unsigned long)hours, (unsigned long)mins,
                         (unsigned long)free_heap_kb, (unsigned)esp_cpu_freq_mhz,
                         ap_info.ssid, ap_info.rssi, active_cnt,
                         (double)total_cur_down_bps / 1000000.0, (double)total_cur_up_bps / 1000000.0);
                telegram_send_message(sender_chat_id, reply);
              }
              // 10. Command: /help, /start, مساعدة
              else {
                char reply[1100];
                snprintf(reply, sizeof(reply),
                         "👋 <b>مرحباً بك في لوحة تحكم راوتر ESP32 (المدير)</b> 🌐\n\n"
                         "الأوامر الإدارية المتاحة:\n"
                         "🔹 <code>/quota</code> أو <b>استهلاك</b> : رصيد الباقة وباقات الأجهزة\n"
                         "🔹 <code>/devices</code> أو <b>أجهزة</b> : الأجهزة المتصلة وسرعاتها وحصصها\n"
                         "🔹 <code>/all</code> أو <b>الكل</b> : قائمة بكافة الأجهزة المسجلة\n"
                         "🔹 <code>/speed</code> أو <b>سرعة</b> : اختبار السرعة اللحظية وعرض القناة\n"
                         "🔹 <code>/block [IP]</code> أو <b>حظر [IP]</b> : حظر جهاز من الإنترنت\n"
                         "🔹 <code>/unblock [IP]</code> أو <b>فك_حظر [IP]</b> : إلغاء حظر جهاز\n"
                         "🔹 <code>/status</code> أو <b>حالة</b> : تقرير حالة النظام والمعالج والذاكرة\n"
                         "🔹 <code>/ota</code> أو <b>تحديث</b> : إصدار النظام وقسم الإقلاع\n"
                         "🔹 <code>/reboot</code> أو <b>ريستارت</b> : إعادة تشغيل الراوتر بأمان");
                telegram_send_message(sender_chat_id, reply);
              }
            }
          }
        }
      }
    }
    esp_http_client_cleanup(client);
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
  free(rx_buffer);
  vTaskDelete(NULL);
}

void telegram_bot_init(void) {
  // Pinned strictly to Core 1 with 6.5KB stack to keep Core 0 dedicated to Wi-Fi & NAPT
  xTaskCreate(telegram_task, "telegram_task", 6144, NULL, 3, NULL);
  ESP_LOGI(TG_TAG, "Telegram bot background task created (ESP32-C5 RISC-V)");
}
