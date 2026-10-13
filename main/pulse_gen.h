#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Gerador de pulsos por hardware (periférico RMT).
 * Gera exatamente `periods` períodos de `freq_hz` com `duty_pct` % em nível ativo.
 * `invert` = true quando o estágio de saída inverte o sinal do GPIO.
 * Com `periods` = PULSE_GEN_CONTINUOUS a geração só termina com pulse_gen_stop(),
 * e freq/duty podem ser alterados durante a geração com pulse_gen_set().
 */

#define PULSE_GEN_CONTINUOUS UINT64_MAX

esp_err_t pulse_gen_init(int gpio, bool invert);
esp_err_t pulse_gen_start(uint32_t freq_hz, uint32_t duty_pct, uint64_t periods);
/* Altera freq/duty da geração em andamento; vale a partir do próximo período */
esp_err_t pulse_gen_set(uint32_t freq_hz, uint32_t duty_pct);
void pulse_gen_stop(void);
bool pulse_gen_running(void);
