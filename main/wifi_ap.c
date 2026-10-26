#include "wifi_ap.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "wifi_core.h"

static const char *TAG = "wifi";

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

esp_err_t wifi_ap_start(void)
{
    if (s_on) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(wifi_core_init(), TAG, "wifi");

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
    esp_wifi_set_max_tx_power(WIFI_AP_TX_DBM * 4);  // unidade de 0,25 dBm
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
