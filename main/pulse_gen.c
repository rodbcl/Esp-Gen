#include "pulse_gen.h"

#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "pulse";

/* 10 MHz -> 0,1 us de resolução (a 10 kHz, 1% de duty = 10 ticks) */
#define RMT_RES_HZ    (1000000UL * PULSE_GEN_TICKS_PER_US)
/*
 * Duração máxima de cada meio-símbolo. O limite do hardware é 32767, mas
 * mantê-lo curto garante que a memória do RMT (64 símbolos) esvazie
 * rapidamente quando a geração é interrompida.
 */
#define MAX_HALF_TICKS 2000

// Motor: pede trechos à fonte e os entrega ao RMT em pedaços de até MAX_HALF_TICKS
typedef struct {
    pulse_gen_source_t src;
    void *ctx;
    uint32_t seg_level;
    uint32_t seg_left;
    volatile bool abort;
} engine_t;

// Fonte de frequência fixa, usada por pulse_gen_start()
typedef struct {
    uint32_t freq;
    uint32_t duty;
    uint32_t period_base;   /* RMT_RES_HZ / freq */
    uint32_t period_rem;    /* RMT_RES_HZ % freq, distribuído via acumulador */
    uint32_t err;
    uint64_t periods_left;
    uint32_t cur_toff;
    bool off_pending;
} fixed_t;

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;
static engine_t s_eng;
static fixed_t s_fixed;
static volatile bool s_running;

// Nova freq/duty pedida por pulse_gen_set(), aplicada pela fonte fixa no início do próximo período
static portMUX_TYPE s_upd_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool pending;
    uint32_t freq;
    uint32_t duty;
} s_upd;

/* ---------- Fonte fixa ---------- */
static void set_freq(fixed_t *f, uint32_t freq_hz, uint32_t duty_pct)
{
    f->freq = freq_hz;
    f->duty = duty_pct;
    f->period_base = RMT_RES_HZ / freq_hz;
    f->period_rem = RMT_RES_HZ % freq_hz;
    f->err = 0;
}

static void apply_update(fixed_t *f)
{
    portENTER_CRITICAL_SAFE(&s_upd_lock);
    if (s_upd.pending) {
        s_upd.pending = false;
        set_freq(f, s_upd.freq, s_upd.duty);
    }
    portEXIT_CRITICAL_SAFE(&s_upd_lock);
}

// Cada período vira dois trechos: ativo (ton) e inativo (toff)
static bool fixed_next(void *ctx, uint32_t *level, uint32_t *ticks)
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

/* ---------- Motor ---------- */
// Fornece o próximo meio-símbolo (nível, duração). Retorna false quando acabou.
static bool next_half(engine_t *e, uint32_t *level, uint32_t *dur)
{
    while (e->seg_left == 0) {
        if (e->abort || !e->src(e->ctx, &e->seg_level, &e->seg_left)) {
            return false;
        }
    }
    if (e->abort) {
        e->seg_left = 0;
        return false;
    }
    uint32_t chunk = e->seg_left > MAX_HALF_TICKS ? MAX_HALF_TICKS : e->seg_left;
    e->seg_left -= chunk;
    *level = e->seg_level;
    *dur = chunk;
    return true;
}

static size_t encode_cb(const void *data, size_t data_size, size_t symbols_written, size_t symbols_free,
                        rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    engine_t *e = arg;
    size_t n = 0;
    while (n < symbols_free) {
        uint32_t l0, d0, l1, d1;
        if (!next_half(e, &l0, &d0)) {
            *done = true;
            break;
        }
        bool last = false;
        if (!next_half(e, &l1, &d1)) {
            l1 = 0; /* completa o símbolo com 0,1 us em nível inativo */
            d1 = 1;
            last = true;
        }
        symbols[n++] = (rmt_symbol_word_t) {
            .level0 = l0, .duration0 = d0, .level1 = l1, .duration1 = d1,
        };
        if (last) {
            *done = true;
            break;
        }
    }
    return n;
}

static bool on_done(rmt_channel_handle_t chan, const rmt_tx_done_event_data_t *ev, void *ctx)
{
    s_running = false;
    return false;
}

esp_err_t pulse_gen_init(int gpio, bool invert)
{
    rmt_tx_channel_config_t cfg = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RES_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 2,
        .flags.invert_out = invert,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&cfg, &s_chan), TAG, "rmt channel");

    rmt_tx_event_callbacks_t cbs = { .on_trans_done = on_done };
    ESP_RETURN_ON_ERROR(rmt_tx_register_event_callbacks(s_chan, &cbs, NULL), TAG, "rmt cbs");

    rmt_simple_encoder_config_t enc_cfg = {
        .callback = encode_cb,
        .arg = &s_eng,
        .min_chunk_size = 2,
    };
    ESP_RETURN_ON_ERROR(rmt_new_simple_encoder(&enc_cfg, &s_enc), TAG, "rmt encoder");
    ESP_RETURN_ON_ERROR(rmt_enable(s_chan), TAG, "rmt enable");
    return ESP_OK;
}

esp_err_t pulse_gen_start_source(pulse_gen_source_t src, void *ctx)
{
    if (s_running || src == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_eng = (engine_t) { .src = src, .ctx = ctx };

    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
        .flags.eot_level = 0, /* nível inativo ao terminar */
    };
    s_running = true;
    esp_err_t err = rmt_transmit(s_chan, s_enc, &s_eng, sizeof(s_eng), &tx_cfg);
    if (err != ESP_OK) {
        s_running = false;
    }
    return err;
}

esp_err_t pulse_gen_start(uint32_t freq_hz, uint32_t duty_pct, uint64_t periods)
{
    if (s_running || freq_hz == 0 || periods == 0 || freq_hz > RMT_RES_HZ / 20) {
        return ESP_ERR_INVALID_ARG;
    }
    if (duty_pct > 100) {
        duty_pct = 100;
    }

    s_fixed = (fixed_t) { .periods_left = periods };
    set_freq(&s_fixed, freq_hz, duty_pct);
    s_upd.pending = false;

    esp_err_t err = pulse_gen_start_source(fixed_next, &s_fixed);
    if (err != ESP_OK) {
        return err;
    }
    if (periods == PULSE_GEN_CONTINUOUS) {
        ESP_LOGI(TAG, "start: %lu Hz, %lu %%, contínuo", (unsigned long)freq_hz, (unsigned long)duty_pct);
    } else {
        ESP_LOGI(TAG, "start: %lu Hz, %lu %%, %llu períodos",
                 (unsigned long)freq_hz, (unsigned long)duty_pct, (unsigned long long)periods);
    }
    return ESP_OK;
}

esp_err_t pulse_gen_set(uint32_t freq_hz, uint32_t duty_pct)
{
    if (freq_hz == 0 || freq_hz > RMT_RES_HZ / 20) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_running || s_eng.src != fixed_next) {
        return ESP_ERR_INVALID_STATE;
    }
    if (duty_pct > 100) {
        duty_pct = 100;
    }
    portENTER_CRITICAL(&s_upd_lock);
    s_upd.freq = freq_hz;
    s_upd.duty = duty_pct;
    s_upd.pending = true;
    portEXIT_CRITICAL(&s_upd_lock);
    ESP_LOGI(TAG, "set: %lu Hz, %lu %%", (unsigned long)freq_hz, (unsigned long)duty_pct);
    return ESP_OK;
}

void pulse_gen_stop(void)
{
    if (!s_running) {
        return;
    }
    s_eng.abort = true;
    /* O encoder encerra na próxima recarga; a memória do RMT esvazia em poucos ms */
    if (rmt_tx_wait_all_done(s_chan, 500) != ESP_OK) {
        ESP_LOGW(TAG, "stop forçado");
        rmt_disable(s_chan);
        rmt_enable(s_chan);
    }
    s_running = false;
    ESP_LOGI(TAG, "stop");
}

bool pulse_gen_running(void)
{
    return s_running;
}
