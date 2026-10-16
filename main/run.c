#include "run.h"

#include <string.h>

#include "esp_timer.h"
#include "pulse_gen.h"

static settings_t s_backup;
static int64_t s_start_us;
static uint64_t s_periods;

const char *run_start(void)
{
    settings_t *cfg = settings_get();
    if (cfg->freq == 0) {
        return "Frequencia = 0";
    }
    if (cfg->duty == 0) {
        return "Duty = 0%";
    }
    uint64_t periods;
    if (cfg->mode == MODE_CONT) {
        periods = PULSE_GEN_CONTINUOUS;
    } else {
        periods = cfg->mode == MODE_TIME ? (uint64_t)cfg->freq * cfg->time_s : cfg->qty;
    }
    if (periods == 0) {
        return cfg->mode == MODE_TIME ? "Tempo = 0" : "Quantidade = 0";
    }
    if (pulse_gen_start(cfg->freq, cfg->duty, periods) != ESP_OK) {
        return "Falha ao iniciar";
    }
    s_periods = periods;
    s_start_us = esp_timer_get_time();
    s_backup = *cfg;
    return NULL;
}

void run_adjust(settings_field_t field, int steps, int accel)
{
    settings_t *cfg = settings_get();
    uint32_t freq = cfg->freq;
    uint16_t duty = cfg->duty;
    settings_adjust(field, steps, accel);
    /* 0 Hz ou 0% não são aceitos com a geração rodando */
    if (cfg->freq == 0) {
        cfg->freq = 1;
    }
    if (cfg->duty == 0) {
        cfg->duty = 1;
    }
    if (cfg->freq != freq || cfg->duty != duty) {
        pulse_gen_set(cfg->freq, cfg->duty);
    }
}

void run_stop(void)
{
    pulse_gen_stop();
    if (memcmp(&s_backup, settings_get(), sizeof(s_backup)) != 0) {
        settings_save();
    }
}

bool run_finished(void)
{
    return !pulse_gen_running();
}

int64_t run_elapsed_us(void)
{
    return esp_timer_get_time() - s_start_us;
}

uint64_t run_periods(void)
{
    return s_periods;
}
