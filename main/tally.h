#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Contabilidade da rotina automática: o que foi enviado ao contador sob teste.
 * Com o stop report ativo o sistema ignora os demais sinais, então os pulsos enviados nesse
 * período são somados à parte (`*_rep`) e não entram no que o contador deve mostrar.
 * As funções tally_add_*() podem ser chamadas de ISR.
 */

typedef struct {
    uint64_t valid;         // pulsos de produção fora do stop report (o que o contador deve mostrar)
    uint64_t noise;         // pulsos de ruído injetados (o contador não deve contar)
    uint32_t stops;         // paradas da máquina simulada
    uint64_t rejects;       // pulsos de qualidade (refugos) fora do stop report
    uint64_t valid_rep;     // pulsos de produção com o stop report ativo
    uint64_t rejects_rep;   // refugos com o stop report ativo
    uint32_t reports;       // stop reports
} tally_t;

// Zera os contadores, ou parte de `from` (retomada após queda de energia)
void tally_reset(const tally_t *from);
tally_t tally_get(void);

// `reported` = enviado com o stop report ativo
void tally_add_valid(bool reported);
void tally_add_reject(bool reported);
void tally_add_noise(void);
void tally_add_stop(void);
void tally_add_report(void);
