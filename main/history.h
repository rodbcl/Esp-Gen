#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "tally.h"

/*
 * Histórico dos testes da Automação, gravado em anel na partição "history":
 * um intervalo a cada HISTORY_INTERVAL_S de teste (pulsos, ruídos, tempo parado) e os
 * eventos (início, fim, queda de energia, conferências). Também guarda em RAM os pulsos
 * por minuto da última hora, para o gráfico do Painel.
 *
 * Após uma queda de energia a hora é estimada; quando o navegador confirma a hora,
 * a diferença é gravada e aplicada na leitura aos registros com hora não confirmada.
 * As funções são thread-safe.
 */

#define HISTORY_INTERVAL_S  3600
#define HISTORY_MINUTES     60

esp_err_t history_init(void);

// Ciclo do teste (chamado pela automação)
void history_test_started(void);
void history_test_stopped(const tally_t *t, uint32_t elapsed_s);
// Retomada após queda de energia; a hora atual é a do último estado salvo, que era
// confirmada se `last_time_ok`
void history_power_resumed(const tally_t *t, uint32_t elapsed_s, bool last_time_ok);
// A hora foi confirmada; `delta_s` = hora real - hora estimada
void history_time_fixed(int32_t delta_s);
void history_check(uint64_t value, uint64_t expected);
// Uma vez por segundo enquanto o teste roda
void history_tick(const tally_t *t, uint32_t elapsed_s, bool machine_stopped);

// Pulsos e ruídos por minuto, do mais antigo ao atual (o último é o minuto em andamento)
void history_per_minute(uint32_t valid[HISTORY_MINUTES], uint32_t noise[HISTORY_MINUTES]);

// Saída do teste atual (ou do último), em partes, para o servidor web
typedef void (*history_out_t)(void *ctx, const char *text);
void history_json(history_out_t out, void *ctx);
void history_csv_intervals(history_out_t out, void *ctx);
void history_csv_events(history_out_t out, void *ctx);
