#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "settings.h"

/* Execução da geração conforme as configurações (modos Tempo, Qtde e Contínuo). */

/* Inicia a geração. Retorna NULL se iniciou, ou a mensagem do erro */
const char *run_start(void);
/* Modo contínuo: ajusta freq/duty com a geração em andamento */
void run_adjust(settings_field_t field, int steps, int accel);
/* Encerra a geração; no modo contínuo salva freq/duty se foram alterados */
void run_stop(void);
bool run_finished(void);
int64_t run_elapsed_us(void);
/* Períodos pedidos (PULSE_GEN_CONTINUOUS no modo contínuo) */
uint64_t run_periods(void);
