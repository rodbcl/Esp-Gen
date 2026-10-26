#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Rotina automática: simula uma máquina de produção para testes longos de contadores.
 * Gera pulsos de produção com ritmo variável, paradas (running) e ruído entre os pulsos, e soma
 * cada pulso enviado na contabilidade (tally). Cada saída tem a sua fonte para um canal rmt_out:
 *
 *     auto_gen_begin(&perfil, semente, hora_local);
 *     pulse_gen_start_source(auto_gen_next, NULL);        // produção
 *     auto_out_start(&perfil);                            // running, qualidade e stop report
 *
 * Cada parte da simulação (ritmo, paradas, ruído, refugos, stop report) sorteia numa sequência
 * própria a partir da semente, e as paradas são uma função do tempo. Assim as fontes das saídas
 * extras refazem as mesmas paradas e os mesmos pulsos da produção, sem depender uma da outra, e
 * todas seguem a mesma linha do tempo.
 *
 * As regras de validação e de sorteio são as mesmas da prévia na página web (web/index.html).
 */

#define AUTO_SCHED_MAX  8           // horários de parada por dia

// Parada num horário fixo do dia
typedef struct {
    uint32_t start_min;             // minutos desde a meia-noite (hora local)
    uint32_t len_min;
} auto_sched_t;

typedef struct {
    // Pulsos de produção
    uint32_t rate_min_mhz;          // frequência mínima, em mHz (1 Hz = 1000)
    uint32_t rate_max_mhz;          // igual à mínima = frequência fixa
    uint32_t regime_s;              // a cada quantos s sorteia um novo ritmo base; 0 = sorteia a cada pulso
    uint32_t jitter_pct;            // oscilação de cada pulso em torno do ritmo base (± %)
    uint32_t pulse_on_us;           // largura dos pulsos de produção
    // Ruído entre os pulsos de produção
    bool noise_enabled;
    uint32_t noise_every_min_ms;    // intervalo entre eventos de ruído
    uint32_t noise_every_max_ms;
    uint32_t noise_on_min_us;       // largura de cada pulso de ruído
    uint32_t noise_on_max_us;
    uint32_t noise_burst_min;       // pulsos por evento (rajada simula bounce)
    uint32_t noise_burst_max;
    uint32_t noise_gap_us;          // intervalo entre os pulsos de uma rajada
    uint32_t noise_guard_us;        // distância mínima dos pulsos de produção
    /*
     * Running: ativo com a máquina rodando, inativo nas paradas. Desligado, a máquina nunca para.
     * As paradas vêm das regras ativadas, somadas; regras sobrepostas viram uma parada só.
     */
    bool running_enabled;
    bool cycle_enabled;             // ciclo fixo desde o início do teste: roda, para, roda...
    uint32_t cycle_run_s;
    uint32_t cycle_stop_s;
    uint32_t sched_count;           // horários fixos do dia (0 = nenhum)
    auto_sched_t sched[AUTO_SCHED_MAX];
    bool daily_enabled;             // quantidade por dia, em horários sorteados
    uint32_t daily_count;
    uint32_t daily_len_min_s;       // duração de cada uma
    uint32_t daily_len_max_s;
    // Qualidade: pulsos de refugo, cada um depois de um pulso de produção
    bool quality_enabled;
    uint32_t reject_bp;             // chance de refugo por pulso de produção, em centésimos de % (1 % = 100)
    uint32_t reject_delay_ms;       // atraso do pulso de refugo em relação ao de produção
    uint32_t reject_on_us;          // largura dos pulsos de refugo
    // Stop report: paradas justificadas, independentes das paradas da máquina
    bool report_enabled;
    uint32_t report_every_min_s;    // intervalo entre reports
    uint32_t report_every_max_s;
    uint32_t report_len_min_s;      // duração de cada report
    uint32_t report_len_max_s;
} auto_profile_t;

typedef struct {
    uint32_t rate_mhz;              // ritmo do último pulso de produção; 0 durante parada
    bool stopped;                   // máquina parada
    uint32_t stop_left_s;           // quanto falta para a parada acabar
    bool report;                    // stop report ativo
    uint32_t report_left_s;         // quanto falta para o report acabar
} auto_gen_status_t;

void auto_gen_default(auto_profile_t *p);
// Retorna NULL se o perfil é válido, ou a mensagem do problema
const char *auto_gen_validate(const auto_profile_t *p);
// O perfil usa a hora do dia (horários fixos ou quantidade por dia)
bool auto_gen_needs_clock(const auto_profile_t *p);
/*
 * Prepara a simulação; o perfil deve ser válido. `local_epoch` = hora local no início (epoch + fuso),
 * ou 0 se desconhecida: nesse caso as regras que dependem da hora do dia ficam desativadas.
 */
void auto_gen_begin(const auto_profile_t *p, uint32_t seed, uint32_t local_epoch);
// Fontes para rmt_out (produção, running, qualidade e stop report); nunca terminam sozinhas
bool auto_gen_next(void *ctx, uint32_t *level, uint32_t *ticks);
bool auto_gen_next_running(void *ctx, uint32_t *level, uint32_t *ticks);
bool auto_gen_next_quality(void *ctx, uint32_t *level, uint32_t *ticks);
bool auto_gen_next_report(void *ctx, uint32_t *level, uint32_t *ticks);
auto_gen_status_t auto_gen_status(void);
