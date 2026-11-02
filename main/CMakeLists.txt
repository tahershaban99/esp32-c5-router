idf_component_register(SRCS "main.c"
                            "router_config.c"
                            "meter.c"
                            "wifi_manager.c"
                            "telegram_bot.c"
                            "web_server.c"
                            "ota_updater.c"
                            "dns_server.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_event esp_netif lwip nvs_flash esp_http_server esp_http_client esp_timer esp_driver_gpio mbedtls esp-tls app_update esp_https_ota)

# Add the router packet-accounting and rate-limiting hook directly to lwIP build
idf_component_get_property(lwip lwip COMPONENT_LIB)
target_compile_options(${lwip} PRIVATE "-I${CMAKE_CURRENT_LIST_DIR}")
target_compile_definitions(${lwip} PRIVATE "-DESP_IDF_LWIP_HOOK_FILENAME=\"meter_hook.h\"")
