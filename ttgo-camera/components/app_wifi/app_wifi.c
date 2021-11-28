#include "app_wifi.h"

#include "nvs_flash.h"
#include "esp_spi_flash.h"
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"

uint8_t sta_connected = 0;

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    // wifi event?
    if (event_base == WIFI_EVENT) {
        switch(event_id) {
            case WIFI_EVENT_WIFI_READY:
                break;

            case WIFI_EVENT_SCAN_DONE:
                break;

            case WIFI_EVENT_STA_START:
                // wifi is started, connect to AP
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_STOP:
                // wifi is stopped
                break;

            case WIFI_EVENT_STA_CONNECTED:
                // set connected bit
                ESP_LOGI("WIFI", "Connected");
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                // clear connected bit
                ESP_LOGI("WIFI", "Disconnected");
                sta_connected = 0;
                break;
        }
    }

    // ip event?
    if (event_base == IP_EVENT) {
        // station got ip event?
        if (event_id == IP_EVENT_STA_GOT_IP) {
            // print IP
            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            ESP_LOGI("WIFI", "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
            sta_connected = 1;
        }
    }
}

void wifi_init_sta(char *ssid, char *pass) {
    // create default event loop for wifi station
    esp_netif_create_default_wifi_sta();
    // register event handlers
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL);

    // get default wifi init config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    //
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // build config with ssid and password
    wifi_config_t wifi_config = { 0 };

    // copy ssid and password
    snprintf((char *) wifi_config.sta.ssid, 32, "%s", ssid);
    snprintf((char *) wifi_config.sta.password, 64, "%s", pass);

    /* Setting a password implies station will connect to all security modes including WEP/WPA.
     * However these modes are deprecated and not advisable to be used. Incase your Access point
     * doesn't support WPA2, these mode can be enabled by commenting below line */
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    // configure wifi
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));

    // start wifi
    ESP_ERROR_CHECK(esp_wifi_start());
}

uint8_t wifi_sta_is_connected(void) {
    return sta_connected;
}

void app_flash_init(void) {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

	// spifss configuration
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };

    // Use settings defined above to initialize and mount SPIFFS filesystem.
    // Note: esp_vfs_spiffs_register is an all-in-one convenience function.
    if (esp_vfs_spiffs_register(&conf) != ESP_OK) {
        ESP_LOGE("SPIFFS", "Failed to mount or format filesystem");
        return;
    }
    
    // feedback
    ESP_LOGI("SPIFFS", "SPIFFS mounted");
}