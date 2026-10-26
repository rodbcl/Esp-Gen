#pragma once

#include "esp_err.h"
#include "esp_netif.h"

/*
 * Inicialização do Wi-Fi comum ao AP (Automação) e ao STA (Agente): netif, loop de eventos e driver.
 * Feita uma vez, na primeira vez que um dos dois liga.
 */

esp_err_t wifi_core_init(void);
// Interface da estação (STA); NULL antes de wifi_core_init()
esp_netif_t *wifi_core_sta_netif(void);
