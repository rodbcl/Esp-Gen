#pragma once

#include "esp_err.h"

/*
 * Servidor HTTP do modo Automação: serve a página (web/index.html, embutida comprimida)
 * e a API JSON usada por ela.
 *
 *   GET  /                  página
 *   GET  /api/status        andamento do teste
 *   GET  /api/profile       perfil salvo          POST /api/profile  valida e salva
 *   POST /api/start         inicia o teste        POST /api/stop     interrompe
 *   POST /api/time          {"epoch", "tz"} hora e fuso do navegador
 *   POST /api/check         {"value", "rejects"?} conferência com o display do contador
 *   GET  /api/history       intervalos e eventos do teste
 *   GET  /api/history.csv   intervalos            GET  /api/events.csv  eventos
 */

esp_err_t web_server_start(void);
void web_server_stop(void);
