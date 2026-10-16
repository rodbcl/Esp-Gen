#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Encoder rotativo em quadratura (KY-040) lido pelo PCNT, contagem x4. */

void encoder_init(int clk_gpio, int dt_gpio, int counts_per_detent, bool reverse);
/* Retorna o número de "cliques" girados desde a última chamada (com sinal) */
int encoder_read_detents(void);
/* Aceleração: girar rápido aumenta o passo. Retorna 1, 10 ou 100 */
int encoder_accel(int steps, int64_t now_us);
