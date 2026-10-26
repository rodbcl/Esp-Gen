#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "auto_gen.h"
#include "tally.h"

/*
 * Modo Automação: liga o AP Wi-Fi e o servidor web e conduz os testes longos configurados
 * pela página. Guarda o perfil na NVS, o estado do teste a cada segundo (state_log), para
 * retomar sozinho após uma queda de energia, e o histórico (history).
 * Funções seguras para chamar da UI e do servidor web.
 */

typedef struct {
    bool active;                // modo Automação ligado (AP no ar)
    bool running;               // teste em andamento
    bool time_ok;               // hora confirmada pelo navegador
    tally_t tally;
    uint32_t elapsed_s;         // tempo de teste, sem contar períodos desligado
    uint32_t started_at;        // epoch do início do teste; 0 = desconhecido
    auto_gen_status_t gen;
} automation_status_t;

// Carrega perfil e último estado. Chamar uma vez no boot, após nvs_flash_init()
void automation_init(void);
// Havia um teste rodando quando o gerador desligou
bool automation_resume_pending(void);

// Entra no modo: liga o AP e retoma o teste pendente, se houver
void automation_enter(void);
// Sai do modo: interrompe o teste em andamento e desliga o AP
void automation_exit(void);

// Inicia um teste com o perfil salvo. Retorna NULL ou a mensagem de erro
const char *automation_start(void);
void automation_stop(void);

void automation_get_profile(auto_profile_t *p);
// Valida e salva o perfil. Retorna NULL ou a mensagem de erro
const char *automation_set_profile(const auto_profile_t *p);

// Hora e fuso (minutos em relação ao UTC) enviados pelo navegador
void automation_set_time(uint32_t epoch, int32_t tz_min);
// Conferência: registra o valor lido no contador e, se `rej_value` não é NULL, os refugos lidos.
// Preenche o que foi enviado fora do stop report. Retorna NULL ou a mensagem de erro
const char *automation_check(uint64_t counter_value, const uint64_t *rej_value, uint64_t *expected,
                             uint64_t *rej_expected);
automation_status_t automation_status(void);
