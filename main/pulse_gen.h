#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Gerador de pulsos por hardware (periférico RMT).
 * `invert` = true quando o estágio de saída inverte o sinal do GPIO.
 *
 * pulse_gen_start(): gera exatamente `periods` períodos de `freq_dhz` (décimos de Hz) com `duty_pct` %
 * em nível ativo.
 * Com `periods` = PULSE_GEN_CONTINUOUS a geração só termina com pulse_gen_stop(),
 * e freq/duty podem ser alterados durante a geração com pulse_gen_set().
 *
 * pulse_gen_start_source(): a forma de onda vem de uma fonte externa, trecho a trecho.
 */

#define PULSE_GEN_CONTINUOUS    UINT64_MAX
#define PULSE_GEN_TICKS_PER_US  10      /* resolução: 0,1 us */

/*
 * Fonte da forma de onda: preenche o nível (0 = inativo, 1 = ativo) e a duração em ticks do próximo
 * trecho, e retorna false quando a geração acabou. Trechos de duração 0 são ignorados.
 * É chamada em contexto de ISR: só aritmética inteira, sem bloquear e sem ponto flutuante.
 */
typedef bool (*pulse_gen_source_t)(void *ctx, uint32_t *level, uint32_t *ticks);

esp_err_t pulse_gen_init(int gpio, bool invert);
esp_err_t pulse_gen_start(uint32_t freq_dhz, uint32_t duty_pct, uint64_t periods);
esp_err_t pulse_gen_start_source(pulse_gen_source_t src, void *ctx);
/* Altera freq/duty da geração iniciada por pulse_gen_start(); vale a partir do próximo período */
esp_err_t pulse_gen_set(uint32_t freq_dhz, uint32_t duty_pct);
void pulse_gen_stop(void);
bool pulse_gen_running(void);
