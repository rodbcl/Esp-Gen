#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Saída de forma de onda por um canal RMT, em streaming: cada trecho (nível, duração) vem de uma
 * fonte e vira símbolos sob demanda, enquanto a transmissão acontece. Cada canal é independente;
 * usado pelo pulse_gen (saída de pulsos) e pelas saídas extras da Automação (auto_out).
 * `invert` = true quando o estágio de saída inverte o sinal do GPIO.
 */

#define RMT_OUT_TICKS_PER_US    10      /* resolução: 0,1 us */
#define RMT_OUT_MAX             4       /* canais disponíveis */

/*
 * Fonte da forma de onda: preenche o nível (0 = inativo, 1 = ativo) e a duração em ticks do próximo
 * trecho, e retorna false quando a geração acabou. Trechos de duração 0 são ignorados.
 * É chamada em contexto de ISR: só aritmética inteira, sem bloquear e sem ponto flutuante.
 */
typedef bool (*rmt_out_source_t)(void *ctx, uint32_t *level, uint32_t *ticks);

typedef struct rmt_out rmt_out_t;

// Configura o canal; a saída fica no nível inativo
esp_err_t rmt_out_new(int gpio, bool invert, rmt_out_t **out);
esp_err_t rmt_out_start(rmt_out_t *o, rmt_out_source_t src, void *ctx);
// Interrompe a geração; a saída volta ao nível inativo em poucos ms
void rmt_out_stop(rmt_out_t *o);
bool rmt_out_running(const rmt_out_t *o);
