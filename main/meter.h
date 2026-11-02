#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_attr.h"
#include "router_globals.h"

extern active_client_t active_clients[MAX_CLIENTS];
extern usage_record_t usage_records[MAX_RECORDS];
extern size_t usage_count;
extern device_policy_t device_policies[MAX_RECORDS];
extern size_t policy_count;
extern device_name_t device_names[MAX_RECORDS];
extern size_t name_count;
extern device_speed_t device_speeds[MAX_RECORDS];
extern size_t speed_count;
extern device_speed_bucket_t speed_buckets[MAX_RECORDS];
extern device_live_speed_t device_live_speeds[MAX_RECORDS];
extern device_binding_t device_bindings[MAX_RECORDS];
extern size_t binding_count;

extern uint32_t total_cur_up_bps;
extern uint32_t total_cur_down_bps;

extern uint16_t meter_period_days;
extern uint32_t meter_period_elapsed_s;

extern uint64_t main_quota_bytes;
extern char quota_start_date[12];
extern char quota_end_date[12];

extern int32_t reboot_countdown_s;
extern bool device_catalog_dirty;

void meter_load_usage(void);
void meter_save_usage(void);
void meter_tick_second(uint32_t elapsed_s);

// Device management
int meter_find_usage_locked(const uint8_t mac[6], bool create);
int meter_find_policy_locked(const uint8_t mac[6], bool create);
int meter_find_name_locked(const uint8_t mac[6], bool create);
int meter_find_speed_locked(const uint8_t mac[6], bool create);
int meter_find_binding_locked(const uint8_t mac[6], bool create);

void meter_sync_device_on_dhcp(const uint8_t physical_mac[6], uint32_t ip);
void meter_set_device_policy(const uint8_t mac[6], uint64_t quota_bytes, bool blocked, bool gaming_mode);
void meter_set_device_speed(const uint8_t mac[6], uint32_t upload_bps, uint32_t download_bps);
void meter_set_device_name(const uint8_t mac[6], const char *name);
void meter_set_device_binding(const uint8_t mac[6], uint32_t ip);
void meter_set_device_blocked(const uint8_t mac[6], bool blocked);
void meter_set_device_gaming(const uint8_t mac[6], bool gaming);
void meter_set_device_telegram(const uint8_t mac[6], const char *chat_id);
void meter_reset_device_usage(const uint8_t mac[6]);
bool meter_delete_device(const uint8_t mac[6]);

void meter_set_main_quota(uint64_t quota_bytes, const char *start_date, const char *end_date);
void meter_set_period_days(uint16_t days);
void meter_reset_all_usage(void);

time_t meter_parse_date(const char *date_str, bool end_of_day);
void meter_rebuild_fast_lookup(void);
void meter_reset_reboot_fired_state(void);

