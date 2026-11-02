#include <stdbool.h>
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#if defined(CONFIG_BT_ENABLED)
#include "esp_bt.h"
#endif

#include "meter.h"
#include "router_config.h"
#include "router_globals.h"
#include "web_server.h"
#include "wifi_manager.h"
#include "telegram_bot.h"
#include "ota_updater.h"

#include "driver/gpio.h"
// NiceMCU ESP32-C5 Mini uses GPIO 9 for the hardware BOOT button
#define BOOT_BUTTON_GPIO 9

void app_main(void) {
#if defined(CONFIG_BT_ENABLED)
  // Reclaim Bluetooth controller DRAM memory if BT is compiled in
  esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
#endif

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(ROUTER_TAG, "NVS flash needed erase: %s", esp_err_to_name(err));
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  } else {
    ESP_ERROR_CHECK(err);
    ESP_LOGI(ROUTER_TAG, "NVS flash initialized successfully");
  }

  nvs_stats_t nvs_stats;
  if (nvs_get_stats(NULL, &nvs_stats) == ESP_OK) {
    ESP_LOGI(ROUTER_TAG, "NVS Storage: Used=%zu, Free=%zu, Avail=%zu, Total=%zu",
             nvs_stats.used_entries, nvs_stats.free_entries, nvs_stats.available_entries, nvs_stats.total_entries);
  }

  // Setup GPIO 0 (BOOT button) for factory credentials reset
  gpio_config_t io_conf = {
    .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&io_conf);

  config_load();
  config_apply_performance(esp_cpu_freq_mhz, esp_wifi_power_save);
  meter_load_usage();
  ota_updater_init();
  wifi_manager_init();
  web_server_start();
  telegram_bot_init();

  ESP_LOGI(ROUTER_TAG, "=================================================");
  ESP_LOGI(ROUTER_TAG, "ESP32-C5 5GHz Wi-Fi 6 Router & Bandwidth Manager Ready!");
  ESP_LOGI(ROUTER_TAG, "Dashboard URL: http://192.168.4.1");
  if (!has_saved_config) {
    ESP_LOGI(ROUTER_TAG, "Setup Wi-Fi: %s / %s (5GHz Wi-Fi 6)", SETUP_SSID, SETUP_PASSWORD);
  }
  ESP_LOGI(ROUTER_TAG, "Free Internal Heap at boot: %lu KB (Min: %lu KB)",
           (unsigned long)(esp_get_free_heap_size() / 1024),
           (unsigned long)(esp_get_minimum_free_heap_size() / 1024));
  ESP_LOGI(ROUTER_TAG, "=================================================");

  TickType_t last_save = xTaskGetTickCount();
  TickType_t last_period_tick = last_save;
  uint32_t boot_press_seconds = 0;

  while (true) {
    wifi_manager_refresh_clients();

    // Hardware BOOT button 10-second hold detection for admin credentials reset
    if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) { // Pressed (Active Low)
      boot_press_seconds++;
      ESP_LOGW(ROUTER_TAG, "BOOT button held: %lu / 10s", (unsigned long)boot_press_seconds);
      if (boot_press_seconds >= 10) {
        ESP_LOGW(ROUTER_TAG, "BOOT button held for 10s! Resetting admin credentials to admin/admin");
        config_reset_admin_default();
        boot_press_seconds = 0;
      }
    } else {
      boot_press_seconds = 0;
    }

    bool persist_new_device = false;
    portENTER_CRITICAL(&data_mux);
    persist_new_device = device_catalog_dirty;
    portEXIT_CRITICAL(&data_mux);
    if (persist_new_device) {
      meter_save_usage();
    }

    TickType_t now = xTaskGetTickCount();
    if ((now - last_period_tick) >= pdMS_TO_TICKS(1000)) {
      uint32_t elapsed_s = (uint32_t)(now - last_period_tick) / configTICK_RATE_HZ;
      if (elapsed_s == 0) elapsed_s = 1;
      last_period_tick = now;
      meter_tick_second(elapsed_s);
    }

    if ((now - last_save) >= pdMS_TO_TICKS(USAGE_SAVE_INTERVAL_MS)) {
      meter_save_usage();
      last_save = now;
      ESP_LOGI(ROUTER_TAG, "System Health: Free Heap=%lu B, Min Heap=%lu B, Largest Block=%lu B",
               (unsigned long)esp_get_free_heap_size(),
               (unsigned long)esp_get_minimum_free_heap_size(),
               (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

