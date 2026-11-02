#pragma once

#include <stdbool.h>
#include <stdint.h>

void telegram_bot_init(void);
void telegram_check_quota_alerts(uint64_t total_used_bytes, uint64_t main_quota_bytes);
void telegram_reset_alert_flags(void);
bool telegram_send_test_alert(void);
bool telegram_send_message(const char *chat_id, const char *text);
bool telegram_send_message_with_keyboard(const char *chat_id, const char *text, const char *inline_keyboard_json);
bool telegram_answer_callback_query(const char *callback_query_id, const char *text);
