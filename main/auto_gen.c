#include "auto_gen.h"

#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "pulse_gen.h"
#include "tally.h"

#define TPU         ((uint64_t)PULSE_GEN_TICKS_PER_US)  // ticks por us
#define TPS         (TPU * 1000000)                     // ticks por s
#define IDLE_CHUNK  TPS                                 // trechos inativos de no máximo 1 s

// Limites aceitos no perfil
#define RATE_MIN_MHZ    10              // 0,01 Hz
#define RATE_MAX_MHZ    1000000         // 1 kHz
#define LONG_MAX_S      (7 * 86400)     // regime, intervalo e duração de paradas
#define NOISE_EVERY_MAX_MS (86400 * 1000)
#define NOISE_US_MAX    100000          // largura, intervalo na rajada
#define GUARD_US_MAX    10000000
#define BURST_MAX       20

typedef enum { EV_NONE, EV_PULSE, EV_NOISE } event_t;

/*
 * Toda a simulação acontece numa linha do tempo em ticks desde o início (`t` = posição atual
 * da saída). Cada evento (pulso de produção ou rajada de ruído) é escolhido em ordem de tempo
 * e emitido como trecho inativo até o seu início, seguido do trecho ativo.
 */
static struct {
    auto_profile_t p;
    uint32_t rng;
    uint64_t t;
    // Produção
    uint64_t pulse_at;          // início do próximo pulso de produção
    uint64_t last_pulse_end;
    uint32_t base_mhz;          // ritmo base do período atual
    uint64_t regime_at;         // quando sortear um novo ritmo base
    uint32_t rate_mhz;          // ritmo do último pulso
    // Paradas
    uint64_t stop_at;           // próxima parada
    uint64_t stop_end;
    uint64_t cur_stop_from;     // parada em andamento (para o status)
    uint64_t cur_stop_until;
    // Ruído
    uint64_t noise_at;          // próximo evento de ruído
    // Evento em emissão
    event_t ev;
    uint64_t ev_at;
    uint32_t burst_left;
} s;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* ---------- Sorteios (só inteiros: roda em ISR) ---------- */
static uint32_t rnd(void)
{
    uint32_t x = s.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return s.rng = x;
}

// Inteiro uniforme em [lo, hi]
static uint32_t rnd_range(uint32_t lo, uint32_t hi)
{
    if (hi <= lo) {
        return lo;
    }
    return lo + (uint32_t)(((uint64_t)rnd() * ((uint64_t)hi - lo + 1)) >> 32);
}

static uint64_t period_ticks(uint32_t mhz)
{
    return TPS * 1000 / mhz;
}

static uint64_t max_u64(uint64_t a, uint64_t b)
{
    return a > b ? a : b;
}

/* ---------- Produção e paradas ---------- */
// Ritmo de um pulso que começa em `at`
static uint32_t draw_rate(uint64_t at)
{
    const auto_profile_t *p = &s.p;
    if (p->regime_s == 0) {
        return rnd_range(p->rate_min_mhz, p->rate_max_mhz);
    }
    if (at >= s.regime_at) {
        s.base_mhz = rnd_range(p->rate_min_mhz, p->rate_max_mhz);
        s.regime_at = at + p->regime_s * TPS;
    }
    int32_t pct = (int32_t)rnd_range(0, 2 * p->jitter_pct) - (int32_t)p->jitter_pct;
    uint64_t f = (uint64_t)s.base_mhz * (uint32_t)(100 + pct) / 100;
    if (f < p->rate_min_mhz) {
        f = p->rate_min_mhz;
    } else if (f > p->rate_max_mhz) {
        f = p->rate_max_mhz;
    }
    return (uint32_t)f;
}

static void schedule_stop(uint64_t from)
{
    const auto_profile_t *p = &s.p;
    s.stop_at = from + rnd_range(p->stop_every_min_s, p->stop_every_max_s) * TPS;
    s.stop_end = s.stop_at + rnd_range(p->stop_len_min_s, p->stop_len_max_s) * TPS;
}

// Agenda o pulso seguinte ao que começou em `start`, respeitando as paradas
static void after_pulse(uint64_t start)
{
    s.rate_mhz = draw_rate(start);
    uint64_t next = start + period_ticks(s.rate_mhz);
    if (s.p.stops_enabled && next >= s.stop_at) {
        tally_add_stop();
        s.cur_stop_from = s.stop_at;
        s.cur_stop_until = s.stop_end;
        next = max_u64(next, s.stop_end);
        schedule_stop(s.stop_end);
    }
    s.pulse_at = next;
}

/* ---------- Escolha do próximo evento ---------- */
static uint64_t noise_every_ticks(void)
{
    return rnd_range(s.p.noise_every_min_ms, s.p.noise_every_max_ms) * (TPS / 1000);
}

static void commit_event(void)
{
    const auto_profile_t *p = &s.p;
    uint64_t pulse_at = s.pulse_at;
    uint64_t pulse_end = pulse_at + p->pulse_on_us * TPU;

    if (p->noise_enabled) {
        uint64_t guard = p->noise_guard_us * TPU;
        uint64_t n = max_u64(max_u64(s.noise_at, s.t), s.last_pulse_end + guard);
        uint32_t k = rnd_range(p->noise_burst_min, p->noise_burst_max);
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

/* ---------- API ---------- */
void auto_gen_default(auto_profile_t *p)
{
    *p = (auto_profile_t) {
        .rate_min_mhz = 1000,
        .rate_max_mhz = 8000,
        .regime_s = 3600,
        .jitter_pct = 15,
        .pulse_on_us = 50000,
        .stops_enabled = true,
        .stop_every_min_s = 3600,
        .stop_every_max_s = 4 * 3600,
        .stop_len_min_s = 300,
        .stop_len_max_s = 1800,
        .noise_enabled = true,
        .noise_every_min_ms = 5000,
        .noise_every_max_ms = 60000,
        .noise_on_min_us = 50,
        .noise_on_max_us = 500,
        .noise_burst_min = 1,
        .noise_burst_max = 3,
        .noise_gap_us = 200,
        .noise_guard_us = 10000,
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
    if (p->stops_enabled) {
        if (p->stop_every_min_s < 60 || p->stop_len_min_s < 60) {
            return "Paradas: use valores de pelo menos 1 minuto.";
        }
        if (p->stop_every_max_s > LONG_MAX_S || p->stop_len_max_s > LONG_MAX_S) {
            return "Paradas: use valores de até 7 dias.";
        }
        if (p->stop_every_min_s > p->stop_every_max_s) {
            return "Paradas: o intervalo mínimo está maior que o máximo.";
        }
        if (p->stop_len_min_s > p->stop_len_max_s) {
            return "Paradas: a duração mínima está maior que a máxima.";
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
    return NULL;
}

void auto_gen_begin(const auto_profile_t *p, uint32_t seed)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    s = (typeof(s)) { .p = *p, .rng = seed ? seed : 0x9E3779B9, .ev = EV_NONE };
    if (p->stops_enabled) {
        schedule_stop(0);
    }
    if (p->noise_enabled) {
        s.noise_at = noise_every_ticks();
    }
    portEXIT_CRITICAL_SAFE(&s_lock);
}

bool auto_gen_next(void *ctx, uint32_t *level, uint32_t *ticks)
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
        uint32_t w = s.p.pulse_on_us * TPU;
        s.t += w;
        s.last_pulse_end = s.t;
        *level = 1;
        *ticks = w;
        tally_add_valid();
        after_pulse(s.ev_at);
        s.ev = EV_NONE;
    } else {
        uint32_t w = rnd_range(s.p.noise_on_min_us, s.p.noise_on_max_us) * TPU;
        s.t += w;
        *level = 1;
        *ticks = w;
        tally_add_noise();
        if (--s.burst_left > 0) {
            s.ev_at = s.t + s.p.noise_gap_us * TPU;
        } else {
            s.ev = EV_NONE;
            s.noise_at = s.t + noise_every_ticks();
        }
    }
    portEXIT_CRITICAL_SAFE(&s_lock);
    return true;
}

auto_gen_status_t auto_gen_status(void)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    bool stopped = s.p.stops_enabled && s.t >= s.cur_stop_from && s.t < s.cur_stop_until;
    auto_gen_status_t st = {
        .rate_mhz = stopped ? 0 : s.rate_mhz,
        .stopped = stopped,
        .stop_left_s = stopped ? (uint32_t)((s.cur_stop_until - s.t) / TPS) : 0,
    };
    portEXIT_CRITICAL_SAFE(&s_lock);
    return st;
}
