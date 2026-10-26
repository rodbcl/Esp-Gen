#include "wifi_sta.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "wifi_core.h"

#include "wifi_secrets.h"

static const char *TAG = "wifi_sta";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_handlers;
static volatile bool s_on;
static volatile wifi_sta_state_t s_state;
static char s_ip[16];
static char s_host[16];

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        portENTER_CRITICAL(&s_lock);
        s_ip[0] = 0;
        portEXIT_CRITICAL(&s_lock);
        if (s_on) {
            // Rede caiu ou ainda não apareceu: tenta de novo
            s_state = WIFI_STA_CONNECTING;
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        portENTER_CRITICAL(&s_lock);
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        portEXIT_CRITICAL(&s_lock);
        s_state = WIFI_STA_CONNECTED;
        ESP_LOGI(TAG, "conectado a %s: %s", WIFI_STA_SSID, s_ip);
    }
}

const char *wifi_sta_hostname(void)
{
    if (!s_host[0]) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_host, sizeof(s_host), "espgen-%02x%02x", mac[4], mac[5]);
    }
    return s_host;
}

esp_err_t wifi_sta_start(void)
{
    if (s_on) {
        return ESP_OK;
    }
    if (!WIFI_STA_SSID[0]) {
        ESP_LOGE(TAG, "rede não configurada: crie main/wifi_secrets.h");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(wifi_core_init(), TAG, "wifi");
    if (!s_handlers) {
        ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL), TAG, "evt");
        ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL), TAG, "evt");
        esp_netif_set_hostname(wifi_core_sta_netif(), wifi_sta_hostname());
        s_handlers = true;
    }

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, WIFI_STA_SSID, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, WIFI_STA_PASS, sizeof(wc.sta.password));
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "config");
    s_on = true;
    s_state = WIFI_STA_CONNECTING;
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        s_on = false;
        s_state = WIFI_STA_OFF;
        return err;
    }
    esp_wifi_set_max_tx_power(WIFI_STA_TX_DBM * 4);  // unidade de 0,25 dBm
    ESP_LOGI(TAG, "conectando a %s como %s", WIFI_STA_SSID, wifi_sta_hostname());
    return ESP_OK;
}

void wifi_sta_stop(void)
{
    if (!s_on) {
        return;
    }
    s_on = false;
    esp_wifi_disconnect();
    esp_wifi_stop();
    s_state = WIFI_STA_OFF;
    portENTER_CRITICAL(&s_lock);
    s_ip[0] = 0;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "desconectado");
}

wifi_sta_state_t wifi_sta_state(void)
{
    return s_state;
}

const char *wifi_sta_ip(void)
{
    // Cópia estável para quem lê de outra task
    static char ip[16];
    portENTER_CRITICAL(&s_lock);
    memcpy(ip, s_ip, sizeof(ip));
    portEXIT_CRITICAL(&s_lock);
    return ip;
}

const char *wifi_sta_ssid(void)
{
    return WIFI_STA_SSID;
}
