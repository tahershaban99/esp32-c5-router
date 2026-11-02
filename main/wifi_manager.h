#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_netif.h"
#include "router_globals.h"

extern esp_netif_t *ap_netif;
extern esp_netif_t *sta_netif;

extern scan_entry_t scan_results[MAX_SCAN_RESULTS];
extern uint16_t scan_result_count;
extern bool scan_running;
extern bool scan_ready;

void wifi_manager_init(void);
void wifi_manager_refresh_clients(void);
bool wifi_manager_start_scan(void);
void wifi_manager_deauth_station(uint16_t aid);
void wifi_manager_advertise_uplink_dns(void);

