#include "settings.h"

#include "nvs.h"
#include "util.h"

#define SETTINGS_VERSION 2     // v1 guardava a frequência em Hz

static settings_t s_cfg = {
    .version = SETTINGS_VERSION,
    .mode = MODE_TIME,
    .duty = 50,
    .freq_dhz = 10000,
    .time_s = 10,
    .qty = 100,
};

void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    settings_t tmp;
    size_t len = sizeof(tmp);
    if (nvs_get_blob(h, "s", &tmp, &len) == ESP_OK && len == sizeof(tmp) && tmp.mode < MODE_COUNT &&
        tmp.mode != MODE_AUTO && tmp.mode != MODE_AGENT) {
        if (tmp.version == 1) {
            tmp.freq_dhz *= 10;
            tmp.version = SETTINGS_VERSION;
        }
        if (tmp.version == SETTINGS_VERSION) {
            s_cfg = tmp;
        }
    }
    nvs_close(h);
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, "s", &s_cfg, sizeof(s_cfg));
    nvs_commit(h);
    nvs_close(h);
}

settings_t *settings_get(void)
{
    return &s_cfg;
}

int settings_field_count(void)
{
    if (s_cfg.mode == MODE_AUTO || s_cfg.mode == MODE_AGENT) {
        return FIELD_FREQ;
    }
    return s_cfg.mode == MODE_CONT ? FIELD_AMOUNT : FIELD_COUNT;
}

/*
 * Abaixo de 1 Hz cada clique vale 0,1 Hz; a partir de 1 Hz, 1, 10 ou 100 Hz conforme `accel`.
 * Descendo, o passo inteiro para em 1 Hz antes de entrar na faixa de décimos.
 */
static uint32_t freq_step(uint32_t f, int dir, int accel)
{
    if (dir > 0) {
        return f < 10 ? f + 1 : (uint32_t)clamp_i32((int32_t)f + accel * 10, 0, FREQ_DHZ_MAX);
    }
    if (f <= 10) {
        return f > 0 ? f - 1 : 0;
    }
    return (uint32_t)clamp_i32((int32_t)f - accel * 10, 10, FREQ_DHZ_MAX);
}

void settings_adjust(settings_field_t field, int steps, int accel)
{
    switch (field) {
    case FIELD_MODE: {
        int m = ((int)s_cfg.mode + steps) % MODE_COUNT;
        s_cfg.mode = m < 0 ? m + MODE_COUNT : m;
        break;
    }
    case FIELD_FREQ:
        for (int i = 0; i < (steps < 0 ? -steps : steps); i++) {
            s_cfg.freq_dhz = freq_step(s_cfg.freq_dhz, steps, accel);
        }
        break;
    case FIELD_DUTY:
        s_cfg.duty = clamp_i32((int32_t)s_cfg.duty + steps * (accel > 1 ? 5 : 1), 0, DUTY_MAX);
        break;
    case FIELD_AMOUNT:
        if (s_cfg.mode == MODE_TIME) {
            // 1 s, 10 s ou 1 min por clique conforme a velocidade
            int step = accel >= 100 ? 60 : accel;
            s_cfg.time_s = clamp_i32((int32_t)s_cfg.time_s + steps * step, 0, TIME_MAX_S);
        } else {
            s_cfg.qty = clamp_i32((int32_t)s_cfg.qty + steps * accel, 0, QTY_MAX);
        }
        break;
    default:
        break;
    }
}
