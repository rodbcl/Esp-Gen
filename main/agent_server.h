#pragma once

#include "esp_err.h"

/*
 * Servidor de comandos do modo Agente (HTTP na porta 80, JSON):
 *
 *   GET  /health   gerador operante: versão, modo, IP, tempo ligado
 *   GET  /status   andamento do programa de pulsos
 *   POST /start    inicia um programa (linha do tempo por saída)
 *   POST /stop     interrompe o programa; todas as saídas em 0 V
 *
 * Erros respondem {"error": "mensagem"} com o código HTTP correspondente.
 */

esp_err_t agent_server_start(void);
void agent_server_stop(void);
