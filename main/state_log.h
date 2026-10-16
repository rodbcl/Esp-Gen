#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Registro do estado da rotina automática numa partição própria ("autostate"), em anel.
 * Cada gravação ocupa um registro novo; o mais recente é encontrado ao ligar.
 * Serve para retomar o teste após uma queda de energia.
 */

typedef struct {
    bool running;           // havia um teste rodando
    bool time_ok;           // a hora `epoch` era confirmada
    uint16_t stops;
    uint64_t valid;
    uint32_t noise;
    uint32_t elapsed_s;     // tempo de teste acumulado (sem contar períodos desligado)
    uint32_t epoch;         // hora em que o registro foi gravado (0 = desconhecida)
} state_rec_t;

esp_err_t state_log_init(void);
// Retorna false se não há nenhum registro válido
bool state_log_last(state_rec_t *rec);
esp_err_t state_log_write(const state_rec_t *rec);
