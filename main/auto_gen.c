#include "auto_gen.h"

#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "esp_attr.h"
#include "pulse_gen.h"
#include "tally.h"

#define TPU         ((uint64_t)PULSE_GEN_TICKS_PER_US)  // ticks por us
#define TPS         (TPU * 1000000)                     // ticks por s
#define TPD         (TPS * 86400)                       // ticks por dia
#define IDLE_CHUNK  TPS                                 // trechos inativos de no máximo 1 s
#define NEVER       UINT64_MAX

// Limites aceitos no perfil
#define RATE_MIN_MHZ    10              // 0,01 Hz
#define RATE_MAX_MHZ    1000000         // 1 kHz
#define LONG_MAX_S      (7 * 86400)     // regime, intervalo e duração de paradas
#define NOISE_EVERY_MAX_MS (86400 * 1000)
#define NOISE_US_MAX    100000          // largura, intervalo na rajada
#define GUARD_US_MAX    10000000
#define BURST_MAX       20
#define REJECT_BP_MAX   10000           // 100 %
#define REJECT_DELAY_MAX_MS 60000
#define DAILY_MAX       48              // paradas sorteadas por dia
#define SCHED_LEN_MAX_MIN (12 * 60)
#define MERGE_MAX       32              // regras sobrepostas juntadas numa parada só

// Sequências de sorteio, uma para cada parte da simulação
enum { RNG_RATE = 1, RNG_DAILY, RNG_NOISE, RNG_REPORT, RNG_REJECT };

typedef enum { EV_NONE, EV_PULSE, EV_NOISE } event_t;

// Intervalo [from, until) na linha do tempo; NEVER = nenhum
typedef struct {
    uint64_t from;
    uint64_t until;
} span_t;

// Janelas [from, until) sorteadas em sequência (stop report)
typedef struct {
    uint32_t rng;
    uint32_t every_min_s, every_max_s;
    uint32_t len_min_s, len_max_s;
    uint64_t from;                  // janela atual ou a próxima; NEVER = nenhuma
    uint64_t until;
} windows_t;

// Sequência dos pulsos de produção (ritmo e paradas)
typedef struct {
    uint32_t rng;
    span_t stops;                   // parada atual ou a próxima
    uint64_t at;                    // início do próximo pulso
    uint32_t base_mhz;              // ritmo base do período atual
    uint64_t regime_at;             // quando sortear um novo ritmo base
    uint32_t rate_mhz;              // ritmo do último pulso
    uint64_t stop_from;             // última parada (para o status)
    uint64_t stop_until;
} prod_t;

// Saída de nível que segue uma sequência de janelas (stop report)
typedef struct {
    windows_t w;
    uint64_t t;
    uint64_t counted;               // início da última janela contabilizada
} level_out_t;

static auto_profile_t s_p;
static uint32_t s_on_ticks;         // largura dos pulsos de produção
static uint32_t s_seed;
static bool s_clock;                // hora do dia conhecida
static uint64_t s_local0;           // hora local no início, em ticks desde a meia-noite de 01/01/1970

/*
 * Produção: toda a simulação acontece numa linha do tempo em ticks desde o início (`t` = posição
 * atual da saída). Cada evento (pulso de produção ou rajada de ruído) é escolhido em ordem de tempo
 * e emitido como trecho inativo até o seu início, seguido do trecho ativo.
 */
static struct {
    prod_t prod;
    windows_t report;               // separa o que é enviado com o stop report ativo
    uint32_t rng;                   // ruído
    uint64_t t;
    uint64_t last_pulse_end;
    uint64_t noise_at;              // próximo evento de ruído
    // Evento em emissão
    event_t ev;
    uint64_t ev_at;
    uint32_t burst_left;
} s;

// Qualidade: refaz a sequência de pulsos de produção e sorteia quais viram refugo
static struct {
    prod_t prod;
    windows_t report;
    uint32_t rng;
    uint64_t t;
    uint64_t at;                    // pulso de refugo candidato: pulso de produção + atraso
    bool reject;
} s_q;

// Running: inativo durante as paradas
static struct {
    span_t stops;
    uint64_t t;
} s_run;
static level_out_t s_rep;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* ---------- Sorteios (só inteiros: roda em ISR) ---------- */
static uint32_t IRAM_ATTR rnd(uint32_t *rng)
{
    uint32_t x = *rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *rng = x;
}

// Inteiro uniforme em [lo, hi]
static uint32_t IRAM_ATTR rnd_range(uint32_t *rng, uint32_t lo, uint32_t hi)
{
    if (hi <= lo) {
        return lo;
    }
    return lo + (uint32_t)(((uint64_t)rnd(rng) * ((uint64_t)hi - lo + 1)) >> 32);
}

// Semente da sequência `k`, derivada da semente do teste (nunca 0)
static uint32_t IRAM_ATTR stream_seed(uint32_t seed, uint32_t k)
{
    uint32_t x = seed + k * 0x9E3779B9;
    x ^= x >> 16;
    x *= 0x85EBCA6B;
    x ^= x >> 13;
    x *= 0xC2B2AE35;
    x ^= x >> 16;
    return x ? x : k;
}

static uint64_t IRAM_ATTR period_ticks(uint32_t mhz)
{
    return TPS * 1000 / mhz;
}

static uint64_t IRAM_ATTR max_u64(uint64_t a, uint64_t b)
{
    return a > b ? a : b;
}

/* ---------- Janelas ---------- */
static void IRAM_ATTR win_schedule(windows_t *w, uint64_t after)
{
    w->from = after + rnd_range(&w->rng, w->every_min_s, w->every_max_s) * TPS;
    w->until = w->from + rnd_range(&w->rng, w->len_min_s, w->len_max_s) * TPS;
}

static void win_init(windows_t *w, bool enabled, uint32_t seed, uint32_t every_min_s, uint32_t every_max_s,
                     uint32_t len_min_s, uint32_t len_max_s)
{
    *w = (windows_t) {
        .rng = seed, .every_min_s = every_min_s, .every_max_s = every_max_s,
        .len_min_s = len_min_s, .len_max_s = len_max_s, .from = NEVER, .until = NEVER,
    };
    if (enabled) {
        win_schedule(w, 0);
    }
}

// Avança até a janela que contém `t` ou a próxima depois dele
static void IRAM_ATTR win_seek(windows_t *w, uint64_t t)
{
    while (t >= w->until) {
        win_schedule(w, w->until);
    }
}

static bool IRAM_ATTR win_inside(windows_t *w, uint64_t t)
{
    win_seek(w, t);
    return t >= w->from;
}

/* ---------- Paradas ----------
 * Cada regra do running dá, para um instante `t`, a sua parada que termina depois de `t` e começa
 * mais cedo. As paradas são uma função do tempo (e da semente): quem precisa delas calcula sozinho.
 */
// Guarda em `best` o intervalo [from, until) se ele termina depois de `t` e começa antes de `best`
static void IRAM_ATTR consider(span_t *best, int64_t from, int64_t until, uint64_t t)
{
    if (until <= (int64_t)t) {
        return;
    }
    uint64_t f = from < 0 ? 0 : (uint64_t)from;
    if (f < best->from) {
        *best = (span_t) { f, (uint64_t)until };
    }
}

static void IRAM_ATTR rule_cycle(span_t *best, uint64_t t)
{
    uint64_t run = s_p.cycle_run_s * TPS, period = run + s_p.cycle_stop_s * TPS;
    uint64_t k = t / period;
    consider(best, (int64_t)(k * period + run), (int64_t)((k + 1) * period), t);
}

// Horários do dia e quantidade por dia, no dia de `t` e nos vizinhos (paradas que passam da meia-noite)
static void IRAM_ATTR rule_days(span_t *best, uint64_t t)
{
    int64_t local0 = (int64_t)s_local0;
    uint64_t day = (s_local0 + t) / TPD;
    for (uint64_t d = day ? day - 1 : 0; d <= day + 1; d++) {
        int64_t midnight = (int64_t)(d * TPD) - local0;
        for (uint32_t i = 0; i < s_p.sched_count; i++) {
            int64_t from = midnight + (int64_t)(s_p.sched[i].start_min * 60 * TPS);
            consider(best, from, from + (int64_t)(s_p.sched[i].len_min * 60 * TPS), t);
        }
        if (s_p.daily_enabled) {
            uint32_t rng = stream_seed(s_seed + (uint32_t)d * 0x632BE5AB, RNG_DAILY);
            for (uint32_t i = 0; i < s_p.daily_count; i++) {
                int64_t from = midnight + (int64_t)(rnd_range(&rng, 0, 86399) * TPS);
                consider(best, from, from + (int64_t)(rnd_range(&rng, s_p.daily_len_min_s, s_p.daily_len_max_s) * TPS), t);
            }
        }
    }
}

// Parada de qualquer regra que termina depois de `t` e começa mais cedo
static span_t IRAM_ATTR rules_next(uint64_t t)
{
    span_t best = { NEVER, NEVER };
    if (!s_p.running_enabled) {
        return best;
    }
    if (s_p.cycle_enabled) {
        rule_cycle(&best, t);
    }
    if (s_clock && (s_p.sched_count || s_p.daily_enabled)) {
        rule_days(&best, t);
    }
    return best;
}

// Parada que contém `t` ou a próxima depois dele, já juntando as regras sobrepostas
static span_t IRAM_ATTR stop_window(uint64_t t)
{
    span_t w = rules_next(t);
    for (int i = 0; i < MERGE_MAX && w.until != NEVER; i++) {
        span_t n = rules_next(w.until);
        if (n.from > w.until) {
            break;
        }
        w.until = n.until;
    }
    return w;
}

/* ---------- Produção ---------- */
/*
 * Um pulso em `*at` que não terminaria antes da próxima parada fica para o fim dela, então
 * nenhum pulso sai com a máquina parada. Retorna quantas paradas o pulso atravessou.
 */
static uint32_t IRAM_ATTR skip_stops(prod_t *q, uint64_t *at)
{
    uint32_t stops = 0;
    while (*at + s_on_ticks > q->stops.from) {
        *at = max_u64(*at, q->stops.until);
        q->stop_from = q->stops.from;
        q->stop_until = q->stops.until;
        q->stops = stop_window(q->stops.until);
        stops++;
    }
    return stops;
}

// Retorna quantas paradas há antes do primeiro pulso (teste iniciado durante uma parada)
static uint32_t prod_init(prod_t *q, uint32_t seed)
{
    *q = (prod_t) { .rng = stream_seed(seed, RNG_RATE), .stops = stop_window(0) };
    return skip_stops(q, &q->at);
}

// Ritmo de um pulso que começa em `at`
static uint32_t IRAM_ATTR draw_rate(prod_t *q, uint64_t at)
{
    const auto_profile_t *p = &s_p;
    if (p->regime_s == 0) {
        return rnd_range(&q->rng, p->rate_min_mhz, p->rate_max_mhz);
    }
    if (at >= q->regime_at) {
        q->base_mhz = rnd_range(&q->rng, p->rate_min_mhz, p->rate_max_mhz);
        q->regime_at = at + p->regime_s * TPS;
    }
    int32_t pct = (int32_t)rnd_range(&q->rng, 0, 2 * p->jitter_pct) - (int32_t)p->jitter_pct;
    uint64_t f = (uint64_t)q->base_mhz * (uint32_t)(100 + pct) / 100;
    if (f < p->rate_min_mhz) {
        f = p->rate_min_mhz;
    } else if (f > p->rate_max_mhz) {
        f = p->rate_max_mhz;
    }
    return (uint32_t)f;
}

// Agenda o pulso seguinte ao que começa em `q->at`. Retorna quantas paradas começaram entre os dois
static uint32_t IRAM_ATTR prod_advance(prod_t *q)
{
    q->rate_mhz = draw_rate(q, q->at);
    uint64_t next = q->at + period_ticks(q->rate_mhz);
    uint32_t stops = skip_stops(q, &next);
    q->at = next;
    return stops;
}

/* ---------- Escolha do próximo evento ---------- */
static uint64_t IRAM_ATTR noise_every_ticks(void)
{
    return rnd_range(&s.rng, s_p.noise_every_min_ms, s_p.noise_every_max_ms) * (TPS / 1000);
}

static void IRAM_ATTR commit_event(void)
{
    const auto_profile_t *p = &s_p;
    uint64_t pulse_at = s.prod.at;
    uint64_t pulse_end = pulse_at + s_on_ticks;

    if (p->noise_enabled) {
        uint64_t guard = p->noise_guard_us * TPU;
        uint64_t n = max_u64(max_u64(s.noise_at, s.t), s.last_pulse_end + guard);
        uint32_t k = rnd_range(&s.rng, p->noise_burst_min, p->noise_burst_max);
        uint64_t span = (k * (uint64_t)p->noise_on_max_us + (k - 1) * (uint64_t)p->noise_gap_us) * TPU;
        if (n < pulse_at) {
            if (n + span + guard <= pulse_at) {
                s.ev = EV_NOISE;
                s.ev_at = n;
                s.burst_left = k;
                return;
            }
            // Não cabe antes do pulso: fica para depois dele
            s.noise_at = pulse_end + guard;
        }
    }
    s.ev = EV_PULSE;
    s.ev_at = pulse_at;
}

/* ---------- Saídas de nível ---------- */
static void level_init(level_out_t *o, bool enabled, uint32_t seed, uint32_t every_min_s, uint32_t every_max_s,
                       uint32_t len_min_s, uint32_t len_max_s)
{
    *o = (level_out_t) { .counted = NEVER };
    win_init(&o->w, enabled, seed, every_min_s, every_max_s, len_min_s, len_max_s);
}

// Próximo trecho: `inside` dentro das janelas, o oposto fora. Retorna true ao entrar numa janela nova
static bool IRAM_ATTR level_next(level_out_t *o, uint32_t inside, uint32_t *level, uint32_t *ticks)
{
    bool entered = false;
    uint64_t end;
    if (win_inside(&o->w, o->t)) {
        *level = inside;
        end = o->w.until;
        entered = o->counted != o->w.from;
        o->counted = o->w.from;
    } else {
        *level = !inside;
        end = o->w.from;
    }
    uint64_t d = end - o->t;
    if (d > IDLE_CHUNK) {
        d = IDLE_CHUNK;
    }
    o->t += d;
    *ticks = (uint32_t)d;
    return entered;
}

/* ---------- API ---------- */
void auto_gen_default(auto_profile_t *p)
{
    *p = (auto_profile_t) {
        .rate_min_mhz = 1000,
        .rate_max_mhz = 8000,
        .regime_s = 3600,
        .jitter_pct = 15,
        .pulse_on_us = 50000,
        .noise_enabled = true,
        .noise_every_min_ms = 5000,
        .noise_every_max_ms = 60000,
        .noise_on_min_us = 50,
        .noise_on_max_us = 500,
        .noise_burst_min = 1,
        .noise_burst_max = 3,
        .noise_gap_us = 200,
        .noise_guard_us = 10000,
        .running_enabled = true,
        .cycle_enabled = false,
        .cycle_run_s = 50 * 60,
        .cycle_stop_s = 10 * 60,
        .sched_count = 1,
        .sched = { { .start_min = 12 * 60, .len_min = 60 } },
        .daily_enabled = true,
        .daily_count = 4,
        .daily_len_min_s = 300,
        .daily_len_max_s = 1800,
        .quality_enabled = true,
        .reject_bp = 200,
        .reject_delay_ms = 500,
        .reject_on_us = 50000,
        .report_enabled = true,
        .report_every_min_s = 4 * 3600,
        .report_every_max_s = 8 * 3600,
        .report_len_min_s = 600,
        .report_len_max_s = 1800,
    };
}

const char *auto_gen_validate(const auto_profile_t *p)
{
    if (p->rate_min_mhz < RATE_MIN_MHZ || p->rate_max_mhz > RATE_MAX_MHZ) {
        return "A frequência deve ficar entre 0,01 e 1000 Hz.";
    }
    if (p->rate_min_mhz > p->rate_max_mhz) {
        return "A frequência mínima está maior que a máxima.";
    }
    if (p->pulse_on_us < 10) {
        return "A largura do pulso deve ser de pelo menos 0,01 ms.";
    }
    uint64_t period_min_us = 1000000000ULL / p->rate_max_mhz;
    if (p->pulse_on_us >= period_min_us) {
        return "A largura do pulso precisa ser menor que o período na frequência máxima.";
    }
    if (p->regime_s > LONG_MAX_S || p->jitter_pct > 50) {
        return "A oscilação entre pulsos vai de 0 a 50 %.";
    }
    if (p->running_enabled) {
        if (p->cycle_enabled) {
            if (p->cycle_run_s < 60 || p->cycle_stop_s < 60) {
                return "Ciclo fixo: use tempos de pelo menos 1 minuto.";
            }
            if (p->cycle_run_s > LONG_MAX_S || p->cycle_stop_s > LONG_MAX_S) {
                return "Ciclo fixo: use tempos de até 7 dias.";
            }
        }
        if (p->sched_count > AUTO_SCHED_MAX) {
            return "Horários do dia: no máximo 8.";
        }
        for (uint32_t i = 0; i < p->sched_count; i++) {
            if (p->sched[i].start_min >= 24 * 60) {
                return "Horários do dia: confira os horários.";
            }
            if (p->sched[i].len_min < 1 || p->sched[i].len_min > SCHED_LEN_MAX_MIN) {
                return "Horários do dia: cada parada dura de 1 min a 12 h.";
            }
        }
        if (p->daily_enabled) {
            if (p->daily_count < 1 || p->daily_count > DAILY_MAX) {
                return "Paradas por dia: de 1 a 48.";
            }
            if (p->daily_len_min_s < 60 || p->daily_len_max_s > SCHED_LEN_MAX_MIN * 60) {
                return "Paradas por dia: cada uma dura de 1 min a 12 h.";
            }
            if (p->daily_len_min_s > p->daily_len_max_s) {
                return "Paradas por dia: a duração mínima está maior que a máxima.";
            }
        }
    }
    if (p->noise_enabled) {
        if (p->noise_every_min_ms < 10 || p->noise_every_max_ms > NOISE_EVERY_MAX_MS) {
            return "Ruído: o intervalo vai de 0,01 s a 1 dia.";
        }
        if (p->noise_every_min_ms > p->noise_every_max_ms) {
            return "Ruído: o intervalo mínimo está maior que o máximo.";
        }
        if (p->noise_on_min_us < 1 || p->noise_on_min_us > p->noise_on_max_us || p->noise_on_max_us > NOISE_US_MAX ||
            p->noise_gap_us > NOISE_US_MAX || p->noise_guard_us > GUARD_US_MAX) {
            return "Ruído: confira a largura mínima e a máxima.";
        }
        if (p->noise_burst_min < 1 || p->noise_burst_min > p->noise_burst_max || p->noise_burst_max > BURST_MAX) {
            return "Ruído: pulsos por rajada vão de 1 a 20, com o mínimo menor ou igual ao máximo.";
        }
        uint64_t span = (uint64_t)p->noise_burst_max * p->noise_on_max_us + (p->noise_burst_max - 1) * (uint64_t)p->noise_gap_us;
        if (p->pulse_on_us + 2 * (uint64_t)p->noise_guard_us + span >= period_min_us) {
            return "O ruído não cabe entre os pulsos na frequência máxima. Diminua a largura do pulso, a rajada ou a distância dos pulsos.";
        }
    }
    if (p->quality_enabled) {
        if (p->reject_bp < 1 || p->reject_bp > REJECT_BP_MAX) {
            return "Qualidade: a taxa de refugo vai de 0,01 a 100 %.";
        }
        if (p->reject_delay_ms > REJECT_DELAY_MAX_MS) {
            return "Qualidade: o atraso do refugo vai de 0 a 60 s.";
        }
        if (p->reject_on_us < 10) {
            return "Qualidade: a largura do pulso de refugo deve ser de pelo menos 0,01 ms.";
        }
        if (p->reject_on_us >= period_min_us) {
            return "Qualidade: a largura do pulso de refugo precisa ser menor que o período na frequência máxima.";
        }
    }
    if (p->report_enabled) {
        if (p->report_every_min_s < 60 || p->report_len_min_s < 60) {
            return "Stop report: use valores de pelo menos 1 minuto.";
        }
        if (p->report_every_max_s > LONG_MAX_S || p->report_len_max_s > LONG_MAX_S) {
            return "Stop report: use valores de até 7 dias.";
        }
        if (p->report_every_min_s > p->report_every_max_s) {
            return "Stop report: o intervalo mínimo está maior que o máximo.";
        }
        if (p->report_len_min_s > p->report_len_max_s) {
            return "Stop report: a duração mínima está maior que a máxima.";
        }
    }
    return NULL;
}

bool auto_gen_needs_clock(const auto_profile_t *p)
{
    return p->running_enabled && (p->sched_count || p->daily_enabled);
}

void auto_gen_begin(const auto_profile_t *p, uint32_t seed, uint32_t local_epoch)
{
    seed = seed ? seed : 0x9E3779B9;
    portENTER_CRITICAL_SAFE(&s_lock);
    s_p = *p;
    s_on_ticks = p->pulse_on_us * TPU;
    s_seed = seed;
    s_clock = local_epoch != 0;
    s_local0 = local_epoch * TPS;
    uint32_t rep_seed = stream_seed(seed, RNG_REPORT);

    s = (typeof(s)) { .rng = stream_seed(seed, RNG_NOISE), .ev = EV_NONE };
    for (uint32_t n = prod_init(&s.prod, seed); n > 0; n--) {
        tally_add_stop();
    }
    win_init(&s.report, p->report_enabled, rep_seed, p->report_every_min_s, p->report_every_max_s,
             p->report_len_min_s, p->report_len_max_s);
    if (p->noise_enabled) {
        s.noise_at = noise_every_ticks();
    }

    s_q = (typeof(s_q)) { .rng = stream_seed(seed, RNG_REJECT) };
    prod_init(&s_q.prod, seed);
    win_init(&s_q.report, p->report_enabled, rep_seed, p->report_every_min_s, p->report_every_max_s,
             p->report_len_min_s, p->report_len_max_s);
    s_q.at = s_q.prod.at + p->reject_delay_ms * (TPS / 1000);
    s_q.reject = rnd_range(&s_q.rng, 1, REJECT_BP_MAX) <= p->reject_bp;

    s_run.t = 0;
    s_run.stops = stop_window(0);
    level_init(&s_rep, p->report_enabled, rep_seed, p->report_every_min_s, p->report_every_max_s,
               p->report_len_min_s, p->report_len_max_s);
    portEXIT_CRITICAL_SAFE(&s_lock);
}

bool IRAM_ATTR auto_gen_next(void *ctx, uint32_t *level, uint32_t *ticks)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    if (s.ev == EV_NONE) {
        commit_event();
    }
    if (s.t < s.ev_at) {
        // Inativo até o início do evento
        uint64_t d = s.ev_at - s.t;
        if (d > IDLE_CHUNK) {
            d = IDLE_CHUNK;
        }
        s.t += d;
        *level = 0;
        *ticks = (uint32_t)d;
    } else if (s.ev == EV_PULSE) {
        s.t += s_on_ticks;
        s.last_pulse_end = s.t;
        *level = 1;
        *ticks = s_on_ticks;
        tally_add_valid(win_inside(&s.report, s.ev_at));
        for (uint32_t n = prod_advance(&s.prod); n > 0; n--) {
            tally_add_stop();
        }
        s.ev = EV_NONE;
    } else {
        uint32_t w = rnd_range(&s.rng, s_p.noise_on_min_us, s_p.noise_on_max_us) * TPU;
        s.t += w;
        *level = 1;
        *ticks = w;
        tally_add_noise();
        if (--s.burst_left > 0) {
            s.ev_at = s.t + s_p.noise_gap_us * TPU;
        } else {
            s.ev = EV_NONE;
            s.noise_at = s.t + noise_every_ticks();
        }
    }
    portEXIT_CRITICAL_SAFE(&s_lock);
    return true;
}

bool IRAM_ATTR auto_gen_next_running(void *ctx, uint32_t *level, uint32_t *ticks)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    // Mesmas paradas da produção: inativo dentro delas
    if (s_run.t >= s_run.stops.until) {
        s_run.stops = stop_window(s_run.t);
    }
    uint64_t end;
    if (s_run.t >= s_run.stops.from) {
        *level = 0;
        end = s_run.stops.until;
    } else {
        *level = 1;
        end = s_run.stops.from;
    }
    uint64_t d = end - s_run.t;
    if (d > IDLE_CHUNK) {
        d = IDLE_CHUNK;
    }
    s_run.t += d;
    *ticks = (uint32_t)d;
    portEXIT_CRITICAL_SAFE(&s_lock);
    return true;
}

bool IRAM_ATTR auto_gen_next_quality(void *ctx, uint32_t *level, uint32_t *ticks)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    if (s_q.t < s_q.at) {
        uint64_t d = s_q.at - s_q.t;
        if (d > IDLE_CHUNK) {
            d = IDLE_CHUNK;
        }
        s_q.t += d;
        *level = 0;
        *ticks = (uint32_t)d;
    } else {
        if (s_q.reject) {
            tally_add_reject(win_inside(&s_q.report, s_q.t));
            *level = 1;
            *ticks = s_p.reject_on_us * TPU;
            s_q.t += *ticks;
        } else {
            *level = 0;
            *ticks = 0;     // ignorado: o motor pede o próximo trecho
        }
        // Próximo pulso de produção e se ele vira refugo
        prod_advance(&s_q.prod);
        s_q.at = s_q.prod.at + s_p.reject_delay_ms * (TPS / 1000);
        s_q.reject = rnd_range(&s_q.rng, 1, REJECT_BP_MAX) <= s_p.reject_bp;
    }
    portEXIT_CRITICAL_SAFE(&s_lock);
    return true;
}

bool IRAM_ATTR auto_gen_next_report(void *ctx, uint32_t *level, uint32_t *ticks)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    if (level_next(&s_rep, 1, level, ticks)) {
        tally_add_report();
    }
    portEXIT_CRITICAL_SAFE(&s_lock);
    return true;
}

auto_gen_status_t auto_gen_status(void)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    bool stopped = s.t >= s.prod.stop_from && s.t < s.prod.stop_until;
    bool report = s_rep.t >= s_rep.w.from && s_rep.t < s_rep.w.until;
    auto_gen_status_t st = {
        .rate_mhz = stopped ? 0 : s.prod.rate_mhz,
        .stopped = stopped,
        .stop_left_s = stopped ? (uint32_t)((s.prod.stop_until - s.t) / TPS) : 0,
        .report = report,
        .report_left_s = report ? (uint32_t)((s_rep.w.until - s_rep.t) / TPS) : 0,
    };
    portEXIT_CRITICAL_SAFE(&s_lock);
    return st;
}
