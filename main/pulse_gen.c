#include "pulse_gen.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "util.h"

static const char *TAG = "pulse";

/* Ticks em 10 s: período = RMT_RES_DHZ / freq em décimos de Hz (a 0,1 Hz, 1e8 ticks) */
#define RMT_RES_DHZ   (1000000UL * PULSE_GEN_TICKS_PER_US * 10)
/* Frequência máxima: período de 20 ticks (2 us) */
#define FREQ_DHZ_LIM  (RMT_RES_DHZ / 20)

// Fonte de frequência fixa, usada por pulse_gen_start()
typedef struct {
    uint32_t freq;          /* décimos de Hz */
    uint32_t duty;
    uint32_t period_base;   /* RMT_RES_DHZ / freq */
    uint32_t period_rem;    /* RMT_RES_DHZ % freq, distribuído via acumulador */
    uint32_t err;
    uint64_t periods_left;
    uint32_t cur_toff;
    bool off_pending;
} fixed_t;

static rmt_out_t *s_out;
static fixed_t s_fixed;
static bool s_fixed_src;    // a geração em andamento usa a fonte fixa

// Nova freq/duty pedida por pulse_gen_set(), aplicada pela fonte fixa no início do próximo período
static portMUX_TYPE s_upd_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool pending;
    uint32_t freq;
    uint32_t duty;
} s_upd;

/* ---------- Fonte fixa ---------- */
static void IRAM_ATTR set_freq(fixed_t *f, uint32_t freq_dhz, uint32_t duty_pct)
{
    f->freq = freq_dhz;
    f->duty = duty_pct;
    f->period_base = RMT_RES_DHZ / freq_dhz;
    f->period_rem = RMT_RES_DHZ % freq_dhz;
    f->err = 0;
}

static void IRAM_ATTR apply_update(fixed_t *f)
{
    portENTER_CRITICAL_SAFE(&s_upd_lock);
    if (s_upd.pending) {
        s_upd.pending = false;
        set_freq(f, s_upd.freq, s_upd.duty);
    }
    portEXIT_CRITICAL_SAFE(&s_upd_lock);
}

// Cada período vira dois trechos: ativo (ton) e inativo (toff)
static bool IRAM_ATTR fixed_next(void *ctx, uint32_t *level, uint32_t *ticks)
{
    fixed_t *f = ctx;
    if (f->off_pending) {
        f->off_pending = false;
        *level = 0;
        *ticks = f->cur_toff;
        return true;
    }
    if (f->periods_left == 0) {
        return false;
    }
    if (f->periods_left != PULSE_GEN_CONTINUOUS) {
        f->periods_left--;
    }
    apply_update(f);

    uint32_t p = f->period_base;
    f->err += f->period_rem;
    if (f->err >= f->freq) {
        f->err -= f->freq;
        p++;
    }
    uint32_t ton = (uint32_t)(((uint64_t)p * f->duty) / 100);
    f->cur_toff = p - ton;
    f->off_pending = true;
    *level = 1;
    *ticks = ton;
    return true;
}

/* ---------- API ---------- */
esp_err_t pulse_gen_init(int gpio, bool invert)
{
    return rmt_out_new(gpio, invert, &s_out);
}

esp_err_t pulse_gen_start_source(pulse_gen_source_t src, void *ctx)
{
    esp_err_t err = rmt_out_start(s_out, src, ctx);
    if (err == ESP_OK) {
        s_fixed_src = src == fixed_next;
    }
    return err;
}

esp_err_t pulse_gen_start(uint32_t freq_dhz, uint32_t duty_pct, uint64_t periods)
{
    if (rmt_out_running(s_out) || freq_dhz == 0 || periods == 0 || freq_dhz > FREQ_DHZ_LIM) {
        return ESP_ERR_INVALID_ARG;
    }
    if (duty_pct > 100) {
        duty_pct = 100;
    }

    s_fixed = (fixed_t) { .periods_left = periods };
    set_freq(&s_fixed, freq_dhz, duty_pct);
    s_upd.pending = false;

    esp_err_t err = pulse_gen_start_source(fixed_next, &s_fixed);
    if (err != ESP_OK) {
        return err;
    }
    char fs[16];
    fmt_freq(fs, sizeof(fs), freq_dhz);
    if (periods == PULSE_GEN_CONTINUOUS) {
        ESP_LOGI(TAG, "start: %s, %lu %%, contínuo", fs, (unsigned long)duty_pct);
    } else {
        ESP_LOGI(TAG, "start: %s, %lu %%, %llu períodos", fs, (unsigned long)duty_pct, (unsigned long long)periods);
    }
    return ESP_OK;
}

esp_err_t pulse_gen_set(uint32_t freq_dhz, uint32_t duty_pct)
{
    if (freq_dhz == 0 || freq_dhz > FREQ_DHZ_LIM) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!rmt_out_running(s_out) || !s_fixed_src) {
        return ESP_ERR_INVALID_STATE;
    }
    if (duty_pct > 100) {
        duty_pct = 100;
    }
    portENTER_CRITICAL(&s_upd_lock);
    s_upd.freq = freq_dhz;
    s_upd.duty = duty_pct;
    s_upd.pending = true;
    portEXIT_CRITICAL(&s_upd_lock);
    char fs[16];
    fmt_freq(fs, sizeof(fs), freq_dhz);
    ESP_LOGI(TAG, "set: %s, %lu %%", fs, (unsigned long)duty_pct);
    return ESP_OK;
}

void pulse_gen_stop(void)
{
    if (!rmt_out_running(s_out)) {
        return;
    }
    rmt_out_stop(s_out);
    ESP_LOGI(TAG, "stop");
}

bool pulse_gen_running(void)
{
    return rmt_out_running(s_out);
}
