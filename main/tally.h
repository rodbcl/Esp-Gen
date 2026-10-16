#pragma once

#include <stdint.h>

/*
 * Contabilidade da rotina automática: o que foi enviado ao contador sob teste.
 * As funções tally_add_*() podem ser chamadas de ISR.
 */

typedef struct {
    uint64_t valid;     // pulsos de produção (o que o contador deve mostrar)
    uint64_t noise;     // pulsos de ruído injetados (o contador não deve contar)
    uint32_t stops;     // paradas da máquina simulada
} tally_t;

// Zera os contadores, ou parte de `from` (retomada após queda de energia)
void tally_reset(const tally_t *from);
tally_t tally_get(void);

void tally_add_valid(void);
void tally_add_noise(void);
void tally_add_stop(void);
