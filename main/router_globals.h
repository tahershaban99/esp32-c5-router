#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

// Capacity constants
#define MAX_CLIENTS 32           // L3 IP/MAC tracking table for up to 32 concurrent active devices
#define MAX_WIFI_STATIONS 12     // SoftAP maximum direct Wi-Fi connections
#define MAX_RECORDS 64           // Persistent stored records in NVS
#define MAX_SCAN_RESULTS 20      // Nearby Wi-Fi scan results

// SoftAP Subnet 192.168.4.1/24
#define AP_IP_A 192
#define AP_IP_B 168
#define AP_IP_C 4
#define AP_IP_D 1

#define USAGE_SAVE_INTERVAL_MS 30000
#define METER_SCALE_DEFAULT_PCT 100
extern uint8_t router_meter_scale_pct;
extern uint16_t router_gb_standard;

#define DEVICE_NAME_BYTES 64

#define ROUTER_TAG "router"
#define SETUP_SSID "ESP32-C5-Router-5G"
#define SETUP_PASSWORD "configure-123"
#define BRIDGED_CLIENT_TIMEOUT_S 60

// Memory-optimized packed C structs (Ultra-Low RAM Footprint)
typedef struct __attribute__((packed)) {
  uint8_t mac[6];
  uint32_t ip; // IPv4 address in network byte order, 0 when inactive.
  int8_t rssi;
  uint8_t aid;
  uint32_t last_seen_s;
} active_client_t;

typedef struct __attribute__((packed)) {
  uint8_t mac[6];
  uint64_t upload_bytes;   // client -> Internet
  uint64_t download_bytes; // Internet -> client
} usage_record_t;

typedef struct __attribute__((packed)) {
  uint8_t mac[6];
  uint64_t quota_bytes; // 0 means unlimited.
  bool blocked;
  bool gaming_mode;     // VIP low-latency priority for gaming & ping protection.
  uint16_t alert_flags; // Bitmask of triggered quota alert thresholds (e.g. 1<<0 for 50%, etc.)
  char telegram_chat_id[32]; // Linked Telegram Chat ID for this device owner
} device_policy_t;

typedef struct __attribute__((packed)) {
  uint8_t mac[6];
  char name[DEVICE_NAME_BYTES];
} device_name_t;

typedef struct __attribute__((packed)) {
  uint8_t mac[6];
  uint32_t upload_bps;
  uint32_t download_bps;
} device_speed_t;

typedef struct __attribute__((packed)) {
  uint8_t mac[6];
  uint32_t bound_ip; // IPv4 address in network byte order (0 means not bound)
} device_binding_t;

typedef struct {
  uint64_t upload_tokens;
  uint64_t download_tokens;
  int64_t upload_updated_us;
  int64_t download_updated_us;
} device_speed_bucket_t;

typedef struct {
  uint64_t last_up_bytes;
  uint64_t last_down_bytes;
  uint32_t cur_up_bps;
  uint32_t cur_down_bps;
} device_live_speed_t;

typedef struct {
  char ssid[33];
  int8_t rssi;
  bool secured;
  bool is_5g;
  bool is_ax;
  uint8_t channel;
} scan_entry_t;

// Fast O(1) device runtime cache structure for direct lookup array ip_lookup[256]
typedef struct {
  uint32_t ip;               // network byte order
  uint8_t mac[6];            // canonical MAC (02:00:C0:A8:04:XX or physical)
  int usage_idx;             // index in usage_records, or -1
  int policy_idx;            // index in device_policies, or -1
  int speed_idx;             // index in device_speeds, or -1
  volatile bool blocked;     // direct atomic flag for 0-overhead drop
  volatile bool gaming_mode; // VIP priority flag
  volatile uint32_t last_seen_s;
} dev_fast_entry_t;

extern dev_fast_entry_t* ip_lookup[256];
extern dev_fast_entry_t fast_device_pool[MAX_CLIENTS];
extern portMUX_TYPE data_mux;

static inline bool valid_device_mac(const uint8_t mac[6]) {
  bool nonzero = false;
  for (size_t i = 0; i < 6; ++i) {
    if (mac[i] != 0) {
      nonzero = true;
      break;
    }
  }
  return nonzero && (mac[0] & 1U) == 0;
}
