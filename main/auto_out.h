#pragma once

#include "esp_err.h"
#include "auto_gen.h"

/*
 * Saídas extras do modo Automação, cada uma num canal rmt_out (pinos em board.h):
 *   running      24 V com a máquina rodando, 0 V nas paradas
 *   qualidade    pulsos de refugo
 *   stop report  24 V durante as paradas justificadas
 * Só as ativadas no perfil são geradas; as demais ficam em 0 V.
 */

// Configura os canais com as saídas em 0 V. Chamar no boot, o quanto antes
esp_err_t auto_out_init(void);
// Inicia as saídas ativadas no perfil; chamar logo após auto_gen_begin() e o início da produção
esp_err_t auto_out_start(const auto_profile_t *p);
void auto_out_stop(void);
