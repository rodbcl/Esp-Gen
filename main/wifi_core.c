#include "wifi_core.h"

#include <stdbool.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_wifi.h"

static const char *TAG = "wifi";

static bool s_init;
static esp_netif_t *s_sta;

esp_err_t wifi_core_init(void)
{
    if (s_init) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    esp_netif_create_default_wifi_ap();
    s_sta = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    s_init = true;
    return ESP_OK;
}

esp_netif_t *wifi_core_sta_netif(void)
{
    return s_sta;
}
