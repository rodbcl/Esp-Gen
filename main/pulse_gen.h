#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "rmt_out.h"

/*
 * Gerador de pulsos da saída principal (um canal rmt_out).
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
#define PULSE_GEN_TICKS_PER_US  RMT_OUT_TICKS_PER_US

/* Fonte da forma de onda (ver rmt_out_source_t) */
typedef rmt_out_source_t pulse_gen_source_t;

esp_err_t pulse_gen_init(int gpio, bool invert);
esp_err_t pulse_gen_start(uint32_t freq_dhz, uint32_t duty_pct, uint64_t periods);
esp_err_t pulse_gen_start_source(pulse_gen_source_t src, void *ctx);
/* Altera freq/duty da geração iniciada por pulse_gen_start(); vale a partir do próximo período */
esp_err_t pulse_gen_set(uint32_t freq_dhz, uint32_t duty_pct);
void pulse_gen_stop(void);
bool pulse_gen_running(void);
