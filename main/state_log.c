#include "state_log.h"

#include <string.h>

#include "esp_log.h"
#include "ringlog.h"

static const char *TAG = "state";

// Dados de cada registro do anel
typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t stops;
    uint64_t valid;
    uint32_t noise;
    uint32_t elapsed_s;
    uint32_t epoch;
    uint64_t rejects;
    uint64_t valid_rep;
    uint32_t rejects_rep;
    uint16_t reports;
    uint8_t rsv[10];
} rec_t;

_Static_assert(sizeof(rec_t) == RINGLOG_DATA, "rec_t deve ocupar um registro do ringlog");

#define FLAG_RUNNING    0x0001
#define FLAG_TIME_OK    0x0002

static ringlog_t s_log;

esp_err_t state_log_init(void)
{
    return ringlog_open(&s_log, "autostate");
}

bool state_log_last(state_rec_t *rec)
{
    ringlog_pos_t pos;
    rec_t r;
    if (!ringlog_newest(&s_log, &pos, &r)) {
        return false;
    }
    *rec = (state_rec_t) {
        .running = r.flags & FLAG_RUNNING,
        .time_ok = r.flags & FLAG_TIME_OK,
        .stops = r.stops,
        .valid = r.valid,
        .noise = r.noise,
        .elapsed_s = r.elapsed_s,
        .epoch = r.epoch,
        .rejects = r.rejects,
        .valid_rep = r.valid_rep,
        .rejects_rep = r.rejects_rep,
        .reports = r.reports,
    };
    ESP_LOGI(TAG, "último estado: %s", rec->running ? "teste rodando" : "sem teste");
    return true;
}

esp_err_t state_log_write(const state_rec_t *rec)
{
    rec_t r = {
        .flags = (rec->running ? FLAG_RUNNING : 0) | (rec->time_ok ? FLAG_TIME_OK : 0),
        .stops = rec->stops,
        .valid = rec->valid,
        .noise = rec->noise,
        .elapsed_s = rec->elapsed_s,
        .epoch = rec->epoch,
        .rejects = rec->rejects,
        .valid_rep = rec->valid_rep,
        .rejects_rep = rec->rejects_rep,
        .reports = rec->reports,
    };
    return ringlog_append(&s_log, &r);
}
