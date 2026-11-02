#include "wifi_manager.h"
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/lwip_napt.h"
#include "lwip/etharp.h"
#include "meter.h"
#include "router_config.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include <time.h>

esp_netif_t *ap_netif = NULL;
esp_netif_t *sta_netif = NULL;

scan_entry_t scan_results[MAX_SCAN_RESULTS];
uint16_t scan_result_count = 0;
bool scan_running = false;
bool scan_ready = false;

static void collect_scan_results(void) {
  uint16_t found = 0;
  if (esp_wifi_scan_get_ap_num(&found) != ESP_OK) found = 0;
  if (found > MAX_SCAN_RESULTS) found = MAX_SCAN_RESULTS;
  wifi_ap_record_t records[MAX_SCAN_RESULTS] = {0};
  if (found > 0 && esp_wifi_scan_get_ap_records(&found, records) != ESP_OK) found = 0;

  portENTER_CRITICAL(&data_mux);
  scan_result_count = found;
  memset(scan_results, 0, sizeof(scan_results));
  for (uint16_t i = 0; i < found; ++i) {
    memcpy(scan_results[i].ssid, records[i].ssid, sizeof(scan_results[i].ssid) - 1);
    scan_results[i].ssid[sizeof(scan_results[i].ssid) - 1] = 0;
    scan_results[i].rssi = records[i].rssi;
    scan_results[i].secured = (records[i].authmode != WIFI_AUTH_OPEN);
    scan_results[i].is_5g = (records[i].primary >= 36);
    scan_results[i].is_ax = (records[i].phy_11ax != 0);
    scan_results[i].channel = records[i].primary;
  }
  scan_running = false;
  scan_ready = true;
  portEXIT_CRITICAL(&data_mux);
}

void wifi_manager_advertise_uplink_dns(void) {
  esp_netif_dns_info_t dns = {0};
  if (esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK) return;
  uint8_t offer_dns = 0x02;
  esp_netif_dhcps_stop(ap_netif);
  esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                         ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns));
  esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns);
  esp_netif_dhcps_start(ap_netif);
}

static void wifi_manager_init_sntp(void) {
  if (!esp_sntp_enabled()) {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_init();
    setenv("TZ", "UTC-2", 1);
    tzset();
    ESP_LOGI(ROUTER_TAG, "SNTP time synchronization initialized");
  }
}

static void handle_ap_station_connected(const wifi_event_ap_staconnected_t *event) {
  if (!event) return;
  portENTER_CRITICAL(&data_mux);
  size_t slot = MAX_CLIENTS;
  for (size_t i = 0; i < MAX_CLIENTS; ++i) {
    if (memcmp(active_clients[i].mac, event->mac, 6) == 0) {
      slot = i;
      break;
    }
    if (slot == MAX_CLIENTS && active_clients[i].mac[0] == 0 &&
        active_clients[i].mac[1] == 0 && active_clients[i].mac[2] == 0 &&
        active_clients[i].mac[3] == 0 && active_clients[i].mac[4] == 0 &&
        active_clients[i].mac[5] == 0) slot = i;
  }
  if (slot < MAX_CLIENTS) {
    memcpy(active_clients[slot].mac, event->mac, 6);
    active_clients[slot].aid = event->aid;
    active_clients[slot].last_seen_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
  }
  meter_find_usage_locked(event->mac, true);
  meter_find_policy_locked(event->mac, true);
  meter_rebuild_fast_lookup();
  portEXIT_CRITICAL(&data_mux);
}

static void handle_ap_station_disconnected(const wifi_event_ap_stadisconnected_t *event) {
  if (!event) return;
  portENTER_CRITICAL(&data_mux);
  for (size_t i = 0; i < MAX_CLIENTS; ++i) {
    if (memcmp(active_clients[i].mac, event->mac, 6) == 0) {
      active_clients[i].aid = 0;
      break;
    }
  }
  meter_rebuild_fast_lookup();
  portEXIT_CRITICAL(&data_mux);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
    collect_scan_results();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
    handle_ap_station_connected((const wifi_event_ap_staconnected_t *)data);
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
    handle_ap_station_disconnected((const wifi_event_ap_stadisconnected_t *)data);
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    if (has_saved_config) esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    esp_netif_napt_disable(ap_netif);
    if (has_saved_config) esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED && sta_static_enabled) {
    esp_netif_set_default_netif(sta_netif);
    esp_netif_ip_info_t info = {0};
    if (esp_netif_get_ip_info(sta_netif, &info) == ESP_OK && info.ip.addr != 0) {
      esp_netif_dns_info_t dns = {0};
      if (!sta_dns_text[0] || esp_netif_str_to_ip4(sta_dns_text, &dns.ip.u_addr.ip4) != ESP_OK)
        dns.ip.u_addr.ip4 = info.gw;
      esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns);
      wifi_manager_advertise_uplink_dns();
      esp_netif_napt_enable(ap_netif);
      ESP_LOGI(ROUTER_TAG, "Static uplink active at " IPSTR, IP2STR(&info.ip));
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    esp_wifi_set_ps(esp_wifi_power_save ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
    esp_netif_set_default_netif(sta_netif);
    wifi_manager_advertise_uplink_dns();
    esp_err_t err = esp_netif_napt_enable(ap_netif);
    wifi_manager_init_sntp();
    uint8_t primary_ch = 36;
    wifi_second_chan_t sec_ch = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&primary_ch, &sec_ch) == ESP_OK) {
      if (primary_ch <= 14 && primary_ch > 11) {
        wifi_bandwidth_t cur_bw = WIFI_BW20;
        if (esp_wifi_get_bandwidth(WIFI_IF_AP, &cur_bw) == ESP_OK && cur_bw == WIFI_BW40) {
          ESP_LOGW(ROUTER_TAG, "Upstream AP is on 2.4G Channel %u > 11; clamping AP/STA bandwidth to HT20", (unsigned)primary_ch);
          esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20);
          esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
        }
      }
      ESP_LOGI(ROUTER_TAG, "Upstream connected on 5GHz Channel %u (Wi-Fi 6); NAPT result=%s", (unsigned)primary_ch, esp_err_to_name(err));
    } else {
      ESP_LOGI(ROUTER_TAG, "Upstream connected; NAPT result=%s", esp_err_to_name(err));
    }
  } else if (base == IP_EVENT && id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
    const ip_event_assigned_ip_to_client_t *event = (const ip_event_assigned_ip_to_client_t *)data;
    if (event) {
      portENTER_CRITICAL(&data_mux);
      for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        if (memcmp(active_clients[i].mac, event->mac, 6) == 0) {
          active_clients[i].ip = event->ip.addr;
          break;
        }
      }
      portEXIT_CRITICAL(&data_mux);
      meter_sync_device_on_dhcp(event->mac, event->ip.addr);
      meter_set_device_binding(event->mac, event->ip.addr);
      ESP_LOGI(ROUTER_TAG, "Station assigned IP " IPSTR " -> MAC %02X:%02X:%02X:%02X:%02X:%02X",
               IP2STR(&event->ip),
               event->mac[0], event->mac[1], event->mac[2], event->mac[3], event->mac[4], event->mac[5]);
    }
  }
}

void wifi_manager_refresh_clients(void) {
  wifi_sta_list_t wifi_list = {0};
  esp_netif_pair_mac_ip_t ip_list[MAX_CLIENTS] = {0};
  active_client_t fresh[MAX_CLIENTS] = {0};
  if (esp_wifi_ap_get_sta_list(&wifi_list) != ESP_OK) return;

  uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);

  size_t count = wifi_list.num < MAX_WIFI_STATIONS ? wifi_list.num : MAX_WIFI_STATIONS;
  for (size_t i = 0; i < count; ++i) {
    memcpy(fresh[i].mac, wifi_list.sta[i].mac, 6);
    memcpy(ip_list[i].mac, wifi_list.sta[i].mac, 6);
    fresh[i].rssi = wifi_list.sta[i].rssi;
    fresh[i].last_seen_s = now_s;
  }
  esp_netif_dhcps_get_clients_by_mac(ap_netif, count, ip_list);
  for (size_t i = 0; i < count; ++i) fresh[i].ip = ip_list[i].ip.addr;

  // Query lwIP ARP table for devices using a static IP
  for (size_t i = 0; i < count; ++i) {
    if (fresh[i].ip == 0) {
      for (size_t a = 0; a < ARP_TABLE_SIZE; ++a) {
        ip4_addr_t *arp_ip = NULL;
        struct netif *arp_netif = NULL;
        struct eth_addr *arp_eth = NULL;
        if (etharp_get_entry(a, &arp_ip, &arp_netif, &arp_eth) && arp_ip && arp_eth) {
          if (memcmp(arp_eth->addr, fresh[i].mac, 6) == 0 && arp_ip->addr != 0) {
            fresh[i].ip = arp_ip->addr;
            break;
          }
        }
      }
    }
  }

  // Discover bridged or secondary router devices on 192.168.4.x
  for (size_t a = 0; a < ARP_TABLE_SIZE && count < MAX_CLIENTS; ++a) {
    ip4_addr_t *arp_ip = NULL;
    struct netif *arp_netif = NULL;
    struct eth_addr *arp_eth = NULL;
    if (etharp_get_entry(a, &arp_ip, &arp_netif, &arp_eth) && arp_ip && arp_eth && arp_ip->addr != 0) {
      uint32_t ip4 = arp_ip->addr;
      if ((ip4 & 0x00FFFFFF) == 0x0004A8C0 && (ip4 >> 24) != 1) {
        bool already_known = false;
        for (size_t k = 0; k < count; ++k) {
          if (fresh[k].ip == ip4) {
            already_known = true;
            break;
          }
        }
        if (!already_known) {
          fresh[count].ip = ip4;
          fresh[count].aid = 0xFF; // Virtual/bridged client
          fresh[count].mac[0] = 0x02;
          fresh[count].mac[1] = 0x00;
          memcpy(fresh[count].mac + 2, &ip4, 4);
          fresh[count].rssi = -60;
          fresh[count].last_seen_s = now_s;
          count++;
        }
      }
    }
  }

  portENTER_CRITICAL(&data_mux);
  memcpy(active_clients, fresh, sizeof(fresh));
  for (size_t i = 0; i < count; ++i) {
    meter_find_usage_locked(fresh[i].mac, true);
    meter_find_policy_locked(fresh[i].mac, true);
  }
  meter_rebuild_fast_lookup();
  portEXIT_CRITICAL(&data_mux);
}

bool wifi_manager_start_scan(void) {
  portENTER_CRITICAL(&data_mux);
  if (scan_running) {
    portEXIT_CRITICAL(&data_mux);
    return false;
  }
  scan_running = true;
  scan_ready = false;
  scan_result_count = 0;
  portEXIT_CRITICAL(&data_mux);

  wifi_scan_config_t config = {.show_hidden = false};
  esp_err_t err = esp_wifi_scan_start(&config, false);
  if (err != ESP_OK) {
    portENTER_CRITICAL(&data_mux);
    scan_running = false;
    portEXIT_CRITICAL(&data_mux);
    return false;
  }
  return true;
}

void wifi_manager_deauth_station(uint16_t aid) {
  esp_wifi_deauth_sta(aid);
}

void wifi_manager_init(void) {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  ap_netif = esp_netif_create_default_wifi_ap();
  sta_netif = esp_netif_create_default_wifi_sta();
  ESP_ERROR_CHECK(esp_netif_set_default_netif(sta_netif));

  if (has_saved_config && sta_static_enabled) {
    esp_netif_ip_info_t sta_ip = {0};
    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(sta_netif));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(sta_ip_text, &sta_ip.ip));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(sta_mask_text, &sta_ip.netmask));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(sta_gateway_text, &sta_ip.gw));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(sta_netif, &sta_ip));
    esp_netif_dns_info_t dns = {0};
    if (sta_dns_text[0]) ESP_ERROR_CHECK(esp_netif_str_to_ip4(sta_dns_text, &dns.ip.u_addr.ip4));
    else dns.ip.u_addr.ip4 = sta_ip.gw;
    ESP_ERROR_CHECK(esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns));
  }

  esp_netif_ip_info_t ap_ip = {0};
  IP4_ADDR(&ap_ip.ip, AP_IP_A, AP_IP_B, AP_IP_C, AP_IP_D);
  IP4_ADDR(&ap_ip.gw, AP_IP_A, AP_IP_B, AP_IP_C, AP_IP_D);
  IP4_ADDR(&ap_ip.netmask, 255, 255, 255, 0);
  ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
  ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ap_ip));
  ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));

  wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init_config));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, &on_wifi_event, NULL));

  wifi_config_t ap_config = {0};
  const char *selected_ap_ssid = has_saved_config ? ap_ssid : SETUP_SSID;
  const char *selected_ap_pass = has_saved_config ? ap_password : SETUP_PASSWORD;
  ap_config.ap.ssid_len = strnlen(selected_ap_ssid, sizeof(ap_config.ap.ssid));
  memcpy(ap_config.ap.ssid, selected_ap_ssid, ap_config.ap.ssid_len);
  memcpy(ap_config.ap.password, selected_ap_pass,
         strnlen(selected_ap_pass, sizeof(ap_config.ap.password) - 1));
  ap_config.ap.channel = 36; // 5GHz default UNII-1 channel
  ap_config.ap.max_connection = MAX_WIFI_STATIONS; // 12 max direct Wi-Fi connections
  ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
  ap_config.ap.pmf_cfg.required = false;
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

  if (has_saved_config) {
    wifi_config_t sta_config = {0};
    memcpy(sta_config.sta.ssid, router_ssid,
           strnlen(router_ssid, sizeof(sta_config.sta.ssid)));
    memcpy(sta_config.sta.password, router_password,
           strnlen(router_password, sizeof(sta_config.sta.password) - 1));
    sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_config.sta.failure_retry_cnt = 0;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
  }
  ESP_ERROR_CHECK(esp_wifi_start());

#if CONFIG_SOC_WIFI_SUPPORT_5G
  // Configure pure 5GHz Wi-Fi 6 operation on ESP32-C5
  ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY));
#endif

  ESP_ERROR_CHECK(esp_wifi_set_ps(esp_wifi_power_save ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE));

  // Max TX Power: 20 dBm (80 * 0.25 dBm)
  esp_wifi_set_max_tx_power(80);

  // Bandwidth & Protocol Tuning: 5GHz 802.11a/n/ac/ax (Wi-Fi 6)
  uint8_t proto_mask = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX;
  wifi_bandwidth_t bw = WIFI_BW20; // HE20 on 5GHz for low latency and high stability

  esp_wifi_set_protocol(WIFI_IF_AP, proto_mask);
  esp_wifi_set_bandwidth(WIFI_IF_AP, bw);
  esp_wifi_set_protocol(WIFI_IF_STA, proto_mask);
  esp_wifi_set_bandwidth(WIFI_IF_STA, bw);
  ESP_LOGI(ROUTER_TAG, "5GHz Wi-Fi 6 radio active: proto=11ax/ac/n/a, bw=HE20, channel=36, max_conn=%d, tx_power=20dBm",
           MAX_WIFI_STATIONS);
}
