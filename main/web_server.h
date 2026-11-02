#pragma once

#include <stdbool.h>
#include "esp_http_server.h"

extern httpd_handle_t web_server;

void web_server_start(void);
void web_server_stop(void);

