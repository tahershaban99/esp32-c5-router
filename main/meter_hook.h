#pragma once

#include <stdint.h>
#include "esp_attr.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"

int meter_ip4_canforward(struct pbuf *packet, uint32_t destination_host_order);

// Apply per-device upload/download rate limits, count IPv4 traffic, and enforce
// quotas for forwarded packets. Local ESP32 dashboard traffic is not forwarded.
#define LWIP_HOOK_IP4_CANFORWARD(packet, destination_host_order) \
  meter_ip4_canforward((packet), (destination_host_order))

