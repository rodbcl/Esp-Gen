#include "wifi_ap.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "wifi";

static bool s_init;
static bool s_on;
static char s_ssid[16];

const char *wifi_ap_ssid(void)
{
    if (!s_ssid[0]) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        snprintf(s_ssid, sizeof(s_ssid), "EspGen-%02X%02X", mac[4], mac[5]);
    }
    return s_ssid;
}

static esp_err_t init_once(void)
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
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    s_init = true;
    return ESP_OK;
}

esp_err_t wifi_ap_start(void)
{
    if (s_on) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(init_once(), TAG, "wifi");

    wifi_config_t wc = {
        .ap = {
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    const char *ssid = wifi_ap_ssid();
    memcpy(wc.ap.ssid, ssid, strlen(ssid));
    wc.ap.ssid_len = strlen(ssid);
    memcpy(wc.ap.password, WIFI_AP_PASS, strlen(WIFI_AP_PASS));

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wc), TAG, "config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    s_on = true;
    ESP_LOGI(TAG, "AP %s ligado (%s)", ssid, WIFI_AP_IP);
    return ESP_OK;
}

void wifi_ap_stop(void)
{
    if (!s_on) {
        return;
    }
    esp_wifi_stop();
    s_on = false;
    ESP_LOGI(TAG, "AP desligado");
}
