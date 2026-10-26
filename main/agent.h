#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "wifi_sta.h"

/*
 * Modo Agente de testes: conecta o gerador à rede Wi-Fi (STA) e liga o servidor de comandos
 * (agent_server), pelo qual um sistema externo inicia e acompanha programas de pulsos.
 * Funções seguras para chamar da UI e do servidor.
 */

typedef struct {
    bool active;                // modo Agente ligado
    bool failed;                // o Wi-Fi não ligou (rede não configurada ou erro do driver)
    wifi_sta_state_t wifi;
    char ip[16];                // "" se não conectado
} agent_status_t;

// Entra no modo: conecta à rede e liga o servidor
void agent_enter(void);
// Sai do modo: interrompe o programa em andamento, desliga o servidor e o Wi-Fi
void agent_exit(void);
agent_status_t agent_status(void);
