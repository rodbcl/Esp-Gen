#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Gerador de pulsos por hardware (periférico RMT).
 * Gera exatamente `periods` períodos de `freq_hz` com `duty_pct` % em nível ativo.
 * `invert` = true quando o estágio de saída inverte o sinal do GPIO.
 */

esp_err_t pulse_gen_init(int gpio, bool invert);
esp_err_t pulse_gen_start(uint32_t freq_hz, uint32_t duty_pct, uint64_t periods);
void pulse_gen_stop(void);
bool pulse_gen_running(void);
