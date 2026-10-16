#include "history.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "ringlog.h"
#include "wallclock.h"

static const char *TAG = "history";

typedef enum { REC_START = 1, REC_STOP, REC_POWER, REC_CHECK, REC_TIMEFIX, REC_INTERVAL } rec_type_t;

#define FLAG_TIME_OK    0x01
#define MAX_FIXES       16

// Dados de cada registro do anel
typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t flags;
    uint16_t rsv;
    uint32_t epoch;         // hora do evento ou início do intervalo (0 = desconhecida)
    union {
        struct __attribute__((packed)) {
            uint32_t valid;
            uint32_t noise;
            uint16_t stop_s;
            uint16_t stops;
            uint32_t dur_s;
        } iv;               // REC_INTERVAL
        struct __attribute__((packed)) {
            uint64_t value;
            uint64_t expected;
        } check;            // REC_CHECK
        struct __attribute__((packed)) {
            uint64_t valid;
            uint32_t noise;
            uint32_t stops;
        } total;            // REC_STOP, REC_POWER
        int32_t delta_s;    // REC_TIMEFIX
        uint8_t raw[16];
    };
} rec_t;

_Static_assert(sizeof(rec_t) == RINGLOG_DATA, "rec_t deve ocupar um registro do ringlog");

// Correção de hora: vale para os registros não confirmados com from_seq < seq < to_seq
typedef struct {
    uint32_t from_seq;
    uint32_t to_seq;
    int32_t delta_s;
    uint32_t power_seq;     // queda de energia cuja duração é delta_s (0 = nenhuma)
} fix_t;

typedef struct {
    fix_t f[MAX_FIXES];
    int n;
} fixes_t;

static SemaphoreHandle_t s_mtx;
static ringlog_t s_log;
static bool s_has_test;
static ringlog_pos_t s_start;       // REC_START do teste atual
static bool s_fix_pending;          // houve queda de energia e a hora ainda não foi confirmada

// Intervalo em andamento
static struct {
    uint64_t base_valid;
    uint64_t base_noise;
    uint32_t base_stops;
    uint32_t count;                 // intervalos completos
    uint32_t stop_s;
    uint32_t start_epoch;
    bool start_ok;
} s_iv;

// Pulsos por minuto da última hora (só em RAM)
static struct {
    uint32_t valid[HISTORY_MINUTES];
    uint32_t noise[HISTORY_MINUTES];
    uint32_t mark_s;                // tempo de teste no início do minuto atual
    tally_t base;
    tally_t cur;
} s_min;

#define LOCK()      xSemaphoreTake(s_mtx, portMAX_DELAY)
#define UNLOCK()    xSemaphoreGive(s_mtx)

/* ---------- Gravação ---------- */
static void append(rec_t *r, uint32_t epoch, bool time_ok)
{
    r->epoch = epoch;
    r->flags = time_ok ? FLAG_TIME_OK : 0;
    if (ringlog_append(&s_log, r) != ESP_OK) {
        ESP_LOGW(TAG, "falha ao gravar registro %d", r->type);
    }
}

static void append_now(rec_t *r)
{
    append(r, wallclock_now(), wallclock_confirmed());
}

static void reset_minutes(const tally_t *t, uint32_t elapsed_s)
{
    memset(&s_min, 0, sizeof(s_min));
    s_min.mark_s = elapsed_s;
    s_min.base = *t;
    s_min.cur = *t;
}

static void begin_interval(const tally_t *t)
{
    s_iv.base_valid = t->valid;
    s_iv.base_noise = t->noise;
    s_iv.base_stops = t->stops;
    s_iv.stop_s = 0;
    s_iv.start_epoch = wallclock_now();
    s_iv.start_ok = wallclock_confirmed();
}

static void write_interval(const tally_t *t, uint32_t dur_s)
{
    rec_t r = {
        .type = REC_INTERVAL,
        .iv = {
            .valid = (uint32_t)(t->valid - s_iv.base_valid),
            .noise = (uint32_t)(t->noise - s_iv.base_noise),
            .stop_s = s_iv.stop_s > UINT16_MAX ? UINT16_MAX : s_iv.stop_s,
            .stops = (uint16_t)(t->stops - s_iv.base_stops),
            .dur_s = dur_s,
        },
    };
    append(&r, s_iv.start_epoch, s_iv.start_ok);
    s_iv.count++;
    begin_interval(t);
}

/* ---------- Leitura ---------- */
// Primeiro registro do teste atual (ou o mais antigo, se o início já foi sobrescrito)
static bool first_rec(ringlog_pos_t *pos, rec_t *r)
{
    if (!s_has_test) {
        return false;
    }
    *pos = s_start;
    if (ringlog_read(&s_log, pos, r)) {
        return true;
    }
    if (!ringlog_newest(&s_log, pos, r)) {
        return false;
    }
    ringlog_pos_t p = *pos;
    rec_t tmp;
    while (ringlog_prev(&s_log, &p, &tmp)) {
        *pos = p;
        *r = tmp;
    }
    return true;
}

static void collect_fixes(fixes_t *fx)
{
    fx->n = 0;
    ringlog_pos_t pos;
    rec_t r;
    uint32_t from = 0, power = 0;
    for (bool ok = first_rec(&pos, &r); ok; ok = ringlog_next(&s_log, &pos, &r)) {
        if (r.type == REC_START) {
            from = pos.seq;
        } else if (r.type == REC_POWER) {
            power = pos.seq;
        } else if (r.type == REC_TIMEFIX && fx->n < MAX_FIXES) {
            fx->f[fx->n++] = (fix_t) { .from_seq = from, .to_seq = pos.seq, .delta_s = r.delta_s, .power_seq = power };
            from = pos.seq;
            power = 0;
        }
    }
}

// Hora do registro já corrigida; `ok` = hora confirmada
static uint32_t rec_epoch(const rec_t *r, uint32_t seq, const fixes_t *fx, bool *ok)
{
    *ok = r->flags & FLAG_TIME_OK;
    if (*ok || r->epoch == 0) {
        return r->epoch;
    }
    for (int i = 0; i < fx->n; i++) {
        if (seq > fx->f[i].from_seq && seq < fx->f[i].to_seq) {
            *ok = true;
            return r->epoch + fx->f[i].delta_s;
        }
    }
    return r->epoch;
}

// Duração de uma queda de energia (s), ou -1 se desconhecida
static int32_t power_off_s(uint32_t seq, const fixes_t *fx)
{
    for (int i = 0; i < fx->n; i++) {
        if (fx->f[i].power_seq == seq) {
            return fx->f[i].delta_s;
        }
    }
    return -1;
}

/* ---------- API ---------- */
esp_err_t history_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    esp_err_t err = ringlog_open(&s_log, "history");
    if (err != ESP_OK) {
        return err;
    }
    // Procura, de trás para frente, o início do último teste e se há hora a confirmar
    ringlog_pos_t pos;
    rec_t r;
    bool decided = false;
    for (bool ok = ringlog_newest(&s_log, &pos, &r); ok; ok = ringlog_prev(&s_log, &pos, &r)) {
        if (!decided && (r.type == REC_POWER || r.type == REC_TIMEFIX)) {
            s_fix_pending = r.type == REC_POWER;
            decided = true;
        }
        if (r.type == REC_START) {
            s_start = pos;
            s_has_test = true;
            break;
        }
    }
    return ESP_OK;
}

void history_test_started(void)
{
    LOCK();
    rec_t r = { .type = REC_START };
    append_now(&r);
    s_start = (ringlog_pos_t) { .slot = s_log.head, .seq = s_log.seq };
    s_has_test = true;
    s_fix_pending = false;
    tally_t zero = { 0 };
    memset(&s_iv, 0, sizeof(s_iv));
    begin_interval(&zero);
    reset_minutes(&zero, 0);
    UNLOCK();
}

void history_test_stopped(const tally_t *t, uint32_t elapsed_s)
{
    LOCK();
    uint32_t done = s_iv.count * HISTORY_INTERVAL_S;
    if (elapsed_s > done) {
        write_interval(t, elapsed_s - done);
    }
    rec_t r = { .type = REC_STOP, .total = { .valid = t->valid, .noise = (uint32_t)t->noise, .stops = t->stops } };
    append_now(&r);
    UNLOCK();
}

void history_power_resumed(const tally_t *t, uint32_t elapsed_s, bool last_time_ok)
{
    LOCK();
    // Reconstrói o intervalo em andamento a partir dos intervalos já gravados
    memset(&s_iv, 0, sizeof(s_iv));
    tally_t base = { 0 };
    ringlog_pos_t pos;
    rec_t r;
    for (bool ok = first_rec(&pos, &r); ok; ok = ringlog_next(&s_log, &pos, &r)) {
        if (r.type == REC_INTERVAL) {
            base.valid += r.iv.valid;
            base.noise += r.iv.noise;
            base.stops += r.iv.stops;
            s_iv.count++;
        }
    }
    begin_interval(&base);
    // O intervalo começou antes da queda: a hora agora é a de logo antes dela
    uint32_t partial = elapsed_s - s_iv.count * HISTORY_INTERVAL_S;
    if (s_iv.start_epoch > partial) {
        s_iv.start_epoch -= partial;
        s_iv.start_ok = last_time_ok;
    }
    reset_minutes(t, elapsed_s);

    rec_t ev = { .type = REC_POWER, .total = { .valid = t->valid, .noise = (uint32_t)t->noise, .stops = t->stops } };
    append_now(&ev);
    s_fix_pending = true;
    UNLOCK();
}

void history_time_fixed(int32_t delta_s)
{
    LOCK();
    if (s_fix_pending) {
        rec_t r = { .type = REC_TIMEFIX, .delta_s = delta_s };
        append_now(&r);
        s_fix_pending = false;
        if (!s_iv.start_ok && s_iv.start_epoch) {
            s_iv.start_epoch += delta_s;
            s_iv.start_ok = true;
        }
        ESP_LOGI(TAG, "hora confirmada: correção de %ld s", (long)delta_s);
    }
    UNLOCK();
}

void history_check(uint64_t value, uint64_t expected)
{
    LOCK();
    rec_t r = { .type = REC_CHECK, .check = { .value = value, .expected = expected } };
    append_now(&r);
    UNLOCK();
}

void history_tick(const tally_t *t, uint32_t elapsed_s, bool machine_stopped)
{
    LOCK();
    if (machine_stopped) {
        s_iv.stop_s++;
    }
    if (elapsed_s >= (s_iv.count + 1) * HISTORY_INTERVAL_S) {
        write_interval(t, HISTORY_INTERVAL_S);
    }
    if (elapsed_s >= s_min.mark_s + 60) {
        memmove(s_min.valid, s_min.valid + 1, sizeof(s_min.valid) - sizeof(s_min.valid[0]));
        memmove(s_min.noise, s_min.noise + 1, sizeof(s_min.noise) - sizeof(s_min.noise[0]));
        s_min.valid[HISTORY_MINUTES - 1] = (uint32_t)(t->valid - s_min.base.valid);
        s_min.noise[HISTORY_MINUTES - 1] = (uint32_t)(t->noise - s_min.base.noise);
        s_min.base = *t;
        s_min.mark_s += 60;
    }
    s_min.cur = *t;
    UNLOCK();
}

void history_per_minute(uint32_t valid[HISTORY_MINUTES], uint32_t noise[HISTORY_MINUTES])
{
    LOCK();
    for (int i = 0; i < HISTORY_MINUTES - 1; i++) {
        valid[i] = s_min.valid[i + 1];
        noise[i] = s_min.noise[i + 1];
    }
    valid[HISTORY_MINUTES - 1] = (uint32_t)(s_min.cur.valid - s_min.base.valid);
    noise[HISTORY_MINUTES - 1] = (uint32_t)(s_min.cur.noise - s_min.base.noise);
    UNLOCK();
}

/* ---------- Saída ---------- */
void history_json(history_out_t out, void *ctx)
{
    char line[160];
    fixes_t fx;
    ringlog_pos_t pos;
    rec_t r;
    bool ok_t, first;

    LOCK();
    collect_fixes(&fx);

    out(ctx, "{\"intervals\":[");
    first = true;
    for (bool ok = first_rec(&pos, &r); ok; ok = ringlog_next(&s_log, &pos, &r)) {
        if (r.type != REC_INTERVAL) {
            continue;
        }
        uint32_t t = rec_epoch(&r, pos.seq, &fx, &ok_t);
        snprintf(line, sizeof(line),
                 "%s{\"t\":%" PRIu32 ",\"ok\":%d,\"dur\":%" PRIu32 ",\"valid\":%" PRIu32 ",\"noise\":%" PRIu32
                 ",\"stop_s\":%u,\"stops\":%u}",
                 first ? "" : ",", t, ok_t, r.iv.dur_s, r.iv.valid, r.iv.noise, r.iv.stop_s, r.iv.stops);
        out(ctx, line);
        first = false;
    }

    out(ctx, "],\"events\":[");
    first = true;
    for (bool ok = first_rec(&pos, &r); ok; ok = ringlog_next(&s_log, &pos, &r)) {
        uint32_t t = rec_epoch(&r, pos.seq, &fx, &ok_t);
        const char *sep = first ? "" : ",";
        switch (r.type) {
        case REC_START:
            snprintf(line, sizeof(line), "%s{\"t\":%" PRIu32 ",\"ok\":%d,\"type\":\"start\"}", sep, t, ok_t);
            break;
        case REC_STOP:
            snprintf(line, sizeof(line), "%s{\"t\":%" PRIu32 ",\"ok\":%d,\"type\":\"stop\",\"valid\":%" PRIu64 "}",
                     sep, t, ok_t, r.total.valid);
            break;
        case REC_POWER: {
            int32_t off = power_off_s(pos.seq, &fx);
            char off_s[16] = "null";
            if (off >= 0) {
                snprintf(off_s, sizeof(off_s), "%ld", (long)off);
            }
            snprintf(line, sizeof(line),
                     "%s{\"t\":%" PRIu32 ",\"ok\":%d,\"type\":\"power\",\"valid\":%" PRIu64 ",\"off_s\":%s}",
                     sep, t, ok_t, r.total.valid, off_s);
            break;
        }
        case REC_CHECK:
            snprintf(line, sizeof(line),
                     "%s{\"t\":%" PRIu32 ",\"ok\":%d,\"type\":\"check\",\"value\":%" PRIu64 ",\"expected\":%" PRIu64 "}",
                     sep, t, ok_t, r.check.value, r.check.expected);
            break;
        default:
            continue;
        }
        out(ctx, line);
        first = false;
    }
    out(ctx, "]}");
    UNLOCK();
}

// Data em hora local, ou vazio se desconhecida
static void fmt_time(uint32_t epoch, char *buf, int len)
{
    if (epoch) {
        wallclock_format(epoch, buf, len);
    } else {
        buf[0] = 0;
    }
}

/*
 * CSV para o Excel em português: separador ";", vírgula decimal e BOM UTF-8 no início.
 */
// Uma linha do CSV de intervalos; `end` = fim real (0 = início + duração)
static void csv_interval_line(history_out_t out, void *ctx, const rec_t *r, uint32_t start, bool ok_t, uint32_t end,
                              uint64_t total)
{
    char line[200], t0[20], t1[20];
    fmt_time(start, t0, sizeof(t0));
    fmt_time(start ? (end ? end : start + r->iv.dur_s) : 0, t1, sizeof(t1));
    uint32_t active = r->iv.dur_s > r->iv.stop_s ? r->iv.dur_s - r->iv.stop_s : 0;
    uint32_t hz100 = active ? (uint32_t)((uint64_t)r->iv.valid * 100 / active) : 0;
    snprintf(line, sizeof(line), "%s;%s;%" PRIu32 ";%" PRIu32 ";%u;%u;%" PRIu32 ",%02" PRIu32 ";%" PRIu64 ";%s\r\n",
             t0, t1, r->iv.valid, r->iv.noise, r->iv.stops, r->iv.stop_s, hz100 / 100, hz100 % 100, total,
             ok_t ? "sim" : "nao");
    out(ctx, line);
}

void history_csv_intervals(history_out_t out, void *ctx)
{
    fixes_t fx;
    ringlog_pos_t pos;
    rec_t r, prev;
    uint32_t prev_t = 0;
    bool prev_ok = false, has_prev = false;
    uint64_t total = 0;

    LOCK();
    collect_fixes(&fx);
    out(ctx, "\xEF\xBB\xBF" "inicio;fim;pulsos_validos;ruidos;paradas;tempo_parado_s;freq_media_hz;total_validos;hora_confirmada\r\n");
    // Cada linha sai quando o próximo intervalo é lido: se ele começa depois do fim
    // calculado, houve uma queda de energia no meio e o fim real é o início dele
    for (bool ok = first_rec(&pos, &r); ok; ok = ringlog_next(&s_log, &pos, &r)) {
        if (r.type != REC_INTERVAL) {
            continue;
        }
        bool ok_t;
        uint32_t t = rec_epoch(&r, pos.seq, &fx, &ok_t);
        if (has_prev) {
            total += prev.iv.valid;
            uint32_t end = prev_t && t > prev_t + prev.iv.dur_s ? t : 0;
            csv_interval_line(out, ctx, &prev, prev_t, prev_ok, end, total);
        }
        prev = r;
        prev_t = t;
        prev_ok = ok_t;
        has_prev = true;
    }
    if (has_prev) {
        total += prev.iv.valid;
        csv_interval_line(out, ctx, &prev, prev_t, prev_ok, 0, total);
    }
    UNLOCK();
}

void history_csv_events(history_out_t out, void *ctx)
{
    char line[200], t0[20], detail[64];
    fixes_t fx;
    ringlog_pos_t pos;
    rec_t r;
    bool ok_t;

    LOCK();
    collect_fixes(&fx);
    out(ctx, "\xEF\xBB\xBF" "data_hora;evento;valor_contador;enviados;diferenca;detalhe\r\n");
    for (bool ok = first_rec(&pos, &r); ok; ok = ringlog_next(&s_log, &pos, &r)) {
        uint32_t t = rec_epoch(&r, pos.seq, &fx, &ok_t);
        fmt_time(t, t0, sizeof(t0));
        const char *unconf = ok_t ? "" : " (hora não confirmada)";
        switch (r.type) {
        case REC_START:
            snprintf(line, sizeof(line), "%s;inicio;;;;%s\r\n", t0, unconf);
            break;
        case REC_STOP:
            snprintf(line, sizeof(line), "%s;fim;;%" PRIu64 ";;teste interrompido%s\r\n", t0, r.total.valid, unconf);
            break;
        case REC_POWER: {
            int32_t off = power_off_s(pos.seq, &fx);
            if (off >= 0) {
                snprintf(detail, sizeof(detail), "desligado por %ld s; teste retomado", (long)off);
            } else {
                snprintf(detail, sizeof(detail), "duração desconhecida; teste retomado");
            }
            snprintf(line, sizeof(line), "%s;queda_energia;;%" PRIu64 ";;%s%s\r\n", t0, r.total.valid, detail, unconf);
            break;
        }
        case REC_CHECK: {
            int64_t diff = (int64_t)(r.check.value - r.check.expected);
            snprintf(line, sizeof(line), "%s;conferencia;%" PRIu64 ";%" PRIu64 ";%" PRId64 ";%s\r\n", t0,
                     r.check.value, r.check.expected, diff, unconf);
            break;
        }
        default:
            continue;
        }
        out(ctx, line);
    }
    UNLOCK();
}
