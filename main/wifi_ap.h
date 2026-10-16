#pragma once

#include "esp_err.h"

/*
 * Ponto de acesso Wi-Fi do modo Automação. O nome é "EspGen-XXXX" (fim do MAC),
 * para não conflitar com outros geradores na bancada.
 */

#define WIFI_AP_PASS    "espgen123"     // mínimo de 8 caracteres (WPA2)
#define WIFI_AP_IP      "192.168.4.1"   // endereço padrão do AP no ESP-IDF

esp_err_t wifi_ap_start(void);
void wifi_ap_stop(void);
const char *wifi_ap_ssid(void);
