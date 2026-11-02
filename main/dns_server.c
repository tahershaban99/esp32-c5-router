#include "dns_server.h"
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "router_globals.h"

static const char *TAG = "dns_server";
static TaskHandle_t s_dns_task_handle = NULL;
static volatile int s_dns_socket = -1;

typedef struct __attribute__((packed)) {
  uint16_t id;
  uint8_t flags_hi;
  uint8_t flags_lo;
  uint16_t qdcount;
  uint16_t ancount;
  uint16_t nscount;
  uint16_t arcount;
} dns_header_t;

static void dns_server_task(void *pvParameters) {
  uint8_t buffer[256];
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(53);

  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (sock < 0) {
    ESP_LOGE(TAG, "Unable to create DNS socket: errno %d", errno);
    s_dns_task_handle = NULL;
    vTaskDelete(NULL);
    return;
  }

  // Set receive timeout of 1 second so the loop can check s_dns_socket
  struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    ESP_LOGE(TAG, "Unable to bind DNS socket to port 53: errno %d", errno);
    close(sock);
    s_dns_socket = -1;
    s_dns_task_handle = NULL;
    vTaskDelete(NULL);
    return;
  }

  s_dns_socket = sock;
  ESP_LOGI(TAG, "Captive Portal DNS Server active on UDP port 53 (redirecting to 192.168.4.1)");

  while (s_dns_socket >= 0) {
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    int len = recvfrom(sock, buffer, sizeof(buffer) - 16, 0,
                       (struct sockaddr *)&client_addr, &client_addr_len);

    if (len < 0) {
      // Timeout or interrupt
      continue;
    }

    if (len < (int)sizeof(dns_header_t)) {
      continue;
    }

    dns_header_t *hdr = (dns_header_t *)buffer;
    // Only answer standard DNS queries (QR == 0)
    if ((hdr->flags_hi & 0x80) != 0) {
      continue;
    }

    // Traverse Question section
    size_t pos = sizeof(dns_header_t);
    while (pos < (size_t)len && buffer[pos] != 0) {
      pos += (size_t)buffer[pos] + 1;
    }
    if (pos >= (size_t)len) continue; // Corrupt
    pos++; // Skip 0x00 terminator

    // Skip QTYPE (2 bytes) + QCLASS (2 bytes)
    if (pos + 4 > (size_t)len) continue;
    pos += 4;

    // Response Header
    hdr->flags_hi = 0x81; // QR=1, AA=1, RD=1
    hdr->flags_lo = 0x80; // RA=1, RCODE=0 (NoError)
    hdr->ancount = htons(1);
    hdr->nscount = 0;
    hdr->arcount = 0;

    // Answer RR
    buffer[pos++] = 0xc0;
    buffer[pos++] = 0x0c; // Pointer to QNAME
    buffer[pos++] = 0x00;
    buffer[pos++] = 0x01; // Type A
    buffer[pos++] = 0x00;
    buffer[pos++] = 0x01; // Class IN
    buffer[pos++] = 0x00;
    buffer[pos++] = 0x00;
    buffer[pos++] = 0x00;
    buffer[pos++] = 0x3c; // TTL: 60 seconds
    buffer[pos++] = 0x00;
    buffer[pos++] = 0x04; // Len: 4
    buffer[pos++] = AP_IP_A;
    buffer[pos++] = AP_IP_B;
    buffer[pos++] = AP_IP_C;
    buffer[pos++] = AP_IP_D;

    sendto(sock, buffer, pos, 0, (struct sockaddr *)&client_addr, client_addr_len);
  }

  ESP_LOGI(TAG, "DNS Server shutting down");
  if (sock >= 0) close(sock);
  s_dns_task_handle = NULL;
  vTaskDelete(NULL);
}

void dns_server_start(void) {
  if (s_dns_task_handle != NULL) return;
  s_dns_socket = 0;
  xTaskCreate(dns_server_task, "dns_server", 2560, NULL, 3, &s_dns_task_handle);
}

void dns_server_stop(void) {
  if (s_dns_socket >= 0) {
    s_dns_socket = -1;
  }
}

