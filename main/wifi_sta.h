#pragma once

#include <stdbool.h>
#include "esp_err.h"

/*
 * Conexão a uma rede Wi-Fi existente (modo Agente). O nome e a senha da rede ficam em
 * wifi_secrets.h, fora do git; se ele não existe, o build o cria a partir de
 * wifi_secrets.example.h. Reconecta sozinho se a rede cair. O nome do gerador na rede (DHCP)
 * é "espgen-xxxx" (fim do MAC).
 */

/*
 * Potência máxima de transmissão, em dBm (padrão do ESP32: 20). Maior que a do AP: o roteador
 * pode estar longe da bancada.
 */
#define WIFI_STA_TX_DBM 15

typedef enum { WIFI_STA_OFF, WIFI_STA_CONNECTING, WIFI_STA_CONNECTED } wifi_sta_state_t;

// Retorna ESP_ERR_INVALID_STATE se a rede não foi configurada em wifi_secrets.h
esp_err_t wifi_sta_start(void);
void wifi_sta_stop(void);
wifi_sta_state_t wifi_sta_state(void);
// Endereço IP recebido ("" se não conectado)
const char *wifi_sta_ip(void);
// Nome da rede configurada ("" se não há)
const char *wifi_sta_ssid(void);
const char *wifi_sta_hostname(void);
